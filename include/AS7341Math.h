#pragma once
#include <cstdint>

// Arduino-free helper calculations for the AS7341, factored out so they can
// be tested on the host via `pio test -e native`.

// ADC full-scale per the AS7341 datasheet: (ATIME+1)*(ASTEP+1) counts,
// capped by the 16-bit data register at 65535.
inline uint32_t adcFullScale(uint8_t atime, uint16_t astep) {
  uint32_t fs = (uint32_t)(atime + 1u) * (uint32_t)(astep + 1u);
  return fs < 65535u ? fs : 65535u;
}
