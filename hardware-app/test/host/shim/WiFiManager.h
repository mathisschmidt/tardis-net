// Stand-in for tzapu/WiFiManager: the real library's own captive portal is
// what an operator actually drives on hardware, and is not exercised on the
// host. This fake just joins the first network the fake radio can see, so
// everything downstream of "the station is connected" can still be tested.

#pragma once

#include "WiFi.h"

class WiFiManager {
 public:
  void setConfigPortalBlocking(bool) {}
  void setConnectTimeout(unsigned long) {}
  void setEnableConfigPortal(bool) {}
  // The fake radio always "has" a network to join, so report one as saved.
  bool getWiFiIsSaved() { return true; }
  bool startConfigPortal(const char* = nullptr, const char* = nullptr) { return false; }
  bool stopConfigPortal() { return true; }

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
