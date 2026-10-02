#include "HistoryStore.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cmath>

static const char* HISTORY_PATH = "/history.csv";

// Zeilenformat: <label>,<kindNum>,<tempC>,<sessionMs>,<uptimeS>,<filterStateNum>,<gainNum>,<atime>,<astep>,<precisionNum>,<sampleCount>,<relSemWorst|leer>,<channelCount>,<v0>,...,<vN-1>,<sem0|leer>,...,<semN-1|leer>
// Bounds-Check auf kindNum/filterStateNum/gainNum/precisionNum/channelCount
// schuetzt vor einer durch Stromausfall verstuemmelten Zeile, die zufaellig
// trotzdem mit '\n' endet. relSemWorst ist ein LEERES Feld (nicht "0"), wenn
// kein relSEM berechnet wurde -- siehe MeasurementRecord-Kommentar in
// HistoryStore.h. channelCount macht den zweiten (SEM-)Werteblock erst
// parsebar (ohne ihn wuesste man nicht, wo der erste Block endet) --
// Zeilen von VOR dieser Erweiterung (ohne channelCount) werden dadurch
// zuverlaessig verworfen statt fehlinterpretiert: ihr erster Rohwert wird
// als (garantiert zu grosse) channelCount gelesen, das anschliessende Lesen
// so vieler Felder laeuft ueber das Zeilenende hinaus und schlaegt sauber
// fehl (siehe Bounds-Check unten) -- keine Migration, wie ueberall sonst.
static bool parseLine(const std::string& line, MeasurementRecord& rec) {
  size_t pos = 0;
  auto nextField = [&](std::string& out) -> bool {
    if (pos > line.size()) return false;
    size_t next = line.find(',', pos);
    out = (next == std::string::npos) ? line.substr(pos) : line.substr(pos, next - pos);
    pos = (next == std::string::npos) ? line.size() + 1 : next + 1;
    return true;
  };

  std::string field;
  if (!nextField(field) || field.size() >= sizeof(rec.label)) return false;
  strncpy(rec.label, field.c_str(), sizeof(rec.label));
  rec.label[sizeof(rec.label) - 1] = '\0';

  if (!nextField(field)) return false;
  int kindNum = atoi(field.c_str());
  if (kindNum < 0 || kindNum >= static_cast<int>(SampleKind::COUNT)) return false;
  rec.kind = static_cast<SampleKind>(kindNum);

  if (!nextField(field)) return false;
  rec.tempC = strtof(field.c_str(), nullptr);

  if (!nextField(field)) return false;
  rec.sessionMs = static_cast<uint32_t>(strtoul(field.c_str(), nullptr, 10));

  if (!nextField(field)) return false;
  rec.uptimeS = static_cast<uint32_t>(strtoul(field.c_str(), nullptr, 10));

  if (!nextField(field)) return false;
  int filterNum = atoi(field.c_str());
  if (filterNum < 0 || filterNum >= static_cast<int>(FilterState::COUNT)) return false;
  rec.settings.filterState = static_cast<FilterState>(filterNum);

  if (!nextField(field)) return false;
  int gainNum = atoi(field.c_str());
  if (gainNum < 0 || gainNum >= static_cast<int>(AS7341_GAIN_COUNT)) return false;
  rec.settings.sensor.gain = static_cast<as7341_gain_t>(gainNum);

  if (!nextField(field)) return false;
  rec.settings.sensor.atime = static_cast<uint8_t>(strtoul(field.c_str(), nullptr, 10));

  if (!nextField(field)) return false;
  rec.settings.sensor.astep = static_cast<uint16_t>(strtoul(field.c_str(), nullptr, 10));

  if (!nextField(field)) return false;
  int precisionNum = atoi(field.c_str());
  if (precisionNum < 0 || precisionNum >= static_cast<int>(Precision::COUNT)) return false;
  rec.precision = static_cast<Precision>(precisionNum);

  if (!nextField(field)) return false;
  rec.sampleCount = static_cast<uint8_t>(strtoul(field.c_str(), nullptr, 10));

  if (!nextField(field)) return false;
  rec.relSemWorst = field.empty() ? NAN : strtof(field.c_str(), nullptr);

  if (!nextField(field)) return false;
  long channelCountL = strtol(field.c_str(), nullptr, 10);
  if (channelCountL < 0 || channelCountL > 64) return false;  // Sicherheitsnetz, siehe Format-Kommentar oben
  size_t channelCount = (size_t)channelCountL;

  rec.measurement.clear();
  rec.measurement.reserve(channelCount);
  for (size_t i = 0; i < channelCount; i++) {
    if (!nextField(field)) return false;
    rec.measurement.push_back(strtof(field.c_str(), nullptr));
  }

  // Zweiter Werteblock (SEM je Kanal) -- bleibt insgesamt leer, wenn KEIN
  // einzelnes Feld gesetzt war (Precision::Single, oder eine Zeile von vor
  // dieser Erweiterung), statt N Nullen vorzutaeuschen.
  Measurement sem(channelCount, 0.0f);
  bool anySem = false;
  for (size_t i = 0; i < channelCount; i++) {
    if (!nextField(field)) return false;
    if (!field.empty()) { sem[i] = strtof(field.c_str(), nullptr); anySem = true; }
  }
  rec.semPerChannel = anySem ? sem : Measurement();
  return true;
}

static void countingVisitor(const MeasurementRecord&, void* userData) {
  (*reinterpret_cast<size_t*>(userData))++;
}

// clear()-Helfer: merkt sich die LETZTE Dark- bzw. White-Zeile (per
// Ueberschreiben bei jedem weiteren Treffer waehrend des chronologischen
// Durchlaufs bleibt am Ende jeweils die juengste uebrig).
struct LastRefCtx {
  bool haveDark = false, haveWhite = false;
  MeasurementRecord dark, white;
};
static void collectLastRefVisitor(const MeasurementRecord& rec, void* userData) {
  LastRefCtx* ctx = reinterpret_cast<LastRefCtx*>(userData);
  if (rec.kind == SampleKind::Dark) { ctx->dark = rec; ctx->haveDark = true; }
  else if (rec.kind == SampleKind::White) { ctx->white = rec; ctx->haveWhite = true; }
}

bool HistoryStore::begin() {
  if (!LittleFS.begin(/*formatOnFail=*/true)) {
    Serial.println("# LittleFS mount failed -- Historie bleibt deaktiviert");
    mounted_ = false;
    return false;
  }
  mounted_ = true;

  if (!LittleFS.exists(HISTORY_PATH)) {
    File f = LittleFS.open(HISTORY_PATH, FILE_WRITE, true);
    if (f) f.close();
  }

  count_ = 0;
  forEach(countingVisitor, &count_);  // einziger vollstaendiger Scan, danach nur noch RAM-Cache
  return true;
}

bool HistoryStore::append(const MeasurementRecord& rec) {
  if (!mounted_) return false;
  File f = LittleFS.open(HISTORY_PATH, FILE_APPEND, true);
  if (!f) return false;

  f.print(rec.label);
  f.print(',');
  f.print((int)rec.kind);
  f.print(',');
  f.print(rec.tempC, 2);
  f.print(',');
  f.print(rec.sessionMs);
  f.print(',');
  f.print(rec.uptimeS);
  f.print(',');
  f.print((int)rec.settings.filterState);
  f.print(',');
  f.print((int)rec.settings.sensor.gain);
  f.print(',');
  f.print(rec.settings.sensor.atime);
  f.print(',');
  f.print(rec.settings.sensor.astep);
  f.print(',');
  f.print((int)rec.precision);
  f.print(',');
  f.print(rec.sampleCount);
  f.print(',');
  if (!isnan(rec.relSemWorst)) f.print(rec.relSemWorst, 5);  // leer lassen, wenn NAN -- siehe Header-Kommentar
  f.print(',');
  f.print(rec.measurement.size());
  for (float v : rec.measurement) {
    f.print(',');
    f.print(v, 3);
  }
  for (size_t i = 0; i < rec.measurement.size(); i++) {
    f.print(',');
    if (i < rec.semPerChannel.size()) f.print(rec.semPerChannel[i], 3);  // leer lassen, falls nicht ermittelt
  }
  f.print('\n');
  f.flush();
  f.close();
  count_++;
  return true;
}

// Behaelt bewusst die letzte Dark- UND die letzte White-Zeile (falls
// vorhanden), statt die Historie restlos zu leeren: main.cpp berechnet
// Reflexion/Lab/Hex fuer eine Messung gegen die Dark-/Weisszeile, die
// chronologisch zuletzt VOR ihr in der Historie steht (siehe
// SettingsRefState/observeReference() in main.cpp) -- ohne mindestens eine
// erhaltene Referenzzeile koennten danach aufgenommene Messungen (bis zur
// naechsten ECHTEN Referenzmessung) nicht mehr korrekt ausgewertet werden,
// obwohl die zugehoerige Kalibrierung weiterhin gueltig ist. Die vollen,
// ORIGINALEN Zeilen (samt Telemetrie) werden 1:1 zurueckgeschrieben -- keine
// rekonstruierte/vereinfachte Ersatzzeile, deshalb ueber den normalen
// append()-Pfad statt eines main.cpp-seitig neu gebauten MeasurementRecord.
void HistoryStore::clear() {
  if (!mounted_) return;

  LastRefCtx ref;
  forEach(collectLastRefVisitor, &ref);

  File f = LittleFS.open(HISTORY_PATH, FILE_WRITE, true);  // "w" trunkiert automatisch
  if (f) f.close();
  count_ = 0;

  if (ref.haveDark)  append(ref.dark);
  if (ref.haveWhite) append(ref.white);
}

void HistoryStore::forEach(RecordVisitor visitor, void* userData) const {
  if (!mounted_) return;
  File f = LittleFS.open(HISTORY_PATH, FILE_READ);
  if (!f) return;

  std::string line;
  int c;
  while ((c = f.read()) >= 0) {
    if (c == '\n') {
      MeasurementRecord rec;
      if (parseLine(line, rec)) visitor(rec, userData);
      line.clear();
    } else if (c != '\r') {
      line.push_back(static_cast<char>(c));
    }
  }
  // Ein am Dateiende unvollstaendiger Rest (z.B. Stromausfall mitten in
  // append()) wird bewusst verworfen -- nie gezaehlt/besucht. So bleiben
  // count() und forEach() immer konsistent, auch nach einem harten Abbruch.
  f.close();
}

// Liest vorwaerts bis zur ERSTEN erfolgreich parsebaren Zeile, statt nur die
// buchstaeblich erste zu versuchen -- eine einzelne fuehrende Zeile in einem
// mittlerweile veralteten Format (z.B. von vor einer Zeilenformat-Erweiterung,
// siehe parseLine()-Kommentar) soll nicht die Kanalzahl fuer die GESAMTE
// Historie (und damit die Spaltenkoepfe des Exports) unbrauchbar machen, wo
// doch alle nachfolgenden Zeilen bereits korrekt parsebar sind.
size_t HistoryStore::firstRecordChannelCount() const {
  if (!mounted_) return 0;
  File f = LittleFS.open(HISTORY_PATH, FILE_READ);
  if (!f) return 0;

  std::string line;
  int c;
  while ((c = f.read()) >= 0) {
    if (c == '\n') {
      MeasurementRecord rec;
      if (parseLine(line, rec)) { f.close(); return rec.measurement.size(); }
      line.clear();
      continue;
    }
    if (c != '\r') line.push_back(static_cast<char>(c));
  }
  f.close();
  return 0;
}
