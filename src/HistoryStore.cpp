#include "HistoryStore.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cmath>

static const char* HISTORY_PATH = "/history.csv";

// Row format: <label>,<kindNum>,<tempC>,<sessionMs>,<uptimeS>,<filterStateNum>,<gainNum>,<atime>,<astep>,<precisionNum>,<sampleCount>,<relSemWorst|empty>,<channelCount>,<v0>,...,<vN-1>,<sem0|empty>,...,<semN-1|empty>
// The bounds check on kindNum/filterStateNum/gainNum/precisionNum/channelCount
// guards against a row mangled by a power loss that still happens to end with
// '\n'. relSemWorst is an EMPTY field (not "0") when no relSEM was computed --
// see the MeasurementRecord comment in HistoryStore.h. channelCount is what
// makes the second (SEM) value block parsable in the first place (without it
// there would be no way to know where the first block ends) -- rows from
// BEFORE this extension (without channelCount) are thereby reliably discarded
// instead of misinterpreted: their first raw value is read as a (guaranteed
// too large) channelCount, the subsequent read of that many fields runs past
// the end of the line and fails cleanly (see the bounds check below) -- no
// migration, as everywhere else.
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
  if (channelCountL < 0 || channelCountL > 64) return false;  // safety net, see format comment above
  size_t channelCount = (size_t)channelCountL;

  rec.measurement.clear();
  rec.measurement.reserve(channelCount);
  for (size_t i = 0; i < channelCount; i++) {
    if (!nextField(field)) return false;
    rec.measurement.push_back(strtof(field.c_str(), nullptr));
  }

  // Second value block (SEM per channel) -- stays entirely empty if NOT A
  // SINGLE field was set (Precision::Single, or a row from before this
  // extension), instead of faking N zeros.
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

// clear() helper: remembers the LAST dark and white row respectively (by
// overwriting on every further match during the chronological pass, the most
// recent one is what remains at the end).
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
  forEach(countingVisitor, &count_);  // only full scan, RAM cache only from here on
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
  if (!isnan(rec.relSemWorst)) f.print(rec.relSemWorst, 5);  // leave empty if NAN -- see header comment
  f.print(',');
  f.print(rec.measurement.size());
  for (float v : rec.measurement) {
    f.print(',');
    f.print(v, 3);
  }
  for (size_t i = 0; i < rec.measurement.size(); i++) {
    f.print(',');
    if (i < rec.semPerChannel.size()) f.print(rec.semPerChannel[i], 3);  // leave empty if not determined
  }
  f.print('\n');
  f.flush();
  f.close();
  count_++;
  return true;
}

// Deliberately keeps the last dark AND the last white row (if present)
// instead of wiping the history completely: main.cpp computes
// reflectance/Lab/hex for a measurement against the dark/white row that is
// chronologically the last one BEFORE it in the history (see
// SettingsRefState/observeReference() in main.cpp) -- without at least one
// preserved reference row, measurements taken afterward (until the next
// REAL reference measurement) could no longer be evaluated correctly, even
// though the associated calibration remains valid. The full, ORIGINAL rows
// (telemetry included) are written back 1:1 -- not a reconstructed/
// simplified replacement row, hence via the normal append() path rather
// than a MeasurementRecord newly built on the main.cpp side.
void HistoryStore::clear() {
  if (!mounted_) return;

  LastRefCtx ref;
  forEach(collectLastRefVisitor, &ref);

  File f = LittleFS.open(HISTORY_PATH, FILE_WRITE, true);  // "w" truncates automatically
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
  // A remainder incomplete at the end of the file (e.g. power loss in the
  // middle of append()) is deliberately discarded -- never counted/visited.
  // This keeps count() and forEach() always consistent, even after a hard
  // abort.
  f.close();
}

// Reads forward to the FIRST successfully parsable row, instead of only
// trying the literal first one -- a single leading row in a by-now outdated
// format (e.g. from before a row-format extension, see the parseLine()
// comment) should not render the channel count for the ENTIRE history (and
// thus the export's column headers) unusable, when all subsequent rows are
// already correctly parsable.
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
