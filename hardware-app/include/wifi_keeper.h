// When to retry the saved Wi-Fi network, and when (if ever) to fall back to
// the setup network — with the clock injected, so the host tests can play out
// a power cut where the router comes back minutes after the ESP32 does.
//
// Left to itself, WiFiManager tries the saved network exactly once at boot and,
// if that fails, opens its setup AP with no timeout and the station switched
// off — so a device that booted faster than the router never rejoins it. The
// keeper replaces that fallback:
//
//   no saved network   setup AP stays open (first install, or after "Forget
//                      Wi-Fi") — nothing else to do until someone fills it in.
//   saved network      keep retrying it every retryEveryMs. Only after
//                      setupAfterMs offline does the setup AP open, and only
//                      for setupWindowMs (so a changed router password can be
//                      entered), then it closes and the retries resume.
//                      If the device had already been online this boot, its
//                      own portal holds port 80, which the setup AP's web
//                      server needs too — so it asks for a restart instead,
//                      and the next boot takes the path above.

#pragma once

#include <cstdint>

namespace tardis {

enum class WifiAction {
  None,
  Retry,       // join the saved network again
  OpenSetup,   // open WiFiManager's setup AP
  CloseSetup,  // close it again, then retry the saved network
  Restart,     // reboot (the setup AP cannot share port 80 with our portal)
};

struct WifiTiming {
  uint32_t retryEveryMs = 30u * 1000u;
  uint32_t setupAfterMs = 10u * 60u * 1000u;
  uint32_t setupWindowMs = 5u * 60u * 1000u;
};

class WifiKeeper {
 public:
  WifiKeeper(bool credentialsSaved, uint32_t nowMs, WifiTiming timing = WifiTiming())
      : timing_(timing), saved_(credentialsSaved), offlineSince_(nowMs), lastAttempt_(nowMs) {}

  bool setupOpen() const { return setupOpen_; }

  /** Call every loop with the station's state; do what it returns. */
  WifiAction update(bool connected, uint32_t nowMs) {
    if (connected) {
      // Joining at all means there is a network saved now, even if there was
      // none at boot (it was just entered in the setup AP).
      saved_ = true;
      everConnected_ = true;
      setupOpen_ = false;  // WiFiManager closes its AP itself after a save
      offlineSince_ = nowMs;
      lastAttempt_ = nowMs;
      return WifiAction::None;
    }
    if (!saved_) return WifiAction::None;

    if (setupOpen_) {
      if (nowMs - setupOpenedAt_ < timing_.setupWindowMs) return WifiAction::None;
      setupOpen_ = false;
      offlineSince_ = nowMs;
      lastAttempt_ = nowMs;
      return WifiAction::CloseSetup;
    }

    if (nowMs - offlineSince_ >= timing_.setupAfterMs) {
      if (everConnected_) return WifiAction::Restart;
      setupOpen_ = true;
      setupOpenedAt_ = nowMs;
      return WifiAction::OpenSetup;
    }

    if (nowMs - lastAttempt_ >= timing_.retryEveryMs) {
      lastAttempt_ = nowMs;
      return WifiAction::Retry;
    }
    return WifiAction::None;
  }

 private:
  WifiTiming timing_;
  bool saved_;
  bool everConnected_ = false;
  bool setupOpen_ = false;
  uint32_t offlineSince_;
  uint32_t lastAttempt_;
  uint32_t setupOpenedAt_ = 0;
};

}  // namespace tardis
