// Joining the operator's real Wi-Fi network. Credentials, the setup AP for
// entering them, retry and reconnection are all owned by WiFiManager — this
// class just starts it, pumps it, and notices when the station comes up so
// it can announce itself over mDNS.

#pragma once

#include <WiFi.h>
#include <WiFiManager.h>

namespace tardis {

class NetworkStation {
 public:
  /** Starts WiFiManager's non-blocking autoConnect: it reconnects with
   *  whatever credentials it already has, or opens its own setup network if
   *  it has none / they no longer work. */
  void begin();

  /** Call every loop() — pumps WiFiManager's config portal and starts mDNS
   *  the moment the station comes up. */
  void loop();

  bool connected() const { return WiFi.status() == WL_CONNECTED; }

  /** Erase the stored Wi-Fi credentials. Pair with a reboot — WiFiManager
   *  opens its setup AP again the next time autoConnect() has nothing to
   *  join with. */
  void forgetWifi() { wifiManager_.resetSettings(); }

 private:
  WiFiManager wifiManager_;
  bool wasConnected_ = false;
  bool mdnsStarted_ = false;

  void startMdns();
};

}  // namespace tardis
