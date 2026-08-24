// The captive-portal DNS responder is not exercised on the host.

#pragma once

#include "Arduino.h"
#include "WiFi.h"

class DNSServer {
 public:
  bool start(uint16_t, const String&, const IPAddress&) { return true; }
  void processNextRequest() {}
  void stop() {}
};
