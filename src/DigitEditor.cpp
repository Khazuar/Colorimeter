#include "DigitEditor.h"

void DigitEditor::begin(uint8_t digitCount, const uint8_t* cycleLen, uint32_t initialValue) {
  digitCount_ = (digitCount > MAX_DIGITS) ? MAX_DIGITS : digitCount;
  if (digitCount_ == 0) digitCount_ = 1;

  for (uint8_t i = 0; i < digitCount_; i++) cycleLen_[i] = cycleLen[i];

  if (digitCount_ == 1) {
    // Single-digit (enum-like): the "digit" is the value directly, not a
    // decimal digit -- cycleLen[0] may be > 9 (e.g. 11 gain levels).
    digits_[0] = (uint8_t)initialValue;
  } else {
    // Multi-digit: genuine decimal decomposition, each position is a digit
    // 0-9 (cycleLen[i]==10), except possibly the leading position (more
    // tightly limited by the value range -- given by the caller in
    // cycleLen[0]).
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
  if (cursor_ + 1 >= digitCount_) return true;  // was already on the last digit -- done
  cursor_++;
  return false;
}

bool DigitEditor::retreatDigit() {
  if (cursor_ == 0) return true;  // was already on the first digit -- done
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
