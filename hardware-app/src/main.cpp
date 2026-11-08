// tardis-net hardware agent.
//
// An ESP32 wired across the power switch of the machine called `tardis`. It
// polls the console's API for work and, when told to, holds the switch closed
// for the requested time:
//
//   power on            500 ms
//   graceful shutdown   500 ms
//   hard power off      5 s
//
// Wi-Fi is handled by WiFiManager: on first boot it opens its own setup AP to
// collect the SSID/password and remembers them. From then on the saved network
// is retried every 30 s whenever it is unreachable (e.g. the router is slower
// to boot after a power cut); the setup AP only reopens, for 5 minutes at a
// time, after 10 minutes without a connection. The server address and API key are
// entered separately, in this device's own web portal — reachable at
// tardis.local once it is on the real network (or tardis-2.local, tardis-3.local,
// ... if another tardis is already claiming that name there) — which is
// password-protected from the first connection.

#include <Arduino.h>

#include "agent.h"
#include "config_store.h"
#include "portal.h"
#include "services/network_station.h"

namespace {

tardis::ConfigStore store;
tardis::Agent agent(store);
tardis::NetworkStation station;
tardis::Portal portal(store, agent, station);
bool portalStarted = false;

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.printf("tardis-net hardware agent %s\n", tardis::kFirmwareVersion);

  store.begin();
  agent.begin();  // parks the switch in its released state before anything else

  station.begin();

  if (!store.claimed()) {
    Serial.println("[portal] unclaimed — open the portal to set a password");
  }
}

void loop() {
  station.loop();

  // WiFiManager's own setup AP runs its captive portal on port 80 too — ours
  // can only bind that port once the station is up and WiFiManager has torn
  // its webserver back down.
  if (!portalStarted && station.connected()) {
    portal.begin();
    portalStarted = true;
    Serial.println("[portal] listening on :80");
  }
  if (portalStarted) portal.handle();
  agent.update(station.connected());

  // The station asks for a restart only after being offline for minutes, so
  // waiting for a pulse that is still running to finish costs nothing.
  if (portal.rebootRequested() || (station.restartRequested() && !agent.busy())) {
    Serial.println("[portal] rebooting");
    agent.begin();  // release the switch before the restart
    delay(100);
    ESP.restart();
  }

  delay(2);  // let the WiFi stack breathe
}
