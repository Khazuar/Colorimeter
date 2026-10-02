#include "TipCatalog.h"
#include <algorithm>
#include <cmath>

namespace {

// Classic outlier test: 3 standard deviations around the mean
// are considered "still normal" (3-sigma rule) -- standard heuristic for
// approximately normally distributed measurement series.
const float PLAUSIBILITY_Z_THRESHOLD = 3.0f;

// Floor for the effective standard deviation per channel, relative to the
// channel mean -- prevents a sample mean that happens to lie very
// tightly clustered from making the test oversensitive.
// Chosen conservatively relative to the precision of 1% relSEM already
// targeted per individual measurement anyway (PRECISE_TARGET_REL_SEM, AS7341Spectrometer.cpp).
const float MIN_RELATIVE_STD = 0.02f;

std::vector<float> channelMean(const std::vector<const Measurement*>& samples, size_t ch) {
  std::vector<float> mean(ch, 0.0f);
  for (const Measurement* s : samples)
    for (size_t c = 0; c < ch; c++) mean[c] += (*s)[c];
  for (size_t c = 0; c < ch; c++) mean[c] /= (float)samples.size();
  return mean;
}

// Sample standard deviation (Bessel-corrected, n-1) per channel around an
// already-computed mean. Precondition: samples.size() >= 2 (ensured by
// the caller).
void channelStdDev(const std::vector<const Measurement*>& samples, size_t ch,
                    const std::vector<float>& mean, std::vector<float>& outStd) {
  outStd.assign(ch, 0.0f);
  for (const Measurement* s : samples)
    for (size_t c = 0; c < ch; c++) { float d = (*s)[c] - mean[c]; outStd[c] += d * d; }
  for (size_t c = 0; c < ch; c++) outStd[c] = sqrtf(outStd[c] / (float)(samples.size() - 1));
}

// Aggregates the spread (as a coefficient of variation per channel) across all
// OTHER tips, WITHOUT mixing their raw values: each tip gets its own
// mean/own standard deviation (its fingerprints with a matching
// sensorId), the coefficients of variation derived from these are weighted
// (weight = number of fingerprints - 1, as with a pooled variance)
// and averaged -- different tips have different optical
// paths, i.e. a different absolute scale, so this is computed separately
// per tip instead of simply throwing all raw values together. Tips with < 2
// matching fingerprints contribute nothing (standard deviation not
// defined). false if, in total, too few entries
// (< MIN_FINGERPRINTS_FOR_FALLBACK_STATS) contributed.
bool pooledCoefficientOfVariation(const std::vector<MeasurementTip>& tips, const std::string& excludeName,
                                   const std::string& sensorId, size_t ch, std::vector<float>& outCv) {
  std::vector<float> weightedSum(ch, 0.0f);
  float weightTotal = 0.0f;
  size_t contributingSamples = 0;
  for (const MeasurementTip& t : tips) {
    if (t.name == excludeName) continue;
    std::vector<const Measurement*> samples;
    for (const WhiteFingerprint& fp : t.whiteFingerprints)
      if (fp.sensorId == sensorId) samples.push_back(&fp.normalized);
    if (samples.size() < 2) continue;
    std::vector<float> mean = channelMean(samples, ch);
    std::vector<float> std_;
    channelStdDev(samples, ch, mean, std_);
    float weight = (float)(samples.size() - 1);
    for (size_t c = 0; c < ch; c++) {
      float cv = (mean[c] > 0.0f) ? (std_[c] / mean[c]) : 0.0f;
      weightedSum[c] += weight * cv;
    }
    weightTotal += weight;
    contributingSamples += samples.size();
  }
  if (contributingSamples < MIN_FINGERPRINTS_FOR_FALLBACK_STATS || weightTotal <= 0.0f) return false;
  outCv.assign(ch, 0.0f);
  for (size_t c = 0; c < ch; c++) outCv[c] = weightedSum[c] / weightTotal;
  return true;
}

}  // namespace

PlausibilityResult MeasurementTip::isPlausible(const WhiteFingerprint& candidate,
                                                const TipCatalog& catalog) const {
  // Measurement is never empty per Spectrometer contract (buildWhiteFingerprint()
  // is only called with an already validated measurement) -- no
  // special handling needed for the empty case.
  const size_t ch = candidate.normalized.size();

  // Only fingerprints of THE SAME sensor are absolutely comparable (see
  // the WhiteFingerprint comment in TipCatalog.h).
  std::vector<const Measurement*> own;
  for (const WhiteFingerprint& fp : whiteFingerprints)
    if (fp.sensorId == candidate.sensorId) own.push_back(&fp.normalized);

  if (own.empty()) return PlausibilityResult::Indeterminate;  // no basis for comparison

  std::vector<float> ownMean = channelMean(own, ch);
  std::vector<float> effectiveStd;
  bool viaFallback = false;

  if (own.size() >= MIN_FINGERPRINTS_FOR_DIRECT_STATS) {
    channelStdDev(own, ch, ownMean, effectiveStd);
  } else {
    // Fallback: too few own fingerprints for a direct estimate
    // -- instead estimate the spread from the fingerprints of ALL OTHER tips
    // (only the pooled coefficient of variation is adopted,
    // see pooledCoefficientOfVariation()).
    std::vector<float> cv;
    if (!pooledCoefficientOfVariation(catalog.tips, name, candidate.sensorId, ch, cv)) {
      return PlausibilityResult::Indeterminate;
    }

    effectiveStd.assign(ch, 0.0f);
    for (size_t c = 0; c < ch; c++) effectiveStd[c] = cv[c] * ownMean[c];
    viaFallback = true;
  }

  for (size_t c = 0; c < ch; c++) {
    float stdFloor = MIN_RELATIVE_STD * ownMean[c];
    float std_c = std::max(effectiveStd[c], stdFloor);
    if (std_c <= 0.0f) continue;  // mean 0 -- no meaningful test possible
    float z = fabsf(candidate.normalized[c] - ownMean[c]) / std_c;
    if (z > PLAUSIBILITY_Z_THRESHOLD) return PlausibilityResult::Implausible;
  }
  return viaFallback ? PlausibilityResult::PlausibleViaFallback : PlausibilityResult::Plausible;
}

WhiteFingerprintStats MeasurementTip::whiteFingerprintStats(const std::string& sensorId) const {
  WhiteFingerprintStats result;
  std::vector<const Measurement*> own;
  for (const WhiteFingerprint& fp : whiteFingerprints)
    if (fp.sensorId == sensorId) own.push_back(&fp.normalized);

  result.n = own.size();
  if (own.empty()) return result;

  const size_t ch = own[0]->size();
  result.mean = channelMean(own, ch);
  if (own.size() >= 2) {
    std::vector<float> std_;
    channelStdDev(own, ch, result.mean, std_);
    float sqrtN = sqrtf((float)own.size());
    result.sem.assign(ch, 0.0f);
    for (size_t c = 0; c < ch; c++) result.sem[c] = std_[c] / sqrtN;
  }
  return result;
}

std::vector<size_t> TipCatalog::rankPlausibleTips(const WhiteFingerprint& candidate,
                                                    const std::string& excludeName) const {
  std::vector<size_t> direct, viaFallback;
  for (size_t i = 0; i < tips.size(); i++) {
    if (tips[i].name == excludeName) continue;
    PlausibilityResult r = tips[i].isPlausible(candidate, *this);
    if (r == PlausibilityResult::Plausible) direct.push_back(i);
    else if (r == PlausibilityResult::PlausibleViaFallback) viaFallback.push_back(i);
  }
  direct.insert(direct.end(), viaFallback.begin(), viaFallback.end());
  return direct;
}
