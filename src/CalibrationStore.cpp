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

// Gemeinsames Lese-/Schreibpaar fuer die Felder von OpticalSettings (siehe
// AppConfig.h) -- genutzt sowohl fuer das "optical"-Unterobjekt von
// RootSettings als auch fuer ein eigenstaendiges OpticalSettings-Dokument
// (Dark-/Weiss-Referenzen) als auch fuer je ein "optical"-Unterobjekt pro
// Messspitzen-Katalog-Eintrag -- damit die JSON-Form nicht mehrfach gepflegt
// werden muss. Generisch auf JsonVariant/JsonVariantConst, damit sowohl ein
// ganzes JsonDocument als auch ein einzelnes JsonObject (Array-Element,
// Unterobjekt) als Ziel/Quelle dienen koennen. Enum-Werte werden als Strings
// kodiert (siehe SettingsCodec.h), damit eine kuenftige Umsortierung/
// Erweiterung des Enums nicht stillschweigend die Bedeutung eines bereits
// gespeicherten Werts veraendert. Siehe schema/settings.schema.json fuer die
// dokumentierte Form.
static void writeOpticalFields(JsonVariant target, FilterState fs, const SensorSettings& sensor) {
  target["filter"] = filterStateCsvLabel(fs);
  JsonObject sensorObj = target["sensor"].to<JsonObject>();
  sensorObj["gain"]  = gainCsvLabel(sensor.gain);
  sensorObj["atime"] = sensor.atime;
  sensorObj["astep"] = sensor.astep;
}

// Feldweise robust: jedes fehlende/unbekannte Feld schlaegt fehl -- der
// Aufrufer faellt dann komplett auf den jeweiligen Struct-Default zurueck
// (kein Mischzustand aus altem/neuem Wert), gleiche Philosophie wie zuvor die
// binaere Groessen-Pruefung, jetzt nur feldweise statt "ganz oder gar nicht".
static bool readOpticalFields(JsonVariantConst source, FilterState& fs, SensorSettings& sensor) {
  bool ok = filterStateFromCsvLabel(source["filter"] | "", fs);
  JsonObjectConst sensorObj = source["sensor"];
  ok = ok && gainFromCsvLabel(sensorObj["gain"] | "", sensor.gain);
  if (ok && sensorObj["atime"].is<uint8_t>())  sensor.atime = sensorObj["atime"].as<uint8_t>();   else ok = false;
  if (ok && sensorObj["astep"].is<uint16_t>()) sensor.astep = sensorObj["astep"].as<uint16_t>();  else ok = false;
  return ok;
}

static String serializeRootSettings(const RootSettings& s) {
  JsonDocument doc;
  writeOpticalFields(doc["optical"].to<JsonObject>(), s.optical.filterState, s.optical.sensor);
  // kuenftig: weitere RootSettings-Gruppen hier als weitere Top-Level-Keys ergaenzen
  String out;
  serializeJson(doc, out);
  return out;
}

static bool deserializeRootSettings(const String& json, RootSettings& out) {
  RootSettings result;
  JsonDocument doc;
  bool ok = (deserializeJson(doc, json) == DeserializationError::Ok);
  if (ok) ok = readOpticalFields(doc["optical"], result.optical.filterState, result.optical.sensor);
  out = ok ? result : RootSettings();
  return ok;
}

static String serializeOpticalSettings(const OpticalSettings& s) {
  JsonDocument doc;
  writeOpticalFields(doc.to<JsonObject>(), s.filterState, s.sensor);
  String out;
  serializeJson(doc, out);
  return out;
}

static bool deserializeOpticalSettings(const String& json, OpticalSettings& out) {
  OpticalSettings result;
  JsonDocument doc;
  bool ok = (deserializeJson(doc, json) == DeserializationError::Ok);
  if (ok) ok = readOpticalFields(doc.as<JsonVariantConst>(), result.filterState, result.sensor);
  out = ok ? result : OpticalSettings();
  return ok;
}

static String serializeTipCatalog(const TipCatalog& c) {
  JsonDocument doc;
  doc["active"] = c.active.c_str();
  JsonArray arr = doc["tips"].to<JsonArray>();
  for (const MeasurementTip& t : c.tips) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = t.name.c_str();
    writeOpticalFields(o["optical"].to<JsonObject>(), t.optical.filterState, t.optical.sensor);
  }
  String out;
  serializeJson(doc, out);
  return out;
}

// Ungueltig (-> false, Aufrufer faellt auf einen leeren Katalog zurueck), wenn
// JSON kaputt ist, ODER die resultierende Liste leer waere, ODER 'active'
// keinen der geladenen Eintraege trifft -- die Invariante "immer >=1 Spitze,
// 'active' immer gueltig" wird hier durchgesetzt, nicht erst beim Zugriff.
static bool deserializeTipCatalog(const String& json, TipCatalog& out) {
  TipCatalog result;
  JsonDocument doc;
  bool ok = (deserializeJson(doc, json) == DeserializationError::Ok);
  if (ok) {
    result.active = (const char*)(doc["active"] | "");
    for (JsonObject o : doc["tips"].as<JsonArray>()) {
      MeasurementTip t;
      t.name = (const char*)(o["name"] | "");
      if (t.name.empty() || !readOpticalFields(o["optical"], t.optical.filterState, t.optical.sensor)) {
        ok = false;
        break;
      }
      result.tips.push_back(t);
    }
  }
  ok = ok && !result.tips.empty() && result.find(result.active) != nullptr;
  out = ok ? result : TipCatalog();
  return ok;
}

// NEUE Key-Namen ("_json"-Suffix, bewusst anders als die frueheren binaeren
// Blob-Keys) -- alte Blob-Eintraege werden dadurch nie wieder gelesen
// (verwaiste, harmlose Bytes in NVS) statt als vermeintliches JSON
// missinterpretiert zu werden. Keine Migration: nach dem Flashen dieser
// Aenderung springen Einstellungen/Referenzen einmalig auf ihre Defaults
// zurueck, genau wie bei einem Erstboot.
bool CalibrationStore::loadDark(Measurement& out, OpticalSettings& optical) {
  bool ok = load("dark_raw", out);
  if (ok) {
    String json = prefs_.getString("dark_settings_json", "");
    if (json.isEmpty() || !deserializeOpticalSettings(json, optical)) optical = OpticalSettings();
  }
  return ok;
}

bool CalibrationStore::loadWhite(Measurement& out, OpticalSettings& optical) {
  bool ok = load("white_raw", out);
  if (ok) {
    String json = prefs_.getString("white_settings_json", "");
    if (json.isEmpty() || !deserializeOpticalSettings(json, optical)) optical = OpticalSettings();
  }
  return ok;
}

void CalibrationStore::saveDark(const Measurement& v, const OpticalSettings& optical) {
  save("dark_raw", v);
  prefs_.putString("dark_settings_json", serializeOpticalSettings(optical));
}

void CalibrationStore::saveWhite(const Measurement& v, const OpticalSettings& optical) {
  save("white_raw", v);
  prefs_.putString("white_settings_json", serializeOpticalSettings(optical));
}

bool CalibrationStore::loadSettings(RootSettings& out) {
  String json = prefs_.getString("live_settings_json", "");
  if (json.isEmpty()) { out = RootSettings(); return false; }
  return deserializeRootSettings(json, out);
}

void CalibrationStore::saveSettings(const RootSettings& v) {
  prefs_.putString("live_settings_json", serializeRootSettings(v));
}

bool CalibrationStore::loadTips(TipCatalog& out) {
  String json = prefs_.getString("tips_json", "");
  if (json.isEmpty()) { out = TipCatalog(); return false; }
  return deserializeTipCatalog(json, out);
}

void CalibrationStore::saveTips(const TipCatalog& v) {
  prefs_.putString("tips_json", serializeTipCatalog(v));
}
