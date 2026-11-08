// Host tests for everything the firmware can run off-device: JSON parsing, the
// wire protocol, the pulse/ack state machine and config validation.
//
//   make -C hardware-app test
//
// The ESP32-specific layers (NVS, WiFi, WebServer) are exercised on hardware;
// what is tested here is the logic that decides how long a relay stays closed.

#include <cassert>
#include <cstdio>
#include <string>

#include "device_config.h"
#include "json_lite.h"
#include "password.h"
#include "protocol.h"
#include "runner.h"
#include "sha256.h"
#include "wifi_keeper.h"

static int checks = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    checks++;                                                                  \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
      return 1;                                                                \
    }                                                                          \
  } while (0)

using namespace tardis;

// A realistic poll response, copied from what server-app actually sends.
static const char* kPollWithCommand = R"({
  "server_time": 1787375026.9,
  "poll_interval": 5,
  "machine": {"name": "tardis", "status": "booting", "is_on": false, "linked": true},
  "command": {"id": "528ab6d262a9d986", "action": "power_on", "pulse_ms": 500,
              "requested_at": 1787375026.9, "expires_in": 60}
})";

static const char* kPollIdle = R"({
  "server_time": 1787375026.9, "poll_interval": 5,
  "machine": {"status": "online"}, "command": null
})";

static int testJson() {
  jsonlite::Document doc;
  CHECK(doc.parse(kPollWithCommand));
  CHECK(doc["poll_interval"].asLong() == 5);
  CHECK(doc["machine"]["status"].asString() == "booting");
  CHECK(doc["machine"]["linked"].asBool() == true);
  CHECK(doc["command"]["id"].asString() == "528ab6d262a9d986");
  CHECK(doc["nope"].exists() == false);
  CHECK(doc["machine"]["nope"].asString("fallback") == "fallback");

  // Escapes and unicode in strings.
  jsonlite::Document esc;
  CHECK(esc.parse(R"({"a":"line\nbreak \"quoted\" A"})"));
  CHECK(esc["a"].asString() == "line\nbreak \"quoted\" A");

  // Malformed input must fail rather than half-parse.
  const char* broken[] = {
      "",           "{",          "{\"a\":}",     "{\"a\" 1}",  "{'a':1}",
      "{\"a\":1",   "[1,2",       "{\"a\":\"x}",  "nul",        "{\"a\":1}trailing",
  };
  for (const char* text : broken) {
    jsonlite::Document bad;
    CHECK(!bad.parse(text));
  }

  // Deep nesting is rejected instead of recursing without bound.
  std::string deep;
  for (int i = 0; i < 40; i++) deep += "{\"a\":";
  deep += "1";
  for (int i = 0; i < 40; i++) deep += "}";
  jsonlite::Document nested;
  CHECK(!nested.parse(deep));

  CHECK(jsonlite::escape("a\"b\\c\nd") == "a\\\"b\\\\c\\nd");
  return 0;
}

static int testProtocol() {
  PollResponse poll = parsePollResponse(kPollWithCommand);
  CHECK(poll.ok);
  CHECK(poll.pollSeconds == 5);
  CHECK(poll.machineStatus == "booting");
  CHECK(poll.command.valid());
  CHECK(poll.command.action == Action::PowerOn);
  CHECK(poll.command.pulseMs == 500);

  PollResponse idle = parsePollResponse(kPollIdle);
  CHECK(idle.ok);
  CHECK(!idle.command.valid());
  CHECK(idle.machineStatus == "online");

  PollResponse garbage = parsePollResponse("<html>404</html>");
  CHECK(!garbage.ok);
  CHECK(!garbage.command.valid());

  // The three actions from the table, with their documented hold times.
  CHECK(expectedPulseMs(Action::PowerOn) == 500);
  CHECK(expectedPulseMs(Action::GracefulShutdown) == 500);
  CHECK(expectedPulseMs(Action::HardPowerOff) == 5000);
  CHECK(actionFromString("hard_power_off") == Action::HardPowerOff);
  CHECK(actionFromString("something_else") == Action::None);

  // An unknown action is dropped, not guessed at.
  PollResponse unknown = parsePollResponse(
      R"({"command":{"id":"a","action":"self_destruct","pulse_ms":500}})");
  CHECK(!unknown.command.valid());

  // A hostile pulse length is clamped before it reaches the relay.
  PollResponse huge = parsePollResponse(
      R"({"command":{"id":"a","action":"hard_power_off","pulse_ms":999999}})");
  CHECK(huge.command.pulseMs == kMaxPulseMs);
  PollResponse negative = parsePollResponse(
      R"({"command":{"id":"a","action":"power_on","pulse_ms":-5}})");
  CHECK(negative.command.pulseMs == kMinPulseMs);
  // A missing pulse falls back to the documented duration for the action.
  PollResponse missing = parsePollResponse(
      R"({"command":{"id":"a","action":"hard_power_off"}})");
  CHECK(missing.command.pulseMs == 5000);

  CHECK(clampPollSeconds(0) == kMinPollSeconds);
  CHECK(clampPollSeconds(99999) == kMaxPollSeconds);

  DeviceReport report;
  report.ip = "192.168.1.50";
  report.rssi = -57;
  report.uptimeSeconds = 3600;
  std::string body = buildPollBody(report);
  CHECK(body.find("\"firmware\":\"1.1.0\"") != std::string::npos);
  CHECK(body.find("\"ip\":\"192.168.1.50\"") != std::string::npos);
  CHECK(body.find("\"rssi\":-57") != std::string::npos);
  CHECK(body.find("\"power_sense\":\"unknown\"") != std::string::npos);  // no sense pin fitted
  report.powerSense = 1;
  CHECK(buildPollBody(report).find("\"power_sense\":\"on\"") != std::string::npos);
  report.powerSense = 0;
  CHECK(buildPollBody(report).find("\"power_sense\":\"off\"") != std::string::npos);

  CHECK(buildAckBody("abc", true) == R"({"id":"abc","status":"completed"})");
  CHECK(buildAckBody("abc", false, "gpio busy") ==
        R"({"id":"abc","status":"failed","detail":"gpio busy"})");
  // A malicious id cannot break out of the JSON string.
  CHECK(buildAckBody("a\"b", true).find("a\\\"b") != std::string::npos);
  return 0;
}

static int testRunner() {
  bool closed = false;
  int transitions = 0;
  CommandRunner runner([&](bool on) {
    closed = on;
    transitions++;
  });

  Command command;
  command.id = "c1";
  command.action = Action::PowerOn;
  command.pulseMs = 500;

  CHECK(runner.state() == RunnerState::Idle);
  CHECK(runner.accept(command, 1000));
  CHECK(closed);  // contact closed immediately
  CHECK(runner.state() == RunnerState::Pulsing);

  // A duplicate delivery must not stack a second pulse.
  Command duplicate = command;
  duplicate.id = "c2";
  CHECK(!runner.accept(duplicate, 1100));
  CHECK(runner.current().id == "c1");

  CHECK(!runner.update(1200));   // still holding
  CHECK(closed);
  CHECK(!runner.update(1499));
  CHECK(runner.update(1500));    // released exactly on time
  CHECK(!closed);
  CHECK(runner.state() == RunnerState::AckPending);
  CHECK(!runner.update(9999));   // only fires once

  // Nothing new starts until the ack is done.
  CHECK(!runner.accept(duplicate, 1600));
  runner.ackDone();
  CHECK(runner.state() == RunnerState::Idle);
  CHECK(runner.accept(duplicate, 1700));
  runner.abort();
  CHECK(!closed);
  CHECK(runner.state() == RunnerState::Idle);

  // A five second hold really holds for five seconds.
  Command hard;
  hard.id = "c3";
  hard.action = Action::HardPowerOff;
  hard.pulseMs = expectedPulseMs(Action::HardPowerOff);
  CHECK(runner.accept(hard, 10000));
  CHECK(!runner.update(14999));
  CHECK(closed);
  CHECK(runner.update(15000));
  CHECK(!closed);

  // millis() rollover mid-pulse must not leave the contact closed forever.
  CommandRunner rollover([&](bool on) { closed = on; });
  uint32_t nearMax = 0xFFFFFF00;
  CHECK(rollover.accept(command, nearMax));
  CHECK(!rollover.update(nearMax + 100));
  CHECK(rollover.update(nearMax + 500));  // wraps past zero
  CHECK(!closed);
  return 0;
}

static int testConfig() {
  Config config;
  config.serverUrl = "http://192.168.1.10:8000";
  config.apiKey = "xCVEMxCqquaIsROK";
  CHECK(validate(config).empty());
  CHECK(config.complete());

  Config missing;
  CHECK(!missing.complete());
  CHECK(validate(missing).size() >= 2);

  Config badUrl = config;
  badUrl.serverUrl = "192.168.1.10:8000";
  CHECK(!validate(badUrl).empty());

  // Pins that would brick the board or cannot drive an output.
  for (int pin : {6, 7, 11, 34, 36, 39, -2, 99}) {
    Config badPin = config;
    badPin.switchPin = pin;
    CHECK(!validate(badPin).empty());
  }
  Config senseClash = config;
  senseClash.sensePin = config.switchPin;
  CHECK(!validate(senseClash).empty());
  Config senseOk = config;
  senseOk.sensePin = 34;  // input-only pins are fine for sensing
  CHECK(validate(senseOk).empty());

  Config fastPoll = config;
  fastPoll.pollSeconds = 0;
  CHECK(!validate(fastPoll).empty());

  CHECK(normaliseUrl("  http://host:8000/  ") == "http://host:8000");
  CHECK(normaliseUrl("http://host") == "http://host");

  CHECK(validatePassword("correct-horse", "correct-horse").empty());
  CHECK(!validatePassword("short", "short").empty());
  CHECK(!validatePassword("correct-horse", "typo").empty());
  return 0;
}

static int testCrypto() {
  // FIPS 180-4 vectors — proof the hash is the real SHA-256, not nearly.
  CHECK(crypto::sha256Hex("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(crypto::sha256Hex("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(crypto::sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  // Longer than one block, and exactly on a padding boundary (55/56 bytes).
  CHECK(crypto::sha256Hex(std::string(55, 'a')) ==
        "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  CHECK(crypto::sha256Hex(std::string(56, 'a')) ==
        "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  CHECK(crypto::sha256Hex(std::string(1000, 'a')) ==
        "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");

  CHECK(crypto::constantTimeEquals("abc", "abc"));
  CHECK(!crypto::constantTimeEquals("abc", "abd"));
  CHECK(!crypto::constantTimeEquals("abc", "ab"));

  // Password records: same password + salt -> same digest, different salt -> not.
  uint8_t counter = 0;
  auto fakeRandom = [&counter](uint8_t* out, size_t size) {
    for (size_t i = 0; i < size; i++) out[i] = static_cast<uint8_t>(counter + i);
    counter++;
  };
  PasswordRecord first = makePassword("correct-horse", fakeRandom);
  PasswordRecord second = makePassword("correct-horse", fakeRandom);
  CHECK(first.set());
  CHECK(first.salt.size() == kSaltBytes * 2);
  CHECK(first.digest != second.digest);        // salted, so no rainbow tables
  CHECK(first.matches("correct-horse"));
  CHECK(!first.matches("correct-hors"));
  CHECK(!first.matches(""));
  CHECK(hashPassword(first.salt, "x") == hashPassword(first.salt, "x"));

  PasswordRecord unset;
  CHECK(!unset.set());
  CHECK(!unset.matches("anything"));           // an unclaimed device lets nobody in
  return 0;
}

// Runs the keeper from `from` to `to` (inclusive) one second at a time and
// counts what it asked for. The station's state never changes in between.
struct WifiTally {
  int retries = 0, opens = 0, closes = 0, restarts = 0;
};
static WifiTally runKeeper(WifiKeeper& keeper, bool connected, uint32_t from, uint32_t to) {
  WifiTally tally;
  for (uint32_t t = from; t <= to; t += 1000) {
    switch (keeper.update(connected, t)) {
      case WifiAction::Retry: tally.retries++; break;
      case WifiAction::OpenSetup: tally.opens++; break;
      case WifiAction::CloseSetup: tally.closes++; break;
      case WifiAction::Restart: tally.restarts++; break;
      case WifiAction::None: break;
    }
  }
  return tally;
}

static int testWifiKeeper() {
  const uint32_t s = 1000, min = 60 * s;

  // Power cut: we boot with a saved network, the router is back after 2 min.
  // The saved network is retried every 30 s and the setup AP never opens.
  {
    WifiKeeper keeper(true, 0);
    WifiTally down = runKeeper(keeper, false, 0, 2 * min);
    CHECK(down.retries == 4);
    CHECK(down.opens == 0 && down.restarts == 0);
    CHECK(keeper.update(true, 2 * min + s) == WifiAction::None);
    CHECK(!keeper.setupOpen());
  }

  // Router gone for good at boot (or its password changed): after 10 min the
  // setup AP opens for 5 min, then closes and the retries resume — the cycle
  // repeats rather than the device parking itself in setup mode.
  {
    WifiKeeper keeper(true, 0);
    WifiTally first = runKeeper(keeper, false, 0, 10 * min - s);
    CHECK(first.retries == 19 && first.opens == 0);
    CHECK(keeper.update(false, 10 * min) == WifiAction::OpenSetup);
    CHECK(keeper.setupOpen());
    // No retries while the AP is up — WiFiManager switches the station off.
    WifiTally open = runKeeper(keeper, false, 10 * min + s, 15 * min - s);
    CHECK(open.retries == 0 && open.opens == 0 && open.closes == 0);
    CHECK(keeper.update(false, 15 * min) == WifiAction::CloseSetup);
    CHECK(!keeper.setupOpen());
    WifiTally again = runKeeper(keeper, false, 15 * min + s, 25 * min - s);
    CHECK(again.retries == 19 && again.opens == 0);
    CHECK(keeper.update(false, 25 * min) == WifiAction::OpenSetup);
  }

  // A new network entered in the setup AP: joining closes the window.
  {
    WifiKeeper keeper(true, 0);
    runKeeper(keeper, false, 0, 10 * min);
    CHECK(keeper.setupOpen());
    CHECK(keeper.update(true, 11 * min) == WifiAction::None);
    CHECK(!keeper.setupOpen());
  }

  // Already online this boot, then a short drop: retries only.
  {
    WifiKeeper keeper(true, 0);
    CHECK(keeper.update(true, s) == WifiAction::None);
    WifiTally drop = runKeeper(keeper, false, 2 * s, 3 * min);
    CHECK(drop.retries == 5 && drop.opens == 0 && drop.restarts == 0);
    CHECK(keeper.update(true, 3 * min + s) == WifiAction::None);
  }

  // Already online this boot, then offline for 10 min: our portal holds port
  // 80, so it asks for a restart instead of opening the setup AP.
  {
    WifiKeeper keeper(true, 0);
    CHECK(keeper.update(true, s) == WifiAction::None);
    WifiTally down = runKeeper(keeper, false, 2 * s, 10 * min);
    CHECK(down.opens == 0 && down.restarts == 0);
    CHECK(keeper.update(false, 10 * min + s) == WifiAction::Restart);
  }

  // Nothing saved (first install / after Forget Wi-Fi): WiFiManager's own
  // setup AP is already up and stays up; the keeper stays out of the way.
  {
    WifiKeeper keeper(false, 0);
    WifiTally idle = runKeeper(keeper, false, 0, 60 * min);
    CHECK(idle.retries == 0 && idle.opens == 0 && idle.closes == 0 && idle.restarts == 0);
    // ...until a network is entered there and joined; then drops are retried.
    CHECK(keeper.update(true, 61 * min) == WifiAction::None);
    WifiTally drop = runKeeper(keeper, false, 61 * min + s, 62 * min + s);
    CHECK(drop.retries == 2);
  }

  // millis() wraps after ~49 days; the intervals must survive it.
  {
    const uint32_t nearWrap = 0xFFFFFFFFu - 10 * s;
    WifiKeeper keeper(true, nearWrap);
    CHECK(keeper.update(false, nearWrap + 29 * s) == WifiAction::None);
    CHECK(keeper.update(false, nearWrap + 30 * s) == WifiAction::Retry);
  }
  return 0;
}

int main() {
  if (testCrypto()) return 1;
  if (testJson()) return 1;
  if (testProtocol()) return 1;
  if (testRunner()) return 1;
  if (testConfig()) return 1;
  if (testWifiKeeper()) return 1;
  std::printf("ok — %d checks passed\n", checks);
  return 0;
}
