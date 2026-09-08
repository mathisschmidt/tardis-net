// The device's own configuration, and the rules for what counts as valid.
//
// Portable C++17: the validation the portal enforces is the validation the host
// tests exercise. Persistence lives in src/config_store.cpp (NVS).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "protocol.h"

namespace tardis {

inline constexpr size_t kMinPortalPassword = 8;
inline constexpr size_t kMaxField = 128;

struct Config {
  std::string serverUrl;     // e.g. http://192.168.1.10:8000 — no trailing slash
  std::string apiKey;        // the console's pairing key
  uint32_t pollSeconds = 5;

  int switchPin = 26;
  bool switchActiveHigh = true;   // true: HIGH closes the contact
  int sensePin = -1;              // -1 disables the power-sense input
  bool senseActiveHigh = true;

  /** Everything needed to actually do the job. Wi-Fi is not this device's to
   *  track — WiFiManager owns joining and remembering that on its own. */
  bool complete() const {
    return !serverUrl.empty() && !apiKey.empty() && switchPin >= 0;
  }
};

/** Strip whitespace and any trailing slash so paths concatenate cleanly. */
inline std::string normaliseUrl(std::string url) {
  while (!url.empty() && (url.front() == ' ' || url.front() == '\t')) url.erase(url.begin());
  while (!url.empty() && (url.back() == ' ' || url.back() == '\t' || url.back() == '/')) {
    url.pop_back();
  }
  return url;
}

/** GPIOs that must never be wired to the switch on a classic ESP32.
 *  6-11 are the SPI flash; 34-39 are input-only. */
inline bool isUsableOutputPin(int pin) {
  if (pin < 0 || pin > 33) return false;
  if (pin >= 6 && pin <= 11) return false;
  return true;
}

inline bool isUsableInputPin(int pin) {
  if (pin < 0 || pin > 39) return false;
  if (pin >= 6 && pin <= 11) return false;
  return true;
}

/** Validate a config edit. Returns the list of problems, empty when fine. */
inline std::vector<std::string> validate(const Config& config) {
  std::vector<std::string> problems;

  const std::string& url = config.serverUrl;
  if (url.empty()) {
    problems.push_back("Server address is required.");
  } else if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
    problems.push_back("Server address must start with http:// or https://.");
  } else if (url.size() > kMaxField) {
    problems.push_back("Server address is too long.");
  }

  if (config.apiKey.empty()) {
    problems.push_back("API key is required — copy it from the console's dashboard.");
  } else if (config.apiKey.size() > kMaxField) {
    problems.push_back("API key is too long.");
  }

  if (config.pollSeconds < kMinPollSeconds || config.pollSeconds > kMaxPollSeconds) {
    problems.push_back("Poll interval must be between 1 and 3600 seconds.");
  }

  if (!isUsableOutputPin(config.switchPin)) {
    problems.push_back("Switch GPIO must be an output-capable pin (0-33, not 6-11).");
  }
  if (config.sensePin >= 0) {
    if (!isUsableInputPin(config.sensePin)) {
      problems.push_back("Sense GPIO must be a usable input pin (0-39, not 6-11).");
    } else if (config.sensePin == config.switchPin) {
      problems.push_back("Sense GPIO must differ from the switch GPIO.");
    }
  }

  return problems;
}

/** Portal password rules — mirrors the console's "set it on first contact". */
inline std::vector<std::string> validatePassword(const std::string& password,
                                                 const std::string& confirm) {
  std::vector<std::string> problems;
  if (password.size() < kMinPortalPassword) {
    problems.push_back("Password must be at least 8 characters.");
  }
  if (password.size() > kMaxField) problems.push_back("Password is too long.");
  if (password != confirm) problems.push_back("The two passwords do not match.");
  return problems;
}

}  // namespace tardis
