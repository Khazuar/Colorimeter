#include "TipCatalog.h"
#include <algorithm>
#include <cmath>

namespace {

// Klassischer Ausreisser-Test: 3 Standardabweichungen um den Mittelwert
// gelten als "noch normal" (3-Sigma-Regel) -- Standardheuristik fuer
// angenaehert normalverteilte Messreihen.
const float PLAUSIBILITY_Z_THRESHOLD = 3.0f;

// Boden fuer die effektive Standardabweichung je Kanal, relativ zum
// Kanal-Mittelwert -- verhindert, dass ein zufaellig sehr eng beieinander
// liegender kleiner Stichprobenmittelwert den Test ueberempfindlich macht.
// Konservativ gewaehlt relativ zur ohnehin je Einzelmessung angestrebten
// Praezision von 1% relSEM (PRECISE_TARGET_REL_SEM, AS7341Spectrometer.cpp).
const float MIN_RELATIVE_STD = 0.02f;

std::vector<float> channelMean(const std::vector<const Measurement*>& samples, size_t ch) {
  std::vector<float> mean(ch, 0.0f);
  for (const Measurement* s : samples)
    for (size_t c = 0; c < ch; c++) mean[c] += (*s)[c];
  for (size_t c = 0; c < ch; c++) mean[c] /= (float)samples.size();
  return mean;
}

// Stichproben-Standardabweichung (Bessel-korrigiert, n-1) je Kanal um einen
// bereits berechneten Mittelwert. Voraussetzung: samples.size() >= 2 (vom
// Aufrufer sichergestellt).
void channelStdDev(const std::vector<const Measurement*>& samples, size_t ch,
                    const std::vector<float>& mean, std::vector<float>& outStd) {
  outStd.assign(ch, 0.0f);
  for (const Measurement* s : samples)
    for (size_t c = 0; c < ch; c++) { float d = (*s)[c] - mean[c]; outStd[c] += d * d; }
  for (size_t c = 0; c < ch; c++) outStd[c] = sqrtf(outStd[c] / (float)(samples.size() - 1));
}

// Aggregiert die Streuung (als Variationskoeffizient je Kanal) ueber alle
// ANDEREN Spitzen hinweg, OHNE deren Rohwerte zu mischen: je Spitze eigener
// Mittelwert/eigene Standardabweichung (ihre Fingerabdruecke mit passender
// sensorId), die daraus abgeleiteten Variationskoeffizienten werden gewichtet
// (Gewicht = Anzahl Fingerabdruecke - 1, wie bei einer gepoolten Varianz)
// gemittelt -- unterschiedliche Spitzen haben unterschiedliche optische
// Pfade, also eine andere absolute Skala, deshalb je Spitze getrennt
// berechnet statt alle Rohwerte einfach zusammenzuwerfen. Spitzen mit < 2
// passenden Fingerabdruecken tragen nichts bei (Standardabweichung nicht
// definiert). false, wenn in Summe zu wenige Eintraege
// (< MIN_FINGERPRINTS_FOR_FALLBACK_STATS) beigetragen haben.
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
  // Measurement ist per Spectrometer-Vertrag nie leer (buildWhiteFingerprint()
  // wird nur mit einer bereits validierten Messung aufgerufen) -- keine
  // Sonderbehandlung fuer den leeren Fall noetig.
  const size_t ch = candidate.normalized.size();

  // Nur Fingerabdruecke DESSELBEN Sensors sind absolut vergleichbar (siehe
  // WhiteFingerprint-Kommentar in TipCatalog.h).
  std::vector<const Measurement*> own;
  for (const WhiteFingerprint& fp : whiteFingerprints)
    if (fp.sensorId == candidate.sensorId) own.push_back(&fp.normalized);

  if (own.empty()) return PlausibilityResult::Indeterminate;  // keine Vergleichsbasis

  std::vector<float> ownMean = channelMean(own, ch);
  std::vector<float> effectiveStd;
  bool viaFallback = false;

  if (own.size() >= MIN_FINGERPRINTS_FOR_DIRECT_STATS) {
    channelStdDev(own, ch, ownMean, effectiveStd);
  } else {
    // Fallback: zu wenige eigene Fingerabdruecke fuer eine direkte Schaetzung
    // -- Streuung stattdessen aus den Fingerabdruecken ALLER ANDEREN Spitzen
    // schaetzen (nur der gepoolte Variationskoeffizient wird uebernommen,
    // siehe pooledCoefficientOfVariation()).
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
    if (std_c <= 0.0f) continue;  // Mittelwert 0 -- kein sinnvoller Test moeglich
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
