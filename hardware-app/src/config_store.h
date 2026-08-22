// Persistence for the device's config, portal password and AP password (NVS).

#pragma once

#include <Preferences.h>

#include <string>

#include "device_config.h"
#include "password.h"

namespace tardis {

class ConfigStore {
 public:
  void begin();

  const Config& config() const { return config_; }
  void save(const Config& config);

  /** True once the operator has set a portal password on first contact. */
  bool claimed() const { return password_.set(); }
  bool checkPassword(const std::string& password) const { return password_.matches(password); }
  void setPassword(const std::string& password);

  /** Password of the setup access point, generated once and kept. */
  const std::string& apPassword() const { return apPassword_; }

  /** Wipe everything — the device comes back up unclaimed and unconfigured. */
  void factoryReset();

  /** Fill ``out`` with hardware random bytes (esp_random under the hood). */
  static void randomBytes(uint8_t* out, size_t size);
  static std::string randomHex(size_t bytes);

 private:
  Preferences prefs_;
  Config config_;
  PasswordRecord password_;
  std::string apPassword_;

  void load();
};

}  // namespace tardis
