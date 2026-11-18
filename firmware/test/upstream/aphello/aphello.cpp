// test_up_aphello's driver: upstream's AuthChallenges::solveApHello, as
// patched (0025) and built with the board's mbedTLS, given a real access
// point's answer to our hello and that answer altered as someone on the path
// would alter it. Prints one line per case: "<name> took" or "<name> refused".
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "AuthChallenges.h"
#include "NanoPBHelper.h"
#include "pb_decode.h"

using namespace cspot;

static std::vector<uint8_t> frame(const std::vector<uint8_t>& body) {
  // the access point's framing: a 4-byte big-endian length, itself included
  uint32_t n = body.size() + 4;
  std::vector<uint8_t> out = {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

static void one(const char* name, std::vector<uint8_t> resp) {
  AuthChallenges a;
  auto hello = a.prepareClientHello();
  try {
    auto plain = a.solveApHello(hello, resp);
    bool keys = a.shanSendKey.size() == 32 && a.shanRecvKey.size() == 32 && !plain.empty();
    printf("%s %s\n", name, keys ? "took" : "took-without-keys");
  } catch (const std::runtime_error& e) {
    printf("%s refused (%s)\n", name, e.what());
  }
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: aphello APRESPONSE\n");
    return 2;
  }
  std::ifstream f(argv[1], std::ios::binary);
  std::vector<uint8_t> real((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (real.size() < 8) {
    fprintf(stderr, "aphello: %s is not an APResponse\n", argv[1]);
    return 2;
  }
  std::vector<uint8_t> body(real.begin() + 4, real.end());
  APResponseMessage m = {};
  pb_istream_t s = pb_istream_from_buffer(body.data(), body.size());
  if (!pb_decode(&s, APResponseMessage_fields, &m) || !m.has_challenge ||
      !m.challenge.login_crypto_challenge.has_diffie_hellman ||
      !m.challenge.login_crypto_challenge.diffie_hellman.has_gs_signature) {
    fprintf(stderr, "aphello: %s has no signed key exchange\n", argv[1]);
    return 2;
  }
  auto& dh = m.challenge.login_crypto_challenge.diffie_hellman;
  auto again = [&]() { return frame(pbEncode(APResponseMessage_fields, &m)); };

  one("real", real);
  one("reencoded", again());  // the encoder round-trips the real answer

  dh.gs[17] ^= 0x01;  // a key of the attacker's own under Spotify's signature
  one("other-key", again());
  dh.gs[17] ^= 0x01;

  dh.gs_signature.bytes[100] ^= 0x80;  // the signature altered
  one("bad-signature", again());
  dh.gs_signature.bytes[100] ^= 0x80;

  auto size = dh.gs_signature.size;
  dh.gs_signature.size = 255;  // a short one
  one("short-signature", again());
  dh.gs_signature.size = size;

  dh.has_gs_signature = false;  // none at all
  one("no-signature", again());
  dh.has_gs_signature = true;

  m.challenge.login_crypto_challenge.has_diffie_hellman = false;  // no key exchange
  one("no-key-exchange", again());
  m.challenge.login_crypto_challenge.has_diffie_hellman = true;

  one("restored", again());
  pb_release(APResponseMessage_fields, &m);
  return 0;
}
