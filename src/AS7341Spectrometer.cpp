#include "AS7341Spectrometer.h"
#include "AS7341Math.h"
#include "AppConfig.h"
#include <Arduino.h>
#include <cmath>

// Debug/analysis purposes: plain-text label per Measurement element, in the
// same order performMeasurement() delivers them (F1..F8, Clear, NIR).
// Do NOT use from generic code to interpret the Measurement content.
const char* const AS7341Spectrometer::MEASUREMENT_LABELS[AS7341Spectrometer::N_CH] = {
  "F1_415nm", "F2_445nm", "F3_480nm", "F4_515nm",
  "F5_555nm", "F6_590nm", "F7_630nm", "F8_680nm",
  "Clear", "NIR_910nm"
};

bool AS7341Spectrometer::begin() {
  return as7341_.begin();
}

void AS7341Spectrometer::applySettings(const OpticalSettings& settings) {
  as7341_.setATIME(settings.sensor.atime);
  as7341_.setASTEP(settings.sensor.astep);
  as7341_.setGain(settings.sensor.gain);
}

// All constants here are a deliberately simple starting point, not a
// fully-tuned solution -- see plan/context: which stop threshold actually
// delivers <1 DeltaE measurement accuracy still needs to be tested empirically.
static const uint8_t SINGLE_SAMPLES         = 1;
// n=4 had ~41% relative uncertainty of the SD estimate itself
// (1/sqrt(2*(n-1))) -- the very first converged() check could therefore turn
// positive purely by chance too early. n=8 (~27%) is noticeably more robust,
// increasing it further brings diminishing returns -- TODO tune.
static const uint8_t PRECISE_MIN_SAMPLES    = 8;
static const uint8_t PRECISE_MAX_SAMPLES    = 32;    // cap, replaces the former fixed N_AVG=16
static const float   PRECISE_TARGET_REL_SEM = 0.01f; // 1% relative standard error of the mean -- TODO tune

// For checkValidity()/normalize() (see there). ADC full-scale per the
// AS7341 datasheet: (ATIME+1)*(ASTEP+1), capped at the 16 bits of the
// data register (65535). With short integration times, the
// full-scale value is therefore well BELOW 65535 (e.g. ATIME 150/ASTEP 100 ->
// 15251) -- a fixed 65535 threshold would never detect clipping there.
// SATURATION_LIMIT_FRAC exactly as in the earlier exposure-assistant
// design. ASTEP_TIME_MS per the datasheet/Adafruit_AS7341::toBasicCounts().
static const float    SATURATION_LIMIT_FRAC = 0.80f;
static const float    ASTEP_TIME_MS         = 0.00278f;
// Order == as7341_gain_t (see Adafruit_AS7341.h), like GAIN_LABELS in
// SettingsCodec.cpp.
static const float GAIN_MULTIPLIERS[AS7341_GAIN_COUNT] = {
  0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f, 256.0f, 512.0f
};
// Channels with a mean below this value do not count toward the convergence
// check, but are reported to performMeasurement() via anyChannelUnmeasurable
// so that an unmeasurable channel does not silently lead to a flattering
// precision figure (see below). The constant itself now lives publicly in the
// header (AS7341Spectrometer::NOISE_FLOOR_COUNTS) because checkValidity()
// needs it too -- here just a short alias.
static constexpr float NOISE_FLOOR_COUNTS = AS7341Spectrometer::NOISE_FLOOR_COUNTS;

// Worst relative standard error of the mean across all channels with
// signal above NOISE_FLOOR_COUNTS (otherwise the noise of very dark
// channels would dominate the relative error without saying anything about
// measurement quality). worstOut therefore continues to describe ONLY the
// measurable channels -- anyChannelUnmeasurable makes it visible when that
// was NOT all channels, so that a caller does not mistake this partial
// information for a complete precision statement about the ENTIRE
// measurement (e.g. a colorful sample where only a single, strongly
// absorbing band stays below the noise floor -- previously this channel was
// simply ignored, which made the result look more optimistic than it
// actually was). The stop/acceptance criterion itself deliberately remains
// dependent ONLY on the measurable channels (see the return value below)
// -- an unmeasurable channel should not prevent convergence, since more
// samples would not raise its mean anyway (otherwise e.g. a black sample
// could never be measured at all in Precise mode). The actual
// correction (setting the reported relSEM value to NAN when
// anyChannelUnmeasurable, instead of reporting a flattering number derived
// from only the good channels) therefore does NOT happen here, but in
// performMeasurement().
//
// anyChannelEvaluated reports whether any channel at all was above the
// noise floor -- worstOut is meaningless if not (it stays at
// its initial value of 0.0). performMeasurement() handles this case (e.g.
// a very dark sample/dark measurement) explicitly and separately, see there.
// worstOut is passed through in addition to the return value (the
// convergence yes/no), so that performMeasurement() can pass on the value
// achieved as telemetry (relSemWorst), instead of discarding it after the
// yes/no decision as before.
static bool converged(const uint16_t buf[][AS7341Spectrometer::N_CH], uint8_t taken,
                       bool& anyChannelEvaluated, bool& anyChannelUnmeasurable, float& worstOut) {
  float worst = 0.0f;
  anyChannelEvaluated = false;
  anyChannelUnmeasurable = false;
  for (uint8_t ch = 0; ch < AS7341Spectrometer::N_CH; ch++) {
    float mean = 0.0f;
    for (uint8_t i = 0; i < taken; i++) mean += buf[i][ch];
    mean /= taken;
    if (mean < NOISE_FLOOR_COUNTS) {
      anyChannelUnmeasurable = true;
      continue;
    }
    anyChannelEvaluated = true;

    float varSum = 0.0f;
    for (uint8_t i = 0; i < taken; i++) { float d = buf[i][ch] - mean; varSum += d * d; }
    float stddev = sqrtf(varSum / (taken - 1));
    float relSEM = (stddev / mean) / sqrtf((float)taken);
    if (relSEM > worst) worst = relSEM;
  }
  worstOut = worst;
  return anyChannelEvaluated && (worst <= PRECISE_TARGET_REL_SEM);
}

// Small helper instead of aggregate initialization: MeasurementTelemetry has
// in-class default initializers (for status/sampleCount/relSemWorst), which
// makes the type no longer an aggregate under the C++ standard used here
// (pre-C++14) -- "MeasurementTelemetry{a,b,c}" would therefore not
// compile (no matching constructor).
static void setTelemetry(MeasurementTelemetry* out, MeasurementStatus status, uint8_t sampleCount, float relSemWorst) {
  if (!out) return;
  out->status = status;
  out->sampleCount = sampleCount;
  out->relSemWorst = relSemWorst;
}

Measurement AS7341Spectrometer::performMeasurement(Precision precision, ProgressCallback onProgress,
                                                    MeasurementTelemetry* outTelemetry) {
  uint8_t maxSamples = (precision == Precision::Single) ? SINGLE_SAMPLES : PRECISE_MAX_SAMPLES;
  uint8_t minSamples = (precision == Precision::Single) ? SINGLE_SAMPLES : PRECISE_MIN_SAMPLES;

  // Resolves the AS7341 raw channel order. readAllChannels() internally does
  // two integration cycles with different SMUX configurations (see
  // Adafruit_AS7341::setup_F1F4_Clear_NIR()/setup_F5F8_Clear_NIR()):
  //   Cycle 1 (slot 0-5):  F1, F2, F3, F4, Clear, NIR
  //   Cycle 2 (slot 6-11): F5, F6, F7, F8, Clear, NIR
  // Slot 4/5 are a first (surplus) Clear/NIR measurement pair -- we
  // ignore it and instead take the second pair from slot 10/11.
  // The only place in the entire code that needs to know this order.
  static const uint8_t SRC_IDX[N_CH] = { 0, 1, 2, 3, 6, 7, 8, 9, 10, 11 };

  static uint16_t buf[PRECISE_MAX_SAMPLES][N_CH];  // ~640B, static to spare the stack
  uint8_t taken = 0;
  uint8_t consecutiveConverged = 0;
  bool stoppedShortNoSignal = false;  // see "no channel evaluated" short-circuit below
  float lastRelSemWorst = NAN;  // only set with Precision::Precise AND at least one evaluated channel
  bool lastAnyUnmeasurable = false;  // at least 1 (but not all) channels below the noise floor -- see converged()

  for (uint8_t n = 0; n < maxSamples; n++) {
    if (onProgress) onProgress(taken, maxSamples);
    uint16_t r[12];
    if (!as7341_.readAllChannels(r)) continue;
    for (uint8_t i = 0; i < N_CH; i++) buf[taken][i] = r[SRC_IDX[i]];
    taken++;

    if (precision == Precision::Precise && taken >= minSamples) {
      bool anyChannelEvaluated;
      bool anyChannelUnmeasurable;
      float relSemWorst;
      bool isConverged = converged(buf, taken, anyChannelEvaluated, anyChannelUnmeasurable, relSemWorst);
      if (!anyChannelEvaluated) {
        // Deliberate short-circuit decision: no channel has signal above
        // the noise floor (e.g. a very dark sample/dark measurement) -- the
        // relative precision threshold is not meaningful for channels without
        // signal, more samples would systematically change nothing about that.
        // Hence an immediate stop at minSamples, without waiting for the
        // usual 2-of-2 confirmation (see below). lastRelSemWorst
        // deliberately stays NAN -- no real value was ever computed.
        stoppedShortNoSignal = true;
        break;
      }
      lastRelSemWorst = relSemWorst;
      lastAnyUnmeasurable = anyChannelUnmeasurable;
      if (isConverged) {
        // Requires two consecutive hits instead of just one --
        // mitigates "optional stopping" bias (a single randomly
        // favorable intermediate value would otherwise end the measurement
        // prematurely with an overly optimistic bias). With a cap of 32, the
        // practical harm of a single hit is limited, but the correction is
        // cheap enough to include anyway -- TODO tune.
        consecutiveConverged++;
        if (consecutiveConverged >= 2) break;
      } else {
        consecutiveConverged = 0;
      }
    }
  }
  if (onProgress) onProgress(taken, maxSamples);

  if (taken == 0) {
    // Sensor delivers no valid data at all -- hardware fault.
    setTelemetry(outTelemetry, MeasurementStatus::SensorError, 0, NAN);
    return Measurement();
  }

  bool converged_enough = (precision != Precision::Precise)
                        || stoppedShortNoSignal
                        || (consecutiveConverged >= 2);
  if (!converged_enough) {
    // maxSamples exhausted without confirming the target precision (two
    // consecutive converged() hits) -- e.g. an ongoing disturbance during
    // the measurement (the device is being moved). Explicitly
    // distinguishable from the sensor-fault case above, see MeasurementStatus.
    // sampleCount/relSemWorst are still passed along (diagnostic value), even
    // though the Measurement itself is discarded.
    setTelemetry(outTelemetry, MeasurementStatus::NotConverged, taken, lastRelSemWorst);
    return Measurement();
  }

  // Precision::Single never computes a relSEM (the convergence branch above
  // does not run there at all) -- lastRelSemWorst then correctly stays NAN.
  //
  // An unmeasurable channel (below the noise floor) must not flatter the
  // reported precision -- the measurement itself remains valid (it is still
  // returned/stored, e.g. for a black or strongly absorbing sample), but
  // without a number that falsely implies precision for a channel that was
  // never actually assessed. Same NAN convention as the already existing
  // "completely dark sample" case above (stoppedShortNoSignal).
  float reportedRelSemWorst = lastRelSemWorst;
  if (lastAnyUnmeasurable) {
    Serial.println("# Hinweis: mindestens ein Kanal blieb unterhalb der Rauschgrenze -- relSEM daher nicht ausgewiesen");
    reportedRelSemWorst = NAN;
  }
  setTelemetry(outTelemetry, MeasurementStatus::Ok, taken, reportedRelSemWorst);

  // Plain mean over all collected samples -- no more
  // outlier trimming (see context: the adaptive sampling itself
  // already dampens short disturbances, without undermining the precision
  // certified by converged() on untrimmed data).
  Measurement m(N_CH);
  for (uint8_t ch = 0; ch < N_CH; ch++) {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < taken; i++) sum += buf[i][ch];
    m[ch] = (float)sum / (float)taken;
  }

  // Absolute standard error of the mean PER CHANNEL (see
  // MeasurementTelemetry::semPerChannel) -- unfiltered, deliberately WITHOUT the
  // NOISE_FLOOR_COUNTS exception from converged() (which only decides on
  // stopping the sample loop, not on the usability of the
  // data). Only computable from 2 samples onward (Precision::Single takes exactly 1).
  if (outTelemetry && taken >= 2) {
    outTelemetry->semPerChannel.assign(N_CH, 0.0f);
    for (uint8_t ch = 0; ch < N_CH; ch++) {
      float varSum = 0.0f;
      for (uint8_t i = 0; i < taken; i++) { float d = (float)buf[i][ch] - m[ch]; varSum += d * d; }
      float stddev = sqrtf(varSum / (float)(taken - 1));
      outTelemetry->semPerChannel[ch] = stddev / sqrtf((float)taken);
    }
  }
  return m;
}

// Optical crosstalk: NIR light affects the VIS channels to varying
// degrees -- WHICH channels are evaluable at all and with what
// factor they need to be corrected depends on the IR-cut filter in
// use (empirically determined via comparison measurements of the same
// samples with/without the 650nm or 700nm filter). channelIndex refers to the
// associated raw F1..F8 slot (0..7) -- the raw channel acquisition itself
// remains unaffected by this.
struct VisBandDef {
  uint8_t channelIndex;
  float center_nm;
  float fwhm_nm;
  float nirFactor;
  float nirFactorErr;  // uncertainty of nirFactor -- basis of Spectrum::valueErrors
};

// No filter: F1-F7 correctable, F8 has a correction factor of unknown
// magnitude -> omitted entirely instead of being output uncorrected.
static const VisBandDef BANDS_NONE[] = {
  { 0, 415.0f, 26.0f, 0.355f, 0.060f },  // F1
  { 1, 445.0f, 30.0f, 0.109f, 0.019f },  // F2
  { 2, 480.0f, 36.0f, 0.089f, 0.012f },  // F3
  { 3, 515.0f, 39.0f, 0.036f, 0.007f },  // F4
  { 4, 555.0f, 39.0f, 0.057f, 0.011f },  // F5
  { 5, 590.0f, 40.0f, 0.0f,   0.12f  },  // F6
  { 6, 630.0f, 50.0f, 0.0f,   0.0f   },  // F7
};

// 700nm filter: F1-F7 with more precise factors; F8 REDEFINED as its own
// 674nm/45nm band instead of being discarded as a "truncated 680nm/52nm
// channel" -- the 700nm cut filter makes this narrower, effectively used
// sensitivity range NIR-free by construction (hence factor 0).
static const VisBandDef BANDS_700NM[] = {
  { 0, 415.0f, 26.0f, 0.125f, 0.045f },  // F1
  { 1, 445.0f, 30.0f, 0.032f, 0.017f },  // F2
  { 2, 480.0f, 36.0f, 0.024f, 0.013f },  // F3
  { 3, 515.0f, 39.0f, 0.009f, 0.011f },  // F4
  { 4, 555.0f, 39.0f, 0.026f, 0.014f },  // F5
  { 5, 590.0f, 40.0f, 0.0f,   0.14f  },  // F6
  { 6, 630.0f, 50.0f, 0.0f,   0.07f  },  // F7
  { 7, 674.0f, 45.0f, 0.0f,   0.03f  },  // F8, redefined, NIR-free by construction, uncertainty geometrically determined (no clean sigmoid shape)
};

// 650nm filter: already blocks from 650nm onward -> F1-F6 NIR-free by
// construction (factor 0), F7/F8 are dropped (the filter already cuts into
// their actual sensitivity range, "not usable").
// The uncertainty "0.0f" is an assumption that has not been further checked.
// There is a lack of available methods to characterize this uncertainty.
// However, the remaining "true" NIR component is fairly certainly negligible.
// The remaining raw display value is caused by VIS crosstalk into the NIR channel.
static const VisBandDef BANDS_650NM[] = {
  { 0, 415.0f, 26.0f, 0.0f, 0.0f },  // F1
  { 1, 445.0f, 30.0f, 0.0f, 0.0f },  // F2
  { 2, 480.0f, 36.0f, 0.0f, 0.0f },  // F3
  { 3, 515.0f, 39.0f, 0.0f, 0.0f },  // F4
  { 4, 555.0f, 39.0f, 0.0f, 0.0f },  // F5
  { 5, 590.0f, 40.0f, 0.0f, 0.0f },  // F6
};

static void bandsForFilterState(FilterState fs, const VisBandDef*& defs, size_t& count) {
  switch (fs) {
    case FilterState::Filter700nm:
      defs = BANDS_700NM;
      count = sizeof(BANDS_700NM) / sizeof(BANDS_700NM[0]);
      break;
    case FilterState::Filter650nm:
      defs = BANDS_650NM;
      count = sizeof(BANDS_650NM) / sizeof(BANDS_650NM[0]);
      break;
    default:
      defs = BANDS_NONE;
      count = sizeof(BANDS_NONE) / sizeof(BANDS_NONE[0]);
      break;
  }
}

uint8_t AS7341Spectrometer::relevantChannels(FilterState fs, uint8_t outIdx[N_CH]) const {
  const VisBandDef* defs;
  size_t n;
  bandsForFilterState(fs, defs, n);
  uint8_t count = 0;
  for (size_t i = 0; i < n && count < N_CH; i++) outIdx[count++] = defs[i].channelIndex;
  return count;
}

// ADC quantization (+-0.5 LSB) is independent of gain/integration time --
// a channel still needs a small absolute minimum value, otherwise pure
// rounding noise dominates, even with an excellent relative SEM.
// Deliberately much smaller than NOISE_FLOOR_COUNTS(=50) -- the actual "is
// this signal at all" question is now handled by SEPARATION_Z_THRESHOLD.
// TODO tune.
static const float MIN_QUANTIZATION_COUNTS = 10.0f;

// Limit-of-Detection test (IUPAC convention: signal > blank mean +
// 3 * blank spread), applied here to two independent means (white,
// dark) each with its own standard error of the mean (SEM): the
// difference must exceed the COMBINED (added in quadrature) uncertainty of
// both by a factor of 3. Same threshold as
// TipCatalog.cpp::PLAUSIBILITY_Z_THRESHOLD (there: outlier detection among
// fingerprints) -- different application, same standard heuristic for
// "statistically significantly different".
static const float SEPARATION_Z_THRESHOLD = 3.0f;

MeasurementValidity AS7341Spectrometer::checkValidity(const Measurement& raw, const Measurement& rawSem,
                                                       const OpticalSettings& settings,
                                                       const Measurement& darkMean, const Measurement& darkSem) const {
  MeasurementValidity result;
  if (raw.size() != N_CH) return result;  // ok bleibt false

  uint32_t fullScale = adcFullScale(settings.sensor.atime, settings.sensor.astep);
  float satLimit = SATURATION_LIMIT_FRAC * (float)fullScale;
  for (uint8_t c = 0; c < N_CH; c++) {
    if (raw[c] > satLimit) result.anyClipping = true;
  }

  // Without a dark reference (e.g. never measured yet) the separation check
  // below cannot be performed -- it is simply skipped then, the quantization
  // lower bound still remains in effect.
  bool haveDark = (darkMean.size() == N_CH);

  uint8_t relIdx[N_CH];
  uint8_t relCount = relevantChannels(settings.filterState, relIdx);
  for (uint8_t i = 0; i < relCount; i++) {
    uint8_t c = relIdx[i];
    if (raw[c] < MIN_QUANTIZATION_COUNTS) { result.anyBelowNoiseFloor = true; continue; }
    if (!haveDark) continue;
    float rawSemC  = (rawSem.size()  == N_CH) ? rawSem[c]  : 0.0f;
    float darkSemC = (darkSem.size() == N_CH) ? darkSem[c] : 0.0f;
    float combinedSem = sqrtf(rawSemC * rawSemC + darkSemC * darkSemC);
    if ((raw[c] - darkMean[c]) <= SEPARATION_Z_THRESHOLD * combinedSem) result.anyBelowNoiseFloor = true;
  }

  result.ok = !result.anyClipping && !result.anyBelowNoiseFloor;
  return result;
}

Measurement AS7341Spectrometer::normalize(const Measurement& raw, const SensorSettings& sensor) const {
  uint8_t gainIdx = static_cast<uint8_t>(sensor.gain);
  float gainMul = (gainIdx < AS7341_GAIN_COUNT) ? GAIN_MULTIPLIERS[gainIdx] : 1.0f;
  float integrationMs = (sensor.atime + 1) * (sensor.astep + 1) * ASTEP_TIME_MS;
  float divisor = gainMul * integrationMs;
  Measurement out(raw.size());
  for (size_t i = 0; i < raw.size(); i++) out[i] = (divisor > 0.0f) ? (raw[i] / divisor) : 0.0f;
  return out;
}

void AS7341Spectrometer::computeReflectance(const Measurement& measurement,
                                             const Measurement& whiteReference,
                                             const Measurement& darkReference,
                                             FilterState filterState,
                                             Spectrum& out) const {
  bool haveCal = (whiteReference.size() == N_CH && darkReference.size() == N_CH);
  bool haveMeasurement = (measurement.size() == N_CH);

  // Same formula for VIS channels and NIR -- no upper clamping,
  // mirrors data/colorimeter.py.
  auto reflectance = [&](uint8_t ch) -> float {
    if (!haveCal || !haveMeasurement) return 0.0f;
    float denom = whiteReference[ch] - darkReference[ch];
    if (fabsf(denom) < 1e-6f) return 0.0f;
    float r = (measurement[ch] - darkReference[ch]) / denom;
    return (r < 0.0f) ? 0.0f : r;
  };

  float R_nir = reflectance(N_CH - 1);

  const VisBandDef* defs;
  size_t n;
  bandsForFilterState(filterState, defs, n);

  out.bands.resize(n);
  out.values.resize(n);
  out.valueErrors.resize(n);

  for (size_t i = 0; i < n; i++) {
    const VisBandDef& def = defs[i];
    out.bands[i] = Band{ def.center_nm, def.fwhm_nm };

    float rawR = reflectance(def.channelIndex);
    auto correct = [&](float factor) -> float {
      float r = (rawR - factor * R_nir) / (1.0f - factor);
      return (r < 0.0f) ? 0.0f : r;  // clamp again -- the NIR correction can pull into negative values
    };
    out.values[i] = correct(def.nirFactor);

    // Error range: half the span of the correction at +-1 sigma of the factor
    // -- a simple numerical sensitivity estimate instead of an
    // analytically derived derivative; sufficient and less error-prone
    // given uncertainties that are only roughly known anyway.
    out.valueErrors[i] = (def.nirFactorErr > 0.0f)
        ? fabsf(correct(def.nirFactor + def.nirFactorErr) - correct(def.nirFactor - def.nirFactorErr)) / 2.0f
        : 0.0f;
  }
}

Spectrum AS7341Spectrometer::getSpectrum(const Measurement& measurement,
                                          const Measurement& whiteReference,
                                          const Measurement& darkReference,
                                          FilterState filterState) const {
  Spectrum s;
  computeReflectance(measurement, whiteReference, darkReference, filterState, s);
  return s;
}
