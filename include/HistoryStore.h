#pragma once
#include <cstddef>
#include <cstdint>
#include <cmath>
#include "AppConfig.h"
#include "Spectrometer.h"

struct MeasurementRecord {
  char label[16];
  SampleKind kind = SampleKind::Regular;
  Measurement measurement;
  // Context at measurement time -- not representative of the actual LED
  // temperature (only measures the ESP32 die), but a tangible clue for later
  // analysis if a series of measurements shows unexplained deviations.
  float tempC = 0.0f;         // ESP32 internal temperature sensor, degrees Celsius
  uint32_t sessionMs = 0;     // millis() at measurement time (runtime since this boot)
  uint32_t uptimeS = 0;       // UptimeLogger::totalSeconds() at measurement time (LED age, lifetime)
  OpticalSettings settings;  // filter/gain/ATIME/ASTEP at measurement time

  // Measurement mode + precision telemetry (see Spectrometer.h::MeasurementTelemetry).
  // sampleCount is always 1 for Precision::Single. relSemWorst is NAN when no
  // relSEM was computed (Precision::Single, or Precision::Precise with no
  // channel above the noise floor) -- deliberately NOT 0.0.
  Precision precision = Precision::Single;
  uint8_t sampleCount = 1;
  float relSemWorst = NAN;

  // Absolute standard error of the mean PER CHANNEL (see
  // MeasurementTelemetry::semPerChannel) -- empty if not determined
  // (Precision::Single, or a row from before this extension). This means a
  // later recomputation (export, diagnostics) has the same uncertainty data
  // available as live at measurement time, not just the aggregated
  // relSemWorst.
  Measurement semPerChannel;
};

// Persists the measurement history row by row as CSV on the (previously
// unused) "spiffs" partition via LittleFS -- physically separate from "nvs"
// (calibration) and "uptime". Each row is a minimal, lossless raw-data
// serialization (label, mode, raw Measurement values); spectrum/Lab/hex are,
// as before, freshly recomputed from the RAW DATA on every export
// (appendCsvRow() remains unchanged) -- this matches exactly today's
// behavior, where a recalibration retroactively affects all history rows in
// the export.
class HistoryStore {
public:
  bool begin();
  bool append(const MeasurementRecord& rec);
  size_t count() const { return count_; }
  void clear();

  using RecordVisitor = void (*)(const MeasurementRecord& rec, void* userData);
  void forEach(RecordVisitor visitor, void* userData) const;

  // Channel count of the first stored row, or 0 if empty/not mounted.
  size_t firstRecordChannelCount() const;

private:
  bool mounted_ = false;
  size_t count_ = 0;
};
