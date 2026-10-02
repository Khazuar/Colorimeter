#pragma once
#include <Arduino.h>
#include <cstdint>

// Pure polling, no interrupts. Call poll() on every loop() iteration.
class DebouncedButton {
public:
  // LongPress already fires while the button is being held, as soon as
  // longPressMs is reached (not only on release) -- exactly once per press,
  // even if the button continues to be held afterwards. ShortRelease fires,
  // as before, only on release, but only if the long-press threshold was not
  // already exceeded during the hold (otherwise the action was already
  // triggered via LongPress, and the subsequent release then reports None).
  enum class Event : uint8_t { None, Pressed, ShortRelease, LongPress };

  explicit DebouncedButton(uint8_t pin, uint32_t debounceMs = 40, uint32_t longPressMs = 600)
    : pin_(pin), debounceMs_(debounceMs), longPressMs_(longPressMs) {}

  void begin() { pinMode(pin_, INPUT_PULLUP); }

  Event poll() {
    bool raw = (digitalRead(pin_) == LOW);  // Pullup + button against GND: LOW = pressed
    uint32_t now = millis();

    if (raw != rawPressed_) { lastChangeMs_ = now; rawPressed_ = raw; }

    if ((now - lastChangeMs_) >= debounceMs_ && raw != stablePressed_) {
      stablePressed_ = raw;
      if (stablePressed_) {
        pressStartMs_ = now;
        longFired_ = false;
        return Event::Pressed;
      }
      return longFired_ ? Event::None : Event::ShortRelease;
    }

    if (stablePressed_ && !longFired_ && (now - pressStartMs_) >= longPressMs_) {
      longFired_ = true;
      return Event::LongPress;
    }

    return Event::None;
  }

private:
  uint8_t pin_;
  uint32_t debounceMs_, longPressMs_;
  bool rawPressed_ = false, stablePressed_ = false;
  bool longFired_ = false;
  uint32_t lastChangeMs_ = 0, pressStartMs_ = 0;
};
