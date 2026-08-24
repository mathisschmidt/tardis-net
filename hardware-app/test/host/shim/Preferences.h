// NVS, backed by a JSON-ish file so a restart keeps the config (same contract).

#pragma once

#include <map>
#include <string>

#include "Arduino.h"

class Preferences {
 public:
  bool begin(const char* name, bool readOnly = false);
  void end() {}
  bool clear();
  bool isKey(const char* key) const;

  String getString(const char* key, const String& fallback = String(""));
  size_t putString(const char* key, const char* value);
  uint32_t getUInt(const char* key, uint32_t fallback = 0);
  size_t putUInt(const char* key, uint32_t value);
  int32_t getInt(const char* key, int32_t fallback = 0);
  size_t putInt(const char* key, int32_t value);
  bool getBool(const char* key, bool fallback = false);
  size_t putBool(const char* key, bool value);

 private:
  std::string path_;
  std::map<std::string, std::string> values_;
  void load();
  void flush();
};
