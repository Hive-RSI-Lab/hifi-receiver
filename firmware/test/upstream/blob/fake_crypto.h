// The Crypto cspot's LoginBlob uses, for test_up_blob.py (see fakes.h): not
// cryptography, identity transforms. LoginBlob.h includes Crypto.h, which
// is this, before fakes.h can know LoginBlob, so it stands alone.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace fake {
// called from every LoginBlob's constructor (dhInit), when the harness sets it
inline void (*on_dh_init)() = nullptr;
}  // namespace fake

// ---- the Crypto LoginBlob uses: identity transforms (see the top) -------------
class Crypto {
 public:
  std::vector<uint8_t> publicKey = std::vector<uint8_t>(96, 0x55);
  std::vector<uint8_t> privateKey;
  static std::vector<uint8_t> base64Decode(const std::string& s) { return {s.begin(), s.end()}; }
  static std::string base64Encode(const std::vector<uint8_t>& v) { return {v.begin(), v.end()}; }
  void sha1Init() {}
  void sha1Update(const std::string&) {}
  void sha1Update(const std::vector<uint8_t>&) {}
  std::vector<uint8_t> sha1FinalBytes() { return std::vector<uint8_t>(20, 0x11); }
  // every MAC is 20 x 0xAA: a blob whose last 20 bytes are that passes the check
  std::vector<uint8_t> sha1HMAC(const std::vector<uint8_t>&, const std::vector<uint8_t>&) {
    return std::vector<uint8_t>(20, 0xAA);
  }
  void aesCTRXcrypt(const std::vector<uint8_t>&, std::vector<uint8_t>&, uint8_t*, size_t) {}
  // decodeBlobSecondary xors every byte from 16 on with the one 16 before
  // it (taken before the xor); undo that here so the parse sees the bytes
  // the harness wrote
  void aesECBdecrypt(const std::vector<uint8_t>&, std::vector<uint8_t>& d) {
    for (size_t k = 16; k < d.size(); k++) d[k] ^= d[k - 16];
  }
  void dhInit() {
    if (fake::on_dh_init) fake::on_dh_init();
  }
  std::vector<uint8_t> dhCalculateShared(const std::vector<uint8_t>&) { return std::vector<uint8_t>(96, 0x22); }
  std::vector<uint8_t> pbkdf2HmacSha1(const std::vector<uint8_t>&, const std::vector<uint8_t>&, int, int len) {
    return std::vector<uint8_t>(len, 0x33);
  }
};
