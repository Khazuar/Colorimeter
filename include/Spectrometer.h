#pragma once
#include <cstdint>
#include <vector>
#include <cmath>

// Result of a physical measurement: the structure and meaning of its
// elements are an implementation detail of the respective sensor (order,
// count, which channel sits at which position). Generic code should only
// pass a Measurement through as an opaque black box (to getSpectrum()
// or for persistence) -- see Spectrometer::measurementLabels() for
// the single permitted exception (debug/analysis).
using Measurement = std::vector<float>;

struct Lab {
  float L;
  float a;
  float b;
};

// A single band as center + full width at half maximum (FWHM), the way
// optical bandpass filters (e.g. in the AS7341 datasheet) are actually
// characterized: sensitivity falls off on both sides of the center
// (approximately bell-shaped) instead of cutting off sharply at one edge;
// fwhm_nm is the width at which sensitivity is still 50% of the maximum.
// Bands need be neither gapless nor non-overlapping -- every sensor has its
// own filter characteristics, and the abstraction must not constrain that.
struct Band {
  float center_nm;
  float fwhm_nm;
};

// Sensor-INDEPENDENT spectrum: N bands (each with its own center/width,
// ascending by center) with calibrated reflectance values (~0..1+).
// Deliberately only the visible spectrum -- channels like Clear or NIR,
// which do not represent a "real" band of the visible spectrum (NIR would
// e.g. be a nonsensically wide gap to the last VIS band), do NOT belong
// here, but remain part of the sensor-specific Measurement.
struct Spectrum {
  std::vector<Band> bands;
  std::vector<float> values;
  // Absolute uncertainty per band, derived from the uncertainty of the NIR
  // correction factor used for it (see AS7341Spectrometer.cpp) -- 0.0 where
  // no error model exists. For now only carried internally (no CSV
  // export), the basis for a future DeltaE uncertainty calculation.
  std::vector<float> valueErrors;
};

enum class Precision : uint8_t { Single = 0, Precise = 1, COUNT = 2 };

// Which physical (bandpass/cut) filter currently sits in front of the
// sensor. A generic concept (any Spectrometer could support something like
// this), even though the concrete values are currently only populated by
// AS7341Spectrometer -- same pragmatism as with Precision::Single/Precise.
enum class FilterState : uint8_t { None = 0, Filter650nm = 1, Filter700nm = 2, COUNT = 3 };

// current: how many individual samples have already been taken; maxEstimate: upper
// bound (with Precision::Precise possibly not exhausted, if convergence happens earlier).
// Plain function pointer (not std::function) -- no heap usage required.
using ProgressCallback = void (*)(uint8_t current, uint8_t maxEstimate);

// SensorError: the sensor delivers no valid data at all
// (hardware fault). NotConverged: only possible with Precision::Precise --
// the target precision was not reached within the sample budget (e.g.
// the device was moved continuously during the measurement). Both cases
// return an empty Measurement (unusable data stays unusable), but differ
// in cause -- for a different error message shown to the user
// ("sensor error" vs. "hold the device still and try again").
enum class MeasurementStatus : uint8_t { Ok, SensorError, NotConverged };

// Telemetry for ONE performMeasurement() execution, beyond the pure
// measurement data -- an optional out-parameter (callers who only want to
// know "did it work" can keep simply checking Measurement::empty()).
//
// sampleCount is the number of individual samples actually averaged (always
// 1 with Precision::Single). relSemWorst is the worst relative
// standard error of the mean across all channels above the
// noise floor (see AS7341Spectrometer.cpp converged()) -- an upper
// bound value, NOT a per-channel value (the actual relSEM of individual
// channels can be lower). NAN if no relSEM was computed: with
// Precision::Single (no convergence happens there at all); with Precision::Precise,
// if NO channel was above the noise floor (e.g. a very dark sample/
// dark measurement); AND with Precision::Precise, if DURING an otherwise
// successful measurement at least one (but not all) channels stayed below
// the noise floor -- the measurement itself (Measurement) is still
// valid and non-empty in that case, only the precision statement is
// deliberately dropped for the ENTIRE measurement (not just the affected
// channel), rather than reporting a number that implies precision for a
// channel that was never actually assessed. Deliberately NOT 0.0, which
// would be a false statement about the precision actually achieved.
struct MeasurementTelemetry {
  MeasurementStatus status = MeasurementStatus::Ok;
  uint8_t sampleCount = 0;
  float relSemWorst = NAN;

  // Absolute standard error of the mean PER CHANNEL, in raw-value units
  // (NOT relative like relSemWorst) -- empty with Precision::Single (only 1
  // sample, no spread computable) or if otherwise not determinable.
  // UNFILTERED (no NOISE_FLOOR_COUNTS exception like relSemWorst) --
  // consumers (e.g. Spectrometer::checkValidity()) evaluate usability
  // themselves, based on an actual comparison against a reference,
  // instead of relying on a pre-filtered value.
  Measurement semPerChannel;
};

// Forward declarations instead of #include "AppConfig.h" -- avoids a
// circular include (AppConfig.h already includes this header for
// FilterState). References to an incomplete type are enough for a
// pure method declaration; every implementation (e.g. AS7341Spectrometer.cpp)
// sees the full definitions via its own #include "AppConfig.h".
struct OpticalSettings;
struct SensorSettings;

// Result of Spectrometer::checkValidity() -- whether a raw measurement
// is reliable enough to be adopted as e.g. a white reference/fingerprint.
// Sensor-independent; WHICH internal channels/thresholds are used for
// that is decided by each implementation itself.
// anyBelowNoiseFloor covers TWO distinct causes here (see the
// respective implementation): a small absolute minimum value (getting out
// of ADC quantization) AND a lack of statistical separation from the
// dark reference (Limit-of-Detection test) -- for main.cpp/the display
// deliberately ONE shared flag ("channel too dark"), since both cases have
// the same consequence for the user.
struct MeasurementValidity {
  bool ok = false;
  bool anyBelowNoiseFloor = false;
  bool anyClipping = false;
};

class Spectrometer {
public:
  virtual ~Spectrometer() = default;

  // A physical measurement. Encapsulates everything sensor-specific (I2C, SMUX
  // configuration, reading/converting registers) -- the result is an
  // opaque Measurement, no structure should be relied upon. onProgress (if
  // set) is called repeatedly during the measurement so that the
  // orchestration can e.g. draw a progress indicator. outTelemetry
  // (if set) provides status/sample count/relSEM -- see MeasurementTelemetry.
  virtual Measurement performMeasurement(Precision precision,
                                          ProgressCallback onProgress = nullptr,
                                          MeasurementTelemetry* outTelemetry = nullptr) = 0;

  // ONLY for debug/analysis purposes: a plain-text label per Measurement
  // element, in the same order and length as a Measurement from the same sensor.
  // Must NOT be used by other code to interpret the CONTENT of a
  // Measurement (that would be a break of the abstraction) --
  // intended solely for human-readable labeling in the debug export.
  virtual const char* const* measurementLabels() const = 0;

  // Short, stable identifier of the sensor type (e.g. "AS7341") -- intended
  // ONLY as a comparison key (see TipCatalog.h::WhiteFingerprint), NOT
  // for display. Two WhiteFingerprint values are only meaningfully
  // comparable if their sensorId matches (a plain length comparison of the
  // Measurement vectors would be no real guarantee,
  // see MeasurementTip::isPlausible()). Must remain
  // PERMANENTLY stable for a given sensor -- a later change would silently
  // make existing fingerprints incomparable with new ones (no
  // data loss, they would then simply never be used again).
  virtual const char* sensorId() const = 0;

  // Pure computation: builds a sensor-independent Spectrum from a
  // measurement plus white/dark reference measurement (same Measurement format).
  // No hardware access, no stored calibration -- every call is
  // a pure function of its three inputs. How to handle an empty/missing
  // reference is decided by the respective implementation.
  virtual Spectrum getSpectrum(const Measurement& measurement,
                                const Measurement& whiteReference,
                                const Measurement& darkReference,
                                FilterState filterState) const = 0;

  // Checks a RAW measurement (e.g. a white reference) for reliability
  // -- e.g. channels that clip, fall below an ADC quantization
  // lower bound, or fail to separate significantly from the dark reference
  // (Limit-of-Detection test: the difference must be large relative to
  // the combined uncertainty of both measurements, see rawSem/darkSem).
  // darkMean/darkSem may be empty (e.g. no dark measurement
  // taken yet) -- in that case only the separation check is skipped, not the
  // others. WHICH channels/thresholds are used in detail is
  // up to the respective implementation. Callers (main.cpp) use ONLY
  // this interface, never sensor-specific details -- prepared for a
  // future second Spectrometer (e.g. AS7343).
  virtual MeasurementValidity checkValidity(const Measurement& raw, const Measurement& rawSem,
                                             const OpticalSettings& settings,
                                             const Measurement& darkMean, const Measurement& darkSem) const = 0;

  // Raw value normalized to gain/integration time (sensor-specific
  // meaning of "gain"/"integration time") -- makes measurements under
  // different sensor settings directly comparable (e.g. for
  // measurement tip fingerprints, see TipCatalog.h).
  virtual Measurement normalize(const Measurement& raw,
                                 const SensorSettings& sensor) const = 0;
};
