// Fake radio. Joining "succeeds" for any SSID the harness was told exists,
// which is what lets the real manageWifi()/portal code run unchanged.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Arduino.h"

typedef enum {
  WL_NO_SHIELD = 255,
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_DISCONNECTED = 6
} wl_status_t;

typedef enum { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 } wifi_mode_t;
#define WIFI_MODE_STA WIFI_STA
typedef enum { WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA2_PSK = 3 } wifi_auth_mode_t;

class IPAddress {
 public:
  IPAddress() = default;
  explicit IPAddress(const std::string& text) : text_(text) {}
  String toString() const { return String(text_); }

 private:
  std::string text_ = "0.0.0.0";
};

class WiFiClass {
 public:
  // ---- what the firmware calls -------------------------------------------
  void persistent(bool) {}
  void setAutoReconnect(bool) {}
  void mode(wifi_mode_t mode) { mode_ = mode; }
  wifi_mode_t getMode() const { return mode_; }

  void begin(const char* ssid, const char* password = nullptr);
  void disconnect(bool = false, bool = false);
  wl_status_t status();

  bool softAP(const char* ssid, const char* password = nullptr);
  bool softAPdisconnect(bool = false);
  IPAddress softAPIP() const { return IPAddress("192.168.4.1"); }

  IPAddress localIP() const;
  String SSID() const { return String(connectedSsid_); }
  int32_t RSSI() const { return status_ == WL_CONNECTED ? -57 : 0; }

  int scanNetworks(bool = false, bool = false);
  String SSID(int index) const;
  int32_t RSSI(int index) const;
  wifi_auth_mode_t encryptionType(int index) const;
  void scanDelete() {}

  // ---- harness controls ---------------------------------------------------
  struct Network {
    std::string ssid;
    std::string password;  // empty means open
    int32_t rssi = -60;
  };
  /** Networks the fake radio can see and join. */
  static std::vector<Network>& airwaves();
  /** How long a join takes, so the retry paths can be exercised. */
  static uint32_t& joinDelayMs();

 private:
  wifi_mode_t mode_ = WIFI_OFF;
  wl_status_t status_ = WL_IDLE_STATUS;
  std::string connectedSsid_;
  std::string pendingSsid_;
  std::string pendingPassword_;
  uint32_t joinCompleteAt_ = 0;
  bool joining_ = false;
  std::vector<Network> scan_;
};

extern WiFiClass WiFi;
