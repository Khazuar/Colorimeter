#include "AS7341Spectrometer.h"
#include "AppConfig.h"
#include <cmath>

const float AS7341Spectrometer::VIS_CENTERS_NM[AS7341Spectrometer::N_VIS] = {
  415.0f, 445.0f, 480.0f, 515.0f, 555.0f, 590.0f, 630.0f, 680.0f
};

const float AS7341Spectrometer::VIS_FWHM_NM[AS7341Spectrometer::N_VIS] = {
  26.0f, 30.0f, 36.0f, 39.0f, 39.0f, 40.0f, 50.0f, 52.0f
};

// Debug-/Analysezwecke: Klartext-Label je Measurement-Element, in derselben
// Reihenfolge wie performMeasurement() sie liefert (F1..F8, Clear, NIR).
// NICHT von generischem Code nutzen, um den Measurement-Inhalt zu interpretieren.
const char* const AS7341Spectrometer::MEASUREMENT_LABELS[AS7341Spectrometer::N_CH] = {
  "F1_415nm", "F2_445nm", "F3_480nm", "F4_515nm",
  "F5_555nm", "F6_590nm", "F7_630nm", "F8_680nm",
  "Clear", "NIR_910nm"
};

bool AS7341Spectrometer::begin() {
  if (!as7341_.begin()) return false;
  as7341_.setATIME(AS_ATIME);
  as7341_.setASTEP(AS_ASTEP);
  as7341_.setGain(AS_GAIN);
  return true;
}

// Alle Konstanten hier sind ein bewusst einfacher Startpunkt, keine fertig
// getunte Loesung -- siehe Plan/Kontext: welche Stoppschwelle tatsaechlich
// <1 DeltaE Messgenauigkeit liefert, muss noch empirisch getestet werden.
static const uint8_t FAST_SAMPLES           = 1;
static const uint8_t PRECISE_MIN_SAMPLES    = 4;
static const uint8_t PRECISE_MAX_SAMPLES    = 32;    // Cap, ersetzt frueheres festes N_AVG=16
static const float   PRECISE_TARGET_REL_SEM = 0.01f; // 1% rel. Standardfehler d. Mittelwerts -- TODO tunen
static const float   NOISE_FLOOR_COUNTS     = 50.0f;  // Kanaele darunter zaehlen nicht zur Konvergenzpruefung

// Schlechtester relativer Standardfehler des Mittelwerts ueber alle Kanaele mit
// Signal oberhalb NOISE_FLOOR_COUNTS (sonst dominiert das Rauschen sehr dunkler
// Kanaele den relativen Fehler, ohne etwas ueber die Messqualitaet auszusagen).
static bool converged(const uint16_t buf[][AS7341Spectrometer::N_CH], uint8_t taken) {
  float worst = 0.0f;
  for (uint8_t ch = 0; ch < AS7341Spectrometer::N_CH; ch++) {
    float mean = 0.0f;
    for (uint8_t i = 0; i < taken; i++) mean += buf[i][ch];
    mean /= taken;
    if (mean < NOISE_FLOOR_COUNTS) continue;

    float varSum = 0.0f;
    for (uint8_t i = 0; i < taken; i++) { float d = buf[i][ch] - mean; varSum += d * d; }
    float stddev = sqrtf(varSum / (taken - 1));
    float relSEM = (stddev / mean) / sqrtf((float)taken);
    if (relSEM > worst) worst = relSEM;
  }
  return worst <= PRECISE_TARGET_REL_SEM;
}

Measurement AS7341Spectrometer::performMeasurement(Precision precision, ProgressCallback onProgress) {
  uint8_t maxSamples = (precision == Precision::Fast) ? FAST_SAMPLES : PRECISE_MAX_SAMPLES;
  uint8_t minSamples = (precision == Precision::Fast) ? FAST_SAMPLES : PRECISE_MIN_SAMPLES;

  // Loest die AS7341-Rohkanal-Reihenfolge auf. readAllChannels() macht intern
  // zwei Integrationszyklen mit unterschiedlicher SMUX-Konfiguration (siehe
  // Adafruit_AS7341::setup_F1F4_Clear_NIR()/setup_F5F8_Clear_NIR()):
  //   Zyklus 1 (Slot 0-5):  F1, F2, F3, F4, Clear, NIR
  //   Zyklus 2 (Slot 6-11): F5, F6, F7, F8, Clear, NIR
  // Slot 4/5 sind ein erstes (ueberzaehliges) Clear/NIR-Messpaar -- wir
  // ignorieren es und nehmen stattdessen das zweite Paar aus Slot 10/11.
  // Einzige Stelle im ganzen Code, die diese Reihenfolge wissen muss.
  static const uint8_t SRC_IDX[N_CH] = { 0, 1, 2, 3, 6, 7, 8, 9, 10, 11 };

  static uint16_t buf[PRECISE_MAX_SAMPLES][N_CH];  // ~640B, static um Stack zu schonen
  uint8_t taken = 0;

  for (uint8_t n = 0; n < maxSamples; n++) {
    if (onProgress) onProgress(taken, maxSamples);
    uint16_t r[12];
    if (!as7341_.readAllChannels(r)) continue;
    for (uint8_t i = 0; i < N_CH; i++) buf[taken][i] = r[SRC_IDX[i]];
    taken++;
    if (precision == Precision::Precise && taken >= minSamples && converged(buf, taken)) break;
  }
  if (onProgress) onProgress(taken, maxSamples);
  if (taken == 0) return Measurement();  // Fehler-Sentinel: leeres Measurement

  // Ausreisser-Trimmung (einfaches Verfahren): pro Kanal hoechsten/niedrigsten
  // Einzelwert verwerfen (ab 5 Samples), Rest mitteln. Kompensiert einzelne
  // verwackelte/durch Fremdlicht gestoerte Messungen etwas -- siehe Plan fuer
  // moegliche Verfeinerung (Ausreisser als ganze Probe statt pro Kanal erkennen).
  Measurement m(N_CH);
  for (uint8_t ch = 0; ch < N_CH; ch++) {
    if (taken >= 5) {
      uint16_t mn = buf[0][ch], mx = buf[0][ch];
      uint32_t sum = 0;
      for (uint8_t i = 0; i < taken; i++) {
        uint16_t v = buf[i][ch];
        sum += v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
      }
      sum -= mn;
      sum -= mx;
      m[ch] = (float)sum / (float)(taken - 2);
    } else {
      uint32_t sum = 0;
      for (uint8_t i = 0; i < taken; i++) sum += buf[i][ch];
      m[ch] = (float)sum / (float)taken;
    }
  }
  return m;
}

// Optisches Uebersprechen: NIR-Licht beeinflusst F1-F4 unterschiedlich stark
// (F1 am staerksten) -- Anteil des NIR-Reflexionswerts, der von der jeweiligen
// VIS-Reflexion abgezogen werden muss. F5-F8 unbeeinflusst (Faktor 0).
static const float NIR_CROSSTALK_FACTOR[AS7341Spectrometer::N_VIS] = {
  0.55f, 0.17f, 0.19f, 0.07f, 0.0f, 0.0f, 0.0f, 0.0f
};

void AS7341Spectrometer::computeReflectance(const Measurement& measurement,
                                             const Measurement& whiteReference,
                                             const Measurement& darkReference,
                                             float R_vis[N_VIS]) const {
  bool haveCal = (whiteReference.size() == N_CH && darkReference.size() == N_CH);
  bool haveMeasurement = (measurement.size() == N_CH);

  // Gleiche Formel fuer VIS-Kanaele und NIR -- kein oberes Clamping,
  // spiegelt data/colorimeter.py.
  auto reflectance = [&](uint8_t ch) -> float {
    if (!haveCal || !haveMeasurement) return 0.0f;
    float denom = whiteReference[ch] - darkReference[ch];
    if (fabsf(denom) < 1e-6f) return 0.0f;
    float r = (measurement[ch] - darkReference[ch]) / denom;
    return (r < 0.0f) ? 0.0f : r;
  };

  float R_nir = reflectance(N_CH - 1);
  for (uint8_t i = 0; i < N_VIS; i++) {
    float r = (reflectance(i) - NIR_CROSSTALK_FACTOR[i] * R_nir) / (1 - NIR_CROSSTALK_FACTOR[i]);
    R_vis[i] = (r < 0.0f) ? 0.0f : r;  // erneut clampen -- die NIR-Korrektur kann ins Negative ziehen
  }
}

Spectrum AS7341Spectrometer::getSpectrum(const Measurement& measurement,
                                          const Measurement& whiteReference,
                                          const Measurement& darkReference) const {
  float R_vis[N_VIS];
  computeReflectance(measurement, whiteReference, darkReference, R_vis);

  Spectrum s;
  s.bands.resize(N_VIS);
  s.values.resize(N_VIS);
  for (uint8_t i = 0; i < N_VIS; i++) {
    s.bands[i] = Band{ VIS_CENTERS_NM[i], VIS_FWHM_NM[i] };
    s.values[i] = R_vis[i];
  }
  return s;
}
