// Host harness for patch 0026 (test_up_shannon.py builds it): upstream's
// PlainConnection.cpp and ShannonConnection.cpp, as patched, over loopback
// TCP, under ASan/LSan. Each case prints "case: took|refused|crashed".
//
//   real          an access point's packet under the session's keys
//   other-key     a packet under another key (what anyone else on the
//                 path can send): its MAC fails, so it must be refused
//   short-size    the handshake answer's size field 00 00 00 01: below
//                 its own 4 bytes, so it must be refused (upstream wrote
//                 the socket past a 1 byte buffer)
//   huge-size     a size of 16 MB: refused (the answer is well under 64 KB)
//   good-size     a well-formed answer of 64 bytes: taken
//   refused-port  connect() to a port nobody listens on, three times:
//                 refused each time; LeakSanitizer at exit sees whether
//                 the address list was freed
#define private public  // the server's side wraps an accepted socket
#include "PlainConnection.h"
#undef private
#include "ShannonConnection.h"
#include "BellLogger.h"

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace cspot;

static int listener(int* port) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, (sockaddr*)&a, sizeof a) || listen(s, 4)) {
    perror("bind");
    exit(2);
  }
  socklen_t l = sizeof a;
  getsockname(s, (sockaddr*)&a, &l);
  *port = ntohs(a.sin_port);
  return s;
}

struct Pair {
  std::shared_ptr<PlainConnection> client, server;
};

static Pair pair() {
  int port;
  int l = listener(&port);
  Pair p{std::make_shared<PlainConnection>(), std::make_shared<PlainConnection>()};
  p.client->timeoutHandler = [] { return true; };
  p.server->timeoutHandler = [] { return true; };
  p.client->connect("127.0.0.1:" + std::to_string(port));
  p.server->apSock = accept(l, nullptr, nullptr);
  close(l);
  return p;
}

static void say(const char* c, const char* r) { printf("%s: %s\n", c, r), fflush(stdout); }

static std::vector<uint8_t> key(uint8_t b) { return std::vector<uint8_t>(32, b); }

int main(int argc, char** argv) {
  // a peer that closes first must fail the write, not kill the harness
  signal(SIGPIPE, SIG_IGN);
  bell::setDefaultLogger();
  std::string only = argc > 1 ? argv[1] : "";
  auto want = [&](const char* c) { return only.empty() || only == c; };

  if (want("real") || want("other-key")) {
    auto p = pair();
    auto k1 = key(0x11), k2 = key(0x22), k3 = key(0x33);
    ShannonConnection client, ap, stranger;
    client.wrapConnection(p.client, k1, k2);  // sends under k1, reads k2
    ap.wrapConnection(p.server, k2, k1);
    stranger.wrapConnection(p.server, k3, k1);
    std::vector<uint8_t> body = {'h', 'e', 'l', 'l', 'o'};
    if (want("real")) {
      ap.sendPacket(0x4a, body);
      try {
        auto got = client.recvPacket();
        say("real", got.command == 0x4a && got.data == body ? "took" : "garbled");
      } catch (const std::exception& e) {
        say("real", "refused");
      }
    }
    if (want("other-key")) {
      // the client reads the stranger's size field as noise (up to 65535):
      // two full packets behind it are more than any size it can read, so
      // the client gets the whole of what it asks for and has only the MAC
      // to go by, not a read that runs dry
      std::vector<uint8_t> evil(65535, 'e');
      std::thread t([&] {
        try {
          stranger.sendPacket(0x4a, evil);
          stranger.sendPacket(0x4a, evil);
        } catch (const std::exception&) {
          // the client hung up first
        }
      });
      try {
        client.recvPacket();
        say("other-key", "took");
      } catch (const std::runtime_error& e) {
        printf("other-key: refused (%s)\n", e.what());
      }
      p.client->close();  // a sender still writing gets an error, not a full buffer
      t.join();
    }
  }

  struct Raw {
    const char* name;
    std::vector<uint8_t> bytes;
  };
  std::vector<uint8_t> good = {0, 0, 0, 68};
  good.resize(68, 0x5a);
  std::vector<uint8_t> shortsz = {0, 0, 0, 1};
  shortsz.resize(4 + 256, 0x41);
  for (auto& r : std::vector<Raw>{{"short-size", shortsz}, {"huge-size", {0x01, 0, 0, 0}}, {"good-size", good}}) {
    if (!want(r.name)) continue;
    auto p = pair();
    p.server->writeBlock(r.bytes);
    // the server's side closes after: a reader that waits for more gets an
    // end of stream, not a hang
    shutdown(p.server->apSock, SHUT_WR);
    try {
      auto got = p.client->recvPacket();
      say(r.name, got.size() == r.bytes.size() ? "took" : "garbled");
    } catch (const std::exception& e) {
      printf("%s: refused (%s)\n", r.name, e.what());
    }
  }

  if (want("refused-port")) {
    int port;
    int l = listener(&port);
    close(l);  // nobody listens there now
    int refused = 0;
    for (int i = 0; i < 3; i++) {
      PlainConnection c;
      c.timeoutHandler = [] { return true; };
      try {
        c.connect("127.0.0.1:" + std::to_string(port));
      } catch (const std::runtime_error& e) {
        refused++;
      }
    }
    say("refused-port", refused == 3 ? "refused" : "took");
  }
  return 0;
}
