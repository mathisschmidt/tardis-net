#pragma once

#include "Arduino.h"
#include "WiFi.h"

class MDNSResponder {
 public:
  bool begin(const char*) { return true; }
  void end() {}
  void addService(const char*, const char*, uint16_t) {}
  /** No real network on the host, so nothing is ever already taken. */
  IPAddress queryHost(const char*, uint32_t = 2000) { return IPAddress(); }
};
extern MDNSResponder MDNS;
