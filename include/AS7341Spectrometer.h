#pragma once
#include <Adafruit_AS7341.h>
#include "Spectrometer.h"
#include "AppConfig.h"

class AS7341Spectrometer : public Spectrometer {
public:
  // Fixed Measurement order: F1..F8, Clear, NIR.
  // readAllChannels() internally delivers 12 raw slots from two integration cycles
  // (cycle 1: F1,F2,F3,F4,Clear,NIR on slot 0-5; cycle 2: F5,F6,F7,F8,Clear,NIR
  // on slot 6-11 -- verified against Adafruit_AS7341::setup_F1F4_Clear_NIR()/
  // setup_F5F8_Clear_NIR()). The first (surplus) Clear/NIR pair on slot 4/5
  // is discarded, the second on slot 10/11 is used. Resolved internally in
  // performMeasurement() -- nobody outside this class sees the
  // 12 raw slots (measurementLabels() too is intended purely for labeling,
  // not for interpretation by generic code).
  static const uint8_t N_CH  = 10;  // length of every Measurement from this sensor

  static const char* const MEASUREMENT_LABELS[N_CH];

  // Raw channels with a mean below this value do not yield a reliable
  // relSEM and therefore do not count toward the stop decision of the sample
  // loop (see converged() in the .cpp) -- a pure efficiency/reporting
  // heuristic, NOT a statement about usability (that decision is made by
  // checkValidity() separately, via a Limit-of-Detection test against the
  // dark reference, see there). Public because main.cpp needs to be able
  // to know this value conceptually (e.g. for error messages).
  static constexpr float NOISE_FLOOR_COUNTS = 50.0f;

  bool begin();  // ONLY as7341_.begin() (I2C startup). No NVS access,
                 // no assumption about gain/ATIME/ASTEP -- see applySettings().

  // Writes gain/ATIME/ASTEP to the sensor -- repeatable any number of times,
  // unlike begin() (which only runs once at boot). Called by the
  // orchestration (main.cpp) both at startup (loaded settings)
  // and on every change made in settings mode.
  void applySettings(const OpticalSettings& settings);

  Measurement performMeasurement(Precision precision,
                                  ProgressCallback onProgress = nullptr,
                                  MeasurementTelemetry* outTelemetry = nullptr) override;
  const char* const* measurementLabels() const override { return MEASUREMENT_LABELS; }
  const char* sensorId() const override { return "AS7341"; }
  Spectrum getSpectrum(const Measurement& measurement,
                        const Measurement& whiteReference,
                        const Measurement& darkReference,
                        FilterState filterState) const override;

  MeasurementValidity checkValidity(const Measurement& raw, const Measurement& rawSem,
                                     const OpticalSettings& settings,
                                     const Measurement& darkMean, const Measurement& darkSem) const override;
  Measurement normalize(const Measurement& raw, const SensorSettings& sensor) const override;

private:
  // Raw channel indices that, for the given filter, actually enter the
  // spectrum as a VIS band (see the BANDS_* tables) -- ONLY for checkValidity(),
  // hence private (not part of the interface, see the Spectrometer.h comment).
  // DELIBERATELY without NIR/Clear: NIR would be structurally
  // dark under an IR-cut filter and could not be lifted above the noise
  // floor by anything; Clear is already indirectly covered by the VIS channels.
  uint8_t relevantChannels(FilterState fs, uint8_t outIdx[N_CH]) const;

  // Computes the calibrated reflectance of the VIS channels usable for the
  // given FilterState (see the BANDS_* tables in the .cpp) directly into 'out'.
  // Internally, the NIR reflectance is also determined (same formula), in order
  // to subtract out optical crosstalk from NIR light -- the strength
  // of this correction is filter-dependent. The result remains only
  // R_vis; NIR itself does not flow into the (sensor-independent) Spectrum.
  void computeReflectance(const Measurement& measurement,
                          const Measurement& whiteReference,
                          const Measurement& darkReference,
                          FilterState filterState,
                          Spectrum& out) const;

  Adafruit_AS7341 as7341_;
};
