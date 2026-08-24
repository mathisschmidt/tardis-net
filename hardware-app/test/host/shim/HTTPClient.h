// Arduino's HTTPClient over real sockets, so the agent really talks to the
// console when it runs on the host.

#pragma once

#include <string>
#include <vector>

#include "Arduino.h"

#define HTTP_CODE_OK 200

class HTTPClient {
 public:
  bool begin(const char* url);
  bool begin(const String& url) { return begin(url.c_str()); }
  void end();
  void setTimeout(uint16_t ms) { timeoutMs_ = ms; }
  void setConnectTimeout(int32_t ms) { timeoutMs_ = static_cast<uint16_t>(ms); }
  void setReuse(bool) {}
  void addHeader(const char* name, const String& value);
  int POST(uint8_t* payload, size_t size);
  int POST(const String& payload);
  String getString() { return String(response_); }

 private:
  std::string host_;
  std::string path_;
  int port_ = 80;
  bool https_ = false;
  uint16_t timeoutMs_ = 8000;
  std::vector<std::pair<std::string, std::string>> headers_;
  std::string response_;
};
