#include "config_store.h"

#include <esp_system.h>

namespace tardis {
namespace {

constexpr const char* kNamespace = "tardis";

std::string readString(Preferences& prefs, const char* key, const std::string& fallback = "") {
  if (!prefs.isKey(key)) return fallback;
  return std::string(prefs.getString(key, fallback.c_str()).c_str());
}

}  // namespace

void ConfigStore::begin() {
  prefs_.begin(kNamespace, false);
  load();
}

void ConfigStore::load() {
  config_.serverUrl = readString(prefs_, "server");
  config_.apiKey = readString(prefs_, "apikey");
  config_.pollSeconds = prefs_.getUInt("poll", 5);
  config_.switchPin = prefs_.getInt("swpin", 26);
  config_.switchActiveHigh = prefs_.getBool("swhigh", true);
  config_.sensePin = prefs_.getInt("sensepin", -1);
  config_.senseActiveHigh = prefs_.getBool("sensehigh", true);

  password_.salt = readString(prefs_, "pwsalt");
  password_.digest = readString(prefs_, "pwhash");
}

void ConfigStore::save(const Config& config) {
  config_ = config;
  prefs_.putString("server", config.serverUrl.c_str());
  prefs_.putString("apikey", config.apiKey.c_str());
  prefs_.putUInt("poll", config.pollSeconds);
  prefs_.putInt("swpin", config.switchPin);
  prefs_.putBool("swhigh", config.switchActiveHigh);
  prefs_.putInt("sensepin", config.sensePin);
  prefs_.putBool("sensehigh", config.senseActiveHigh);
}

void ConfigStore::setPassword(const std::string& password) {
  password_ = makePassword(password, &ConfigStore::randomBytes);
  prefs_.putString("pwsalt", password_.salt.c_str());
  prefs_.putString("pwhash", password_.digest.c_str());
}

void ConfigStore::clearPassword() {
  password_ = PasswordRecord{};
  prefs_.putString("pwsalt", "");
  prefs_.putString("pwhash", "");
}

void ConfigStore::factoryReset() {
  prefs_.clear();
  config_ = Config{};
  password_ = PasswordRecord{};
  load();
}

void ConfigStore::randomBytes(uint8_t* out, size_t size) {
  // esp_random() is fed by the hardware RNG once WiFi/BT is running, which it
  // is by the time anything here asks for a salt.
  for (size_t i = 0; i < size; i += 4) {
    uint32_t word = esp_random();
    for (size_t byte = 0; byte < 4 && i + byte < size; byte++) {
      out[i + byte] = static_cast<uint8_t>(word >> (byte * 8));
    }
  }
}

std::string ConfigStore::randomHex(size_t bytes) {
  uint8_t buffer[32];
  if (bytes > sizeof(buffer)) bytes = sizeof(buffer);
  randomBytes(buffer, bytes);
  return crypto::toHex(buffer, bytes);
}

}  // namespace tardis
