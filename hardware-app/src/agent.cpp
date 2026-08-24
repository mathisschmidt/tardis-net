#include "agent.h"

#include <HTTPClient.h>
#include <WiFi.h>

namespace tardis {
namespace {

constexpr uint32_t kHttpTimeoutMs = 8000;
constexpr uint32_t kMaxBackoffMs = 60000;

}  // namespace

Agent::Agent(ConfigStore& store)
    : store_(store), runner_([this](bool closed) { applySwitch(closed); }) {}

void Agent::begin() {
  const Config& config = store_.config();
  if (config.switchPin >= 0) {
    // Write the released level *before* switching the pin to output, so the
    // boot does not glitch the machine's power button.
    digitalWrite(config.switchPin, config.switchActiveHigh ? LOW : HIGH);
    pinMode(config.switchPin, OUTPUT);
    digitalWrite(config.switchPin, config.switchActiveHigh ? LOW : HIGH);
  }
  if (config.sensePin >= 0) {
    pinMode(config.sensePin, INPUT_PULLUP);
  }
  status_.pollSeconds = clampPollSeconds(config.pollSeconds);
}

void Agent::reconfigure() {
  runner_.abort();
  begin();
  nextPollMs_ = millis();  // pick up the new server/key immediately
}

void Agent::applySwitch(bool closed) {
  const Config& config = store_.config();
  if (config.switchPin < 0) return;
  const bool level = config.switchActiveHigh ? closed : !closed;
  digitalWrite(config.switchPin, level ? HIGH : LOW);
}

int Agent::readPowerSense() const {
  const Config& config = store_.config();
  if (config.sensePin < 0) return -1;
  const bool high = digitalRead(config.sensePin) == HIGH;
  return (high == config.senseActiveHigh) ? 1 : 0;
}

void Agent::update(bool networkReady) {
  const uint32_t now = millis();

  // The pulse runs on its own clock; finishing it is more urgent than polling.
  if (runner_.update(now)) {
    Serial.printf("[agent] pulse done (%s, %ums)\n", actionName(runner_.current().action),
                  runner_.current().pulseMs);
    ackPendingCompleted_ = true;
    ackPendingDetail_.clear();
    nextAckMs_ = now;
  }

  if (!networkReady) return;

  if (runner_.state() == RunnerState::AckPending) {
    if (static_cast<int32_t>(now - nextAckMs_) >= 0) sendAck();
    return;
  }

  if (static_cast<int32_t>(now - nextPollMs_) >= 0) poll();
}

void Agent::poll() {
  const Config& config = store_.config();
  if (!config.complete()) {
    status_.lastError = "Device is not configured yet.";
    scheduleNextPoll(true);
    return;
  }

  DeviceReport report;
  report.ip = std::string(WiFi.localIP().toString().c_str());
  report.rssi = WiFi.RSSI();
  report.uptimeSeconds = millis() / 1000;
  report.powerSense = readPowerSense();

  std::string response;
  const int httpStatus = request(kPollPath, buildPollBody(report), response);
  status_.lastHttpStatus = httpStatus;
  status_.lastPollMs = millis();
  status_.everPolled = true;

  if (httpStatus != 200) {
    status_.linked = false;
    status_.lastError = httpStatus == 401
                            ? "Server rejected the API key (401)."
                            : (httpStatus > 0 ? "Server replied " + std::to_string(httpStatus)
                                              : "Cannot reach the server.");
    scheduleNextPoll(true);
    return;
  }

  const PollResponse parsed = parsePollResponse(response);
  if (!parsed.ok) {
    status_.linked = false;
    status_.lastError = "Unexpected reply from the server.";
    scheduleNextPoll(true);
    return;
  }

  status_.linked = true;
  status_.lastError.clear();
  status_.lastSuccessMs = millis();
  status_.machineStatus = parsed.machineStatus;
  if (parsed.pollSeconds > 0) status_.pollSeconds = parsed.pollSeconds;

  if (parsed.command.valid() && runner_.accept(parsed.command, millis())) {
    status_.lastAction = actionName(parsed.command.action);
    Serial.printf("[agent] %s: holding the switch for %ums\n", status_.lastAction.c_str(),
                  parsed.command.pulseMs);
  }
  scheduleNextPoll(false);
}

void Agent::sendAck() {
  const std::string id = runner_.current().id;
  std::string response;
  const int httpStatus =
      request(kAckPath, buildAckBody(id, ackPendingCompleted_, ackPendingDetail_), response);

  if (httpStatus == 200) {
    if (ackPendingCompleted_) {
      status_.commandsOk++;
    } else {
      status_.commandsFailed++;
    }
    runner_.ackDone();
    nextPollMs_ = millis();  // report the new state straight away
    return;
  }

  if (httpStatus == 409 || httpStatus == 401) {
    // Expired, already settled, or the key was rotated — retrying cannot help.
    Serial.printf("[agent] ack refused (%d), dropping command %s\n", httpStatus, id.c_str());
    status_.commandsFailed++;
    status_.lastError = httpStatus == 401 ? "Server rejected the API key (401)."
                                          : "Server had already closed that command.";
    runner_.ackDone();
    return;
  }

  // Transport failure: keep the command and try again shortly. The pulse is
  // already over, so there is no rush — and no reason to hammer a down server.
  status_.lastError = "Could not confirm the pulse — retrying.";
  nextAckMs_ = millis() + 1000;
}

int Agent::request(const char* path, const std::string& body, std::string& response) {
  const Config& config = store_.config();
  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setReuse(false);

  const std::string url = config.serverUrl + path;
  if (!http.begin(url.c_str())) {
    response.clear();
    return -1;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader(kKeyHeader, config.apiKey.c_str());

  const int httpStatus =
      http.POST(const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(body.data())), body.size());
  if (httpStatus > 0) {
    response = std::string(http.getString().c_str());
  } else {
    response.clear();
  }
  http.end();
  return httpStatus;
}

void Agent::scheduleNextPoll(bool failed) {
  if (failed) {
    failures_++;
    // Exponential backoff, capped, so a server outage does not become a flood.
    uint32_t delayMs = status_.pollSeconds * 1000UL;
    for (uint32_t i = 1; i < failures_ && delayMs < kMaxBackoffMs; i++) delayMs *= 2;
    nextPollMs_ = millis() + (delayMs > kMaxBackoffMs ? kMaxBackoffMs : delayMs);
    return;
  }
  failures_ = 0;
  nextPollMs_ = millis() + status_.pollSeconds * 1000UL;
}

bool Agent::testConnection(std::string& message) {
  const Config& config = store_.config();
  if (!config.complete()) {
    message = "Fill in the server address and API key first.";
    return false;
  }

  DeviceReport report;
  report.ip = std::string(WiFi.localIP().toString().c_str());
  report.rssi = WiFi.RSSI();
  report.uptimeSeconds = millis() / 1000;
  report.powerSense = readPowerSense();

  std::string response;
  const int httpStatus = request(kPollPath, buildPollBody(report), response);
  if (httpStatus == 200) {
    const PollResponse parsed = parsePollResponse(response);
    if (!parsed.ok) {
      message = "Connected, but the reply was not understood.";
      return false;
    }
    message = "Connected. " + store_.config().serverUrl + " reports " +
              (parsed.machineStatus.empty() ? std::string("no status") : parsed.machineStatus) +
              ".";
    return true;
  }
  if (httpStatus == 401) {
    message = "The server rejected this API key.";
  } else if (httpStatus > 0) {
    message = "The server replied " + std::to_string(httpStatus) + ".";
  } else {
    message = "Could not reach " + config.serverUrl +
              " — check the address, and that the console isn't bound to "
              "127.0.0.1 only (it needs --host 0.0.0.0 to answer devices on the network).";
  }
  return false;
}

}  // namespace tardis
