// Portal password storage.
//
// The password never touches flash: what is stored is a random salt plus the
// result of iterating SHA-256 over salt||password, which makes a stolen NVS
// dump expensive to attack rather than a plain read. The iteration count is
// what an ESP32 can do in well under a second.

#pragma once

#include <cstdint>
#include <string>

#include "sha256.h"

namespace tardis {

inline constexpr int kHashIterations = 20000;
inline constexpr size_t kSaltBytes = 16;

/** salt (hex) + password -> digest (hex). Deterministic, so it is testable. */
inline std::string hashPassword(const std::string& saltHex, const std::string& password) {
  uint8_t digest[crypto::Sha256::kDigestSize];
  {
    crypto::Sha256 first;
    first.update(saltHex);
    first.update(password);
    first.finish(digest);
  }
  for (int round = 1; round < kHashIterations; round++) {
    crypto::Sha256 next;
    next.update(digest, sizeof(digest));
    next.update(saltHex);
    next.finish(digest);
  }
  return crypto::toHex(digest, sizeof(digest));
}

struct PasswordRecord {
  std::string salt;    // hex
  std::string digest;  // hex

  bool set() const { return !salt.empty() && !digest.empty(); }

  bool matches(const std::string& password) const {
    if (!set()) return false;
    return crypto::constantTimeEquals(digest, hashPassword(salt, password));
  }
};

/** Build a record for a new password. ``randomBytes`` fills the salt. */
template <typename RandomFn>
inline PasswordRecord makePassword(const std::string& password, RandomFn randomBytes) {
  uint8_t salt[kSaltBytes];
  randomBytes(salt, sizeof(salt));
  PasswordRecord record;
  record.salt = crypto::toHex(salt, sizeof(salt));
  record.digest = hashPassword(record.salt, password);
  return record;
}

}  // namespace tardis
