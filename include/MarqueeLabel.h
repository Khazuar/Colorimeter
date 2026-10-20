#pragma once
#include <cstddef>
#include <cstdint>

// Timing of a "running label": a text too long for its display window is
// shown from its start for START_DELAY_MS, then moves one character every
// STEP_MS until its end is visible, rests there for END_HOLD_MS, jumps back
// to the start and immediately moves on again (the start delay only applies
// to the very first cycle). Pure function of elapsed time -- knows neither
// the display nor a clock, so it is testable on the host.
struct MarqueeTiming {
  static const uint32_t START_DELAY_MS = 2000;
  static const uint32_t END_HOLD_MS    = 2000;
  static const uint32_t STEP_MS        = 250;
};

// Number of characters to skip from the left of a text of 'textLen'
// characters shown in a window of 'window' characters, 'elapsedMs' after the
// label became selected. Always 0 if the text fits.
size_t marqueeOffset(size_t textLen, size_t window, uint32_t elapsedMs);
