// Just enough Arduino to run the firmware on a laptop.
//
// This is test scaffolding, not a second implementation: src/*.cpp is compiled
// unchanged against these headers so the portal, the config store and the agent
// that run here are the ones that run on the board. What is faked is the
// hardware underneath — GPIO becomes a log line, Wi-Fi becomes a state machine,
// NVS becomes a JSON file.

#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#define PROGMEM
#define F(x) (x)
#define IRAM_ATTR

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2

/** Arduino's String, narrowed to what the firmware actually calls. */
class String {
 public:
  String() = default;
  String(const char* text) : value_(text ? text : "") {}
  String(const std::string& text) : value_(text) {}
  String(int number) : value_(std::to_string(number)) {}
  String(unsigned number) : value_(std::to_string(number)) {}

  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }
  bool isEmpty() const { return value_.empty(); }
  const std::string& str() const { return value_; }

  String operator+(const String& other) const { return String(value_ + other.value_); }
  String operator+(const char* other) const { return String(value_ + (other ? other : "")); }
  String& operator+=(const String& other) {
    value_ += other.value_;
    return *this;
  }
  bool operator==(const String& other) const { return value_ == other.value_; }

 private:
  std::string value_;
};

inline String operator+(const char* left, const String& right) {
  return String(std::string(left ? left : "") + right.str());
}

/** Milliseconds since the process started (the same contract as millis()). */
uint32_t millis();
void delay(uint32_t ms);

/** GPIO, recorded so tests can assert on what the switch did. */
void pinMode(int pin, int mode);
void digitalWrite(int pin, int level);
int digitalRead(int pin);

namespace hostgpio {
/** Every level change, in order: (millis, pin, level). */
struct Event {
  uint32_t at;
  int pin;
  int level;
};
const Event* events();
size_t eventCount();
void setInput(int pin, int level);
}  // namespace hostgpio

class SerialClass {
 public:
  void begin(unsigned long) {}
  void println() { std::printf("\n"); }
  void println(const char* text) { std::printf("%s\n", text); }
  void print(const char* text) { std::printf("%s", text); }
  void printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
  }
};
extern SerialClass Serial;

class EspClass {
 public:
  uint64_t getEfuseMac() const { return 0x0000AABBCCDDEEFFULL; }
  void restart();
};
extern EspClass ESP;
