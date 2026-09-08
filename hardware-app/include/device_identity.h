// A stable, per-device id derived from the chip's MAC so two boards do not
// collide — shared by the setup AP's name and the mDNS hostname, so whatever
// a phone saw while joining the setup network is also the device's address
// once it is on the real one.

#pragma once

#include <Arduino.h>

namespace tardis {

inline String deviceSuffix() {
  uint64_t mac = ESP.getEfuseMac();
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%04X", static_cast<uint16_t>(mac >> 32));
  return String(suffix);
}

}  // namespace tardis
