#include "HistoryStore.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <cstdlib>
#include <cstring>
#include <string>

static const char* HISTORY_PATH = "/history.csv";

// Zeilenformat: <label>,<kindNum>,<tempC>,<sessionMs>,<uptimeS>,<filterStateNum>,<gainNum>,<atime>,<astep>,<v0>,...,<vN-1>
// Bounds-Check auf kindNum/filterStateNum/gainNum schuetzt vor einer durch
// Stromausfall verstuemmelten Zeile, die zufaellig trotzdem mit '\n' endet.
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
  rec.settings.gain = static_cast<as7341_gain_t>(gainNum);

  if (!nextField(field)) return false;
  rec.settings.atime = static_cast<uint8_t>(strtoul(field.c_str(), nullptr, 10));

  if (!nextField(field)) return false;
  rec.settings.astep = static_cast<uint16_t>(strtoul(field.c_str(), nullptr, 10));

  rec.measurement.clear();
  while (nextField(field)) {
    if (!field.empty()) rec.measurement.push_back(strtof(field.c_str(), nullptr));
  }
  return true;
}

static void countingVisitor(const MeasurementRecord&, void* userData) {
  (*reinterpret_cast<size_t*>(userData))++;
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
  f.print((int)rec.settings.gain);
  f.print(',');
  f.print(rec.settings.atime);
  f.print(',');
  f.print(rec.settings.astep);
  for (float v : rec.measurement) {
    f.print(',');
    f.print(v, 3);
  }
  f.print('\n');
  f.flush();
  f.close();
  count_++;
  return true;
}

void HistoryStore::clear() {
  if (!mounted_) return;
  File f = LittleFS.open(HISTORY_PATH, FILE_WRITE, true);  // "w" trunkiert automatisch
  if (f) f.close();
  count_ = 0;
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

size_t HistoryStore::firstRecordChannelCount() const {
  if (!mounted_) return 0;
  File f = LittleFS.open(HISTORY_PATH, FILE_READ);
  if (!f) return 0;

  std::string line;
  int c;
  while ((c = f.read()) >= 0) {
    if (c == '\n') break;
    if (c != '\r') line.push_back(static_cast<char>(c));
  }
  f.close();

  MeasurementRecord rec;
  return parseLine(line, rec) ? rec.measurement.size() : 0;
}
