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
// Everything it needs — Wi-Fi, the server address, the API key — is entered in
// its own web portal, which is password-protected from the first connection.

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "agent.h"
#include "config_store.h"
#include "portal.h"

namespace {

tardis::ConfigStore store;
tardis::Agent agent(store);
tardis::Portal portal(store, agent);

constexpr uint32_t kWifiAttemptMs = 20000;   // give a join this long before falling back
constexpr uint32_t kWifiRetryMs = 30000;     // …then try again this often
constexpr uint16_t kDnsPort = 53;
uint32_t wifiDeadlineMs = 0;
uint32_t nextWifiAttemptMs = 0;
bool apActive = false;
bool mdnsStarted = false;
DNSServer dnsServer;

/** A stable, per-device id so two boards do not collide — reused for both the
 *  setup AP name and the mDNS hostname, so whatever the phone saw while
 *  joining the setup network is also its address afterwards. */
String deviceSuffix() {
  uint64_t mac = ESP.getEfuseMac();
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%04X", static_cast<uint16_t>(mac >> 32));
  return String(suffix);
}

String apName() { return "tardis-setup-" + deviceSuffix(); }
String mdnsHostname() { return "tardis-" + deviceSuffix(); }

void startAccessPoint() {
  if (apActive) return;
  WiFi.mode(WIFI_AP_STA);  // keep trying the real network while the AP is up
  WiFi.softAP(apName().c_str());  // open on purpose — it's only up until claimed/configured
  // Answer every DNS lookup a joined phone makes with our own address. That is
  // what makes the OS's captive-portal check fail and pop the "Sign in to
  // network" prompt straight to this portal — no IP to type, no monitor.
  dnsServer.start(kDnsPort, "*", WiFi.softAPIP());
  apActive = true;
  Serial.println();
  Serial.println("=======================================================");
  Serial.printf("  Setup network : %s (open)\n", apName().c_str());
  Serial.println("  Join it on your phone — a \"Sign in to network\" prompt");
  Serial.println("  should open the portal by itself within a few seconds.");
  Serial.printf("  Portal        : http://%s/\n", WiFi.softAPIP().toString().c_str());
  Serial.println("=======================================================");
}

void stopAccessPoint() {
  if (!apActive) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
  Serial.println("[wifi] setup network closed");
}

/** Once on the real network, advertise a name that beats hunting for an IP. */
void startMdns() {
  if (mdnsStarted) return;
  const String host = mdnsHostname();
  mdnsStarted = MDNS.begin(host.c_str());
  if (mdnsStarted) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[portal] also reachable at http://%s.local/\n", host.c_str());
  }
}

void beginJoin() {
  const tardis::Config& config = store.config();
  if (config.wifiSsid.empty()) {
    startAccessPoint();
    return;
  }
  Serial.printf("[wifi] joining %s\n", config.wifiSsid.c_str());
  WiFi.begin(config.wifiSsid.c_str(),
             config.wifiPassword.empty() ? nullptr : config.wifiPassword.c_str());
  wifiDeadlineMs = millis() + kWifiAttemptMs;
  nextWifiAttemptMs = millis() + kWifiRetryMs;
}

/** Keep the station connected; fall back to the setup AP when it will not join. */
void manageWifi() {
  static bool wasConnected = false;
  const bool connected = WiFi.status() == WL_CONNECTED;

  if (connected != wasConnected) {
    wasConnected = connected;
    if (connected) {
      Serial.printf("[wifi] connected, portal at http://%s/\n",
                    WiFi.localIP().toString().c_str());
      startMdns();
      // The portal stays reachable on the LAN, so the AP is no longer needed.
      if (store.config().complete()) stopAccessPoint();
    } else {
      Serial.println("[wifi] connection lost");
      wifiDeadlineMs = millis() + kWifiAttemptMs;
    }
  }
  if (connected) return;

  if (!apActive && static_cast<int32_t>(millis() - wifiDeadlineMs) >= 0) {
    Serial.println("[wifi] could not join — opening the setup network");
    startAccessPoint();
  }
  if (static_cast<int32_t>(millis() - nextWifiAttemptMs) >= 0 &&
      !store.config().wifiSsid.empty()) {
    WiFi.disconnect();
    beginJoin();
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.printf("tardis-net hardware agent %s\n", tardis::kFirmwareVersion);

  store.begin();
  agent.begin();  // parks the switch in its released state before anything else

  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  beginJoin();

  if (!store.claimed()) {
    // Nobody has set a portal password yet: make the setup network visible so
    // the operator can claim the device.
    startAccessPoint();
    Serial.println("[portal] unclaimed — open the portal to set a password");
  }

  portal.begin();
  Serial.println("[portal] listening on :80");
}

void loop() {
  manageWifi();
  if (apActive) dnsServer.processNextRequest();
  portal.handle();
  agent.update(WiFi.status() == WL_CONNECTED);

  if (portal.rebootRequested()) {
    Serial.println("[portal] rebooting");
    agent.begin();  // release the switch before the restart
    delay(100);
    ESP.restart();
  }

  delay(2);  // let the WiFi stack breathe
}
