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

static FilterState filterStateFromByte(uint8_t v) {
  return (v < static_cast<uint8_t>(FilterState::COUNT)) ? static_cast<FilterState>(v) : FilterState::None;
}

bool CalibrationStore::loadDark(Measurement& out, FilterState& filterState) {
  bool ok = load("dark_raw", out);
  if (ok) filterState = filterStateFromByte(prefs_.getUChar("dark_filt", 0));
  return ok;
}

bool CalibrationStore::loadWhite(Measurement& out, FilterState& filterState) {
  bool ok = load("white_raw", out);
  if (ok) filterState = filterStateFromByte(prefs_.getUChar("white_filt", 0));
  return ok;
}

void CalibrationStore::saveDark(const Measurement& v, FilterState filterState) {
  save("dark_raw", v);
  prefs_.putUChar("dark_filt", static_cast<uint8_t>(filterState));
}

void CalibrationStore::saveWhite(const Measurement& v, FilterState filterState) {
  save("white_raw", v);
  prefs_.putUChar("white_filt", static_cast<uint8_t>(filterState));
}

bool CalibrationStore::loadFilterState(FilterState& out) {
  if (!prefs_.isKey("filter_state")) { out = FilterState::None; return false; }
  out = filterStateFromByte(prefs_.getUChar("filter_state", 0));
  return true;
}

void CalibrationStore::saveFilterState(FilterState v) {
  prefs_.putUChar("filter_state", static_cast<uint8_t>(v));
}
