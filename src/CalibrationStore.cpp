#include "CalibrationStore.h"
#include "SettingsCodec.h"
#include <ArduinoJson.h>

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

// Gemeinsames Lese-/Schreibpaar fuer die Felder, die sowohl RootSettings als
// auch AcquisitionParameters heute haben (siehe AppConfig.h) -- damit die
// JSON-Form nicht zweimal gepflegt werden muss. Enum-Werte werden als Strings
// kodiert (siehe SettingsCodec.h), damit eine kuenftige Umsortierung/
// Erweiterung des Enums nicht stillschweigend die Bedeutung eines bereits
// gespeicherten Werts veraendert. Siehe schema/settings.schema.json fuer die
// dokumentierte Form.
static void writeAcquisitionFields(JsonDocument& doc, FilterState fs, const SensorSettings& sensor) {
  doc["filter"] = filterStateCsvLabel(fs);
  JsonObject sensorObj = doc["sensor"].to<JsonObject>();
  sensorObj["gain"]  = gainCsvLabel(sensor.gain);
  sensorObj["atime"] = sensor.atime;
  sensorObj["astep"] = sensor.astep;
}

// Feldweise robust: jedes fehlende/unbekannte Feld schlaegt fehl -- der
// Aufrufer faellt dann komplett auf den jeweiligen Struct-Default zurueck
// (kein Mischzustand aus altem/neuem Wert), gleiche Philosophie wie zuvor die
// binaere Groessen-Pruefung, jetzt nur feldweise statt "ganz oder gar nicht".
static bool readAcquisitionFields(JsonDocument& doc, FilterState& fs, SensorSettings& sensor) {
  bool ok = filterStateFromCsvLabel(doc["filter"] | "", fs);
  JsonObjectConst sensorObj = doc["sensor"];
  ok = ok && gainFromCsvLabel(sensorObj["gain"] | "", sensor.gain);
  if (ok && sensorObj["atime"].is<uint8_t>())  sensor.atime = sensorObj["atime"].as<uint8_t>();   else ok = false;
  if (ok && sensorObj["astep"].is<uint16_t>()) sensor.astep = sensorObj["astep"].as<uint16_t>();  else ok = false;
  return ok;
}

static String serializeRootSettings(const RootSettings& s) {
  JsonDocument doc;
  writeAcquisitionFields(doc, s.filterState, s.sensor);
  // kuenftig: weitere RootSettings-Felder hier ergaenzen
  String out;
  serializeJson(doc, out);
  return out;
}

static bool deserializeRootSettings(const String& json, RootSettings& out) {
  RootSettings result;
  JsonDocument doc;
  bool ok = (deserializeJson(doc, json) == DeserializationError::Ok);
  if (ok) ok = readAcquisitionFields(doc, result.filterState, result.sensor);
  out = ok ? result : RootSettings();
  return ok;
}

static String serializeAcquisitionParameters(const AcquisitionParameters& p) {
  JsonDocument doc;
  writeAcquisitionFields(doc, p.filterState, p.sensor);
  String out;
  serializeJson(doc, out);
  return out;
}

static bool deserializeAcquisitionParameters(const String& json, AcquisitionParameters& out) {
  AcquisitionParameters result;
  JsonDocument doc;
  bool ok = (deserializeJson(doc, json) == DeserializationError::Ok);
  if (ok) ok = readAcquisitionFields(doc, result.filterState, result.sensor);
  out = ok ? result : AcquisitionParameters();
  return ok;
}

// NEUE Key-Namen ("_json"-Suffix, bewusst anders als die frueheren binaeren
// Blob-Keys) -- alte Blob-Eintraege werden dadurch nie wieder gelesen
// (verwaiste, harmlose Bytes in NVS) statt als vermeintliches JSON
// missinterpretiert zu werden. Keine Migration: nach dem Flashen dieser
// Aenderung springen Einstellungen/Referenzen einmalig auf ihre Defaults
// zurueck, genau wie bei einem Erstboot.
bool CalibrationStore::loadDark(Measurement& out, AcquisitionParameters& params) {
  bool ok = load("dark_raw", out);
  if (ok) {
    String json = prefs_.getString("dark_settings_json", "");
    if (json.isEmpty() || !deserializeAcquisitionParameters(json, params)) params = AcquisitionParameters();
  }
  return ok;
}

bool CalibrationStore::loadWhite(Measurement& out, AcquisitionParameters& params) {
  bool ok = load("white_raw", out);
  if (ok) {
    String json = prefs_.getString("white_settings_json", "");
    if (json.isEmpty() || !deserializeAcquisitionParameters(json, params)) params = AcquisitionParameters();
  }
  return ok;
}

void CalibrationStore::saveDark(const Measurement& v, const AcquisitionParameters& params) {
  save("dark_raw", v);
  prefs_.putString("dark_settings_json", serializeAcquisitionParameters(params));
}

void CalibrationStore::saveWhite(const Measurement& v, const AcquisitionParameters& params) {
  save("white_raw", v);
  prefs_.putString("white_settings_json", serializeAcquisitionParameters(params));
}

bool CalibrationStore::loadSettings(RootSettings& out) {
  String json = prefs_.getString("live_settings_json", "");
  if (json.isEmpty()) { out = RootSettings(); return false; }
  return deserializeRootSettings(json, out);
}

void CalibrationStore::saveSettings(const RootSettings& v) {
  prefs_.putString("live_settings_json", serializeRootSettings(v));
}
