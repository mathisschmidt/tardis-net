// Joining the operator's real Wi-Fi network. WiFiManager owns the stored
// credentials and the setup AP for entering them; WifiKeeper (include/
// wifi_keeper.h) decides when to retry the saved network and when to open
// that AP — so a device that boots while the router is still down keeps
// trying instead of parking itself in setup mode. This class wires the two
// together and announces the device over mDNS once it is on the network.

#pragma once

#include <WiFi.h>
#include <WiFiManager.h>

#include <memory>
#include <string>

#include "wifi_keeper.h"

namespace tardis {

class NetworkStation {
 public:
  /** With no saved network, opens WiFiManager's setup AP. With one, tries to
   *  join it — without opening the setup AP if that first attempt fails. */
  void begin();

  /** Call every loop() — pumps WiFiManager, retries the saved network, and
   *  starts mDNS the moment the station comes up. */
  void loop();

  bool connected() const { return WiFi.status() == WL_CONNECTED; }

  /** The setup AP has to open but our portal holds port 80 — reboot. */
  bool restartRequested() const { return restartRequested_; }

  /** Erase the stored Wi-Fi credentials. Pair with a reboot — WiFiManager
   *  opens its setup AP again the next time it boots with nothing to join. */
  void forgetWifi() { wifiManager_.resetSettings(); }

 private:
  WiFiManager wifiManager_;
  std::unique_ptr<WifiKeeper> keeper_;
  std::string apName_;
  bool wasConnected_ = false;
  bool mdnsStarted_ = false;
  bool restartRequested_ = false;

  void startMdns();
};

}  // namespace tardis
