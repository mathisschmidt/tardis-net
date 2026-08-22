// The pulse/ack state machine, with the clock and the switch injected.
//
// Keeping this free of Arduino calls means the host tests drive it with a fake
// clock and a fake switch and assert on exactly what the relay would do — the
// part where a bug leaves a machine's power button held down.
//
//   Idle --accept()--> Pulsing --(pulseMs elapsed)--> AckPending
//        <--ackDone()-- AckPending
//
// One command at a time, always. A second accept() while busy is refused, so a
// duplicate delivery from the server can never stack two pulses.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "protocol.h"

namespace tardis {

enum class RunnerState { Idle, Pulsing, AckPending };

class CommandRunner {
 public:
  // switchWrite(true) closes the contact (button pressed), false releases it.
  using SwitchFn = std::function<void(bool)>;

  explicit CommandRunner(SwitchFn switchWrite) : switchWrite_(std::move(switchWrite)) {}

  RunnerState state() const { return state_; }
  const Command& current() const { return command_; }
  bool busy() const { return state_ != RunnerState::Idle; }

  /** Take a command and start pulsing. Returns false when already busy. */
  bool accept(const Command& command, uint32_t nowMs) {
    if (state_ != RunnerState::Idle || !command.valid()) return false;
    command_ = command;
    startedMs_ = nowMs;
    state_ = RunnerState::Pulsing;
    switchWrite_(true);
    return true;
  }

  /** Advance the pulse. Returns true on the tick the pulse just ended. */
  bool update(uint32_t nowMs) {
    if (state_ != RunnerState::Pulsing) return false;
    // Unsigned subtraction, so a millis() rollover after 49 days is harmless.
    if (nowMs - startedMs_ < command_.pulseMs) return false;
    switchWrite_(false);
    state_ = RunnerState::AckPending;
    return true;
  }

  /** Called once the ack for the current command has been delivered (or given
   *  up on) — the runner is free again. */
  void ackDone() {
    if (state_ != RunnerState::AckPending) return;
    state_ = RunnerState::Idle;
    command_ = Command{};
  }

  /** Release the switch and forget everything — used on a fault or reboot. */
  void abort() {
    switchWrite_(false);
    state_ = RunnerState::Idle;
    command_ = Command{};
  }

  uint32_t elapsedMs(uint32_t nowMs) const {
    return state_ == RunnerState::Pulsing ? nowMs - startedMs_ : 0;
  }

 private:
  SwitchFn switchWrite_;
  RunnerState state_ = RunnerState::Idle;
  Command command_;
  uint32_t startedMs_ = 0;
};

}  // namespace tardis
