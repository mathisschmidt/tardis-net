#include "network_station.h"

#include <ESPmDNS.h>

#include <string>

#include "device_identity.h"

namespace tardis {

void NetworkStation::begin() {
  wifiManager_.setConfigPortalBlocking(false);
  // Non-blocking: this returns immediately so the switch/agent/portal loop
  // keeps running while an operator is still choosing a network on their
  // phone, or while there is simply no route to the real one yet.
  wifiManager_.autoConnect(("tardis-setup-" + deviceSuffix()).c_str());
}

void NetworkStation::loop() {
  wifiManager_.process();

  const bool isConnected = connected();
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
