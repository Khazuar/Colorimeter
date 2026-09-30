#include "CalibrationStore.h"
#include "SettingsCodec.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// Der Messspitzen-Katalog (siehe unten) liegt bewusst auf LittleFS statt in
// den NVS-"Preferences" wie die uebrigen Werte: die "nvs"-Partition ist laut
// partitions.csv nur 20 KB gross und wird bereits von live/dark/white_settings
// geteilt -- mit Fingerabdruecken (siehe TipCatalog.h::WhiteFingerprint,
// bis zu MAX_WHITE_FINGERPRINTS_PER_TIP je Spitze) waechst der Katalog leicht
// auf mehrere KB pro Spitze. LittleFS (die "spiffs"-Partition, 1.4 MB) traegt
// bereits die mindestens ebenso wichtige Messhistorie (HistoryStore) und ist
// genauso stromausfall-sicher -- kein neues Risiko, nur eine passendere
// Partition fuer wachsende Daten.
static const char* TIPS_PATH = "/tips.json";

void CalibrationStore::begin() {
  prefs_.begin("colorim", false);
  LittleFS.begin(/*formatOnFail=*/true);  // idempotent, siehe HistoryStore::begin()
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

// Gemeinsames Lese-/Schreibpaar fuer die drei Felder von SensorSettings --
// genutzt vom "sensor"-Unterobjekt in writeOpticalFields()/readOpticalFields()
// UND direkt fuer die eingefrorenen SensorSettings in einem WhiteFingerprint
// (siehe TipCatalog.h), damit die JSON-Form nicht mehrfach gepflegt werden
// muss. Generisch auf JsonVariant/JsonVariantConst, damit sowohl ein ganzes
// JsonDocument als auch ein einzelnes JsonObject (Array-Element, Unterobjekt)
// als Ziel/Quelle dienen koennen. Enum-Werte werden als Strings kodiert
// (siehe SettingsCodec.h), damit eine kuenftige Umsortierung/Erweiterung des
// Enums nicht stillschweigend die Bedeutung eines bereits gespeicherten
// Werts veraendert.
static void writeSensorFields(JsonVariant target, const SensorSettings& sensor) {
  target["gain"]  = gainCsvLabel(sensor.gain);
  target["atime"] = sensor.atime;
  target["astep"] = sensor.astep;
}

static bool readSensorFields(JsonVariantConst source, SensorSettings& sensor) {
  bool ok = gainFromCsvLabel(source["gain"] | "", sensor.gain);
  if (ok && source["atime"].is<uint8_t>())  sensor.atime = source["atime"].as<uint8_t>();   else ok = false;
  if (ok && source["astep"].is<uint16_t>()) sensor.astep = source["astep"].as<uint16_t>();  else ok = false;
  return ok;
}

// Analog fuer OpticalSettings (Filter + Sensor zusammen) -- genutzt fuer das
// "optical"-Unterobjekt von RootSettings, ein eigenstaendiges OpticalSettings-
// Dokument (Dark-/Weiss-Referenzen) UND je ein "optical"-Unterobjekt pro
// Messspitzen-Katalog-Eintrag. Siehe schema/settings.schema.json fuer die
// dokumentierte Form.
static void writeOpticalFields(JsonVariant target, FilterState fs, const SensorSettings& sensor) {
  target["filter"] = filterStateCsvLabel(fs);
  writeSensorFields(target["sensor"].to<JsonObject>(), sensor);
}

// Feldweise robust: jedes fehlende/unbekannte Feld schlaegt fehl -- der
// Aufrufer faellt dann komplett auf den jeweiligen Struct-Default zurueck
// (kein Mischzustand aus altem/neuem Wert), gleiche Philosophie wie zuvor die
// binaere Groessen-Pruefung, jetzt nur feldweise statt "ganz oder gar nicht".
static bool readOpticalFields(JsonVariantConst source, FilterState& fs, SensorSettings& sensor) {
  bool ok = filterStateFromCsvLabel(source["filter"] | "", fs);
  return ok && readSensorFields(source["sensor"], sensor);
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

// Schreibt direkt in den gegebenen Stream (kein Zwischen-String) -- der
// Katalog kann mit vollen Fingerabdruck-Listen mehrere KB gross werden,
// direktes Streamen in die Datei spart den Umweg ueber eine grosse String-
// Kopie im RAM.
static void serializeTipCatalog(const TipCatalog& c, Print& out) {
  JsonDocument doc;
  doc["active"] = c.active.c_str();
  JsonArray arr = doc["tips"].to<JsonArray>();
  for (const MeasurementTip& t : c.tips) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = t.name.c_str();
    writeOpticalFields(o["optical"].to<JsonObject>(), t.optical.filterState, t.optical.sensor);
    JsonArray fps = o["whiteFingerprints"].to<JsonArray>();
    for (const WhiteFingerprint& fp : t.whiteFingerprints) {
      JsonObject fpObj = fps.add<JsonObject>();
      fpObj["uptimeS"] = fp.uptimeS;
      fpObj["sensorId"] = fp.sensorId.c_str();
      writeSensorFields(fpObj["sensor"].to<JsonObject>(), fp.sensor);
      JsonArray vals = fpObj["values"].to<JsonArray>();
      for (float v : fp.normalized) vals.add(v);
    }
  }
  serializeJson(doc, out);
}

// Ungueltig (-> false, Aufrufer faellt auf einen leeren Katalog zurueck), wenn
// JSON kaputt ist, ODER die resultierende Liste leer waere, ODER 'active'
// keinen der geladenen Eintraege trifft -- die Invariante "immer >=1 Spitze,
// 'active' immer gueltig" wird hier durchgesetzt, nicht erst beim Zugriff.
static bool deserializeTipCatalog(Stream& in, TipCatalog& out) {
  TipCatalog result;
  JsonDocument doc;
  bool ok = (deserializeJson(doc, in) == DeserializationError::Ok);
  if (ok) {
    result.active = (const char*)(doc["active"] | "");
    for (JsonObject o : doc["tips"].as<JsonArray>()) {
      MeasurementTip t;
      t.name = (const char*)(o["name"] | "");
      if (t.name.empty() || !readOpticalFields(o["optical"], t.optical.filterState, t.optical.sensor)) {
        ok = false;
        break;
      }
      // whiteFingerprints ist OPTIONAL (aeltere/neu angelegte Kataloge haben
      // noch keine) -- fehlt der Schluessel, bleibt die Liste einfach leer,
      // das ist KEIN Ladefehler. Ein einzelner kaputter Fingerabdruck-Eintrag
      // wird uebersprungen statt den ganzen Katalog zu verwerfen (reine
      // Zusatzdaten).
      if (o["whiteFingerprints"].is<JsonArray>()) {
        for (JsonObject fpObj : o["whiteFingerprints"].as<JsonArray>()) {
          if (!fpObj["values"].is<JsonArray>()) continue;
          WhiteFingerprint fp;
          fp.uptimeS = fpObj["uptimeS"] | 0;
          fp.sensorId = (const char*)(fpObj["sensorId"] | "");
          if (!readSensorFields(fpObj["sensor"], fp.sensor)) continue;
          for (JsonVariant v : fpObj["values"].as<JsonArray>()) fp.normalized.push_back(v.as<float>());
          t.whiteFingerprints.push_back(fp);
        }
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
//
// WICHTIG: NVS-Keys duerfen laut ESP-IDF hoechstens 15 Zeichen lang sein
// (Preferences::putString()/getString() -> nvs_set_str()/nvs_get_str()
// scheitern sonst mit ESP_ERR_NVS_KEY_TOO_LONG -- die Preferences-Bibliothek
// meldet das nur per log_e(), der Rueckgabewert wird unten deshalb explizit
// geprueft). "*_settings_json" waere 18-19 Zeichen lang gewesen und ist
// NIEMALS tatsaechlich gespeichert worden -- "*_cfg_json" bleibt knapp
// darunter.
bool CalibrationStore::loadDark(Measurement& out, OpticalSettings& optical) {
  bool ok = load("dark_raw", out);
  if (ok) {
    String json = prefs_.getString("dark_cfg_json", "");
    if (json.isEmpty() || !deserializeOpticalSettings(json, optical)) optical = OpticalSettings();
  }
  return ok;
}

bool CalibrationStore::loadWhite(Measurement& out, OpticalSettings& optical) {
  bool ok = load("white_raw", out);
  if (ok) {
    String json = prefs_.getString("white_cfg_json", "");
    if (json.isEmpty() || !deserializeOpticalSettings(json, optical)) optical = OpticalSettings();
  }
  return ok;
}

void CalibrationStore::saveDark(const Measurement& v, const OpticalSettings& optical) {
  save("dark_raw", v);
  if (!prefs_.putString("dark_cfg_json", serializeOpticalSettings(optical))) {
    Serial.println("# dark_cfg_json: NVS-Schreibfehler");
  }
}

void CalibrationStore::saveWhite(const Measurement& v, const OpticalSettings& optical) {
  save("white_raw", v);
  if (!prefs_.putString("white_cfg_json", serializeOpticalSettings(optical))) {
    Serial.println("# white_cfg_json: NVS-Schreibfehler");
  }
}

bool CalibrationStore::loadSettings(RootSettings& out) {
  String json = prefs_.getString("live_cfg_json", "");
  if (json.isEmpty()) { out = RootSettings(); return false; }
  return deserializeRootSettings(json, out);
}

void CalibrationStore::saveSettings(const RootSettings& v) {
  if (!prefs_.putString("live_cfg_json", serializeRootSettings(v))) {
    Serial.println("# live_cfg_json: NVS-Schreibfehler");
  }
}

// Auf LittleFS statt NVS/Preferences -- siehe Kommentar an TIPS_PATH oben.
bool CalibrationStore::loadTips(TipCatalog& out) {
  if (!LittleFS.exists(TIPS_PATH)) { out = TipCatalog(); return false; }
  File f = LittleFS.open(TIPS_PATH, FILE_READ);
  if (!f) { out = TipCatalog(); return false; }
  bool ok = deserializeTipCatalog(f, out);
  f.close();
  return ok;
}

void CalibrationStore::saveTips(const TipCatalog& v) {
  File f = LittleFS.open(TIPS_PATH, FILE_WRITE, true);  // "w" trunkiert automatisch
  if (!f) return;
  serializeTipCatalog(v, f);
  f.close();
}
