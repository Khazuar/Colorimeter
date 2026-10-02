#pragma once
#include <Preferences.h>
#include "Spectrometer.h"
#include "AppConfig.h"
#include "TipCatalog.h"

// Thin NVS wrapper for the dark/white calibration, the settings hierarchy
// (RootSettings) AND the measurement tip catalog (TipCatalog). Belongs to the
// orchestration (main.cpp) -- the Spectrometer itself never touches flash.
// Each is persisted as a JSON string (see CalibrationStore.cpp and
// schema/settings.schema.json resp. schema/tips.schema.json) rather than as a
// binary blob -- robust against fields being added/missing, enum values as
// strings instead of a numeric index. The size of the raw values is
// determined from the stored blob itself (getBytesLength), so the
// orchestration does not need to know any sensor-specific channel count.
class CalibrationStore {
public:
  void begin();  // prefs_.begin("colorim", false) + LittleFS.begin() (for loadTips/saveTips, see there)

  // false if never saved OR JSON not readable (out/optical then fall back to
  // their defaults). outSem (absolute standard error per channel, see
  // MeasurementTelemetry::semPerChannel) stays empty if never saved (e.g. a
  // reference from before this extension) -- not a load error,
  // checkValidity() then falls back to 0 for that part.
  bool loadDark(Measurement& out, Measurement& outSem, OpticalSettings& optical);
  bool loadWhite(Measurement& out, Measurement& outSem, OpticalSettings& optical);

  // optical is saved together with the raw values -- the reference and the
  // settings active while it was taken belong together inseparably (see
  // main.cpp::calibrationValidFor()).
  void saveDark(const Measurement& v, const Measurement& sem, const OpticalSettings& optical);
  void saveWhite(const Measurement& v, const Measurement& sem, const OpticalSettings& optical);

  // The complete configuration currently selected in the settings tree --
  // independent of what is currently calibrated as the dark/white reference.
  // false if never saved OR JSON not readable (out then falls back to the
  // RootSettings defaults).
  bool loadSettings(RootSettings& out);
  void saveSettings(const RootSettings& v);

  // The measurement tip catalog (main.cpp::tipCatalog). false if never saved
  // OR JSON invalid (out then becomes an EMPTY catalog -- main.cpp::setup()
  // bootstraps the very first tip in that case).
  bool loadTips(TipCatalog& out);
  void saveTips(const TipCatalog& v);

private:
  bool load(const char* key, Measurement& out);
  void save(const char* key, const Measurement& v);

  Preferences prefs_;
};
