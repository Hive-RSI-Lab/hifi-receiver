// test_up_blob.py's program: upstream's Shim.cpp and LoginBlob.cpp (as
// patched) on the fakes in fakes.h, built with ThreadSanitizer.
//
//   harness keeps   a zeroconf login that doesn't parse leaves the account
//                   that was there (LoginBlob::loadZeroconf); exit 1 if not
//   harness race    the httpd task takes a second account's zeroconf POST
//                   while the cspot task is logging the first one in; TSan
//                   stops the run (its exit code) on a data race, and exit 2
//                   if the session logged in as an account it wasn't made for
//   harness snap    the second account's POST lands while the cspot task is
//                   copying the first one out of the blob for its session
//                   (loginSnapshot); TSan's exit code on a data race, exit 2
//                   if the session logged in as an account it wasn't made for
//
// A login blob here is written in the clear: fakes.h's Crypto decodes it
// back to what the harness wrote. Its layout, as LoginBlob parses it:
// iv (16) | 0, 1, 'x', tag, authType, tag, authSize, authData... | MAC (20).
#include "fakes.h"

#include <chrono>

#include "cspot_private.h"

using namespace std::chrono_literals;

static std::string raw_blob(int authType, size_t authSize, const std::string& authData) {
  std::string p = {0, 1, 'x', 0x08, (char)authType, 0x10, (char)authSize};
  p += authData;
  while (p.size() < 24) p += 'z';
  return std::string(16, '\x01') + p + std::string(20, '\xAA');
}

static std::string pct(const std::string& s) {
  std::string out;
  char b[4];
  for (unsigned char c : s) {
    snprintf(b, sizeof b, "%%%02X", c);
    out += b;
  }
  return out;
}

static int keeps() {
  cspot::LoginBlob b("hifi");
  std::map<std::string, std::string> q = {
      {"userName", "alice"}, {"blob", raw_blob(1, 8, "alice-ok")}, {"clientKey", "k"}};
  b.loadZeroconfQuery(q);
  if (b.username != "alice" || b.authType != 1 || b.authData != std::vector<uint8_t>{'a', 'l', 'i', 'c', 'e', '-', 'o', 'k'}) {
    fprintf(stderr, "keeps: a good login didn't load (user %s, type %d)\n", b.username.c_str(), b.authType);
    return 1;
  }
  // a second login whose auth data runs past the end of the blob
  q = {{"userName", "mallory"}, {"blob", raw_blob(5, 100, "short")}, {"clientKey", "k"}};
  bool threw = false;
  try {
    b.loadZeroconfQuery(q);
  } catch (const std::exception&) {
    threw = true;
  }
  if (!threw || b.username != "alice" || b.authType != 1 || b.authData.size() != 8) {
    fprintf(stderr, "keeps: a truncated login %s and left user %s, auth type %d, %zu bytes of auth data\n",
            threw ? "was refused" : "was taken", b.username.c_str(), b.authType, b.authData.size());
    return 1;
  }
  printf("keeps: a truncated login is refused and alice's stays\n");
  return 0;
}

static bool cmd_cb(cspot_event_t, ...) { return true; }
static uint32_t data_cb(const uint8_t*, size_t len) { return len; }

static std::string post(const std::string& user, const std::string& data) {
  httpd_req_t r{};
  r.body = "userName=" + user + "&blob=" + pct(raw_blob(1, data.size(), data)) + "&clientKey=k";
  r.content_len = r.body.size();
  fake::post(&r);
  return r.reply;
}

template <class F>
static bool wait_for(F ready) {
  std::unique_lock l(fake::mu);
  return fake::cv.wait_for(l, 10s, ready);
}

static int race() {
  cspot_create("hifi", (httpd_handle_t)1, 80, cmd_cb, data_cb);
  if (!wait_for([] { return fake::post != nullptr; })) {
    fprintf(stderr, "race: the zeroconf handlers were never registered\n");
    return 1;
  }
  if (post("alice", "alice-token").find("\"status\":101") == std::string::npos) {
    fprintf(stderr, "race: alice's login was refused\n");
    return 1;
  }
  // the cspot task is connecting to an access point with alice's login:
  // bob picks the speaker now
  if (!wait_for([] { return fake::connects > 0; })) {
    fprintf(stderr, "race: the cspot task never connected\n");
    return 1;
  }
  if (post("bob", "bob-token-longer").find("\"status\":101") == std::string::npos) {
    fprintf(stderr, "race: bob's login was refused\n");
    return 1;
  }
  if (!wait_for([] { return fake::logged_in; })) {
    fprintf(stderr, "race: the session never logged in\n");
    return 1;
  }
  std::lock_guard l(fake::mu);
  std::string data(fake::auth_data.begin(), fake::auth_data.end());
  if (fake::auth_user != fake::ctx_user || data != fake::ctx_user + "-token") {
    fprintf(stderr, "race: a session made for %s logged in as %s with %s's token\n", fake::ctx_user.c_str(),
            fake::auth_user.c_str(), data.c_str());
    return 2;
  }
  printf("race: the session made for %s logged in as %s, no data race\n", fake::ctx_user.c_str(),
         fake::auth_user.c_str());
  return 0;
}

// the cspot task's second LoginBlob is the copy it makes for a session
// (the first is the speaker's own): hold it there until the harness has
// sent a POST, which then lands while that copy is being taken
static std::atomic<int> dh_inits{0};
static bool snapshotting = false;  // under fake::mu
static void hold_second_blob() {
  if (++dh_inits != 2) return;
  {
    std::lock_guard l(fake::mu);
    snapshotting = true;
    fake::cv.notify_all();
  }
  usleep(150 * 1000);
}

static int snap() {
  fake::on_dh_init = hold_second_blob;
  cspot_create("hifi", (httpd_handle_t)1, 80, cmd_cb, data_cb);
  if (!wait_for([] { return fake::post != nullptr; })) {
    fprintf(stderr, "snap: the zeroconf handlers were never registered\n");
    return 1;
  }
  if (post("alice", "alice-token").find("\"status\":101") == std::string::npos) {
    fprintf(stderr, "snap: alice's login was refused\n");
    return 1;
  }
  if (!wait_for([] { return snapshotting; })) {
    fprintf(stderr, "snap: the cspot task never copied the login for a session\n");
    return 1;
  }
  if (post("bob", "bob-token-longer").find("\"status\":101") == std::string::npos) {
    fprintf(stderr, "snap: bob's login was refused\n");
    return 1;
  }
  if (!wait_for([] { return fake::logged_in; })) {
    fprintf(stderr, "snap: the session never logged in\n");
    return 1;
  }
  std::lock_guard l(fake::mu);
  std::string data(fake::auth_data.begin(), fake::auth_data.end());
  // the POST went in before the copy was taken, so the session is bob's
  if (fake::ctx_user != "bob" || fake::auth_user != "bob" || data != "bob-token-longer") {
    fprintf(stderr, "snap: a session made for %s logged in as %s with the token %s\n", fake::ctx_user.c_str(),
            fake::auth_user.c_str(), data.c_str());
    return 2;
  }
  printf("snap: the session made for %s logged in as %s, no data race\n", fake::ctx_user.c_str(),
         fake::auth_user.c_str());
  return 0;
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  std::string what = argc > 1 ? argv[1] : "";
  int rc = what == "keeps"  ? keeps()
           : what == "race" ? race()
           : what == "snap" ? snap()
                            : (fprintf(stderr, "harness keeps|race|snap\n"), 64);
  // the cspot task is still parked in its second session: leave without
  // tearing down what it uses
  _exit(rc);
}
