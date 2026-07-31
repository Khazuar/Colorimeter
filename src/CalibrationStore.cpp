#include "CalibrationStore.h"

void CalibrationStore::begin() {
  prefs_.begin("colorim", false);
}

bool CalibrationStore::load(const char* key, std::vector<uint32_t>& out) {
  size_t len = prefs_.getBytesLength(key);
  if (len == 0 || (len % sizeof(uint32_t)) != 0) { out.clear(); return false; }

  out.assign(len / sizeof(uint32_t), 0);
  size_t got = prefs_.getBytes(key, out.data(), len);
  if (got != len) { out.clear(); return false; }
  return true;
}

void CalibrationStore::save(const char* key, const std::vector<uint32_t>& v) {
  prefs_.putBytes(key, v.data(), v.size() * sizeof(uint32_t));
}

bool CalibrationStore::loadDark(std::vector<uint32_t>& out) {
  return load("dark_raw", out);
}

bool CalibrationStore::loadWhite(std::vector<uint32_t>& out) {
  return load("white_raw", out);
}

void CalibrationStore::saveDark(const std::vector<uint32_t>& v) {
  save("dark_raw", v);
}

void CalibrationStore::saveWhite(const std::vector<uint32_t>& v) {
  save("white_raw", v);
}
