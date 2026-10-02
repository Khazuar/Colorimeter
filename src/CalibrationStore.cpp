#include "CalibrationStore.h"
#include "SettingsCodec.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// The measurement tip catalog (see below) deliberately lives on LittleFS
// instead of in the NVS "Preferences" like the other values: according to
// partitions.csv the "nvs" partition is only 20 KB in size and is already
// shared by live/dark/white_settings -- with fingerprints (see
// TipCatalog.h::WhiteFingerprint, up to MAX_WHITE_FINGERPRINTS_PER_TIP per
// tip) the catalog grows easily to several KB per tip. LittleFS (the
// "spiffs" partition, 1.4 MB) already carries the at-least-equally-important
// measurement history (HistoryStore) and is just as power-loss-safe -- no
// new risk, just a more suitable partition for growing data.
static const char* TIPS_PATH = "/tips.json";

void CalibrationStore::begin() {
  prefs_.begin("colorim", false);
  LittleFS.begin(/*formatOnFail=*/true);  // idempotent, see HistoryStore::begin()
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

// Shared read/write pair for the three fields of SensorSettings -- used by
// the "sensor" sub-object in writeOpticalFields()/readOpticalFields() AND
// directly for the frozen SensorSettings in a WhiteFingerprint (see
// TipCatalog.h), so the JSON shape doesn't need to be maintained in multiple
// places. Generic over JsonVariant/JsonVariantConst, so that both an entire
// JsonDocument and a single JsonObject (array element, sub-object) can serve
// as target/source. Enum values are encoded as strings (see
// SettingsCodec.h), so that a future reordering/extension of the enum does
// not silently change the meaning of an already-stored value.
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

// Analogous for OpticalSettings (filter + sensor together) -- used for the
// "optical" sub-object of RootSettings, a standalone OpticalSettings document
// (dark/white references) AND one "optical" sub-object per measurement tip
// catalog entry. See schema/settings.schema.json for the documented shape.
static void writeOpticalFields(JsonVariant target, FilterState fs, const SensorSettings& sensor) {
  target["filter"] = filterStateCsvLabel(fs);
  writeSensorFields(target["sensor"].to<JsonObject>(), sensor);
}

// Robust field by field: any missing/unknown field fails -- the caller then
// falls back completely to the respective struct default (no mixed state of
// old/new value), the same philosophy as previously the binary size check,
// just now field by field instead of "all or nothing".
static bool readOpticalFields(JsonVariantConst source, FilterState& fs, SensorSettings& sensor) {
  bool ok = filterStateFromCsvLabel(source["filter"] | "", fs);
  return ok && readSensorFields(source["sensor"], sensor);
}

static String serializeRootSettings(const RootSettings& s) {
  JsonDocument doc;
  writeOpticalFields(doc["optical"].to<JsonObject>(), s.optical.filterState, s.optical.sensor);
  // future: add further RootSettings groups here as additional top-level keys
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

// Writes directly into the given stream (no intermediate string) -- the
// catalog can grow to several KB in size with full fingerprint lists,
// streaming directly into the file saves the detour via a large string copy
// in RAM.
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

// Invalid (-> false, caller falls back to an empty catalog) if JSON is
// broken, OR the resulting list would be empty, OR 'active' matches none of
// the loaded entries -- the invariant "always >=1 tip, 'active' always valid"
// is enforced here, not only at access time.
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
      // whiteFingerprints is OPTIONAL (older/newly created catalogs don't
      // have any yet) -- if the key is missing, the list simply stays empty,
      // that is NOT a load error. A single broken fingerprint entry is
      // skipped instead of discarding the whole catalog (purely supplemental
      // data).
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

// NEW key names ("_json" suffix, deliberately different from the earlier
// binary blob keys) -- this means old blob entries are never read again
// (orphaned, harmless bytes in NVS) instead of being misinterpreted as
// supposed JSON. No migration: after flashing this change, settings/
// references revert to their defaults once, exactly as on a first boot.
//
// IMPORTANT: according to ESP-IDF, NVS keys may be at most 15 characters
// long (Preferences::putString()/getString() -> nvs_set_str()/nvs_get_str()
// otherwise fail with ESP_ERR_NVS_KEY_TOO_LONG -- the Preferences library
// only reports this via log_e(), so the return value is explicitly checked
// below). "*_settings_json" would have been 18-19 characters long and was
// NEVER actually saved -- "*_cfg_json" stays just under the limit.
bool CalibrationStore::loadDark(Measurement& out, Measurement& outSem, OpticalSettings& optical) {
  bool ok = load("dark_raw", out);
  if (ok) {
    String json = prefs_.getString("dark_cfg_json", "");
    if (json.isEmpty() || !deserializeOpticalSettings(json, optical)) optical = OpticalSettings();
    if (!load("dark_sem", outSem)) outSem.clear();  // e.g. a reference from before this extension -- not an error
  }
  return ok;
}

bool CalibrationStore::loadWhite(Measurement& out, Measurement& outSem, OpticalSettings& optical) {
  bool ok = load("white_raw", out);
  if (ok) {
    String json = prefs_.getString("white_cfg_json", "");
    if (json.isEmpty() || !deserializeOpticalSettings(json, optical)) optical = OpticalSettings();
    if (!load("white_sem", outSem)) outSem.clear();
  }
  return ok;
}

void CalibrationStore::saveDark(const Measurement& v, const Measurement& sem, const OpticalSettings& optical) {
  save("dark_raw", v);
  save("dark_sem", sem);
  if (!prefs_.putString("dark_cfg_json", serializeOpticalSettings(optical))) {
    Serial.println("# dark_cfg_json: NVS write error");
  }
}

void CalibrationStore::saveWhite(const Measurement& v, const Measurement& sem, const OpticalSettings& optical) {
  save("white_raw", v);
  save("white_sem", sem);
  if (!prefs_.putString("white_cfg_json", serializeOpticalSettings(optical))) {
    Serial.println("# white_cfg_json: NVS write error");
  }
}

bool CalibrationStore::loadSettings(RootSettings& out) {
  String json = prefs_.getString("live_cfg_json", "");
  if (json.isEmpty()) { out = RootSettings(); return false; }
  return deserializeRootSettings(json, out);
}

void CalibrationStore::saveSettings(const RootSettings& v) {
  if (!prefs_.putString("live_cfg_json", serializeRootSettings(v))) {
    Serial.println("# live_cfg_json: NVS write error");
  }
}

// On LittleFS instead of NVS/Preferences -- see the comment on TIPS_PATH above.
bool CalibrationStore::loadTips(TipCatalog& out) {
  if (!LittleFS.exists(TIPS_PATH)) { out = TipCatalog(); return false; }
  File f = LittleFS.open(TIPS_PATH, FILE_READ);
  if (!f) { out = TipCatalog(); return false; }
  bool ok = deserializeTipCatalog(f, out);
  f.close();
  return ok;
}

void CalibrationStore::saveTips(const TipCatalog& v) {
  File f = LittleFS.open(TIPS_PATH, FILE_WRITE, true);  // "w" truncates automatically
  if (!f) return;
  serializeTipCatalog(v, f);
  f.close();
}
