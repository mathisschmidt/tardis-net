// Implementations behind the shim headers, plus the main() that runs the
// firmware's own setup()/loop().

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#include "Arduino.h"
#include "HTTPClient.h"
#include "Preferences.h"
#include "WebServer.h"
#include "WiFi.h"
#include "ESPmDNS.h"

// ----------------------------------------------------------------- Arduino

SerialClass Serial;
EspClass ESP;
WiFiClass WiFi;
MDNSResponder MDNS;

namespace {
const auto kStart = std::chrono::steady_clock::now();
bool g_running = true;
int g_pinLevels[64] = {};
int g_pinInputs[64] = {};
std::vector<hostgpio::Event> g_events;
}  // namespace

uint32_t millis() {
  const auto now = std::chrono::steady_clock::now();
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now - kStart).count());
}

void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void pinMode(int, int) {}

void digitalWrite(int pin, int level) {
  if (pin < 0 || pin >= 64) return;
  if (g_pinLevels[pin] == level && !g_events.empty()) return;
  g_pinLevels[pin] = level;
  g_events.push_back({millis(), pin, level});
  std::printf("[gpio] %ums pin %d -> %s\n", millis(), pin, level ? "HIGH" : "LOW");
  std::fflush(stdout);
}

int digitalRead(int pin) { return (pin >= 0 && pin < 64) ? g_pinInputs[pin] : 0; }

namespace hostgpio {
const Event* events() { return g_events.data(); }
size_t eventCount() { return g_events.size(); }
void setInput(int pin, int level) {
  if (pin >= 0 && pin < 64) g_pinInputs[pin] = level;
}
}  // namespace hostgpio

void EspClass::restart() {
  std::printf("[host] ESP.restart() — stopping\n");
  g_running = false;
}

uint32_t esp_random() {
  static std::mt19937 rng(std::random_device{}());
  return rng();
}

// -------------------------------------------------------------------- WiFi

std::vector<WiFiClass::Network>& WiFiClass::airwaves() {
  static std::vector<Network> networks = {
      {"home-network", "hunter2hunter2", -47},
      {"neighbour-5g", "somethingelse", -74},
      {"cafe-guest", "", -80},
  };
  return networks;
}

uint32_t& WiFiClass::joinDelayMs() {
  static uint32_t value = 300;
  return value;
}

void WiFiClass::begin(const char* ssid, const char* password) {
  pendingSsid_ = ssid ? ssid : "";
  pendingPassword_ = password ? password : "";
  joining_ = true;
  joinCompleteAt_ = millis() + joinDelayMs();
  status_ = WL_IDLE_STATUS;
}

void WiFiClass::disconnect(bool, bool) {
  joining_ = false;
  status_ = WL_DISCONNECTED;
  connectedSsid_.clear();
}

wl_status_t WiFiClass::status() {
  if (joining_ && static_cast<int32_t>(millis() - joinCompleteAt_) >= 0) {
    joining_ = false;
    const auto& networks = airwaves();
    for (const auto& network : networks) {
      if (network.ssid != pendingSsid_) continue;
      status_ = (network.password == pendingPassword_) ? WL_CONNECTED : WL_CONNECT_FAILED;
      if (status_ == WL_CONNECTED) connectedSsid_ = pendingSsid_;
      return status_;
    }
    status_ = WL_NO_SSID_AVAIL;
  }
  return status_;
}

bool WiFiClass::softAP(const char* ssid, const char*) {
  std::printf("[host] softAP up: %s\n", ssid ? ssid : "");
  return true;
}

bool WiFiClass::softAPdisconnect(bool) { return true; }

IPAddress WiFiClass::localIP() const {
  return IPAddress(status_ == WL_CONNECTED ? "192.168.1.50" : "0.0.0.0");
}

int WiFiClass::scanNetworks(bool, bool) {
  scan_ = airwaves();
  return static_cast<int>(scan_.size());
}

String WiFiClass::SSID(int index) const {
  return String(index >= 0 && index < static_cast<int>(scan_.size()) ? scan_[index].ssid : "");
}

int32_t WiFiClass::RSSI(int index) const {
  return index >= 0 && index < static_cast<int>(scan_.size()) ? scan_[index].rssi : 0;
}

wifi_auth_mode_t WiFiClass::encryptionType(int index) const {
  const bool secure =
      index >= 0 && index < static_cast<int>(scan_.size()) && !scan_[index].password.empty();
  return secure ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
}

// ------------------------------------------------------------- Preferences

namespace {
std::string prefsPath(const char* name) {
  const char* dir = std::getenv("TARDIS_HOST_STATE_DIR");
  return std::string(dir ? dir : ".") + "/nvs-" + name + ".txt";
}

std::string escapeLine(const std::string& value) {
  std::string out;
  for (char c : value) {
    if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else out.push_back(c);
  }
  return out;
}

std::string unescapeLine(const std::string& value) {
  std::string out;
  for (size_t i = 0; i < value.size(); i++) {
    if (value[i] == '\\' && i + 1 < value.size()) {
      out.push_back(value[++i] == 'n' ? '\n' : value[i]);
    } else {
      out.push_back(value[i]);
    }
  }
  return out;
}
}  // namespace

bool Preferences::begin(const char* name, bool) {
  path_ = prefsPath(name);
  load();
  return true;
}

void Preferences::load() {
  values_.clear();
  std::ifstream in(path_);
  std::string line;
  while (std::getline(in, line)) {
    const size_t split = line.find('=');
    if (split == std::string::npos) continue;
    values_[line.substr(0, split)] = unescapeLine(line.substr(split + 1));
  }
}

void Preferences::flush() {
  std::ofstream out(path_, std::ios::trunc);
  for (const auto& [key, value] : values_) out << key << "=" << escapeLine(value) << "\n";
}

bool Preferences::clear() {
  values_.clear();
  flush();
  return true;
}

bool Preferences::isKey(const char* key) const { return values_.count(key) != 0; }

String Preferences::getString(const char* key, const String& fallback) {
  auto found = values_.find(key);
  return found == values_.end() ? fallback : String(found->second);
}

size_t Preferences::putString(const char* key, const char* value) {
  values_[key] = value ? value : "";
  flush();
  return values_[key].size();
}

uint32_t Preferences::getUInt(const char* key, uint32_t fallback) {
  auto found = values_.find(key);
  return found == values_.end() ? fallback
                                : static_cast<uint32_t>(std::strtoul(found->second.c_str(), nullptr, 10));
}

size_t Preferences::putUInt(const char* key, uint32_t value) {
  return putString(key, std::to_string(value).c_str());
}

int32_t Preferences::getInt(const char* key, int32_t fallback) {
  auto found = values_.find(key);
  return found == values_.end() ? fallback
                                : static_cast<int32_t>(std::strtol(found->second.c_str(), nullptr, 10));
}

size_t Preferences::putInt(const char* key, int32_t value) {
  return putString(key, std::to_string(value).c_str());
}

bool Preferences::getBool(const char* key, bool fallback) {
  auto found = values_.find(key);
  return found == values_.end() ? fallback : found->second == "1";
}

size_t Preferences::putBool(const char* key, bool value) {
  return putString(key, value ? "1" : "0");
}

// --------------------------------------------------------------- WebServer

WebServer::WebServer(int port) : port_(port) {
  if (const char* override = std::getenv("TARDIS_PORTAL_PORT")) port_ = std::atoi(override);
}

WebServer::~WebServer() {
  if (listener_ >= 0) close(listener_);
}

void WebServer::on(const char* uri, HTTPMethod method, Handler handler) {
  routes_.push_back({uri, method, std::move(handler)});
}

void WebServer::collectHeaders(const char** headers, size_t count) {
  for (size_t i = 0; i < count; i++) collected_.push_back(headers[i]);
}

void WebServer::begin() {
  listener_ = socket(AF_INET, SOCK_STREAM, 0);
  int reuse = 1;
  setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(static_cast<uint16_t>(port_));
  if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    std::printf("[host] cannot bind port %d\n", port_);
    return;
  }
  listen(listener_, 8);
  std::printf("[host] portal listening on http://127.0.0.1:%d/\n", port_);
  std::fflush(stdout);
}

bool WebServer::readRequest() {
  std::string raw;
  char buffer[2048];
  size_t headerEnd = std::string::npos;

  while (headerEnd == std::string::npos) {
    const ssize_t got = recv(client_, buffer, sizeof(buffer), 0);
    if (got <= 0) return false;
    raw.append(buffer, static_cast<size_t>(got));
    headerEnd = raw.find("\r\n\r\n");
  }

  std::istringstream stream(raw.substr(0, headerEnd));
  std::string line;
  std::getline(stream, line);
  std::istringstream request(line);
  std::string verb, target, version;
  request >> verb >> target >> version;

  method_ = verb == "POST" ? HTTP_POST : (verb == "GET" ? HTTP_GET : HTTP_ANY);
  const size_t query = target.find('?');
  uri_ = query == std::string::npos ? target : target.substr(0, query);

  headers_.clear();
  size_t contentLength = 0;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    std::string lower = name;
    for (char& c : lower) c = static_cast<char>(tolower(c));
    if (lower == "content-length") contentLength = std::strtoul(value.c_str(), nullptr, 10);
    headers_[lower] = value;
  }

  body_ = raw.substr(headerEnd + 4);
  while (body_.size() < contentLength) {
    const ssize_t got = recv(client_, buffer, sizeof(buffer), 0);
    if (got <= 0) break;
    body_.append(buffer, static_cast<size_t>(got));
  }
  return true;
}

void WebServer::handleClient() {
  if (listener_ < 0) return;
  pollfd waiting{listener_, POLLIN, 0};
  if (poll(&waiting, 1, 0) <= 0) return;

  client_ = accept(listener_, nullptr, nullptr);
  if (client_ < 0) return;

  timeval timeout{2, 0};
  setsockopt(client_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  responded_ = false;
  pendingHeaders_.clear();
  if (readRequest()) {
    bool handled = false;
    for (const auto& route : routes_) {
      if (route.uri != uri_) continue;
      if (route.method != HTTP_ANY && route.method != method_) continue;
      route.handler();
      handled = true;
      break;
    }
    if (!handled && notFound_) notFound_();
    if (!responded_) writeResponse(404, "text/plain", "not found");
  }
  close(client_);
  client_ = -1;
}

void WebServer::writeResponse(int code, const std::string& contentType, const std::string& body) {
  std::string response = "HTTP/1.1 " + std::to_string(code) + " OK\r\n";
  response += "Content-Type: " + contentType + "\r\n";
  response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  response += "Connection: close\r\n";
  for (const auto& [name, value] : pendingHeaders_) {
    response += name + ": " + value + "\r\n";
  }
  response += "\r\n" + body;
  ssize_t sent = 0;
  while (sent < static_cast<ssize_t>(response.size())) {
    const ssize_t wrote = ::send(client_, response.data() + sent, response.size() - sent, 0);
    if (wrote <= 0) break;
    sent += wrote;
  }
  responded_ = true;
}

void WebServer::send(int code, const char* contentType, const String& body) {
  writeResponse(code, contentType, body.str());
}

void WebServer::send(int code, const char* contentType, const char* body) {
  writeResponse(code, contentType, body ? body : "");
}

void WebServer::send_P(int code, const char* contentType, const char* body) {
  writeResponse(code, contentType, body ? body : "");
}

void WebServer::sendHeader(const String& name, const String& value, bool) {
  pendingHeaders_.emplace_back(name.str(), value.str());
}

String WebServer::arg(const char* name) const {
  if (std::string(name) == "plain") return String(body_);
  return String("");
}

bool WebServer::hasHeader(const char* name) const {
  std::string lower(name);
  for (char& c : lower) c = static_cast<char>(tolower(c));
  return headers_.count(lower) != 0;
}

String WebServer::header(const char* name) const {
  std::string lower(name);
  for (char& c : lower) c = static_cast<char>(tolower(c));
  auto found = headers_.find(lower);
  return String(found == headers_.end() ? "" : found->second);
}

// -------------------------------------------------------------- HTTPClient

bool HTTPClient::begin(const char* url) {
  headers_.clear();
  response_.clear();
  std::string text(url ? url : "");
  https_ = text.rfind("https://", 0) == 0;
  const size_t schemeEnd = text.find("://");
  if (schemeEnd == std::string::npos) return false;
  text = text.substr(schemeEnd + 3);

  const size_t slash = text.find('/');
  std::string hostPort = slash == std::string::npos ? text : text.substr(0, slash);
  path_ = slash == std::string::npos ? "/" : text.substr(slash);

  const size_t colon = hostPort.find(':');
  if (colon == std::string::npos) {
    host_ = hostPort;
    port_ = https_ ? 443 : 80;
  } else {
    host_ = hostPort.substr(0, colon);
    port_ = std::atoi(hostPort.c_str() + colon + 1);
  }
  return !host_.empty();
}

void HTTPClient::end() {}

void HTTPClient::addHeader(const char* name, const String& value) {
  headers_.emplace_back(name, value.str());
}

int HTTPClient::POST(const String& payload) {
  const std::string body = payload.str();
  return POST(reinterpret_cast<uint8_t*>(const_cast<char*>(body.data())), body.size());
}

int HTTPClient::POST(uint8_t* payload, size_t size) {
  if (https_) return -1;  // the host harness speaks plain HTTP only

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* resolved = nullptr;
  if (getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &resolved) != 0) {
    return -1;
  }

  const int socketFd = socket(resolved->ai_family, resolved->ai_socktype, 0);
  timeval timeout{timeoutMs_ / 1000, (timeoutMs_ % 1000) * 1000};
  setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  if (connect(socketFd, resolved->ai_addr, resolved->ai_addrlen) < 0) {
    freeaddrinfo(resolved);
    close(socketFd);
    return -1;
  }
  freeaddrinfo(resolved);

  std::string request = "POST " + path_ + " HTTP/1.1\r\nHost: " + host_ + ":" +
                        std::to_string(port_) + "\r\nConnection: close\r\n";
  for (const auto& [name, value] : headers_) request += name + ": " + value + "\r\n";
  request += "Content-Length: " + std::to_string(size) + "\r\n\r\n";
  request.append(reinterpret_cast<const char*>(payload), size);
  ::send(socketFd, request.data(), request.size(), 0);

  std::string raw;
  char buffer[2048];
  ssize_t got;
  while ((got = recv(socketFd, buffer, sizeof(buffer), 0)) > 0) {
    raw.append(buffer, static_cast<size_t>(got));
  }
  close(socketFd);

  if (raw.empty()) return -1;
  const size_t statusEnd = raw.find("\r\n");
  const size_t firstSpace = raw.find(' ');
  if (firstSpace == std::string::npos || statusEnd == std::string::npos) return -1;
  const int status = std::atoi(raw.c_str() + firstSpace + 1);

  const size_t headerEnd = raw.find("\r\n\r\n");
  response_ = headerEnd == std::string::npos ? "" : raw.substr(headerEnd + 4);
  return status;
}

// --------------------------------------------------------------------- main

void setup();
void loop();

int main() {
  // Unbuffered: the log is the harness's only window into what the firmware
  // did, and a buffered tail looks exactly like "nothing happened".
  setvbuf(stdout, nullptr, _IONBF, 0);
  setup();
  while (g_running) {
    loop();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return 0;
}
