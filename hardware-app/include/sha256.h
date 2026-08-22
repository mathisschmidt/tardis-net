// A compact SHA-256, so the portal's password hashing is portable and testable.
//
// The ESP32's mbedtls could do this, but its function names moved between IDF 4
// and IDF 5; keeping our own means the firmware builds against either Arduino
// core, and the host tests can check it against the published FIPS 180-4
// vectors instead of trusting it.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace crypto {

class Sha256 {
 public:
  static constexpr size_t kDigestSize = 32;

  Sha256() { reset(); }

  void reset() {
    length_ = 0;
    bufferLength_ = 0;
    state_[0] = 0x6a09e667;
    state_[1] = 0xbb67ae85;
    state_[2] = 0x3c6ef372;
    state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f;
    state_[5] = 0x9b05688c;
    state_[6] = 0x1f83d9ab;
    state_[7] = 0x5be0cd19;
  }

  void update(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; i++) {
      buffer_[bufferLength_++] = data[i];
      if (bufferLength_ == 64) {
        transform(buffer_);
        length_ += 64;
        bufferLength_ = 0;
      }
    }
  }

  void update(const std::string& text) {
    update(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  }

  void finish(uint8_t out[kDigestSize]) {
    uint64_t bitLength = (length_ + bufferLength_) * 8;
    size_t i = bufferLength_;

    buffer_[i++] = 0x80;
    if (i > 56) {
      while (i < 64) buffer_[i++] = 0;
      transform(buffer_);
      i = 0;
    }
    while (i < 56) buffer_[i++] = 0;
    for (int shift = 56; shift >= 0; shift -= 8) {
      buffer_[i++] = static_cast<uint8_t>(bitLength >> shift);
    }
    transform(buffer_);

    for (int word = 0; word < 8; word++) {
      out[word * 4 + 0] = static_cast<uint8_t>(state_[word] >> 24);
      out[word * 4 + 1] = static_cast<uint8_t>(state_[word] >> 16);
      out[word * 4 + 2] = static_cast<uint8_t>(state_[word] >> 8);
      out[word * 4 + 3] = static_cast<uint8_t>(state_[word]);
    }
  }

 private:
  uint32_t state_[8];
  uint8_t buffer_[64] = {};
  uint64_t length_ = 0;
  size_t bufferLength_ = 0;

  static uint32_t rotr(uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
  }

  void transform(const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};

    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
      w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
             (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
             static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int i = 0; i < 64; i++) {
      uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t temp1 = h + s1 + ch + k[i] + w[i];
      uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t temp2 = s0 + maj;

      h = g; g = f; f = e;
      e = d + temp1;
      d = c; c = b; b = a;
      a = temp1 + temp2;
    }

    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
  }
};

inline std::string toHex(const uint8_t* data, size_t size) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; i++) {
    out.push_back(kHex[data[i] >> 4]);
    out.push_back(kHex[data[i] & 0x0f]);
  }
  return out;
}

inline std::string sha256Hex(const std::string& text) {
  uint8_t digest[Sha256::kDigestSize];
  Sha256 hash;
  hash.update(text);
  hash.finish(digest);
  return toHex(digest, sizeof(digest));
}

/** Compare two equal-length strings without leaking where they differ. */
inline bool constantTimeEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); i++) {
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

}  // namespace crypto
