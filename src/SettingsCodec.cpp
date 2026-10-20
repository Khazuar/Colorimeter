#include "SettingsCodec.h"
#include "AppConfig.h"
#include <cstring>

// Order == as7341_gain_t (see Adafruit_AS7341.h), verified.
static const char* const GAIN_LABELS[AS7341_GAIN_COUNT] = {
  "0.5X", "1X", "2X", "4X", "8X", "16X", "32X", "64X", "128X", "256X", "512X"
};

const char* gainCsvLabel(as7341_gain_t g) {
  uint8_t i = static_cast<uint8_t>(g);
  return (i < AS7341_GAIN_COUNT) ? GAIN_LABELS[i] : "?";
}

bool gainFromCsvLabel(const char* s, as7341_gain_t& out) {
  if (!s) return false;
  for (uint8_t i = 0; i < AS7341_GAIN_COUNT; i++) {
    if (strcmp(s, GAIN_LABELS[i]) == 0) { out = static_cast<as7341_gain_t>(i); return true; }
  }
  return false;
}
