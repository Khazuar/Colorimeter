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

// AcquisitionSettings ist ein kleines, plain-POD-Struct -- als EIN Blob pro
// Schluessel gespeichert statt einzelner NVS-Keys je Feld. Einfacher und
// automatisch erweiterbar, falls das Buendel kuenftig noch waechst. Passt der
// gespeicherte Blob nicht zur aktuellen Struct-Groesse (z.B. nach einem
// Firmware-Update, das das Buendel veraendert hat), fallen die Einstellungen
// auf ihre Default-Werte zurueck statt Muell zu liefern.
static bool loadSettingsBlob(Preferences& prefs, const char* key, AcquisitionSettings& out) {
  size_t got = prefs.getBytes(key, &out, sizeof(out));
  if (got != sizeof(out)) { out = AcquisitionSettings(); return false; }
  return true;
}

bool CalibrationStore::loadDark(Measurement& out, AcquisitionSettings& settings) {
  bool ok = load("dark_raw", out);
  if (ok) loadSettingsBlob(prefs_, "dark_settings", settings);
  return ok;
}

bool CalibrationStore::loadWhite(Measurement& out, AcquisitionSettings& settings) {
  bool ok = load("white_raw", out);
  if (ok) loadSettingsBlob(prefs_, "white_settings", settings);
  return ok;
}

void CalibrationStore::saveDark(const Measurement& v, const AcquisitionSettings& settings) {
  save("dark_raw", v);
  prefs_.putBytes("dark_settings", &settings, sizeof(settings));
}

void CalibrationStore::saveWhite(const Measurement& v, const AcquisitionSettings& settings) {
  save("white_raw", v);
  prefs_.putBytes("white_settings", &settings, sizeof(settings));
}

bool CalibrationStore::loadSettings(AcquisitionSettings& out) {
  if (!prefs_.isKey("live_settings")) { out = AcquisitionSettings(); return false; }
  return loadSettingsBlob(prefs_, "live_settings", out);
}

void CalibrationStore::saveSettings(const AcquisitionSettings& v) {
  prefs_.putBytes("live_settings", &v, sizeof(v));
}
