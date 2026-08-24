// The half of the firmware that talks to tardis-net: poll, pulse, ack.

#pragma once

#include <Arduino.h>

#include <string>

#include "config_store.h"
#include "protocol.h"
#include "runner.h"

namespace tardis {

/** What the portal shows about the link, and what the serial log prints. */
struct AgentStatus {
  bool everPolled = false;
  bool linked = false;              // last poll succeeded
  int lastHttpStatus = 0;
  uint32_t lastPollMs = 0;
  uint32_t lastSuccessMs = 0;
  uint32_t pollSeconds = 5;         // effective cadence (server's, else config)
  uint32_t commandsOk = 0;
  uint32_t commandsFailed = 0;
  std::string machineStatus;
  std::string lastError;
  std::string lastAction;           // last command acted on, for the portal
};

class Agent {
 public:
  explicit Agent(ConfigStore& store);

  /** Put the switch in its released state before anything else can drive it. */
  void begin();

  /** Called from loop(); never blocks longer than one HTTP request. */
  void update(bool networkReady);

  /** Re-read pins and cadence after the operator saves new config. */
  void reconfigure();

  const AgentStatus& status() const { return status_; }
  bool busy() const { return runner_.busy(); }

  /** One poll right now, for the portal's "test connection" button.
   *
   *  ``url``/``key`` are what the operator currently has typed in the form; an
   *  empty one falls back to what is stored, so Test works before the first
   *  save — the same rule the Wi-Fi test follows. */
  bool testConnection(const std::string& url, const std::string& key, std::string& message);
  bool testConnection(std::string& message) { return testConnection("", "", message); }

 private:
  ConfigStore& store_;
  CommandRunner runner_;
  AgentStatus status_;
  uint32_t nextPollMs_ = 0;
  uint32_t nextAckMs_ = 0;
  uint32_t failures_ = 0;
  bool ackPendingCompleted_ = true;
  std::string ackPendingDetail_;

  void applySwitch(bool closed);
  int readPowerSense() const;
  void poll();
  void sendAck();
  /** Empty url/key mean "use the stored configuration". */
  int request(const char* path, const std::string& body, std::string& response,
              const std::string& url = "", const std::string& key = "");
  void scheduleNextPoll(bool failed);
};

}  // namespace tardis
