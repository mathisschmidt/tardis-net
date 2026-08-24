// The device's configuration UI and the small JSON API behind it.
//
// Auth mirrors the console: the first person to reach the portal sets the
// password (the device is "claimed"), and every visit after that asks for it.
// A signed-in browser holds a random session token in a cookie; five bad
// passwords lock the form for five minutes.

#pragma once

#include <WebServer.h>

#include <string>

#include "agent.h"
#include "config_store.h"

namespace tardis {

class Portal {
 public:
  Portal(ConfigStore& store, Agent& agent);

  void begin();
  void handle() { server_.handleClient(); }

  /** Set by main once the reboot request has been answered. */
  bool rebootRequested() const { return rebootAt_ != 0 && millis() >= rebootAt_; }

 private:
  ConfigStore& store_;
  Agent& agent_;
  WebServer server_;

  std::string sessionToken_;
  uint32_t sessionExpiresMs_ = 0;
  uint32_t failedAttempts_ = 0;
  uint32_t lockedUntilMs_ = 0;
  uint32_t rebootAt_ = 0;

  // routes
  void handleIndex();
  void handleStatus();
  void handleClaim();
  void handleLogin();
  void handleLogout();
  void handleGetConfig();
  void handleSaveConfig();
  void handleTest();
  void handleWifiScan();
  void handleWifiTest();
  void handleReboot();
  void handleNotFound();

  // helpers
  bool authed();
  void startSession();
  void endSession();
  bool lockedOut() const;
  uint32_t lockRemaining() const;
  void registerFailure();

  std::string body();
  void sendJson(int code, const std::string& json);
  void sendError(int code, const std::string& message);
};

}  // namespace tardis
