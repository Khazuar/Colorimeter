#pragma once
#include <Adafruit_AS7341.h>
#include <cstdint>
#include "Spectrometer.h"  // FilterState (part of OpticalSettings, see below)

// ------------------------- I2C / Bus -------------------------
static const uint8_t  SDA_PIN = 6;
static const uint8_t  SCL_PIN = 7;
static const uint32_t I2C_HZ  = 100000;

// ------------------------- OLED (SSD1306, 0.96", 128x64, I2C) ---------
static const uint8_t OLED_WIDTH  = 128;
static const uint8_t OLED_HEIGHT = 64;
static const uint8_t OLED_ADDR   = 0x3C;

// ------------------------- AS7341 Timing -------------------------
// Only startup values for the very first boot (previously hard-wired) --
// configurable at runtime in the settings tree, see SensorSettings below.
static const uint8_t       AS_ATIME = 100;
static const uint16_t      AS_ASTEP = 999;                 // ~281 ms integration at this startup value
static const as7341_gain_t AS_GAIN  = AS7341_GAIN_512X;    // white stays below full-scale

// ------------------------- Buttons -------------------------
static const uint8_t  TRIGGER_PIN   = 3;
static const uint8_t  MODE_PIN      = 9;
static const uint32_t DEBOUNCE_MS   = 40;
static const uint32_t LONG_PRESS_MS = 600;

// Which top-level screen is currently active (cycled through by long
// pressing the mode button: Measure -> Calibration -> Export -> Settings -> Info -> Measure ...).
// Pure UI/orchestration concept -- has NOTHING to do with sensor measurement
// quality (that's Spectrometer::Precision) and NOTHING to do with the kind of
// a stored measurement value (that's SampleKind, see below).
//
// Measure combines ALL physical measurement modes (currently: Precise, Single)
// into a single top-level entry, so that the main navigation doesn't keep
// growing with every new, rarely used measurement mode. For this, Measure has
// its own two-stage sub-navigation (main.cpp::MeasurePage): a selection page
// (cursor moved via short mode-button press over main.cpp::MEASUREMENT_MODES,
// trigger fires the selected measurement) and a result page (identical to the
// previous Fast/Precise view, see DisplayViews.h; short mode-button press
// switches the result view there as before, long mode-button press returns to
// the selection page WITHOUT changing the top-level DisplayMode -- see
// main.cpp::cycleMode()).
//
// Calibration combines white and dark reference in a single screen
// (short mode-button press switches in main.cpp between main.cpp::CalibrationTarget
// White/Dark, long trigger press always measures the currently selected reference
// with Precision::Precise). Export doesn't measure physically: trigger instead
// sends the measurement history over BLE there. Settings doesn't measure:
// trigger there rotates the value of the currently selected setting (e.g.
// FilterState), mode (short) switches WHICH setting is being edited. Info
// doesn't measure at all (trigger is a no-op there): pure status screen for
// uptime/measurement counter from UptimeLogger.
enum class DisplayMode : uint8_t { Measure = 0, Calibration = 1, Export = 2, Settings = 3, Info = 4, COUNT = 5 };

// What a stored/exported measurement value represents
// (HistoryStore::MeasurementRecord) -- independent of which
// DisplayMode/Precision it was recorded with. Regular = normal
// Precise/Single sample (from the Measure DisplayMode); Dark/White =
// reference recordings from the Calibration DisplayMode. Deliberately a
// separate enum instead of reusing DisplayMode: which screen is currently
// active and what a measurement value means are two independent questions.
enum class SampleKind : uint8_t { Regular = 0, Dark = 1, White = 2, COUNT = 3 };

// Number of known as7341_gain_t values (0.5X..512X, see Adafruit_AS7341.h)
// -- single source for bounds checks (HistoryStore::parseLine()) and the
// settings registry/label table (main.cpp).
static const uint8_t AS7341_GAIN_COUNT = 11;

// Gain/ATIME/ASTEP as a block -- see schema/settings.schema.json ("sensor").
// Used UNCHANGED as a whole as part of OpticalSettings (see there).
// AS_ATIME/AS_ASTEP/AS_GAIN here only serve as startup values for the very
// first boot.
struct SensorSettings {
  as7341_gain_t gain = AS_GAIN;
  uint8_t atime = AS_ATIME;
  uint16_t astep = AS_ASTEP;
  bool operator==(const SensorSettings& o) const {
    return gain == o.gain && atime == o.atime && astep == o.astep;
  }
  bool operator!=(const SensorSettings& o) const { return !(*this == o); }
};

// All settings that concern the device's optical path (filter +
// sensor tuning) -- a REAL thematic group (not just coincidentally shaped
// like RootSettings::optical, see there). Used UNCHANGED as a whole, passed
// to AS7341Spectrometer::applySettings()/main.cpp::calibrationValidFor(),
// frozen per measurement/reference (see
// HistoryStore::MeasurementRecord::settings, main.cpp darkRefSettings/
// whiteRefSettings -- these are snapshots of "what was this measured with")
// AND is exactly what every tip stores in the catalog (main.cpp/TipCatalog.h)
// -- a tip is at its core "a named OpticalSettings state". Itself NOT a
// settings node -- is never edited itself or navigated in the settings tree,
// see RootSettings below for that. See schema/settings.schema.json
// ("optical").
struct OpticalSettings {
  FilterState filterState = FilterState::None;
  SensorSettings sensor;
  bool operator==(const OpticalSettings& o) const {
    return filterState == o.filterState && sensor == o.sensor;
  }
  bool operator!=(const OpticalSettings& o) const { return !(*this == o); }
};

// The actual settings hierarchy: UI tree root AND what gets persisted as
// ONE JSON document (CalibrationStore::save/loadSettings()). See
// schema/settings.schema.json ("Settings") -- maintain BOTH (this struct and
// the schema file) together when making changes. Currently groups exactly
// one thematic group (optical); future, different-natured settings (e.g.
// language, Bluetooth name) will be added here as additional, SEPARATE
// sibling fields -- NOT mixed into optical.
struct RootSettings {
  OpticalSettings optical;
  bool operator==(const RootSettings& o) const { return optical == o.optical; }
  bool operator!=(const RootSettings& o) const { return !(*this == o); }
};

// BLE export (Nordic UART Service) -- device name is relevant app-wide (main.cpp
// starts/stops BleExporter with it); chunk size/MTU/delays remain purely internal
// implementation details of BleExporter.cpp.
static constexpr char BLE_DEVICE_NAME[] = "Colorimeter";
