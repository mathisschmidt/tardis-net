// The tardis-net wire protocol, from the device's side.
//
// Mirrors server-app/app/core/machine.py. Two calls, both authenticated with
// the pairing key in `X-Tardis-Key` — never a session cookie:
//
//   POST /api/hardware/poll  {firmware, ip, rssi, uptime_s,
//                              power_sense: "on"|"off"|"unknown"}
//     -> {server_time, poll_interval, machine:{...}, command: null | {
//           id, action, pulse_ms, requested_at, expires_in}}
//
// `power_sense` is always present and is the server's *only* source of truth
// for the machine's actual power state — a device with no sense pin fitted
// reports "unknown" on every poll rather than omitting the field, and an ack
// never moves the state on its own (see the docstring on MachineController in
// machine.py).
//
//   POST /api/hardware/ack   {id, status: "completed"|"failed", detail?}
//
// The server repeats an unacked command on every poll, so a dropped response
// costs one cycle and nothing more. Pulse lengths come from the server, but are
// clamped here as well: this file is the last thing between a malformed number
// and a relay held closed.
//
// Portable C++17 — no Arduino headers — so the host tests cover it directly.

#pragma once

#include <cstdint>
#include <string>

#include "json_lite.h"

namespace tardis {

inline constexpr const char* kFirmwareVersion = "1.0.0";
inline constexpr const char* kPollPath = "/api/hardware/poll";
inline constexpr const char* kAckPath = "/api/hardware/ack";
inline constexpr const char* kKeyHeader = "X-Tardis-Key";

// Nothing may hold the switch longer than this, whatever the server asks for.
inline constexpr uint32_t kMaxPulseMs = 10000;
inline constexpr uint32_t kMinPulseMs = 50;

inline constexpr uint32_t kMinPollSeconds = 1;
inline constexpr uint32_t kMaxPollSeconds = 3600;

enum class Action { None, PowerOn, GracefulShutdown, HardPowerOff };

/** Expected hold time for each action, per the table in the READMEs. */
inline uint32_t expectedPulseMs(Action action) {
  switch (action) {
    case Action::PowerOn: return 500;
    case Action::GracefulShutdown: return 500;
    case Action::HardPowerOff: return 5000;
    default: return 0;
  }
}

inline Action actionFromString(const std::string& name) {
  if (name == "power_on") return Action::PowerOn;
  if (name == "graceful_shutdown") return Action::GracefulShutdown;
  if (name == "hard_power_off") return Action::HardPowerOff;
  return Action::None;
}

inline const char* actionName(Action action) {
  switch (action) {
    case Action::PowerOn: return "power_on";
    case Action::GracefulShutdown: return "graceful_shutdown";
    case Action::HardPowerOff: return "hard_power_off";
    default: return "none";
  }
}

struct Command {
  std::string id;
  Action action = Action::None;
  uint32_t pulseMs = 0;

  bool valid() const { return !id.empty() && action != Action::None && pulseMs > 0; }
};

struct PollResponse {
  bool ok = false;              // document parsed and looked like a poll response
  uint32_t pollSeconds = 0;     // 0 when the server did not say
  std::string machineStatus;    // "offline" | "booting" | "online" | "shutting_down"
  Command command;              // command.valid() == false when there is nothing to do
};

/** Clamp a server-supplied pulse into what the hardware is willing to do. */
inline uint32_t clampPulseMs(long requested) {
  if (requested < static_cast<long>(kMinPulseMs)) return kMinPulseMs;
  if (requested > static_cast<long>(kMaxPulseMs)) return kMaxPulseMs;
  return static_cast<uint32_t>(requested);
}

inline uint32_t clampPollSeconds(long requested) {
  if (requested < static_cast<long>(kMinPollSeconds)) return kMinPollSeconds;
  if (requested > static_cast<long>(kMaxPollSeconds)) return kMaxPollSeconds;
  return static_cast<uint32_t>(requested);
}

/** Parse a poll response body. Returns ok=false for anything unexpected. */
inline PollResponse parsePollResponse(const std::string& body) {
  PollResponse result;
  jsonlite::Document doc;
  if (!doc.parse(body) || doc.root().type() != jsonlite::Type::Object) return result;

  result.ok = true;
  result.machineStatus = doc["machine"]["status"].asString();

  auto interval = doc["poll_interval"];
  if (interval.type() == jsonlite::Type::Number) {
    result.pollSeconds = clampPollSeconds(interval.asLong());
  }

  auto command = doc["command"];
  if (command.type() != jsonlite::Type::Object) return result;  // null: nothing to do

  Command parsed;
  parsed.id = command["id"].asString();
  parsed.action = actionFromString(command["action"].asString());
  if (parsed.action == Action::None || parsed.id.empty()) return result;

  auto pulse = command["pulse_ms"];
  // A command without a usable pulse falls back to the documented duration
  // rather than being dropped — the action itself is unambiguous.
  parsed.pulseMs = pulse.type() == jsonlite::Type::Number ? clampPulseMs(pulse.asLong())
                                                          : expectedPulseMs(parsed.action);
  result.command = parsed;
  return result;
}

/** Body for a poll. Fields the device cannot measure are simply left out. */
struct DeviceReport {
  std::string ip;
  int rssi = 0;             // 0 means "not connected / unknown"
  uint32_t uptimeSeconds = 0;
  int powerSense = -1;      // -1 unknown, 0 off, 1 on (only with a sense pin)
};

inline std::string buildPollBody(const DeviceReport& report) {
  std::string body = "{\"firmware\":\"";
  body += jsonlite::escape(kFirmwareVersion);
  body += "\"";
  if (!report.ip.empty()) {
    body += ",\"ip\":\"" + jsonlite::escape(report.ip) + "\"";
  }
  if (report.rssi < 0) {
    body += ",\"rssi\":" + std::to_string(report.rssi);
  }
  body += ",\"uptime_s\":" + std::to_string(report.uptimeSeconds);
  // Always sent, and always trusted over anything the server might otherwise
  // infer from a command ack — "unknown" is the honest report from a device
  // with no sense pin wired up, not an omission.
  body += std::string(",\"power_sense\":\"") +
          (report.powerSense < 0 ? "unknown" : (report.powerSense ? "on" : "off")) + "\"";
  body += "}";
  return body;
}

inline std::string buildAckBody(const std::string& id, bool completed,
                                const std::string& detail = "") {
  std::string body = "{\"id\":\"" + jsonlite::escape(id) + "\",\"status\":\"";
  body += completed ? "completed" : "failed";
  body += "\"";
  if (!detail.empty()) {
    body += ",\"detail\":\"" + jsonlite::escape(detail.substr(0, 200)) + "\"";
  }
  body += "}";
  return body;
}

}  // namespace tardis
