#include "DigitEditor.h"

void DigitEditor::begin(uint8_t digitCount, const uint8_t* cycleLen, uint32_t initialValue) {
  digitCount_ = (digitCount > MAX_DIGITS) ? MAX_DIGITS : digitCount;
  if (digitCount_ == 0) digitCount_ = 1;

  for (uint8_t i = 0; i < digitCount_; i++) cycleLen_[i] = cycleLen[i];

  if (digitCount_ == 1) {
    // Einstellig (enum-artig): die "Ziffer" ist der Wert direkt, kein
    // Dezimal-Digit -- cycleLen[0] darf > 9 sein (z.B. 11 Gain-Stufen).
    digits_[0] = (uint8_t)initialValue;
  } else {
    // Mehrstellig: echte Dezimalzerlegung, jede Position ist ein Digit 0-9
    // (cycleLen[i]==10), ausser ggf. der fuehrenden Position (enger begrenzt
    // durch den Wertebereich -- vom Aufrufer in cycleLen[0] vorgegeben).
    uint32_t remaining = initialValue;
    for (int8_t pos = (int8_t)digitCount_ - 1; pos >= 0; pos--) {
      digits_[pos] = (uint8_t)(remaining % 10);
      remaining /= 10;
    }
  }
  cursor_ = 0;
}

void DigitEditor::incrementCurrentDigit() {
  uint8_t len = cycleLen_[cursor_];
  if (len == 0) return;
  digits_[cursor_] = (uint8_t)((digits_[cursor_] + 1) % len);
}

void DigitEditor::decrementCurrentDigit() {
  uint8_t len = cycleLen_[cursor_];
  if (len == 0) return;
  digits_[cursor_] = (uint8_t)((digits_[cursor_] + len - 1) % len);
}

bool DigitEditor::advanceDigit() {
  if (cursor_ + 1 >= digitCount_) return true;  // war schon auf der letzten Ziffer -- fertig
  cursor_++;
  return false;
}

bool DigitEditor::retreatDigit() {
  if (cursor_ == 0) return true;  // war schon auf der ersten Ziffer -- fertig
  cursor_--;
  return false;
}

uint32_t DigitEditor::assembledValue(uint32_t maxValue) const {
  uint32_t v;
  if (digitCount_ == 1) {
    v = digits_[0];
  } else {
    v = 0;
    for (uint8_t i = 0; i < digitCount_; i++) v = v * 10 + digits_[i];
  }
  return (v > maxValue) ? maxValue : v;
}
