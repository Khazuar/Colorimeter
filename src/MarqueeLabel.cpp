#include "MarqueeLabel.h"

size_t marqueeOffset(size_t textLen, size_t window, uint32_t elapsedMs) {
  if (window == 0 || textLen <= window) return 0;
  if (elapsedMs < MarqueeTiming::START_DELAY_MS) return 0;

  const size_t maxOffset = textLen - window;
  const uint32_t cycleMs = (uint32_t)maxOffset * MarqueeTiming::STEP_MS + MarqueeTiming::END_HOLD_MS;
  const uint32_t t = (elapsedMs - MarqueeTiming::START_DELAY_MS) % cycleMs;
  const size_t offset = t / MarqueeTiming::STEP_MS;
  return offset < maxOffset ? offset : maxOffset;
}
