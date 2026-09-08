// Stand-in for tzapu/WiFiManager: the real library's own captive portal is
// what an operator actually drives on hardware, and is not exercised on the
// host. This fake just joins the first network the fake radio can see, so
// everything downstream of "the station is connected" can still be tested.

#pragma once

#include "WiFi.h"

class WiFiManager {
 public:
  void setConfigPortalBlocking(bool) {}

  bool autoConnect(const char* apName = nullptr, const char* apPassword = nullptr) {
    (void)apName;
    (void)apPassword;
    auto& networks = WiFiClass::airwaves();
    if (!networks.empty()) {
      WiFi.begin(networks.front().ssid.c_str(), networks.front().password.c_str());
    }
    return false;
  }

  void process() {}
  void resetSettings() {}
};
