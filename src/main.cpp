#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <cstring>
#include <cmath>
#include <string>
#include <algorithm>

#include "AppConfig.h"
#include "Spectrometer.h"
#include "AS7341Spectrometer.h"
#include "SettingsCodec.h"
#include "CalibrationStore.h"
#include "TipCatalog.h"
#include "Buttons.h"
#include "DisplayViews.h"
#include "BleExporter.h"
#include "ColorimetryTables.h"
#include "UptimeLogger.h"
#include "HistoryStore.h"
#include "DigitEditor.h"

// ------------------------- Display -------------------------
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
bool displayOk = false;

// ------------------------- Sensor (use only via the abstraction) -------------------------
AS7341Spectrometer sensorImpl;
Spectrometer& spectrometer = sensorImpl;

// ------------------------- Calibration: a concern of the orchestration -------------------------
CalibrationStore calStore;
Measurement darkRef, whiteRef;
// Absolute standard error of the mean per channel, frozen WITH the
// respective reference (see MeasurementTelemetry::semPerChannel) -- basis
// for the limit-of-detection test in checkValidity() on the next
// white measurement. Empty if not (yet) available.
Measurement darkRefSem, whiteRefSem;
OpticalSettings darkRefSettings;   // frozen WITH darkRef, see CalibrationStore
OpticalSettings whiteRefSettings;  // frozen WITH whiteRef
bool calibrated = false;  // "reference matches the CURRENTLY selected settings" -- see calibrationValidFor()

// Currently selected settings hierarchy in the settings tree (see
// DisplayMode::Settings as well as RootSettings/OpticalSettings in AppConfig.h).
RootSettings currentSettings;

// Returns true if both the dark and white reference are present AND both
// were recorded under EXACTLY the given OpticalSettings (filter+gain+ATIME+
// ASTEP). Central place for the rule "if even a single setting deviates,
// the reference counts as not present" -- used equally for the live
// display (against lastMeasurementSettings), the global "ready/need cal"
// status (against currentSettings.optical), AND the CSV export (against
// rec.settings of each row).
bool calibrationValidFor(const OpticalSettings& s) {
  return !darkRef.empty() && !whiteRef.empty()
      && darkRefSettings == s && whiteRefSettings == s;
}

// ------------------------- Uptime logging (dedicated NVS partition) -------------------------
UptimeLogger uptimeLogger;

// ------------------------- last measurement / display state -------------------------
Measurement lastMeasurement;
char lastLabel[16] = "";
// Settings that were actually active at the time OF lastMeasurement --
// frozen, NOT the currentSettings that are live-editable in settings mode
// (see renderCurrentView() for the reasoning behind this asymmetry).
OpticalSettings lastMeasurementSettings;
bool busy = false;  // while true: no further measurement/export can be triggered

DisplayMode currentDisplayMode = DisplayMode::Measure;
DisplayView currentView = DisplayView::ColorInfo;

// Within the Measure DisplayMode: selection page (choose measurement mode
// with a short Mode press, trigger fires it) vs. result page (as before).
// See the AppConfig.h::DisplayMode comment and MEASUREMENT_MODES below.
enum class MeasurePage : uint8_t { Select = 0, Result = 1 };
MeasurePage measurePage = MeasurePage::Select;

// Forward declaration: the actual shared measurement entry point is only
// defined further below (details on the sensor/display flow belong there),
// but is already needed here by the MEASUREMENT_MODES trigger functions.
bool performMeasurement(Precision precision, SampleKind kind);

// An entry in the Measure selection list. trigger() performs the actual
// measurement and returns true on success (lastMeasurement updated) --
// the return value controls in loop() whether to switch to the result page
// or (after a sensor error/a non-convergence, see renderMeasurementError())
// stay on the selection page for an immediate retry.
struct MeasurementModeDescriptor {
  const char* name;         // "Precise", "Single" -- menu display on the selection page
  const char* cornerLabel;  // "P", "S" -- corner display on the result screen/while measuring
  bool (*trigger)();
};

bool triggerPreciseMeasurement() { return performMeasurement(Precision::Precise, SampleKind::Regular); }
bool triggerSingleMeasurement()  { return performMeasurement(Precision::Single,  SampleKind::Regular); }

// Precise first: most-used mode, so it's preselected after every fresh
// entry into Measure (see cycleMode()). A future additional measurement
// mode (series, rotation, ...) is just another entry here -- the
// selection/result navigation itself doesn't need to be touched for that.
const MeasurementModeDescriptor MEASUREMENT_MODES[] = {
  { "Precise", "P", triggerPreciseMeasurement },
  { "Single",  "S", triggerSingleMeasurement },
};
const uint8_t MEASUREMENT_MODE_COUNT = sizeof(MEASUREMENT_MODES) / sizeof(MEASUREMENT_MODES[0]);
uint8_t measurementModeIndex = 0;

// Reference selected within the Calibration DisplayMode via a short Mode
// press -- a long trigger press then measures exactly that one. Deliberately
// a separate, main.cpp-local enum instead of reusing SampleKind: one is
// transient UI state ("which page am I currently looking at"), the other a
// data-model concept ("what kind of stored reading is this") -- even though
// both know White/Dark, these are different questions.
enum class CalibrationTarget : uint8_t { White = 0, Dark = 1 };
CalibrationTarget calibrationTarget = CalibrationTarget::White;

// ------------------------- Measurement history (persisted, see HistoryStore) -------------------------
// Every measurement is collected here together with its mode -- the basis
// for the serial and BLE export (buildHistoryCsv()). Survives restarts/
// power loss (LittleFS on the "spiffs" partition), see HistoryStore.h.
HistoryStore historyStore;

// ------------------------- BLE export -------------------------
BleExporter bleExporter;
bool exportSending = false;
bool exportSentOk  = false;
bool exportHint    = false;  // "no phone connected"
size_t exportSentBytes = 0, exportTotalBytes = 0;

// The export mode has three "pages" cyclable via a short Mode press
// (see cycleView()): Normal/Debug control, as before, whether the BLE
// export additionally appends the raw Measurement (labeled with
// measurementLabels()) to every row -- over USB the Debug variant is ALWAYS
// sent, regardless of this (see loop()). Clear is a separate page for
// deleting the persisted history (see renderExportClear() / loop() trigger
// dispatch) -- deliberately triggered by a LONG rather than short trigger
// press.
enum class ExportPage : uint8_t { Normal = 0, Debug = 1, Clear = 2, COUNT = 3 };
ExportPage exportPage = ExportPage::Normal;

// ------------------------- Settings mode -------------------------
// A short Mode press selects (outside of editing) WHICH setting is
// displayed. Every setting is operated via the same DigitEditor
// (see DigitEditor.h): a long trigger press starts editing, a short
// trigger press increments the current digit, a short Mode press
// decrements it, a long trigger/Mode press moves to the next/previous
// digit -- at the respective end (last digit going forward, or first
// going backward) the assembled value is committed (see loop()).
// Enum-like settings (filter, gain) are thereby "single-digit numbers":
// digitCount=1, the digit is the option index.
const char* filterStateUiLabel(FilterState fs) {
  switch (fs) {
    case FilterState::Filter650nm: return "650nm";
    case FilterState::Filter700nm: return "700nm";
    default:                       return "no filter";
  }
}
// filterStateCsvLabel()/gainCsvLabel() (for CSV export AND the new
// settings tree node labels) now live in SettingsCodec.h/.cpp -- the same
// source that the JSON persistence (CalibrationStore.cpp) also uses, so
// that the two can never drift apart.

// "single"/"precision" -- literally as named by the user, instead of the
// internal enum names Single/Precise.
const char* precisionCsvLabel(Precision p) {
  return (p == Precision::Precise) ? "precision" : "single";
}

// Settings are an arbitrarily deeply nestable tree structure (see Plan)
// instead of a flat list -- its shape follows RootSettings/AppConfig.h.
// A node is either a leaf (editable value, like SettingDescriptor before),
// a navigation node (goes one level deeper via a long trigger press),
// the fictitious "<Back>" entry (goes one level up), OR TipList -- a
// main.cpp-local special case that branches off into its own, main.cpp-
// local subflow instead of the generic tree stack (see TipMenuStage/
// tipCatalog further below), because the tip catalog is dynamic (no
// compile-time-fixed child list). A node ALWAYS carries all fields, even
// if only part of them is used depending on 'kind' -- the same pragmatism
// as before with SettingDescriptor (there, e.g., valueLabel was only
// populated when digitCount==1).
enum class SettingsNodeKind : uint8_t { Leaf, Branch, Back, TipList };

struct SettingsNode {
  const char* name;
  SettingsNodeKind kind;
  // Leaf:
  uint8_t digitCount;                             // 1 = enum-like (filter, gain)
  uint8_t digitCycleLen[DigitEditor::MAX_DIGITS];  // cycle length per digit position
  uint32_t maxValue;                               // clamp of the final value (see DigitEditor::assembledValue())
  const char* (*valueLabel)(uint32_t value);       // only for digitCount==1, otherwise nullptr (digits rendered directly)
  uint32_t (*getValue)();
  void (*setValue)(uint32_t value);                // called once when editing is finished
  // Branch:
  const SettingsNode* children;
  uint8_t childCount;
};

// ------------------------- Tip catalog -------------------------
// See TipCatalog.h for the invariant (never empty, 'active' always valid)
// and main.cpp::setup() for how it's produced on the very first boot.
TipCatalog tipCatalog;

// Shared completion step for every settings change: persist + re-evaluate
// calibrated (can flip immediately from a pure settings change, with no new
// measurement at all) + sync the active tip with the new state (see
// context/plan: "last-set values per tip").
void commitCurrentSettings() {
  calStore.saveSettings(currentSettings);
  calibrated = calibrationValidFor(currentSettings.optical);

  MeasurementTip* active = tipCatalog.activeTip();
  if (active) {
    active->optical = currentSettings.optical;
    calStore.saveTips(tipCatalog);
  }
}

const char* filterSettingLabel(uint32_t v) { return filterStateUiLabel(static_cast<FilterState>(v)); }
uint32_t getFilterSetting() { return static_cast<uint32_t>(currentSettings.optical.filterState); }
void setFilterSetting(uint32_t v) {
  currentSettings.optical.filterState = static_cast<FilterState>(v);
  commitCurrentSettings();
}

const char* gainSettingLabel(uint32_t v) { return gainCsvLabel(static_cast<as7341_gain_t>(v)); }
uint32_t getGainSetting() { return static_cast<uint32_t>(currentSettings.optical.sensor.gain); }
void setGainSetting(uint32_t v) {
  currentSettings.optical.sensor.gain = static_cast<as7341_gain_t>(v);
  sensorImpl.applySettings(currentSettings.optical);
  commitCurrentSettings();
}

uint32_t getATimeSetting() { return currentSettings.optical.sensor.atime; }
void setATimeSetting(uint32_t v) {
  currentSettings.optical.sensor.atime = static_cast<uint8_t>(v);
  sensorImpl.applySettings(currentSettings.optical);
  commitCurrentSettings();
}

uint32_t getAStepSetting() { return currentSettings.optical.sensor.astep; }
void setAStepSetting(uint32_t v) {
  currentSettings.optical.sensor.astep = static_cast<uint16_t>(v);
  sensorImpl.applySettings(currentSettings.optical);
  commitCurrentSettings();
}

// Children of "Sensor" -- "<Back>" is deliberately the first entry
// (see Plan). Only Leaf fields are set, Branch fields (children/childCount)
// remain 0/nullptr -- unused for Leaf/Back.
const SettingsNode SETTINGS_SENSOR[] = {
  { "<Back>", SettingsNodeKind::Back },
  { "Gain",  SettingsNodeKind::Leaf, 1, {AS7341_GAIN_COUNT}, AS7341_GAIN_COUNT - 1, gainSettingLabel, getGainSetting, setGainSetting },
  // ATIME (uint8_t, max 255): 3 decimal digits, leading digit 0-2.
  { "ATIME", SettingsNodeKind::Leaf, 3, {3, 10, 10},         255,                  nullptr,          getATimeSetting, setATimeSetting },
  // ASTEP (uint16_t, max 65535): 5 decimal digits, leading digit 0-6.
  { "ASTEP", SettingsNodeKind::Leaf, 5, {7, 10, 10, 10, 10}, 65535,                nullptr,          getAStepSetting, setAStepSetting },
};

// Children of "Optical" -- filter, sensor settings AND tips (tip
// selection affects only optical, so it belongs in here rather than at
// the root -- see Plan/context: this keeps the tree and JSON structure
// congruent).
const SettingsNode SETTINGS_OPTICAL[] = {
  { "<Back>", SettingsNodeKind::Back },
  { "Tips",   SettingsNodeKind::TipList, 0, {}, 0, nullptr, nullptr, nullptr, nullptr, 0 },
  { "Filter", SettingsNodeKind::Leaf,   1, {3}, 2, filterSettingLabel, getFilterSetting, setFilterSetting },
  { "Sensor", SettingsNodeKind::Branch, 0, {},  0, nullptr, nullptr, nullptr,
    SETTINGS_SENSOR, sizeof(SETTINGS_SENSOR) / sizeof(SETTINGS_SENSOR[0]) },
};

// Root of the settings tree. Currently only ONE entry -- deliberately
// accepted (see Plan/context), preferable to a UI level that wouldn't
// correspond to the JSON schema.
const SettingsNode SETTINGS_ROOT[] = {
  { "Optical", SettingsNodeKind::Branch, 0, {}, 0, nullptr, nullptr, nullptr,
    SETTINGS_OPTICAL, sizeof(SETTINGS_OPTICAL) / sizeof(SETTINGS_OPTICAL[0]) },
};

// Navigation state in the settings tree: a small, fixed-size stack of
// (sibling list, its length, current cursor). A level's cursor is left
// untouched when descending -- so when later ascending (Depth--) it
// automatically points again at exactly the Branch entry it was descended
// from, with no extra bookkeeping needed.
struct SettingsLevel { const SettingsNode* nodes; uint8_t count; uint8_t index; };
static const uint8_t SETTINGS_TREE_MAX_DEPTH = 4;  // root + 3 levels reserved for future use
SettingsLevel settingsStack[SETTINGS_TREE_MAX_DEPTH] = {
  { SETTINGS_ROOT, sizeof(SETTINGS_ROOT) / sizeof(SETTINGS_ROOT[0]), 0 }
};
uint8_t settingsDepth = 0;  // 0 == root -- no "<Back>" there (see Plan/context)

const SettingsNode& currentSettingsNode() {
  const SettingsLevel& lvl = settingsStack[settingsDepth];
  return lvl.nodes[lvl.index];
}

// ------------------------- Tip subflow -------------------------
// Its own small state instead of being part of the generic tree stack (see
// the SettingsNodeKind::TipList comment above) -- only 3 levels, no
// dedicated stack needed. Closed = we are NOT in the tip menu entry (the
// generic tree is then shown/operated normally). FingerprintStats hangs off
// of Detail (see renderWhiteFingerprintStats()/loop()) -- a pure display
// screen, no further cursor needed.
enum class TipMenuStage : uint8_t { Closed, List, Detail, FingerprintStats };
TipMenuStage tipMenuStage = TipMenuStage::Closed;
// 0 = <Back>, 1 = "Create new tip",
// TIP_LIST_FIXED_ENTRIES.. = tips[index-TIP_LIST_FIXED_ENTRIES].
static const uint8_t TIP_LIST_FIXED_ENTRIES = 2;
uint8_t tipListIndex = 0;
size_t  tipDetailIndex = 0;  // which tip (index into tipCatalog.tips) is shown in the detail screen
uint8_t tipActionIndex = 0;  // cursor in the detail screen's action list (see renderTipDetail())
// First visible channel in renderWhiteFingerprintStats() -- a pure
// wraparound counter (no modulo here, the renderer does that based on the
// actual channel count), advanced by a short Mode press (see cycleView()).
// Reset to 0 when the screen is entered.
uint8_t fingerprintStatsScroll = 0;

// A white measurement that passed checkValidity() but, according to
// MeasurementTip::isPlausible(), is Implausible/Indeterminate -- waits for
// an explicit user decision (see renderWhitePlausibilityConfirm()/loop())
// before being adopted (or not). 'active' = false means "no decision
// pending", the only state in which the normal Calibration screen is shown.
struct PendingWhiteDecision {
  bool active = false;
  Precision precision = Precision::Precise;
  Measurement measurement;
  MeasurementTelemetry telemetry;
  PlausibilityResult plausibility = PlausibilityResult::Indeterminate;
  // Other tips (indices into tipCatalog.tips) for which 'measurement' would
  // be plausible according to TipCatalog::rankPlausibleTips() -- computed
  // once when this decision arises (performMeasurement()), NOT recomputed
  // on every render (tipCatalog does not change while a decision is open,
  // see cycleMode()). Order = display order in
  // renderWhitePlausibilityConfirm().
  std::vector<size_t> suggestedTipIndices;
};
PendingWhiteDecision pendingWhite;
// Cursor in the selection list of renderWhitePlausibilityConfirm(): 0..
// suggestedTipIndices.size()-1 = a suggested tip, followed by "As new
// tip", "Accept", "Discard" (see there). Held as an index (not a
// bool) so a variable number of entries is possible without restructuring
// the cursor logic.
uint8_t whiteConfirmIndex = 0;

std::string nextTipName() {
  for (uint32_t n = 1; ; n++) {
    char buf[24];
    snprintf(buf, sizeof(buf), "Tip %lu", (unsigned long)n);
    if (!tipCatalog.find(buf)) return buf;
  }
}

// Registers a NEW tip with the CURRENTLY live-set OpticalSettings (no free
// text entry -- "what's currently set" is the only sensible source for a
// freshly named tip) and makes it the active one.
void createTipFromCurrentSettings() {
  MeasurementTip t;
  t.name = nextTipName();
  t.optical = currentSettings.optical;
  tipCatalog.tips.push_back(t);
  tipCatalog.active = t.name;
  commitCurrentSettings();  // persists the catalog (and harmlessly RootSettings again, see there)
}

// Applies a tip's stored filter/sensor values (hardware + RootSettings) and
// makes it the active one.
void activateTip(const MeasurementTip& tip) {
  currentSettings.optical = tip.optical;
  sensorImpl.applySettings(currentSettings.optical);
  tipCatalog.active = tip.name;
  commitCurrentSettings();
}

// Only reachable for NON-active tips (see renderTipDetail()/loop()) -- no
// special case needed for "last remaining tip": the active tip can never be
// deleted, so the catalog never becomes empty this way (see Plan/context).
void deleteTip(size_t index) {
  tipCatalog.tips.erase(tipCatalog.tips.begin() + index);
  calStore.saveTips(tipCatalog);
}

// Builds a SINGLE WhiteFingerprint from a raw white measurement -- pure
// computation, doesn't append anything (see appendWhiteFingerprint()). Must
// already be available BEFORE the "is this measurement adopted" decision,
// since MeasurementTip::isPlausible() needs exactly such a candidate. Uses
// EXCLUSIVELY the Spectrometer interface (normalize()/sensorId()), never the
// concrete AS7341Spectrometer -- prepared for a future second sensor (see
// Spectrometer.h).
WhiteFingerprint buildWhiteFingerprint(const Measurement& raw, const OpticalSettings& settings) {
  WhiteFingerprint fp;
  fp.uptimeS = uptimeLogger.totalSeconds();
  fp.sensorId = spectrometer.sensorId();
  fp.sensor = settings.sensor;
  fp.normalized = spectrometer.normalize(raw, settings.sensor);
  return fp;
}

// Appends an ALREADY-built fingerprint to a tip -- FIFO, the oldest drops
// out once the list would reach MAX_WHITE_FINGERPRINTS_PER_TIP (see
// TipCatalog.h).
void appendWhiteFingerprint(MeasurementTip& tip, const WhiteFingerprint& fp) {
  if (tip.whiteFingerprints.size() >= MAX_WHITE_FINGERPRINTS_PER_TIP) {
    tip.whiteFingerprints.erase(tip.whiteFingerprints.begin());  // oldest dropped first
  }
  tip.whiteFingerprints.push_back(fp);
}

// Editing state: as long as editingActive, Trigger/Mode hijack their
// otherwise meaning (measuring/mode switching) in favor of digit editing --
// see loop().
bool editingActive = false;
DigitEditor editor;

// ------------------------- Buttons -------------------------
DebouncedButton triggerBtn(TRIGGER_PIN, DEBOUNCE_MS, LONG_PRESS_MS);
DebouncedButton modeBtn(MODE_PIN, DEBOUNCE_MS, LONG_PRESS_MS);

// ------------------------- Serial (pure data export, see loop()) -------------------------
bool serialWasConnected = false;

// Periodic auto-refresh of the info screen (see loop()/renderInfoStatus()).
uint32_t lastInfoRenderMs = 0;

const char* displayModeLabel(DisplayMode m) {
  switch (m) {
    case DisplayMode::Calibration: return "K";  // never shown directly in practice, see activeModeLabel()
    case DisplayMode::Export:      return "E";
    case DisplayMode::Settings:    return "C";
    case DisplayMode::Info:        return "I";
    default:                       return "M";  // Measure -- never shown directly in practice, see activeModeLabel()
  }
}
const char* calibrationTargetLabel(CalibrationTarget t) {
  return (t == CalibrationTarget::White) ? "W" : "D";
}

// Returns the mode abbreviation appropriate for the current display (e.g.
// the "MEASURING" screen) -- in Calibration mode that of the currently
// selected reference (White/Dark), in Measure mode that of the currently
// selected measurement mode (see MEASUREMENT_MODES), otherwise that of the
// DisplayMode itself. Needed because showMeasuringScreen(), as a
// ProgressCallback, has a fixed signature and therefore can't directly know
// which reference calibrationTarget currently means.
const char* activeModeLabel() {
  if (currentDisplayMode == DisplayMode::Calibration) return calibrationTargetLabel(calibrationTarget);
  if (currentDisplayMode == DisplayMode::Measure) return MEASUREMENT_MODES[measurementModeIndex].cornerLabel;
  return displayModeLabel(currentDisplayMode);
}

// Appends a single CSV row to 'out'. Shared by buildHistoryCsv() (full dump)
// and printCsvRow() (live row) so both are guaranteed to produce the same
// format.
//
// Context columns (temperature/runtime/uptime/settings) for a CSV row --
// see the MeasurementRecord comment in HistoryStore.h. ctx==nullptr for the
// "*_ref" rows further below (those show the CURRENTLY loaded calibration,
// not a concrete measurement event -- for them there is no meaningful
// timestamp/temperature/settings state, so those columns stay empty there).
struct MeasurementContext {
  float tempC;
  uint32_t sessionMs;
  uint32_t uptimeS;
  OpticalSettings settings;
  // Measurement mode + precision telemetry -- see the MeasurementRecord
  // comment in HistoryStore.h. relSemWorst NAN = empty (no relSEM
  // computed), NOT "0".
  Precision precision;
  uint8_t sampleCount;
  float relSemWorst;
};

// "674/45nm" -- center/FWHM, modeled on common bandpass filter notation
// (e.g. "680/52 BrightLine"). Needed because different FilterStates deliver
// different bands -- a plain "674nm" name would no longer be unique enough.
std::string bandColumnName(const Band& b) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%d/%dnm", (int)lroundf(b.center_nm), (int)lroundf(b.fwhm_nm));
  return buf;
}

// computeSpectrum=false (for dark/white references, the "*_ref" rows
// appended further below, OR a measurement for which no matching reference
// could be resolved -- see SettingsRefState/collectBandsVisitor()/
// appendRecordToCsv()): there is no meaningful derived reflectance -- the
// spectrum/Lab/hex columns then stay empty (but present, `bandColumns.size()`
// empty columns). The raw Measurement values (if includeRaw) are unaffected
// by this.
//
// whiteForSpectrum/darkForSpectrum: the reference that applies to THIS row --
// explicitly determined by the caller (loaded live for the current row/the
// "*_ref" special rows, or resolved from history for a past row, see
// SettingsRefState) instead of blindly reading the currently loaded global
// reference here. Only relevant/needed when computeSpectrum is true.
void appendCsvRow(std::string& out, const char* label, const Measurement& measurement,
                   FilterState filterState, bool computeSpectrum, bool includeRaw,
                   const std::vector<Band>& bandColumns,
                   const MeasurementContext* ctx = nullptr,
                   const Measurement* whiteForSpectrum = nullptr,
                   const Measurement* darkForSpectrum = nullptr) {
  out += label;
  char buf[16];

  if (ctx) {
    snprintf(buf, sizeof(buf), ",%.1f", ctx->tempC); out += buf;
    snprintf(buf, sizeof(buf), ",%.1f", ctx->sessionMs / 1000.0f); out += buf;
    snprintf(buf, sizeof(buf), ",%lu", (unsigned long)ctx->uptimeS); out += buf;
    out += ',';
    out += filterStateCsvLabel(ctx->settings.filterState);
    out += ',';
    out += gainCsvLabel(ctx->settings.sensor.gain);
    snprintf(buf, sizeof(buf), ",%u", ctx->settings.sensor.atime); out += buf;
    snprintf(buf, sizeof(buf), ",%u", ctx->settings.sensor.astep); out += buf;
    out += ',';
    out += precisionCsvLabel(ctx->precision);
    snprintf(buf, sizeof(buf), ",%u", ctx->sampleCount); out += buf;
    out += ',';
    if (!isnan(ctx->relSemWorst)) { snprintf(buf, sizeof(buf), "%.5f", ctx->relSemWorst); out += buf; }
  } else {
    out += ",,,,,,,,,,";
  }

  if (computeSpectrum) {
    Spectrum spec = spectrometer.getSpectrum(measurement, *whiteForSpectrum, *darkForSpectrum, filterState);
    for (const Band& col : bandColumns) {
      out += ',';
      int idx = -1;
      for (size_t k = 0; k < spec.bands.size(); k++) {
        if (spec.bands[k].center_nm == col.center_nm && spec.bands[k].fwhm_nm == col.fwhm_nm) { idx = (int)k; break; }
      }
      if (idx >= 0) { snprintf(buf, sizeof(buf), "%.4f", spec.values[idx]); out += buf; }
    }
    Lab lab = getColor(spec);
    LCh lch = labToLCh(lab);
    uint8_t r, g, b;
    labToSRGB255(lab, r, g, b);
    snprintf(buf, sizeof(buf), ",%.2f", lab.L); out += buf;
    snprintf(buf, sizeof(buf), ",%.2f", lab.a); out += buf;
    snprintf(buf, sizeof(buf), ",%.2f", lab.b); out += buf;
    snprintf(buf, sizeof(buf), ",%.2f", lch.C); out += buf;
    snprintf(buf, sizeof(buf), ",%.2f", lch.h); out += buf;
    snprintf(buf, sizeof(buf), ",#%02X%02X%02X", r, g, b); out += buf;
  } else {
    for (size_t i = 0; i < bandColumns.size(); i++) out += ',';
    out += ",,,,,,";
  }

  if (includeRaw) {
    for (size_t i = 0; i < measurement.size(); i++) {
      out += ',';
      snprintf(buf, sizeof(buf), "%.1f", measurement[i]);
      out += buf;
    }
  }
  out += '\n';
}

// Mapping of settings -> most recently measured dark/white reference FOR
// them, maintained during a forEach() pass (chronologically ascending).
// Replaces the former "take the CURRENTLY loaded reference" approach for
// export: a historical row thereby gets the reference that ACTUALLY applied
// at its recording time (the last dark/white row measured BEFORE it WITH
// THE SAME settings), regardless of what has since been newly measured
// (e.g. after a tip change). Linear scan instead of a map: the number of
// distinct settings combinations in a history is always small (typically
// <= the number of tips).
struct SettingsRefState {
  OpticalSettings settings;
  Measurement dark, white;  // each empty if never measured for 'settings'
};

// Updates 'states' with a dark/white row (no effect for Regular). NO
// starting seed from the live-loaded reference (deliberately -- an earlier
// version had one: it ALWAYS seeded with the darkRef/whiteRef current at
// EXPORT time, which would have silently evaluated measurements taken in
// between, after a later reference change, against the WRONG, namely the
// NEWER, reference. historyStore.clear() has since kept the last dark/white
// row itself (see there), which fully covers the normal case. Without any
// matching row, affected measurements now deliberately stay WITHOUT
// reflectance/Lab/hex in the export -- visibly missing raw-data-only rows
// instead of a secretly wrong calculation.
void observeReference(std::vector<SettingsRefState>& states, const MeasurementRecord& rec) {
  if (rec.kind != SampleKind::Dark && rec.kind != SampleKind::White) return;
  for (SettingsRefState& s : states) {
    if (s.settings == rec.settings) {
      (rec.kind == SampleKind::Dark ? s.dark : s.white) = rec.measurement;
      return;
    }
  }
  SettingsRefState s{rec.settings, {}, {}};
  (rec.kind == SampleKind::Dark ? s.dark : s.white) = rec.measurement;
  states.push_back(s);
}

// Reference that currently applies to 'settings' given the present state of
// 'states' -- nullptr if nothing has ever been measured for it.
const SettingsRefState* findRefState(const std::vector<SettingsRefState>& states, const OpticalSettings& settings) {
  for (const SettingsRefState& s : states) if (s.settings == settings) return &s;
  return nullptr;
}

// HistoryStore::forEach() visitor: collects the union of all occurring
// bands (only from records for which a matching reference can be resolved
// -- all others wouldn't get real spectrum columns anyway, so their bands
// don't "earn" a header column).
struct BandCollectCtx {
  std::vector<Band>* cols;
  std::vector<SettingsRefState> refState;
};
void collectBandsVisitor(const MeasurementRecord& rec, void* userData) {
  BandCollectCtx* c = reinterpret_cast<BandCollectCtx*>(userData);
  observeReference(c->refState, rec);
  if (rec.kind != SampleKind::Regular) return;
  const SettingsRefState* ref = findRefState(c->refState, rec.settings);
  if (!ref || ref->dark.empty() || ref->white.empty()) return;
  Spectrum spec = spectrometer.getSpectrum(rec.measurement, ref->white, ref->dark, rec.settings.filterState);
  for (const Band& b : spec.bands) {
    bool known = false;
    for (const Band& existing : *c->cols) {
      if (existing.center_nm == b.center_nm && existing.fwhm_nm == b.fwhm_nm) { known = true; break; }
    }
    if (!known) c->cols->push_back(b);
  }
}

// HistoryStore::forEach() visitor: appends a persisted record via
// appendCsvRow() to 'out' -- the reflectance is computed against the
// reference that, according to SettingsRefState, applied at the RECORDING
// TIME of this row (not against the currently loaded one). Rows for which
// no matching reference can (yet) be resolved get no derived columns
// (computeSpectrum=false), but keep their raw values.
struct CsvBuildCtx {
  std::string* out;
  bool includeRaw;
  const std::vector<Band>* bandColumns;
  std::vector<SettingsRefState> refState;
};
void appendRecordToCsv(const MeasurementRecord& rec, void* userData) {
  CsvBuildCtx* ctx = reinterpret_cast<CsvBuildCtx*>(userData);
  observeReference(ctx->refState, rec);
  bool isRef = (rec.kind != SampleKind::Regular);
  const SettingsRefState* ref = isRef ? nullptr : findRefState(ctx->refState, rec.settings);
  bool computeSpectrum = ref && !ref->dark.empty() && !ref->white.empty();
  MeasurementContext mctx{ rec.tempC, rec.sessionMs, rec.uptimeS, rec.settings,
                           rec.precision, rec.sampleCount, rec.relSemWorst };
  appendCsvRow(*ctx->out, rec.label, rec.measurement, rec.settings.filterState, computeSpectrum,
               ctx->includeRaw, *ctx->bandColumns, &mctx,
               computeSpectrum ? &ref->white : nullptr, computeSpectrum ? &ref->dark : nullptr);
}

// Builds the complete measurement history as CSV. includeRaw additionally
// appends the raw Measurement columns (column names from
// measurementLabels() -- the only place in the code that uses these
// labels). The currently valid calibration reference is always sent along
// as its own row (only in Debug/raw mode, otherwise there'd be nothing
// meaningful to show), even if it wasn't freshly measured in this session
// but loaded from flash.
//
// Two passes through the history: the first determines the union of all
// occurring bands (different FilterStates can structurally deliver
// different bands -- a fixed column count is no longer possible), the
// second emits the actual rows.
std::string buildHistoryCsv(bool includeRaw) {
  std::string out;

  std::vector<Band> bandColumns;
  BandCollectCtx collectCtx{ &bandColumns, {} };
  historyStore.forEach(collectBandsVisitor, &collectCtx);
  std::sort(bandColumns.begin(), bandColumns.end(), [](const Band& a, const Band& b) {
    return a.center_nm < b.center_nm;
  });

  out += "label,temp_c,session_s,uptime_s,filter,gain,atime,astep,measurement_mode,sample_count,rel_sem_worst";
  for (const Band& b : bandColumns) {
    out += ',';
    out += bandColumnName(b);
  }
  out += ",L,a,b,C,h,hex";
  if (includeRaw) {
    const char* const* labels = spectrometer.measurementLabels();
    size_t n = historyStore.firstRecordChannelCount();
    for (size_t i = 0; i < n; i++) { out += ','; out += labels[i]; }
  }
  out += '\n';

  // Cumulative uptime (aging tracking for the illumination LED that stays
  // continuously on) -- always sent along, even in the "Normal" export,
  // since it's just a single value and requires no calibration/reflectance.
  char uptimeLine[32];
  snprintf(uptimeLine, sizeof(uptimeLine), "uptime_seconds,%lu\n",
           (unsigned long)uptimeLogger.totalSeconds());
  out += uptimeLine;
  snprintf(uptimeLine, sizeof(uptimeLine), "measurement_count,%lu\n",
           (unsigned long)uptimeLogger.measurementCount());
  out += uptimeLine;

  CsvBuildCtx ctx{ &out, includeRaw, &bandColumns, {} };
  historyStore.forEach(appendRecordToCsv, &ctx);
  return out;
}

// Live row after every single measurement. Uses the same appendCsvRow() as
// the full dump, ALWAYS with raw values over USB (see the ExportPage
// comment further above). Needs NO column union (just one row, nothing to
// reconcile it with) -- simply uses its own bands. Can therefore have a
// different number/naming of columns than other rows, depending on
// filter status/calibration match -- an unavoidable consequence of the
// filter dependency, not a bug.
void printCsvRow(const MeasurementRecord& rec) {
  bool isRef = (rec.kind != SampleKind::Regular);
  bool computeSpectrum = !isRef && calibrationValidFor(rec.settings);
  std::vector<Band> bandColumns;
  if (computeSpectrum) {
    bandColumns = spectrometer.getSpectrum(rec.measurement, whiteRef, darkRef, rec.settings.filterState).bands;
  }
  std::string row;
  MeasurementContext ctx{ rec.tempC, rec.sessionMs, rec.uptimeS, rec.settings,
                          rec.precision, rec.sampleCount, rec.relSemWorst };
  appendCsvRow(row, rec.label, rec.measurement, rec.settings.filterState, computeSpectrum,
               /*includeRaw=*/true, bandColumns, &ctx,
               computeSpectrum ? &whiteRef : nullptr, computeSpectrum ? &darkRef : nullptr);
  Serial.print(row.c_str());
}

// Its own simple status display for export mode -- needs no Spectrometer,
// which is why it doesn't fit DisplayViews.cpp (which deliberately only
// lets views work against the sensor abstraction).
void renderExportStatus() {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("Export mode");

  display.setCursor(0, 10);
  display.println(exportPage == ExportPage::Debug ? "Mode: Debug" : "Mode: Normal");

  display.setCursor(0, 20);
  if (!bleExporter.isActive()) {
    display.println("Trigger: BLE on");
  } else if (bleExporter.isConnected()) {
    display.println("BLE: connected");
  } else {
    display.println("BLE: waiting...");
  }

  char line[24];
  snprintf(line, sizeof(line), "%u measurements", (unsigned)historyStore.count());
  display.setCursor(0, 30);
  display.println(line);

  if (exportSending) {
    display.setCursor(0, 40);
    display.println("sending...");
    int barX = 4, barY = 50, barW = OLED_WIDTH - 8, barH = 10;
    display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
    int fillW = exportTotalBytes
                  ? (int)((uint64_t)exportSentBytes * (barW - 2) / exportTotalBytes)
                  : 0;
    if (fillW > 0) display.fillRect(barX + 1, barY + 1, fillW, barH - 2, SSD1306_WHITE);
  } else if (exportHint) {
    display.setCursor(0, 42);
    display.println("No phone");
    display.println("connected!");
  } else if (exportSentOk) {
    display.setTextSize(2);
    display.setCursor(0, 46);
    display.println("sent");
  }

  display.display();
}

// Shared screen for Calibration mode (white OR dark reference, depending on
// calibrationTarget -- a short Mode press switches between them): shows the
// selected reference as a raw Measurement (labeled via measurementLabels())
// instead of presenting it as a calibrated reflectance -- since getSpectrum()
// now takes explicit references instead of a stored calibration, there is no
// longer a meaningful reflectance for "the reference against itself" (see
// the appendCsvRow() comment).
//
// If the stored reference's filter does NOT match the currently selected one
// (see calibrationValidFor()), it counts as not present here -- exactly the
// same behavior as when measuring/exporting, no special handling (no
// warning hint, it's simply not displayed).
void renderReferenceStatus() {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  bool isWhite = (calibrationTarget == CalibrationTarget::White);
  display.setCursor(0, 0);
  display.println(isWhite ? "White reference" : "Dark reference");

  const Measurement& ref = isWhite ? whiteRef : darkRef;
  const OpticalSettings& refSettings = isWhite ? whiteRefSettings : darkRefSettings;
  bool refValid = !ref.empty() && (refSettings == currentSettings.optical);

  if (!refValid) {
    display.setCursor(0, 16);
    display.println("no measurement");
    display.println("Hold trigger");
  } else {
    // Deliberately WITHOUT a filter/gain/ATIME/ASTEP summary here -- it took
    // up too much space and offered little added value for this screen (the
    // matching itself still happens invisibly via refValid above).
    const char* const* labels = spectrometer.measurementLabels();
    char line[27];
    int y = 16;
    size_t i = 0;
    for (; i + 1 < ref.size(); i += 2) {
      snprintf(line, sizeof(line), "%-4.4s %5.0f %-4.4s %5.0f",
               labels[i], ref[i], labels[i + 1], ref[i + 1]);
      display.setCursor(0, y);
      display.print(line);
      y += 8;
    }
    if (i < ref.size()) {
      snprintf(line, sizeof(line), "%-4.4s %5.0f", labels[i], ref[i]);
      display.setCursor(0, y);
      display.print(line);
    }
  }

  display.display();
}

// Pure status screen for info mode -- uptime in hh:mm format (hour portion
// deliberately not limited to 2 digits, since it can well become three
// digits over the device's lifetime), the lifetime measurement counter from
// UptimeLogger, as well as the current ESP32 die temperature (no substitute
// for a real LED temperature measurement, but a tangible clue for later
// analysis of unexplained deviations). Automatically redrawn every 10s
// while staying in this mode (see loop()), so temperature/uptime visibly
// keep updating.
void renderInfoStatus() {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("Info");

  uint32_t totalSec = uptimeLogger.totalSeconds();
  uint32_t hh = totalSec / 3600;
  uint32_t mm = (totalSec % 3600) / 60;
  char line[24];
  snprintf(line, sizeof(line), "Uptime: %lu:%02lu", (unsigned long)hh, (unsigned long)mm);
  display.setCursor(0, 20);
  display.println(line);

  snprintf(line, sizeof(line), "Measurements: %lu", (unsigned long)uptimeLogger.measurementCount());
  display.setCursor(0, 32);
  display.println(line);

  snprintf(line, sizeof(line), "Temp: %.1f C", temperatureRead());
  display.setCursor(0, 44);
  display.println(line);

  display.display();
}

// Third "page" of export mode: deletes the persisted history, but only
// after a LONG trigger press (see loop()) -- a short press here
// deliberately does nothing, hence the hint text. After deletion, the same
// page immediately shows "0 measurements" -- that is the success feedback.
void renderExportClear() {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("Clear history?");

  char line[24];
  snprintf(line, sizeof(line), "%u measurements", (unsigned)historyStore.count());
  display.setCursor(0, 16);
  display.println(line);

  display.setCursor(0, 32);
  display.println("Hold trigger");
  display.setCursor(0, 42);
  display.println("to clear");

  display.display();
}

// Status screen for settings mode: name of the setting currently selected
// via a short Mode press (see the SETTINGS registry further above), large
// value text underneath -- either in browsing state (no cursor) or while
// editing (active digit/option shown inverted, see editingActive/editor).
// Start index of a scroll window of 'visible' rows that shows 'selected'
// within [0, count) as centered as possible (clamps at the list's edges).
// Shared by renderSettingsStatus() (tree navigation) and renderTipList()
// (tip list).
uint8_t computeScrollStart(uint8_t selected, uint8_t count, uint8_t visible) {
  if (count <= visible) return 0;
  int start = (int)selected - visible / 2;
  if (start < 0) start = 0;
  if (start > (int)count - visible) start = (int)count - visible;
  return (uint8_t)start;
}

// Forward declarations: defined below (after renderSettingsStatus(), closer
// to their main.cpp-local tip state), but already needed here.
void renderTipList();
void renderTipDetail();
void renderWhiteFingerprintStats();

// Forward declaration: defined below (closer to performMeasurement(), which
// populates the associated pendingWhite state), but already needed by
// renderCurrentView().
void renderWhitePlausibilityConfirm();

void renderSettingsStatus() {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  if (tipMenuStage == TipMenuStage::List)   { renderTipList();   return; }
  if (tipMenuStage == TipMenuStage::Detail) { renderTipDetail(); return; }
  if (tipMenuStage == TipMenuStage::FingerprintStats) { renderWhiteFingerprintStats(); return; }

  if (editingActive) {
    // Digit/option editing of a leaf -- UNCHANGED compared to the earlier
    // flat list, just reads the current node now from the tree instead of
    // from SETTINGS[currentSettingIndex].
    const SettingsNode& s = currentSettingsNode();
    display.setCursor(0, 0);
    display.println("Settings");
    display.setCursor(0, 20);
    display.print(s.name);
    display.println(":");

    display.setTextSize(2);
    if (s.digitCount == 1) {
      // Enum-like: the only "digit" is the entire option value -- render it
      // inverted as a whole (only one position, always active).
      const char* label = s.valueLabel(editor.digitAt(0));
      int16_t x1, y1;
      uint16_t w, h;
      display.getTextBounds(label, 0, 34, &x1, &y1, &w, &h);
      display.fillRect(0, 34, w + 4, h + 4, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
      display.setCursor(2, 36);
      display.println(label);
      display.setTextColor(SSD1306_WHITE);
    } else {
      // Multi-digit: draw each digit individually, the one at the cursor inverted.
      const int digitW = 14;
      int x = 0;
      for (uint8_t i = 0; i < editor.digitCount(); i++) {
        char ch[2] = { (char)('0' + editor.digitAt(i)), '\0' };
        if (i == editor.cursor()) {
          display.fillRect(x, 34, digitW, 18, SSD1306_WHITE);
          display.setTextColor(SSD1306_BLACK);
          display.setCursor(x + 3, 36);
          display.print(ch);
          display.setTextColor(SSD1306_WHITE);
        } else {
          display.setCursor(x + 3, 36);
          display.print(ch);
        }
        x += digitW;
      }
    }
    display.display();
    return;
  }

  // Tree navigation (not editing): list of all siblings of the current
  // level with a ">" cursor. Title is "Settings" at the root,
  // otherwise the name of the branch node that was descended from
  // (breadcrumb). If not all siblings fit on the display, a scroll window
  // of VISIBLE_ROWS rows keeps the cursor as centered as possible (clamps
  // at the start/end of the list).
  const SettingsLevel& lvl = settingsStack[settingsDepth];
  display.setCursor(0, 0);
  if (settingsDepth == 0) {
    display.println("Settings");
  } else {
    const SettingsLevel& parent = settingsStack[settingsDepth - 1];
    display.println(parent.nodes[parent.index].name);
  }

  const uint8_t VISIBLE_ROWS = 4;
  uint8_t start = computeScrollStart(lvl.index, lvl.count, VISIBLE_ROWS);

  int y = 16;
  for (uint8_t i = start; i < start + VISIBLE_ROWS && i < lvl.count; i++) {
    const SettingsNode& n = lvl.nodes[i];
    char line[22];
    if (n.kind == SettingsNodeKind::Leaf) {
      char val[10];
      if (n.valueLabel) strncpy(val, n.valueLabel(n.getValue()), sizeof(val));
      else snprintf(val, sizeof(val), "%lu", (unsigned long)n.getValue());
      val[sizeof(val) - 1] = '\0';
      snprintf(line, sizeof(line), "%-10.10s%s", n.name, val);
    } else if (n.kind == SettingsNodeKind::Branch || n.kind == SettingsNodeKind::TipList) {
      // Both go one level deeper via a long trigger press (TipList into the
      // tip subflow instead of the generic tree) -- visually
      // indistinguishable, ">" indicates "leads further in".
      snprintf(line, sizeof(line), "%-10.10s>", n.name);
    } else {
      snprintf(line, sizeof(line), "%s", n.name);
    }
    display.setCursor(0, y);
    display.print(i == lvl.index ? "> " : "  ");
    display.println(line);
    y += 10;
  }
  // Small scroll hint, only if there actually are more siblings outside the
  // window -- top right next to the title, bottom right below the last
  // row (non-overlapping there, see the layout above).
  if (start > 0) {
    display.setCursor(122, 0);
    display.print("^");
  }
  if (start + VISIBLE_ROWS < lvl.count) {
    display.setCursor(122, 56);
    display.print("v");
  }

  display.display();
}

// List of the tip menu entry: "<Back>", "Create new tip", then all
// tips (the active one with an " ACTIVE" suffix). Same scroll window as the
// generic tree view (see computeScrollStart()).
void renderTipList() {
  display.setCursor(0, 0);
  display.println("Tips");

  uint8_t total = (uint8_t)(TIP_LIST_FIXED_ENTRIES + tipCatalog.tips.size());
  uint8_t start = computeScrollStart(tipListIndex, total, 4);

  int y = 16;
  for (uint8_t i = start; i < start + 4 && i < total; i++) {
    char line[22];
    if (i == 0) {
      snprintf(line, sizeof(line), "<Back>");
    } else if (i == 1) {
      snprintf(line, sizeof(line), "Create new tip");
    } else {
      const MeasurementTip& t = tipCatalog.tips[i - TIP_LIST_FIXED_ENTRIES];
      snprintf(line, sizeof(line), "%s%s", t.name.c_str(),
               (t.name == tipCatalog.active) ? " ACTIVE" : "");
    }
    display.setCursor(0, y);
    display.print(i == tipListIndex ? "> " : "  ");
    display.println(line);
    y += 10;
  }
  if (start > 0) {
    display.setCursor(122, 0);
    display.print("^");
  }
  if (start + 4 < total) {
    display.setCursor(122, 56);
    display.print("v");
  }

  display.display();
}

// Detail menu of a single tip: name (+ "ACTIVE" hint), below it the
// available actions -- "Activate"/"Delete" only for NON-active tips
// (see Plan/context: the active tip cannot be deleted, "Activate" would
// be a no-op for it anyway), "White fingerprint" (see
// renderWhiteFingerprintStats()) for BOTH. Never more than 4 rows, no
// scroll window needed.
void renderTipDetail() {
  const MeasurementTip& tip = tipCatalog.tips[tipDetailIndex];
  bool isActive = (tip.name == tipCatalog.active);

  display.setCursor(0, 0);
  display.println(tip.name.c_str());
  if (isActive) {
    display.setCursor(0, 10);
    display.println("ACTIVE");
  }

  static const char* const ACTIVE_ACTIONS[]   = { "<Back>", "White fingerprint" };
  static const char* const INACTIVE_ACTIONS[] = { "<Back>", "Activate", "Delete", "White fingerprint" };
  const char* const* actions = isActive ? ACTIVE_ACTIONS : INACTIVE_ACTIONS;
  uint8_t count = isActive ? 2 : 4;

  int y = 24;
  for (uint8_t i = 0; i < count; i++) {
    display.setCursor(0, y);
    display.print(i == tipActionIndex ? "> " : "  ");
    display.println(actions[i]);
    y += 10;
  }
  display.display();
}

// Splits 'value' (>= 0) into a normalized mantissa [1,10) + exponent for a
// compact scientific display (see renderWhiteFingerprintStats() -- the
// normalized fingerprint values span many orders of magnitude, a fixed
// decimal format makes small channels unreadable/"0.0"). value <= 0 leaves
// mantissa/exponent at 0 (normalized raw values are never negative; an
// exact 0 is the only special case, e.g. a channel that's structurally
// dark under a given filter).
static void splitScientific(float value, float& mantissa, int& exponent) {
  if (value <= 0.0f) { mantissa = 0.0f; exponent = 0; return; }
  exponent = (int)floorf(log10f(value));
  mantissa = value / powf(10.0f, (float)exponent);
  // Rounding edge case: displayed with 2 decimal places, e.g. 9.996 would
  // round to "10.00" and thereby leave the mantissa interval [1,10) --
  // bump the exponent in this case.
  if (mantissa >= 9.995f) { mantissa /= 10.0f; exponent += 1; }
}

// "White fingerprint" action from renderTipDetail(): mean + absolute
// standard error per channel of THIS tip's own fingerprints, only over the
// currently active sensor (see MeasurementTip::whiteFingerprintStats() --
// other sensors are not comparable, see MeasurementTip::isPlausible()).
// Scientific notation per channel (see splitScientific()) instead of a
// fixed decimal format -- value AND error share the same exponent (both are
// by definition the same order of magnitude), directly comparable without
// writing out the same exponent twice. N_CH(10) channels don't fit at once
// on the 64px display (one row per channel) -- a short Mode press shifts
// the visible window one channel further (see cycleView()), wrapping
// around. Pure display screen (no cursor); a long trigger press goes back
// to the detail menu (see loop()).
void renderWhiteFingerprintStats() {
  const MeasurementTip& tip = tipCatalog.tips[tipDetailIndex];
  WhiteFingerprintStats stats = tip.whiteFingerprintStats(spectrometer.sensorId());

  char header[22];
  snprintf(header, sizeof(header), "N=%u", (unsigned)stats.n);
  display.setCursor(0, 0);
  display.println(header);

  if (stats.n == 0) {
    display.setCursor(0, 10);
    display.println("no measurements");
    display.display();
    return;
  }

  const char* const* labels = spectrometer.measurementLabels();
  const uint8_t VISIBLE_ROWS = 7;  // row 0 is the "N=" header, the rest up to 64px
  uint8_t ch = (uint8_t)stats.mean.size();
  uint8_t start = fingerprintStatsScroll % ch;

  int y = 8;
  for (uint8_t i = 0; i < VISIBLE_ROWS && i < ch; i++) {
    uint8_t c = (start + i) % ch;
    float mantissa;
    int exponent;
    splitScientific(stats.mean[c], mantissa, exponent);

    char line[22];
    if (stats.sem.empty()) {
      // Only 1 fingerprint -- standard error not defined (see the
      // WhiteFingerprintStats comment), only the mean is shown.
      snprintf(line, sizeof(line), "%-4.4s %.2fe%d", labels[c], mantissa, exponent);
    } else {
      // Error on the SAME scale as the value (same exponent, see the
      // function comment). Byte 0xF1 in the built-in 5x7 font (codepage-437
      // position, see Adafruit_GFX glcdfont.c) is supposed to be a "+-"
      // character -- but on the actual display it looks like ">" over "="
      // (the font variant deviates), so the unambiguous ASCII "+-" (2
      // characters) is used here instead, despite costing one extra column
      // of space -- still fits comfortably within the 21-character row
      // budget.
      float errMantissa = stats.sem[c] / powf(10.0f, (float)exponent);
      snprintf(line, sizeof(line), "%-4.4s %.2f+-%.2fe%d", labels[c], mantissa, errMantissa, exponent);
    }
    display.setCursor(0, y);
    display.print(line);
    y += 8;
  }
  display.display();
}

// Selection page of the Measure DisplayMode: list of all MEASUREMENT_MODES
// with a ">" cursor on measurementModeIndex (moved by a short Mode press,
// see cycleView()). A trigger press fires the marked mode (see loop()).
void renderMeasureSelect() {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Measurement mode");
  int y = 16;
  for (uint8_t i = 0; i < MEASUREMENT_MODE_COUNT; i++) {
    display.setCursor(0, y);
    display.print(i == measurementModeIndex ? "> " : "  ");
    display.println(MEASUREMENT_MODES[i].name);
    y += 10;
  }
  display.display();
}

void renderCurrentView() {
  if (!displayOk) return;
  if (currentDisplayMode == DisplayMode::Measure && measurePage == MeasurePage::Select) {
    renderMeasureSelect();
    return;
  }
  if (currentDisplayMode == DisplayMode::Export) {
    if (exportPage == ExportPage::Clear) {
      renderExportClear();
    } else {
      renderExportStatus();
    }
    return;
  }
  if (currentDisplayMode == DisplayMode::Calibration) {
    if (pendingWhite.active) { renderWhitePlausibilityConfirm(); return; }
    renderReferenceStatus();
    return;
  }
  if (currentDisplayMode == DisplayMode::Settings) {
    renderSettingsStatus();
    return;
  }
  if (currentDisplayMode == DisplayMode::Info) {
    renderInfoStatus();
    return;
  }

  // Effective reference against lastMeasurementSettings (NOT against
  // currentSettings!) -- a raw reading was physically recorded with a
  // particular filter/gain/ATIME/ASTEP, a later change in settings mode
  // doesn't retroactively make it "measured with different settings".
  // However, as long as there is NO measurement at all yet (lastMeasurement
  // empty), there's nothing to freeze -- lastMeasurementSettings would then
  // still sit at its boot defaults, which would falsely register as a
  // mismatch against a calibration that's actually valid for OTHER settings
  // ("not calibrated" right after startup, even though a matching
  // reference is stored). In this case, therefore, check against
  // currentSettings instead (the actually relevant question: "would a
  // measurement started RIGHT NOW be validly calibrated").
  const OpticalSettings calCheckSettings = lastMeasurement.empty() ? currentSettings.optical : lastMeasurementSettings;
  bool haveMatchingCal = calibrationValidFor(calCheckSettings);
  static const Measurement emptyRef;
  const Measurement& effDark  = haveMatchingCal ? darkRef  : emptyRef;
  const Measurement& effWhite = haveMatchingCal ? whiteRef : emptyRef;

  // Only reached for Measure/Result from here on (see the special cases
  // above) -- the corner abbreviation is therefore always that of the
  // currently selected measurement mode, not the generic DisplayMode
  // abbreviation.
  ViewContext ctx{ spectrometer, lastMeasurement, effWhite, effDark, haveMatchingCal,
                   MEASUREMENT_MODES[measurementModeIndex].cornerLabel, lastLabel, calCheckSettings.filterState };
  VIEW_RENDERERS[static_cast<uint8_t>(currentView)](display, ctx);
}

// Can be PASSED to performMeasurement() as a ProgressCallback (same
// signature) and also called directly to already show the screen before
// the first sample.
void showMeasuringScreen(uint8_t current, uint8_t maxEstimate) {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(4, 8);
  display.println("MEASURING");
  display.setTextSize(1);
  display.setCursor(0, 30);
  display.print("Mode: ");
  display.println(activeModeLabel());

  int barX = 4, barY = 44, barW = OLED_WIDTH - 8, barH = 10;
  display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
  uint8_t capped = (current > maxEstimate) ? maxEstimate : current;
  int fillW = (maxEstimate > 0) ? (int)((uint32_t)capped * (barW - 2) / maxEstimate) : 0;
  if (fillW > 0) display.fillRect(barX + 1, barY + 1, fillW, barH - 2, SSD1306_WHITE);
  display.display();
}

// Progress throttling for the BLE export: performMeasurement() calls its
// ProgressCallback only ~8-16 times, whereas a BLE transfer can need
// 50-200+ chunks -- unthrottled, every chunk would trigger a full I2C
// display redraw and noticeably slow down the export.
void onExportProgress(size_t sent, size_t total) {
  static uint32_t lastRedrawMs = 0;
  uint32_t now = millis();
  if (sent < total && (now - lastRedrawMs) < 150) return;  // ~7 Hz
  lastRedrawMs = now;
  exportSentBytes = sent;
  exportTotalBytes = total;
  renderExportStatus();
}

// Brief visual feedback for a button press that actually triggers something.
void flashBorder() {
  if (!displayOk) return;
  display.drawRect(0, 0, OLED_WIDTH, OLED_HEIGHT, SSD1306_WHITE);
  display.display();
  delay(80);
}

// Sticky screen (same pattern as renderMeasurementError()): a white
// measurement was never even started because no matching dark reference
// exists yet for the CURRENT settings -- otherwise checkValidity() couldn't
// test for separation (limit-of-detection test against dark, see there), a
// white reference would then always have been accepted regardless of its
// actual quality. Deliberately requires the same settings as
// calibrationValidFor() -- a dark reference from a DIFFERENT gain/ATIME/
// ASTEP combination wouldn't be comparable anyway.
void renderNeedDarkFirstWarning() {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Dark reference");
  display.println("missing for these");
  display.println("settings");
  display.display();
}

// Its own screen for a failed measurement attempt (empty Measurement from
// spectrometer.performMeasurement()) -- stays up until the next action
// (another trigger/Mode press) triggers a regular re-render, the same
// pattern as e.g. the "sent"/"No phone connected" feedback in export
// mode. Applies equally to all performMeasurement() callers
// (Measure/Calibration) -- the cause is relevant regardless.
void renderMeasurementError(MeasurementStatus status) {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("Error");

  display.setCursor(0, 20);
  if (status == MeasurementStatus::NotConverged) {
    display.println("Measurement too");
    display.println("noisy");
    display.println("Hold device still");
    display.println("and try again");
  } else {
    display.println("Sensor not");
    display.println("responding");
  }

  display.display();
}

// Sticky screen (like renderMeasurementError()) for a white measurement
// that fails spectrometer.checkValidity() -- shows the raw channel values
// (same layout as renderReferenceStatus()) plus the reason. whiteRef/
// history/tip catalog are NOT touched for this (see performMeasurement()).
void renderWhiteValidityWarning(const Measurement& raw, const MeasurementValidity& v) {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("White invalid");
  display.setCursor(0, 9);
  if (v.anyClipping && v.anyBelowNoiseFloor) display.println("clipping + too dark");
  else if (v.anyClipping) display.println("Channel clipping");
  else display.println("Channel too dark");

  const char* const* labels = spectrometer.measurementLabels();
  char line[27];
  int y = 20;
  size_t i = 0;
  for (; i + 1 < raw.size(); i += 2) {
    snprintf(line, sizeof(line), "%-4.4s %5.0f %-4.4s %5.0f",
             labels[i], raw[i], labels[i + 1], raw[i + 1]);
    display.setCursor(0, y);
    display.print(line);
    y += 8;
  }
  if (i < raw.size()) {
    snprintf(line, sizeof(line), "%-4.4s %5.0f", labels[i], raw[i]);
    display.setCursor(0, y);
    display.print(line);
  }
  display.display();
}

// A white measurement that passed checkValidity() but, according to
// MeasurementTip::isPlausible(), is Implausible or Indeterminate (see
// pendingWhite). User decides via a selection list (cursor:
// whiteConfirmIndex, a short Mode press moves it, a long trigger press
// selects -- see loop()): first all suggested OTHER tips for which the
// measurement would be plausible (see pendingWhite.suggestedTipIndices/
// TipCatalog::rankPlausibleTips()), then "As new tip", "Accept",
// "Discard". Same scroll scheme as renderTipList() (computeScrollStart(),
// 4 visible rows, "^"/"v" indicators as needed).
void renderWhitePlausibilityConfirm() {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (pendingWhite.plausibility == PlausibilityResult::Implausible) {
    display.println("White differs");
    display.setCursor(0, 9);
    display.println("from old values");
  } else {
    display.println("White unclear");
    display.setCursor(0, 9);
    display.println("not enough data");
  }

  size_t sCount = pendingWhite.suggestedTipIndices.size();
  uint8_t total = (uint8_t)(sCount + 3);  // suggestions + "As new tip" + Accept + Discard
  const uint8_t VISIBLE_ROWS = 4;
  uint8_t start = computeScrollStart(whiteConfirmIndex, total, VISIBLE_ROWS);

  int y = 24;
  for (uint8_t i = start; i < start + VISIBLE_ROWS && i < total; i++) {
    char line[22];
    if (i < sCount) {
      snprintf(line, sizeof(line), "%s", tipCatalog.tips[pendingWhite.suggestedTipIndices[i]].name.c_str());
    } else if (i == sCount) {
      snprintf(line, sizeof(line), "As new tip");
    } else if (i == sCount + 1) {
      snprintf(line, sizeof(line), "Accept");
    } else {
      snprintf(line, sizeof(line), "Discard");
    }
    display.setCursor(0, y);
    display.print(i == whiteConfirmIndex ? "> " : "  ");
    display.println(line);
    y += 10;
  }
  if (start > 0) {
    display.setCursor(122, 0);
    display.print("^");
  }
  if (start + VISIBLE_ROWS < total) {
    display.setCursor(122, 56);
    display.print("v");
  }
  display.display();
}

// Shared completion for an ACCEPTED measurement -- both for the direct
// success case in performMeasurement() and for a white measurement
// previously held back as Implausible/Indeterminate and then confirmed via
// "Accept"/a suggested tip/"As new tip" (see pendingWhite/loop()).
// Writes lastMeasurement/-Settings, assigns the sample number, appends to
// history, updates the dark/white reference including the fingerprint, and
// re-determines 'calibrated'.
//
// targetTip (optional): which tip a white measurement's fingerprint is
// attributed to -- default nullptr means "the currently active tip"
// (tipCatalog.activeTip(), the previous behavior). Set explicitly when the
// user has selected a DIFFERENT, already-known tip in the confirmation
// screen (see loop()) -- IMPORTANT: in this case finalizeMeasurement() is
// called BEFORE the tip is activated (see there), so that
// rec.settings/whiteRefSettings/fp.sensor continue to record the settings
// under which the measurement was actually taken -- activateTip() would
// otherwise already switch currentSettings.optical beforehand to the
// (possibly differing) settings of the new tip.
void finalizeMeasurement(Precision precision, SampleKind kind, const Measurement& measurement,
                          const MeasurementTelemetry& telemetry, MeasurementTip* targetTip = nullptr) {
  lastMeasurement = measurement;
  lastMeasurementSettings = currentSettings.optical;

  // Increment before labeling: the sample number is the new, lifetime
  // counter value -- this way the numbers keep running across
  // reboots/sessions instead of starting over at 1 on every restart.
  // Dark/White also consume a number in the process (they count as a
  // measurement) but don't show up as "sample_NN" -- the resulting gaps in
  // the sample numbering are deliberately accepted.
  uptimeLogger.recordMeasurement();

  char lbl[16];
  if (kind == SampleKind::Dark) {
    strncpy(lbl, "dark", sizeof(lbl));
  } else if (kind == SampleKind::White) {
    strncpy(lbl, "white", sizeof(lbl));
  } else {
    snprintf(lbl, sizeof(lbl), "sample_%02u", (unsigned)uptimeLogger.measurementCount());
  }
  lbl[sizeof(lbl) - 1] = '\0';
  strncpy(lastLabel, lbl, sizeof(lastLabel));
  lastLabel[sizeof(lastLabel) - 1] = '\0';

  MeasurementRecord rec;
  strncpy(rec.label, lbl, sizeof(rec.label));
  rec.label[sizeof(rec.label) - 1] = '\0';
  rec.kind = kind;
  rec.measurement = measurement;
  // Context at measurement time -- see the MeasurementRecord comment in
  // HistoryStore.h: no substitute for a real LED temperature measurement,
  // but a tangible clue for later analysis of unexplained deviations.
  rec.tempC = temperatureRead();
  rec.sessionMs = millis();
  rec.uptimeS = uptimeLogger.totalSeconds();
  rec.settings = currentSettings.optical;
  rec.precision = precision;
  rec.sampleCount = telemetry.sampleCount;
  rec.relSemWorst = telemetry.relSemWorst;
  rec.semPerChannel = telemetry.semPerChannel;
  if (!historyStore.append(rec)) {
    Serial.println("# history append failed (flash full?)");
  }

  if (kind == SampleKind::Dark) {
    darkRef = measurement;
    darkRefSem = telemetry.semPerChannel;
    darkRefSettings = currentSettings.optical;  // freeze the settings at recording time
    calStore.saveDark(darkRef, darkRefSem, darkRefSettings);
  }
  if (kind == SampleKind::White) {
    whiteRef = measurement;
    whiteRefSem = telemetry.semPerChannel;
    whiteRefSettings = currentSettings.optical;
    calStore.saveWhite(whiteRef, whiteRefSem, whiteRefSettings);

    MeasurementTip* active = targetTip ? targetTip : tipCatalog.activeTip();
    if (active) {
      appendWhiteFingerprint(*active, buildWhiteFingerprint(measurement, currentSettings.optical));
      calStore.saveTips(tipCatalog);
    }
  }
  calibrated = calibrationValidFor(currentSettings.optical);

  printCsvRow(rec);
}

// Shared entry point for the trigger button in all measurement modes.
// precision/kind are determined by the caller (loop()), not here -- this
// function no longer knows any DisplayMode, only "how precisely to
// measure" and "what kind of reading is this". Return value: true on
// success (lastMeasurement updated) -- relevant for Measure mode, see
// MEASUREMENT_MODES/loop() (Calibration continues to simply ignore the
// return value).
bool performMeasurement(Precision precision, SampleKind kind) {
  if (busy) return false;  // no second measurement while one is running

  // Without a dark reference valid for the CURRENT settings, checkValidity()
  // cannot test a new white measurement for separation (see there) --
  // instead of silently skipping that (which would accept EVERY white
  // measurement regardless of its actual quality), it's not started at all.
  // Order enforced: dark before white, see renderNeedDarkFirstWarning().
  if (kind == SampleKind::White) {
    bool haveDarkForSettings = !darkRef.empty() && darkRefSettings == currentSettings.optical;
    if (!haveDarkForSettings) {
      renderNeedDarkFirstWarning();
      return false;
    }
  }

  busy = true;

  flashBorder();  // only here, i.e. only when actually started
  showMeasuringScreen(0, 1);

  MeasurementTelemetry telemetry;
  Measurement measurement = spectrometer.performMeasurement(precision, showMeasuringScreen, &telemetry);
  if (measurement.empty()) {
    if (telemetry.status == MeasurementStatus::NotConverged) {
      // relSemWorst is frequently not NAN here (unlike the success case for
      // very dark samples) -- the last value reached before giving up on
      // the sample budget is a useful diagnostic value when tuning
      // PRECISE_TARGET_REL_SEM/PRECISE_MAX_SAMPLES.
      if (!isnan(telemetry.relSemWorst)) {
        Serial.printf("# Measurement did not converge (%u samples, last relSEM %.2f%%) -- hold device still and try again\n",
                      telemetry.sampleCount, telemetry.relSemWorst * 100.0f);
      } else {
        Serial.println("# Measurement did not converge -- hold device still and try again");
      }
    } else {
      Serial.println("# Sensor error during measurement");
    }
    busy = false;
    renderMeasurementError(telemetry.status);
    return false;
  }
  // A white reference that clips or falls below the noise floor in any
  // channel is unfit both as a reference and as a fingerprint -- it is
  // therefore discarded entirely (not adopted as whiteRef, not added to
  // history, no fingerprint), but still shown to the user with the raw
  // values AND the reason (see renderWhiteValidityWarning()).
  // lastMeasurement/lastMeasurementSettings remain unchanged in this case,
  // just like in the sensor-error/non-convergence case above.
  if (kind == SampleKind::White) {
    MeasurementValidity validity = spectrometer.checkValidity(measurement, telemetry.semPerChannel,
                                                               currentSettings.optical, darkRef, darkRefSem);
    if (!validity.ok) {
      busy = false;
      renderWhiteValidityWarning(measurement, validity);
      return false;
    }

    // Reliable (no clipping/noise floor issue), but does it match this
    // tip's previous fingerprints? Only checkable at all with an active tip
    // -- without an active tip (shouldn't happen per the invariant) it is
    // conservatively treated as Indeterminate. Only Implausible/
    // Indeterminate interrupt the flow (see pendingWhite/loop()) --
    // Plausible/PlausibleViaFallback continue to be adopted silently as
    // before.
    MeasurementTip* active = tipCatalog.activeTip();
    WhiteFingerprint candidate = buildWhiteFingerprint(measurement, currentSettings.optical);
    PlausibilityResult plaus = active ? active->isPlausible(candidate, tipCatalog)
                                       : PlausibilityResult::Indeterminate;
    if (plaus == PlausibilityResult::Implausible || plaus == PlausibilityResult::Indeterminate) {
      busy = false;
      pendingWhite.active = true;
      pendingWhite.precision = precision;
      pendingWhite.measurement = measurement;
      pendingWhite.telemetry = telemetry;
      pendingWhite.plausibility = plaus;
      // Maybe the measurement matches a DIFFERENT, already-known tip (e.g. a
      // physical tip swap without informing the device) -- offered in the
      // confirmation screen BEFORE "As new tip"/"Accept"/
      // "Discard" (see renderWhitePlausibilityConfirm()).
      pendingWhite.suggestedTipIndices = active ? tipCatalog.rankPlausibleTips(candidate, active->name)
                                                 : std::vector<size_t>();
      whiteConfirmIndex = 0;
      renderCurrentView();
      return false;
    }
  }

  finalizeMeasurement(precision, kind, measurement, telemetry);
  busy = false;
  renderCurrentView();
  return true;
}

// Sends the complete measurement history as CSV via BLE notify (Nordic UART
// Service) to a connected phone. Uses the same busy guard as
// performMeasurement(), since both operations are blocking and must not
// overlap.
//
// BLE is deliberately NOT started when entering export mode, but only here,
// on the first trigger press -- this way you can "click through" the mode
// with the Mode button without necessarily turning on the radio.
void performExport() {
  if (busy) return;

  if (!bleExporter.isActive()) {
    busy = true;
    flashBorder();
    bleExporter.begin(BLE_DEVICE_NAME);
    exportSending = false;
    exportSentOk = false;
    exportHint = false;
    exportSentBytes = exportTotalBytes = 0;
    busy = false;
    renderExportStatus();
    return;  // this press only activates BLE, doesn't send anything yet
  }

  if (!bleExporter.isConnected()) {
    exportHint = true;
    exportSentOk = false;
    renderExportStatus();
    return;
  }

  busy = true;
  flashBorder();
  exportHint = false;
  exportSentOk = false;
  exportSending = true;
  exportSentBytes = 0;
  exportTotalBytes = 0;
  renderExportStatus();

  std::string csv = buildHistoryCsv(exportPage == ExportPage::Debug);
  bool ok = bleExporter.send(csv, onExportProgress);

  exportSending = false;
  exportSentOk = ok;
  busy = false;
  renderExportStatus();
}

// Short Mode press: moves the cursor on the Measure selection page, switches
// the view/export page on the Measure result page or in export mode,
// switches the setting being edited in Settings, switches between the
// white/dark reference in Calibration.
void cycleView() {
  if (currentDisplayMode == DisplayMode::Measure && measurePage == MeasurePage::Select) {
    measurementModeIndex = (measurementModeIndex + 1) % MEASUREMENT_MODE_COUNT;
    renderCurrentView();
    return;
  }
  if (currentDisplayMode == DisplayMode::Export) {
    uint8_t n = (static_cast<uint8_t>(exportPage) + 1) % static_cast<uint8_t>(ExportPage::COUNT);
    exportPage = static_cast<ExportPage>(n);
    renderCurrentView();
    return;
  }
  if (currentDisplayMode == DisplayMode::Settings) {
    if (tipMenuStage == TipMenuStage::List) {
      uint8_t total = (uint8_t)(TIP_LIST_FIXED_ENTRIES + tipCatalog.tips.size());
      tipListIndex = (tipListIndex + 1) % total;
      renderCurrentView();
      return;
    }
    if (tipMenuStage == TipMenuStage::Detail) {
      bool isActive = (tipCatalog.tips[tipDetailIndex].name == tipCatalog.active);
      // active: <Back>/White fingerprint; otherwise: <Back>/Activate/Delete/White fingerprint.
      uint8_t total = isActive ? 2 : 4;
      tipActionIndex = (tipActionIndex + 1) % total;
      renderCurrentView();
      return;
    }
    if (tipMenuStage == TipMenuStage::FingerprintStats) {
      fingerprintStatsScroll++;  // the renderer handles the modulo based on the channel count
      renderCurrentView();
      return;
    }
    SettingsLevel& lvl = settingsStack[settingsDepth];
    lvl.index = (lvl.index + 1) % lvl.count;
    renderCurrentView();
    return;
  }
  if (currentDisplayMode == DisplayMode::Calibration) {
    if (pendingWhite.active) {
      uint8_t total = (uint8_t)(pendingWhite.suggestedTipIndices.size() + 3);
      whiteConfirmIndex = (whiteConfirmIndex + 1) % total;
      renderCurrentView();
      return;
    }
    calibrationTarget = (calibrationTarget == CalibrationTarget::White) ? CalibrationTarget::Dark : CalibrationTarget::White;
    renderCurrentView();
    return;
  }
  uint8_t n = (static_cast<uint8_t>(currentView) + 1) % static_cast<uint8_t>(DisplayView::COUNT);
  currentView = static_cast<DisplayView>(n);
  renderCurrentView();
}

void cycleMode() {
  // Within Measure, a long Mode press on the result page leads back to the
  // selection page WITHOUT switching the top-level DisplayMode -- with a
  // measurement you only briefly "leave" the main navigation.
  if (currentDisplayMode == DisplayMode::Measure && measurePage == MeasurePage::Result) {
    measurePage = MeasurePage::Select;
    lastMeasurement.clear();
    lastLabel[0] = '\0';
    renderCurrentView();
    return;
  }

  DisplayMode previous = currentDisplayMode;
  uint8_t n = (static_cast<uint8_t>(currentDisplayMode) + 1) % static_cast<uint8_t>(DisplayMode::COUNT);
  currentDisplayMode = static_cast<DisplayMode>(n);
  // Safety net: a full mode switch should never happen in the middle of
  // editing settings (loop() already blocks cycleMode() while
  // editingActive), but a clean reset here costs nothing.
  editingActive = false;

  if (previous == DisplayMode::Export && currentDisplayMode != DisplayMode::Export) {
    bleExporter.end();  // no-op if BLE was never activated during this stay
  }
  if (currentDisplayMode == DisplayMode::Export && previous != DisplayMode::Export) {
    // BLE is deliberately NOT started here, see performExport().
    exportSending = false;
    exportSentOk = false;
    exportHint = false;
    exportSentBytes = exportTotalBytes = 0;
    // Prevents unknowingly landing back on the Clear page after a round
    // trip through the modes.
    exportPage = ExportPage::Normal;
  }
  if (currentDisplayMode == DisplayMode::Settings && previous != DisplayMode::Settings) {
    // Same consideration as for exportPage above, now applied to the whole
    // tree: always reset the position to the root (see Plan) -- so every
    // subsequent entry starts fresh again.
    settingsDepth = 0;
    settingsStack[0] = { SETTINGS_ROOT, sizeof(SETTINGS_ROOT) / sizeof(SETTINGS_ROOT[0]), 0 };
    tipMenuStage = TipMenuStage::Closed;
  }
  if (currentDisplayMode == DisplayMode::Calibration && previous != DisplayMode::Calibration) {
    // Same consideration -- don't unknowingly land on "Dark".
    calibrationTarget = CalibrationTarget::White;
  }
  if (currentDisplayMode == DisplayMode::Measure && previous != DisplayMode::Measure) {
    // Same consideration: always start fresh in Measure on the selection
    // page, with Precise (most-used) preselected -- regardless of what was
    // selected during the last stay.
    measurePage = MeasurePage::Select;
    measurementModeIndex = 0;
  }

  // The most recently shown measurement belongs to the previous mode --
  // after a mode switch it should no longer be displayed (views then fall
  // back to their "no measurement"/"not calibrated" hint).
  lastMeasurement.clear();
  lastLabel[0] = '\0';
  // An open plausibility decision also belongs to the previous mode -- a
  // mode switch counts as an implicit discard (nothing was ever written,
  // see performMeasurement()), the held Measurement is cleanly released in
  // the process.
  pendingWhite = PendingWhiteDecision();

  renderCurrentView();

  // Starting point for the periodic 10s refresh in info mode (see loop())
  // -- prevents an immediate, redundant second redraw right after the
  // renderCurrentView() call above.
  if (currentDisplayMode == DisplayMode::Info) lastInfoRenderMs = millis();
}

void setup() {
  // Thermal: 160->80MHz practically halves active compute power with no
  // risk -- on the C3, the APB clock (I2C/USB timing among others) does NOT
  // depend on the CPU frequency (unlike frequencies < 80MHz, which can
  // therefore break WLAN/BT/USB and are deliberately not touched here).
  setCpuFrequencyMhz(80);

  // Initialize the display as early as possible -- BEFORE the 2s USB-CDC
  // delay and the NVS/LittleFS reads further below, so that something
  // appears on the screen as fast as physically possible (limited only by
  // the ESP32's own bootloader time), instead of staying blank for several
  // hundred ms up to >2s. The display itself has no non-volatile memory
  // (GDDRAM is plain SRAM in the controller) -- "show as early as possible"
  // is the only lever available; "display before the ESP" is simply not
  // possible on this hardware.
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(I2C_HZ);
  displayOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (displayOk) {
    display.setRotation(2);  // display is mounted rotated 180 degrees
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Colorimeter");
    display.println("starting ...");
    display.display();  // push immediately -- no "snow" frame visible anymore
  }

  Serial.begin(115200);
  delay(2000);

  triggerBtn.begin();
  modeBtn.begin();

  calStore.begin();
  calStore.loadDark(darkRef, darkRefSem, darkRefSettings);
  calStore.loadWhite(whiteRef, whiteRefSem, whiteRefSettings);
  calStore.loadSettings(currentSettings);
  calibrated = calibrationValidFor(currentSettings.optical);

  // Catalog never empty, see TipCatalog.h -- on the very first boot (or if
  // the stored JSON is invalid) there's no tip yet: creates "Tip 1"
  // with the settings just loaded/defaults.
  if (!calStore.loadTips(tipCatalog)) {
    createTipFromCurrentSettings();
  }

  uptimeLogger.begin();
  historyStore.begin();  // not fatal on failure -- core functionality continues without history

  if (!displayOk) {
    // Pure boot diagnostics on an error path -- never mixes with the actual
    // CSV export (which only starts later/live in loop()).
    Serial.println("# SSD1306 not found/initialized (address 0x3C) - display stays inactive");
  }

  if (!sensorImpl.begin()) {
    Serial.println("# AS7341 not found");
    while (true) delay(1000);
  }
  sensorImpl.applySettings(currentSettings.optical);  // hardware matches the loaded state from the start

  // Replaces the "starting ..." text from above once everything is
  // initialized -- currentDisplayMode/measurePage are already at their
  // defaults (Measure/Select), so this directly shows the measurement mode
  // selection instead of a separate "ready" intermediate screen.
  renderCurrentView();
}

void loop() {
  // Serial is a pure, passive data export: as soon as a terminal is newly
  // connected (USB-CDC connection status), dump the complete history once
  // (ALWAYS with raw values, regardless of the BLE toggle flag).
  // Afterwards, performMeasurement() continues to append every new
  // measurement live as a single row. This check deliberately sits BEFORE
  // the button polling: if a new connection happens in the same loop()
  // iteration as a trigger press, the dump reflects the history BEFORE this
  // measurement, and the new row appears afterward once, live -- this
  // avoids an otherwise possible cosmetic duplicate.
  bool nowSerialConnected = (bool)Serial;
  if (nowSerialConnected && !serialWasConnected) {
    Serial.print(buildHistoryCsv(/*includeRaw=*/true).c_str());
  }
  serialWasConnected = nowSerialConnected;

  // White/Dark (overwrites the calibration reference) and the export Clear
  // page (deletes the history) require a LONG instead of a short trigger
  // press -- the same "deliberately held instead of accidental" safeguard
  // as the existing long Mode press for switching modes. Triggering a
  // measurement in Measure mode (both the selection AND the result page)
  // and triggering the BLE send remain an immediate, short press, since
  // they are frequent and uncritical.
  DebouncedButton::Event te = triggerBtn.poll();
  if (currentDisplayMode == DisplayMode::Export) {
    if (exportPage == ExportPage::Clear) {
      if (te == DebouncedButton::Event::LongPress) {
        flashBorder();
        historyStore.clear();  // keeps the last dark/white row, see there
        renderCurrentView();
      }
    } else if (te == DebouncedButton::Event::Pressed) {
      performExport();
    }
  } else if (currentDisplayMode == DisplayMode::Calibration) {
    if (pendingWhite.active) {
      // User decides on a white measurement previously held back as
      // Implausible/Indeterminate (see performMeasurement()/
      // renderWhitePlausibilityConfirm()): a suggested tip, "As new
      // tip", "Accept", or "Discard".
      if (te == DebouncedButton::Event::LongPress) {
        size_t sCount = pendingWhite.suggestedTipIndices.size();
        if (whiteConfirmIndex < sCount) {
          // According to the user, the measurement belongs to a DIFFERENT,
          // already-known tip. FIRST finalize it under that tip (while
          // currentSettings.optical still holds the actual recording
          // settings), ONLY THEN activate the tip (which changes
          // currentSettings.optical/the sensor registers for FUTURE
          // measurements) -- see the finalizeMeasurement() comment.
          MeasurementTip& target = tipCatalog.tips[pendingWhite.suggestedTipIndices[whiteConfirmIndex]];
          busy = true;
          finalizeMeasurement(pendingWhite.precision, SampleKind::White, pendingWhite.measurement,
                               pendingWhite.telemetry, &target);
          busy = false;
          activateTip(target);
        } else if (whiteConfirmIndex == sCount) {
          // "As new tip" -- reversed order: FIRST create+activate
          // (does NOT change currentSettings.optical, see
          // createTipFromCurrentSettings()), ONLY THEN finalize (via the
          // default target path it automatically lands on the freshly
          // activated, still-empty tip).
          createTipFromCurrentSettings();
          busy = true;
          finalizeMeasurement(pendingWhite.precision, SampleKind::White, pendingWhite.measurement, pendingWhite.telemetry);
          busy = false;
        } else if (whiteConfirmIndex == sCount + 1) {
          // "Accept" -- for the active tip already flagged as implausible.
          busy = true;
          finalizeMeasurement(pendingWhite.precision, SampleKind::White, pendingWhite.measurement, pendingWhite.telemetry);
          busy = false;
        }
        // sCount+2 ("Discard"): do nothing, the measurement stays discarded.
        pendingWhite = PendingWhiteDecision();  // release the state + held Measurement
        renderCurrentView();
      }
    } else if (te == DebouncedButton::Event::LongPress) {
      SampleKind kind = (calibrationTarget == CalibrationTarget::White) ? SampleKind::White : SampleKind::Dark;
      performMeasurement(Precision::Precise, kind);  // reference measurements are always Precise, as before
    }
  } else if (currentDisplayMode == DisplayMode::Settings) {
    // Outside of editing, only a LONG trigger press starts editing the
    // currently selected setting (prevents accidentally sliding into it) --
    // a short press does nothing in that case. While editing: short =
    // current digit +1, long = next digit (on the last digit: finish
    // editing + save). See DigitEditor.h / the context section in the Plan
    // for the full button mapping (the Mode button mirrors this, see
    // below).
    //
    // IMPORTANT: ShortRelease instead of Pressed is used deliberately here
    // for "+1" -- Pressed fires immediately on press-down, REGARDLESS of
    // how long it's then held. With Pressed, a long press would therefore
    // first fire a Pressed (incorrectly +1) AND then a LongPress (next
    // digit) -- exactly the reported bug. ShortRelease, by contrast, only
    // fires on release, and only if the long-press threshold was NOT
    // exceeded while holding (see Buttons.h) -- exactly like the Mode
    // button already does.
    if (tipMenuStage == TipMenuStage::List) {
      // "<Back>" leaves the tip menu entry (back into the generic tree,
      // positioned on "Tips"); "Create new tip" registers+
      // activates immediately; every other entry opens the detail menu of
      // the selected tip.
      if (te == DebouncedButton::Event::LongPress) {
        if (tipListIndex == 0) {
          tipMenuStage = TipMenuStage::Closed;
        } else if (tipListIndex == 1) {
          createTipFromCurrentSettings();
          tipListIndex = (uint8_t)(TIP_LIST_FIXED_ENTRIES + tipCatalog.tips.size() - 1);  // cursor on the new tip
        } else {
          tipDetailIndex = tipListIndex - TIP_LIST_FIXED_ENTRIES;
          tipActionIndex = 0;
          tipMenuStage = TipMenuStage::Detail;
        }
        renderCurrentView();
      }
    } else if (tipMenuStage == TipMenuStage::Detail) {
      if (te == DebouncedButton::Event::LongPress) {
        const MeasurementTip& tip = tipCatalog.tips[tipDetailIndex];
        bool isActive = (tip.name == tipCatalog.active);
        if (tipActionIndex == 0) {
          tipMenuStage = TipMenuStage::List;
        } else if (!isActive && tipActionIndex == 1) {
          activateTip(tip);
          tipMenuStage = TipMenuStage::List;
        } else if (!isActive && tipActionIndex == 2) {
          deleteTip(tipDetailIndex);
          tipListIndex = 0;
          tipMenuStage = TipMenuStage::List;
        } else if ((isActive && tipActionIndex == 1) || (!isActive && tipActionIndex == 3)) {
          // "White fingerprint" -- always the last entry of both lists
          // (see renderTipDetail()).
          fingerprintStatsScroll = 0;
          tipMenuStage = TipMenuStage::FingerprintStats;
        }
        renderCurrentView();
      }
    } else if (tipMenuStage == TipMenuStage::FingerprintStats) {
      // Pure display screen -- a long trigger press goes back to the detail
      // menu (no "<Back>" entry needed, since there is no cursor).
      if (te == DebouncedButton::Event::LongPress) {
        tipMenuStage = TipMenuStage::Detail;
        renderCurrentView();
      }
    } else if (!editingActive) {
      // Outside of editing, a long trigger press acts differently depending
      // on the node kind: "<Back>" goes one level up, a navigation node
      // one level down, a leaf starts editing (as before), TipList opens
      // the tip menu entry.
      if (te == DebouncedButton::Event::LongPress) {
        const SettingsNode& s = currentSettingsNode();
        switch (s.kind) {
          case SettingsNodeKind::Back:
            if (settingsDepth > 0) settingsDepth--;
            renderCurrentView();
            break;
          case SettingsNodeKind::Branch:
            if (settingsDepth + 1 < SETTINGS_TREE_MAX_DEPTH) {
              settingsDepth++;
              settingsStack[settingsDepth] = { s.children, s.childCount, 0 };
            }
            renderCurrentView();
            break;
          case SettingsNodeKind::Leaf:
            editor.begin(s.digitCount, s.digitCycleLen, s.getValue());
            editingActive = true;
            renderCurrentView();
            break;
          case SettingsNodeKind::TipList:
            tipMenuStage = TipMenuStage::List;
            tipListIndex = 0;
            renderCurrentView();
            break;
        }
      }
    } else if (te == DebouncedButton::Event::ShortRelease) {
      editor.incrementCurrentDigit();
      renderCurrentView();
    } else if (te == DebouncedButton::Event::LongPress) {
      const SettingsNode& s = currentSettingsNode();
      if (editor.advanceDigit()) {  // true = past the last digit -> done
        s.setValue(editor.assembledValue(s.maxValue));
        editingActive = false;
      }
      renderCurrentView();
    }
  } else if (currentDisplayMode == DisplayMode::Measure) {
    if (te == DebouncedButton::Event::Pressed) {
      // Trigger fires the currently selected measurement mode -- on both the
      // selection and the result page. On the result page this is another
      // measurement of the SAME mode, without having to go via the
      // selection page first -- important for capturing many samples in a
      // row quickly (exactly as before the merging of Fast/Precise into
      // Measure).
      //
      // Optimistically set to Result BEFORE the measurement:
      // performMeasurement() itself calls renderCurrentView() on success --
      // so this internal call already shows the correct result screen,
      // instead of briefly flashing the old page. On failure (sensor
      // error/non-convergence), performMeasurement() has already shown
      // renderMeasurementError() -- then back to Select, for an immediate
      // retry without a detour via a long Mode press.
      measurePage = MeasurePage::Result;
      if (!MEASUREMENT_MODES[measurementModeIndex].trigger()) {
        measurePage = MeasurePage::Select;
      }
    }
  }
  // Info: trigger deliberately does nothing here -- pure status screen.

  // While editing settings, the Mode button mirrors the trigger button for
  // digit navigation (short = -1, long = previous digit, or on the first
  // digit: finish editing + save -- deliberately symmetric to the trigger's
  // completion, no separate cancel path, see Plan) -- its other meaning
  // (switch view/mode) is blocked for as long as that lasts.
  DebouncedButton::Event me = modeBtn.poll();
  if (currentDisplayMode == DisplayMode::Settings && editingActive) {
    const SettingsNode& s = currentSettingsNode();
    if (me == DebouncedButton::Event::ShortRelease) {
      editor.decrementCurrentDigit();
      renderCurrentView();
    } else if (me == DebouncedButton::Event::LongPress) {
      if (editor.retreatDigit()) {  // true = went below the first digit -> done
        s.setValue(editor.assembledValue(s.maxValue));
        editingActive = false;
      }
      renderCurrentView();
    }
  } else if (me == DebouncedButton::Event::ShortRelease) {
    cycleView();
  } else if (me == DebouncedButton::Event::LongPress) {
    cycleMode();
  }

  // Info is a purely passive status screen (no button press triggers a
  // redraw there) -- therefore refreshed here via a timer every 10s, so
  // temperature/uptime visibly keep updating.
  if (currentDisplayMode == DisplayMode::Info && (millis() - lastInfoRenderMs) >= 10000UL) {
    lastInfoRenderMs = millis();
    renderCurrentView();
  }

  // Export: the BLE connection status changes asynchronously in the
  // Bluedroid callback (see BleExporter::onConnect()/onDisconnect()), not
  // via a button press here. Event-driven instead of periodically polled --
  // takeConnectionChanged() only returns true EXACTLY WHEN something has
  // actually changed since the last call.
  if (currentDisplayMode == DisplayMode::Export && bleExporter.takeConnectionChanged()) {
    renderCurrentView();
  }

  bleExporter.loop();
  uptimeLogger.loop();

  // Thermal: without this delay(), the idle loop (pure button polling)
  // spins at 100% duty cycle, practically the entire time the device is
  // just waiting for a button press. delay() yields to the FreeRTOS idle
  // task (WFI) -- 1ms is negligible compared to the 40ms debounce.
  delay(1);
}
