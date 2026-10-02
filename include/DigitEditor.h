#pragma once
#include <cstdint>

// Generic, reusable digit editor for a UI with only two buttons (short/long
// press per button) -- see main.cpp Settings mode for the concrete button
// mapping (Trigger short/long, Mode short/long). Pure state machine: knows
// neither buttons nor display nor the concrete setting it is currently
// editing.
//
// Represents both genuine multi-digit decimal numbers (each digit cycling
// 0..cycleLen[i]-1, usually 10, the leading digit possibly more tightly
// limited by the value range) as well as "single-digit" enum-like values
// (digitCount=1, cycleLen[0]=number of possible values) -- for the latter,
// "the digit" is simply the option index, not a decimal digit (and may
// therefore also exceed 9).
class DigitEditor {
public:
  static const uint8_t MAX_DIGITS = 5;  // enough for uint16_t (65535)

  // Initializes digitCount digits. For digitCount>1, initialValue is
  // decomposed into decimal digits (including leading zeros, index 0 =
  // leftmost/most significant). For digitCount==1, initialValue is taken
  // directly as the single "digit" (not a decimal digit). Cursor starts
  // at position 0.
  void begin(uint8_t digitCount, const uint8_t* cycleLen, uint32_t initialValue);

  void incrementCurrentDigit();  // (digit+1) % cycleLen[cursor]
  void decrementCurrentDigit();  // (digit+cycleLen[cursor]-1) % cycleLen[cursor]

  // Moves the cursor one position forward/back. Returns true if the cursor
  // was already on the last/first position (= editing finished) -- in that
  // case it does NOT move further.
  bool advanceDigit();
  bool retreatDigit();

  uint8_t digitCount() const { return digitCount_; }
  uint8_t digitAt(uint8_t i) const { return (i < digitCount_) ? digits_[i] : 0; }
  uint8_t cursor() const { return cursor_; }

  // Digits assembled into a decimal number (digitCount()==1: digitAt(0)
  // directly), clamped to maxValue -- covers the case where a more tightly
  // limited leading digit alone is not sufficient (e.g. 6-9-9-9-9=69999 with
  // a leading digit of 0..6 for a uint16_t value range).
  uint32_t assembledValue(uint32_t maxValue) const;

private:
  uint8_t digitCount_ = 0;
  uint8_t cursor_ = 0;
  uint8_t cycleLen_[MAX_DIGITS] = {0};
  uint8_t digits_[MAX_DIGITS] = {0};
};
