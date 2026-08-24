#include "portal.h"

#include <WiFi.h>

#include <algorithm>
#include <vector>

#include "json_lite.h"
#include "web_assets.h"

namespace tardis {
namespace {

constexpr uint32_t kSessionMs = 30UL * 60UL * 1000UL;  // 30 minutes idle
constexpr uint32_t kMaxAttempts = 5;
constexpr uint32_t kLockoutMs = 5UL * 60UL * 1000UL;
constexpr const char* kCookieName = "tardis_portal";

std::string quote(const std::string& value) { return "\"" + jsonlite::escape(value) + "\""; }

std::string boolean(bool value) { return value ? "true" : "false"; }

/** Mask a stored secret so the UI can show that one exists without leaking it. */
std::string maskHint(const std::string& secret) {
  if (secret.empty()) return "";
  if (secret.size() <= 4) return "••••";
  return std::string("••••••••") + secret.substr(secret.size() - 4);
}

}  // namespace

Portal::Portal(ConfigStore& store, Agent& agent) : store_(store), agent_(agent), server_(80) {}

void Portal::begin() {
  server_.on("/", HTTP_GET, [this]() { handleIndex(); });
  server_.on("/api/status", HTTP_GET, [this]() { handleStatus(); });
  server_.on("/api/claim", HTTP_POST, [this]() { handleClaim(); });
  server_.on("/api/login", HTTP_POST, [this]() { handleLogin(); });
  server_.on("/api/logout", HTTP_POST, [this]() { handleLogout(); });
  server_.on("/api/config", HTTP_GET, [this]() { handleGetConfig(); });
  server_.on("/api/config", HTTP_POST, [this]() { handleSaveConfig(); });
  server_.on("/api/test", HTTP_POST, [this]() { handleTest(); });
  server_.on("/api/wifi/scan", HTTP_GET, [this]() { handleWifiScan(); });
  server_.on("/api/wifi/test", HTTP_POST, [this]() { handleWifiTest(); });
  server_.on("/api/reboot", HTTP_POST, [this]() { handleReboot(); });
  server_.onNotFound([this]() { handleNotFound(); });
  // WebServer drops every header it was not told to keep — including the one
  // carrying the session.
  const char* headers[] = {"Cookie"};
  server_.collectHeaders(headers, 1);
  server_.begin();
}

// ------------------------------------------------------------------ sessions

bool Portal::authed() {
  if (sessionToken_.empty()) return false;
  if (static_cast<int32_t>(millis() - sessionExpiresMs_) >= 0) {
    endSession();
    return false;
  }
  if (!server_.hasHeader("Cookie")) return false;

  const std::string cookies = std::string(server_.header("Cookie").c_str());
  const std::string needle = std::string(kCookieName) + "=";
  size_t at = cookies.find(needle);
  if (at == std::string::npos) return false;
  at += needle.size();
  size_t end = cookies.find(';', at);
  const std::string token = cookies.substr(at, end == std::string::npos ? end : end - at);

  if (!crypto::constantTimeEquals(token, sessionToken_)) return false;
  sessionExpiresMs_ = millis() + kSessionMs;  // sliding expiry
  return true;
}

void Portal::startSession() {
  sessionToken_ = ConfigStore::randomHex(16);
  sessionExpiresMs_ = millis() + kSessionMs;
  failedAttempts_ = 0;
  lockedUntilMs_ = 0;
  server_.sendHeader("Set-Cookie", String(kCookieName) + "=" + sessionToken_.c_str() +
                                       "; Path=/; HttpOnly; SameSite=Lax; Max-Age=1800");
}

void Portal::endSession() {
  sessionToken_.clear();
  sessionExpiresMs_ = 0;
}

bool Portal::lockedOut() const {
  return lockedUntilMs_ != 0 && static_cast<int32_t>(millis() - lockedUntilMs_) < 0;
}

uint32_t Portal::lockRemaining() const {
  return lockedOut() ? (lockedUntilMs_ - millis()) / 1000 : 0;
}

void Portal::registerFailure() {
  if (++failedAttempts_ >= kMaxAttempts) {
    failedAttempts_ = 0;
    lockedUntilMs_ = millis() + kLockoutMs;
  }
}

// -------------------------------------------------------------------- routes

void Portal::handleIndex() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send_P(200, "text/html; charset=utf-8", PORTAL_HTML);
}

void Portal::handleStatus() {
  // Public on purpose: the login screen needs to know whether the device has
  // been claimed. It carries no secrets — no SSID, no key, no config.
  const bool signedIn = authed();
  const AgentStatus& agent = agent_.status();

  std::string json = "{";
  json += "\"claimed\":" + boolean(store_.claimed());
  json += ",\"authenticated\":" + boolean(signedIn);
  json += ",\"configured\":" + boolean(store_.config().complete());
  json += ",\"locked_for\":" + std::to_string(lockRemaining());
  json += ",\"firmware\":" + quote(kFirmwareVersion);
  json += ",\"ap_mode\":" + boolean(WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA);
  json += ",\"wifi\":{";
  json += "\"connected\":" + boolean(WiFi.status() == WL_CONNECTED);
  json += ",\"ssid\":" + quote(std::string(WiFi.SSID().c_str()));
  json += ",\"ip\":" + quote(std::string((WiFi.status() == WL_CONNECTED ? WiFi.localIP()
                                                                        : WiFi.softAPIP())
                                             .toString()
                                             .c_str()));
  json += ",\"rssi\":" + std::to_string(WiFi.RSSI());
  json += "}";
  json += ",\"uptime_s\":" + std::to_string(millis() / 1000);

  if (signedIn) {
    // Link detail is only interesting once you are in, so keep it behind auth.
    json += ",\"link\":{";
    json += "\"polled\":" + boolean(agent.everPolled);
    json += ",\"linked\":" + boolean(agent.linked);
    json += ",\"http_status\":" + std::to_string(agent.lastHttpStatus);
    json += ",\"poll_interval\":" + std::to_string(agent.pollSeconds);
    json += ",\"machine_status\":" + quote(agent.machineStatus);
    json += ",\"last_action\":" + quote(agent.lastAction);
    json += ",\"commands_ok\":" + std::to_string(agent.commandsOk);
    json += ",\"commands_failed\":" + std::to_string(agent.commandsFailed);
    json += ",\"last_error\":" + quote(agent.lastError);
    json += ",\"seconds_since_poll\":" +
            std::to_string(agent.everPolled ? (millis() - agent.lastPollMs) / 1000 : 0);
    json += ",\"busy\":" + boolean(agent_.busy());
    json += "}";
  }
  json += "}";
  sendJson(200, json);
}

void Portal::handleClaim() {
  if (store_.claimed()) {
    sendError(409, "This device already has a portal password.");
    return;
  }
  jsonlite::Document doc;
  if (!doc.parse(body())) {
    sendError(400, "Malformed request.");
    return;
  }
  const std::string password = doc["password"].asString();
  const std::string confirm = doc["confirm"].asString();

  const auto problems = validatePassword(password, confirm);
  if (!problems.empty()) {
    sendError(400, problems.front());
    return;
  }

  store_.setPassword(password);
  startSession();
  Serial.println("[portal] device claimed — portal password set");
  sendJson(200, "{\"ok\":true}");
}

void Portal::handleLogin() {
  if (!store_.claimed()) {
    sendError(409, "This device has no password yet — set one first.");
    return;
  }
  if (lockedOut()) {
    sendError(429, "Too many attempts. Try again in " + std::to_string(lockRemaining()) + "s.");
    return;
  }
  jsonlite::Document doc;
  if (!doc.parse(body())) {
    sendError(400, "Malformed request.");
    return;
  }
  if (!store_.checkPassword(doc["password"].asString())) {
    registerFailure();
    sendError(401, "Wrong password.");
    return;
  }
  startSession();
  sendJson(200, "{\"ok\":true}");
}

void Portal::handleLogout() {
  endSession();
  server_.sendHeader("Set-Cookie", String(kCookieName) + "=; Path=/; HttpOnly; Max-Age=0");
  sendJson(200, "{\"ok\":true}");
}

void Portal::handleGetConfig() {
  if (!authed()) {
    sendError(401, "Sign in first.");
    return;
  }
  const Config& config = store_.config();
  std::string json = "{";
  json += "\"wifi_ssid\":" + quote(config.wifiSsid);
  // Secrets are never sent back — only a hint that one is stored.
  json += ",\"wifi_password_set\":" + boolean(!config.wifiPassword.empty());
  json += ",\"server_url\":" + quote(config.serverUrl);
  json += ",\"api_key_hint\":" + quote(maskHint(config.apiKey));
  json += ",\"api_key_set\":" + boolean(!config.apiKey.empty());
  json += ",\"poll_seconds\":" + std::to_string(config.pollSeconds);
  json += ",\"switch_pin\":" + std::to_string(config.switchPin);
  json += ",\"switch_active_high\":" + boolean(config.switchActiveHigh);
  json += ",\"sense_pin\":" + std::to_string(config.sensePin);
  json += ",\"sense_active_high\":" + boolean(config.senseActiveHigh);
  json += "}";
  sendJson(200, json);
}

void Portal::handleSaveConfig() {
  if (!authed()) {
    sendError(401, "Sign in first.");
    return;
  }
  if (agent_.busy()) {
    sendError(409, "A power pulse is in progress — try again in a moment.");
    return;
  }
  jsonlite::Document doc;
  if (!doc.parse(body())) {
    sendError(400, "Malformed request.");
    return;
  }

  Config next = store_.config();
  next.wifiSsid = doc["wifi_ssid"].asString(next.wifiSsid);
  next.serverUrl = normaliseUrl(doc["server_url"].asString(next.serverUrl));
  next.pollSeconds = static_cast<uint32_t>(doc["poll_seconds"].asLong(next.pollSeconds));
  next.switchPin = static_cast<int>(doc["switch_pin"].asLong(next.switchPin));
  next.switchActiveHigh = doc["switch_active_high"].asBool(next.switchActiveHigh);
  next.sensePin = static_cast<int>(doc["sense_pin"].asLong(next.sensePin));
  next.senseActiveHigh = doc["sense_active_high"].asBool(next.senseActiveHigh);

  // An empty secret field means "keep what is stored", so the UI never has to
  // echo a password back to the browser to preserve it.
  if (doc["wifi_password"].exists() && !doc["wifi_password"].isNull()) {
    const std::string password = doc["wifi_password"].asString();
    if (!password.empty()) next.wifiPassword = password;
  }
  if (doc["wifi_password_clear"].asBool(false)) next.wifiPassword.clear();
  if (doc["api_key"].exists() && !doc["api_key"].isNull()) {
    const std::string key = doc["api_key"].asString();
    if (!key.empty()) next.apiKey = key;
  }

  const auto problems = validate(next);
  if (!problems.empty()) {
    sendError(400, problems.front());
    return;
  }

  const bool wifiChanged = next.wifiSsid != store_.config().wifiSsid ||
                           next.wifiPassword != store_.config().wifiPassword;
  store_.save(next);
  agent_.reconfigure();
  Serial.println("[portal] configuration saved");

  std::string json = "{\"ok\":true,\"wifi_changed\":";
  json += boolean(wifiChanged);
  json += "}";
  sendJson(200, json);
}

void Portal::handleTest() {
  if (!authed()) {
    sendError(401, "Sign in first.");
    return;
  }
  std::string message;
  const bool ok = agent_.testConnection(message);
  sendJson(ok ? 200 : 502, "{\"ok\":" + boolean(ok) + ",\"message\":" + quote(message) + "}");
}

void Portal::handleWifiScan() {
  if (!authed()) {
    sendError(401, "Sign in first.");
    return;
  }
  if (agent_.busy()) {
    sendError(409, "A power pulse is in progress — try again in a moment.");
    return;
  }

  int count = WiFi.scanNetworks();
  if (count < 0) count = 0;

  // Fold repeats of the same SSID (seen on more than one channel/AP) into one
  // entry, keeping the strongest signal, and drop hidden networks — there is
  // nothing useful to offer as a suggestion for those.
  struct Seen {
    std::string ssid;
    int32_t rssi;
    bool secure;
  };
  std::vector<Seen> networks;
  for (int i = 0; i < count; i++) {
    const std::string ssid = std::string(WiFi.SSID(i).c_str());
    if (ssid.empty()) continue;
    const int32_t rssi = WiFi.RSSI(i);
    const bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;

    auto existing = std::find_if(networks.begin(), networks.end(),
                                  [&](const Seen& s) { return s.ssid == ssid; });
    if (existing == networks.end()) {
      networks.push_back({ssid, rssi, secure});
    } else if (rssi > existing->rssi) {
      existing->rssi = rssi;
    }
  }
  std::sort(networks.begin(), networks.end(),
            [](const Seen& a, const Seen& b) { return a.rssi > b.rssi; });
  WiFi.scanDelete();

  std::string json = "{\"networks\":[";
  for (size_t i = 0; i < networks.size(); i++) {
    if (i) json += ",";
    json += "{\"ssid\":" + quote(networks[i].ssid);
    json += ",\"rssi\":" + std::to_string(networks[i].rssi);
    json += ",\"secure\":" + boolean(networks[i].secure);
    json += "}";
  }
  json += "]}";
  sendJson(200, json);
}

void Portal::handleWifiTest() {
  if (!authed()) {
    sendError(401, "Sign in first.");
    return;
  }
  if (agent_.busy()) {
    sendError(409, "A power pulse is in progress — try again in a moment.");
    return;
  }
  jsonlite::Document doc;
  if (!doc.parse(body())) {
    sendError(400, "Malformed request.");
    return;
  }
  const std::string ssid = doc["ssid"].asString();
  std::string password = doc["password"].asString();
  if (ssid.empty()) {
    sendError(400, "Enter a network name first.");
    return;
  }
  // A blank password field means "keep the stored one" everywhere else in this
  // form, so testing the already-saved network without retyping it works too.
  const Config& saved = store_.config();
  if (password.empty() && ssid == saved.wifiSsid) password = saved.wifiPassword;

  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), password.empty() ? nullptr : password.c_str());

  wl_status_t result = WL_IDLE_STATUS;
  const uint32_t deadline = millis() + 10000;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    result = WiFi.status();
    if (result == WL_CONNECTED || result == WL_CONNECT_FAILED || result == WL_NO_SSID_AVAIL) break;
    delay(150);
  }

  const bool ok = result == WL_CONNECTED;
  std::string message;
  if (ok) {
    message = "Connected — " + std::string(WiFi.localIP().toString().c_str());
  } else if (result == WL_NO_SSID_AVAIL) {
    message = "Network not found.";
  } else {
    message = "Could not connect — check the password.";
  }

  // This was only ever a preview — put the radio back on whatever is
  // actually saved so normal operation is not left stuck on the tested network.
  WiFi.disconnect();
  if (!saved.wifiSsid.empty()) {
    WiFi.begin(saved.wifiSsid.c_str(),
               saved.wifiPassword.empty() ? nullptr : saved.wifiPassword.c_str());
  }

  sendJson(ok ? 200 : 502, "{\"ok\":" + boolean(ok) + ",\"message\":" + quote(message) + "}");
}

void Portal::handleReboot() {
  if (!authed()) {
    sendError(401, "Sign in first.");
    return;
  }
  rebootAt_ = millis() + 500;  // answer first, restart just after
  sendJson(200, "{\"ok\":true}");
}

void Portal::handleNotFound() {
  // Anything else (including a captive-portal probe) lands on the UI.
  server_.sendHeader("Location", "/");
  server_.send(302, "text/plain", "");
}

// ------------------------------------------------------------------ helpers

std::string Portal::body() {
  return std::string(server_.arg("plain").c_str());
}

void Portal::sendJson(int code, const std::string& json) {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(code, "application/json", json.c_str());
}

void Portal::sendError(int code, const std::string& message) {
  sendJson(code, "{\"ok\":false,\"detail\":" + quote(message) + "}");
}

}  // namespace tardis
