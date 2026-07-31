#pragma once
#include <Arduino.h>
#include <cstdint>

// Reines Polling, keine Interrupts. poll() jeden loop()-Durchlauf aufrufen.
class DebouncedButton {
public:
  // LongPress feuert bereits waehrend des Haltens, sobald longPressMs erreicht
  // ist (nicht erst beim Loslassen) -- genau einmal pro Druckvorgang, auch
  // wenn der Taster danach weiter gehalten wird. ShortRelease feuert wie
  // bisher erst beim Loslassen, aber nur falls die Lang-Druck-Schwelle waehrend
  // des Haltens nicht schon ueberschritten wurde (sonst wurde die Aktion
  // bereits per LongPress ausgeloest, das anschliessende Loslassen meldet
  // dann None).
  enum class Event : uint8_t { None, Pressed, ShortRelease, LongPress };

  explicit DebouncedButton(uint8_t pin, uint32_t debounceMs = 40, uint32_t longPressMs = 600)
    : pin_(pin), debounceMs_(debounceMs), longPressMs_(longPressMs) {}

  void begin() { pinMode(pin_, INPUT_PULLUP); }

  Event poll() {
    bool raw = (digitalRead(pin_) == LOW);  // Pullup + Taster gegen GND: LOW = gedrueckt
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
