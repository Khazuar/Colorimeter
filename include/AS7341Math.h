#pragma once
#include <cstdint>

// Arduino-freie Hilfsrechnungen zum AS7341, ausgelagert, damit sie per
// `pio test -e native` auf dem Host testbar sind.

// ADC-Vollausschlag laut AS7341-Datenblatt: (ATIME+1)*(ASTEP+1) Counts,
// begrenzt durch das 16-Bit-Datenregister auf 65535.
inline uint32_t adcFullScale(uint8_t atime, uint16_t astep) {
  uint32_t fs = (uint32_t)(atime + 1u) * (uint32_t)(astep + 1u);
  return fs < 65535u ? fs : 65535u;
}
