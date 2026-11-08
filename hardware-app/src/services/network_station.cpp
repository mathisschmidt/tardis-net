#include "network_station.h"

#include <ESPmDNS.h>

#include <string>

#include "device_identity.h"

namespace tardis {

void NetworkStation::begin() {
  apName_ = std::string(("tardis-setup-" + deviceSuffix()).c_str());
  // getWiFiIsSaved() reads the stored station config through the Wi-Fi
  // driver, which returns garbage until the driver is started — start it.
  WiFi.mode(WIFI_STA);
  const bool saved = wifiManager_.getWiFiIsSaved();

  // Non-blocking: the setup AP, when it is up, is pumped from loop() so the
  // switch/agent/portal loop keeps running while an operator is still
  // choosing a network on their phone.
  wifiManager_.setConfigPortalBlocking(false);
  // Bound the one join attempt autoConnect() makes, so boot is not held up.
  wifiManager_.setConnectTimeout(15);
  // With a network saved, a failed first attempt must not open the setup AP:
  // that would switch the station off and never try the saved network again
  // (e.g. after a power cut, when the router takes longer to boot than we do).
  // WifiKeeper keeps retrying instead, and opens the AP only as a last resort.
  wifiManager_.setEnableConfigPortal(!saved);

  keeper_ = std::make_unique<WifiKeeper>(saved, millis());
  if (saved) Serial.println("[wifi] joining the saved network");
  wifiManager_.autoConnect(apName_.c_str());
}

void NetworkStation::loop() {
  wifiManager_.process();

  const bool isConnected = connected();
  switch (keeper_->update(isConnected, millis())) {
    case WifiAction::None:
      break;
    case WifiAction::Retry:
      Serial.println("[wifi] retrying the saved network");
      WiFi.mode(WIFI_STA);
      WiFi.begin();  // no arguments: the credentials WiFiManager saved
      break;
    case WifiAction::OpenSetup:
      Serial.printf("[wifi] still offline — opening setup network %s for a while\n",
                    apName_.c_str());
      wifiManager_.startConfigPortal(apName_.c_str());
      break;
    case WifiAction::CloseSetup:
      Serial.println("[wifi] closing the setup network, retrying the saved one");
      wifiManager_.stopConfigPortal();
      WiFi.mode(WIFI_STA);
      WiFi.begin();
      break;
    case WifiAction::Restart:
      if (!restartRequested_) {
        Serial.println("[wifi] offline too long — restarting to offer the setup network");
      }
      restartRequested_ = true;
      break;
  }
  if (isConnected != wasConnected_) {
    wasConnected_ = isConnected;
    if (isConnected) {
      Serial.printf("[wifi] connected, portal at http://%s/\n",
                    WiFi.localIP().toString().c_str());
      startMdns();
    } else {
      Serial.println("[wifi] connection lost");
    }
  }
}

namespace {

/** True if some other device on the LAN already answers for ``host``.local. */
bool hostnameTaken(const String& host) {
  return std::string(MDNS.queryHost(host.c_str()).toString().c_str()) != "0.0.0.0";
}

}  // namespace

void NetworkStation::startMdns() {
  if (mdnsStarted_) return;

  // Bring the responder up under a throwaway name so we can ask the network
  // what is already claimed before committing to a real one — every device
  // should just be "tardis.local", falling back to "-2", "-3", ... only if
  // another tardis is already on this network.
  MDNS.begin("tardis-probe");
  String host = "tardis";
  for (int suffix = 2; hostnameTaken(host); suffix++) {
    host = "tardis-" + String(suffix);
  }
  MDNS.end();

  mdnsStarted_ = MDNS.begin(host.c_str());
  if (mdnsStarted_) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[portal] also reachable at http://%s.local/\n", host.c_str());
  }
}

}  // namespace tardis
