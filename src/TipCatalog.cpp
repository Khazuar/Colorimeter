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

// Eine Gruppe von Fingerabdruecken, die untereinander direkt vergleichbar sind
// (gleicher Sensor, gleiche Spitze [tipKey], gleiche Generation) -- jede
// Spitze+Generation-Kombination ist statistisch eine eigene "virtuelle
// Spitze" mit eigenem Mittelwert/eigener Streuung: unterschiedliche
// Generationen haben eine andere physische Weissreferenz, unterschiedliche
// Spitzen einen anderen optischen Pfad -- beides veraendert die absolute
// Skala. 'tipKey' ist rein ein Identitaets-Diskriminator (Adresse der
// jeweiligen MeasurementTip innerhalb des aktuellen isPlausible()-Aufrufs),
// wird nie dereferenziert -- noetig, damit beim Poolen ueber MEHRERE andere
// Spitzen hinweg deren Fingerabdruecke nicht faelschlich anhand einer
// zufaellig gleichen 'generation' zusammengeworfen werden (die 'generation'
// ist ein katalogweiter Zaehler, siehe TipCatalog::whiteReferenceGeneration --
// zwei verschiedene Spitzen koennen also denselben Wert tragen, ohne
// deshalb vergleichbar zu sein).
struct FingerprintGroup {
  const void* tipKey;
  uint32_t generation;
  std::vector<const Measurement*> samples;
};

void groupByGeneration(const MeasurementTip& tip, const std::string& sensorId,
                        std::vector<FingerprintGroup>& groups) {
  const void* key = &tip;
  for (const WhiteFingerprint& fp : tip.whiteFingerprints) {
    if (fp.sensorId != sensorId) continue;
    FingerprintGroup* g = nullptr;
    for (FingerprintGroup& existing : groups)
      if (existing.tipKey == key && existing.generation == fp.generation) { g = &existing; break; }
    if (!g) { groups.push_back({key, fp.generation, {}}); g = &groups.back(); }
    g->samples.push_back(&fp.normalized);
  }
}

// Aggregiert die Streuung (als Variationskoeffizient je Kanal) ueber mehrere
// Gruppen hinweg, OHNE deren Rohwerte zu mischen: je Gruppe eigener
// Mittelwert/eigene Standardabweichung, die daraus abgeleiteten
// Variationskoeffizienten werden gewichtet (Gewicht = Gruppengroesse - 1, wie
// bei einer gepoolten Varianz) gemittelt. Gruppen mit < 2 Eintraegen tragen
// nichts bei (Standardabweichung nicht definiert). false, wenn in Summe zu
// wenige Eintraege (< MIN_FINGERPRINTS_FOR_FALLBACK_STATS) beigetragen haben.
bool pooledCoefficientOfVariation(const std::vector<FingerprintGroup>& groups, size_t ch,
                                   std::vector<float>& outCv) {
  std::vector<float> weightedSum(ch, 0.0f);
  float weightTotal = 0.0f;
  size_t contributingSamples = 0;
  for (const FingerprintGroup& g : groups) {
    if (g.samples.size() < 2) continue;
    std::vector<float> mean = channelMean(g.samples, ch);
    std::vector<float> std_;
    channelStdDev(g.samples, ch, mean, std_);
    float weight = (float)(g.samples.size() - 1);
    for (size_t c = 0; c < ch; c++) {
      float cv = (mean[c] > 0.0f) ? (std_[c] / mean[c]) : 0.0f;
      weightedSum[c] += weight * cv;
    }
    weightTotal += weight;
    contributingSamples += g.samples.size();
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

  // Nur Fingerabdruecke DERSELBEN (aktuellen) Generation UND desselben Sensors
  // sind absolut vergleichbar (siehe FingerprintGroup-Kommentar oben).
  std::vector<const Measurement*> own;
  for (const WhiteFingerprint& fp : whiteFingerprints)
    if (fp.generation == candidate.generation && fp.sensorId == candidate.sensorId) own.push_back(&fp.normalized);

  if (own.empty()) return PlausibilityResult::Indeterminate;  // keine Vergleichsbasis

  std::vector<float> ownMean = channelMean(own, ch);
  std::vector<float> effectiveStd;
  bool viaFallback = false;

  if (own.size() >= MIN_FINGERPRINTS_FOR_DIRECT_STATS) {
    channelStdDev(own, ch, ownMean, effectiveStd);
  } else {
    // Fallback: aeltere Generationen DERSELBEN Spitze zuerst, sonst alle
    // Fingerabdruecke ALLER ANDEREN Spitzen -- je Spitze+Generation eine
    // eigene Gruppe (siehe FingerprintGroup), nur der gepoolte
    // Variationskoeffizient wird uebernommen.
    std::vector<FingerprintGroup> ownTipGroups;
    groupByGeneration(*this, candidate.sensorId, ownTipGroups);
    std::vector<FingerprintGroup> olderGenerations;
    for (FingerprintGroup& g : ownTipGroups)
      if (g.generation != candidate.generation) olderGenerations.push_back(g);

    std::vector<float> cv;
    if (!pooledCoefficientOfVariation(olderGenerations, ch, cv)) {
      std::vector<FingerprintGroup> otherTipGroups;
      for (const MeasurementTip& t : catalog.tips) {
        if (t.name == name) continue;
        groupByGeneration(t, candidate.sensorId, otherTipGroups);
      }
      if (!pooledCoefficientOfVariation(otherTipGroups, ch, cv)) return PlausibilityResult::Indeterminate;
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

WhiteFingerprintStats MeasurementTip::whiteFingerprintStats(const std::string& sensorId,
                                                              uint32_t generation) const {
  WhiteFingerprintStats result;
  std::vector<const Measurement*> own;
  for (const WhiteFingerprint& fp : whiteFingerprints)
    if (fp.generation == generation && fp.sensorId == sensorId) own.push_back(&fp.normalized);

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
