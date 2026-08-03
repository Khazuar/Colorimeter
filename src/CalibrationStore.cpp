#include "CalibrationStore.h"

void CalibrationStore::begin() {
  prefs_.begin("colorim", false);
}

bool CalibrationStore::load(const char* key, Measurement& out) {
  size_t len = prefs_.getBytesLength(key);
  if (len == 0 || (len % sizeof(float)) != 0) { out.clear(); return false; }

  out.assign(len / sizeof(float), 0.0f);
  size_t got = prefs_.getBytes(key, out.data(), len);
  if (got != len) { out.clear(); return false; }
  return true;
}

void CalibrationStore::save(const char* key, const Measurement& v) {
  prefs_.putBytes(key, v.data(), v.size() * sizeof(float));
}

bool CalibrationStore::loadDark(Measurement& out) {
  return load("dark_raw", out);
}

bool CalibrationStore::loadWhite(Measurement& out) {
  return load("white_raw", out);
}

void CalibrationStore::saveDark(const Measurement& v) {
  save("dark_raw", v);
}

void CalibrationStore::saveWhite(const Measurement& v) {
  save("white_raw", v);
}
