#include "AS7341Spectrometer.h"
#include "AppConfig.h"
#include "ColorimetryTables.h"
#include <cmath>

const float AS7341Spectrometer::VIS_CENTERS_NM[AS7341Spectrometer::N_VIS] = {
  415.0f, 445.0f, 480.0f, 515.0f, 555.0f, 590.0f, 630.0f, 680.0f
};

// Platzhalter-Groessenordnung aus dem AS7341-Datenblatt ("Optical Characteristics",
// typische FWHM je Kanal) -- vor dem produktiven Einsatz verifizieren.
const float AS7341Spectrometer::VIS_FWHM_NM[AS7341Spectrometer::N_VIS] = {
  26.0f, 30.0f, 36.0f, 39.0f, 39.0f, 40.0f, 50.0f, 52.0f
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

std::vector<uint32_t> AS7341Spectrometer::measureRawSpectrum(Precision precision,
                                                               ProgressCallback onProgress) {
  uint8_t maxSamples = (precision == Precision::Fast) ? FAST_SAMPLES : PRECISE_MAX_SAMPLES;
  uint8_t minSamples = (precision == Precision::Fast) ? FAST_SAMPLES : PRECISE_MIN_SAMPLES;

  // Loest den AS7341-Rohkanal-Quirk auf (Slot 4/5 sind Duplikate von F1-F4;
  // die echten F5-F8 liegen auf Slot 6-9) -- einzige Stelle im ganzen Code,
  // die das wissen muss.
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
  if (taken == 0) return std::vector<uint32_t>();  // Fehler-Sentinel: leerer Vektor

  // Ausreisser-Trimmung (einfaches Verfahren): pro Kanal hoechsten/niedrigsten
  // Einzelwert verwerfen (ab 5 Samples), Rest mitteln. Kompensiert einzelne
  // verwackelte/durch Fremdlicht gestoerte Messungen etwas -- siehe Plan fuer
  // moegliche Verfeinerung (Ausreisser als ganze Probe statt pro Kanal erkennen).
  std::vector<uint32_t> raw(N_CH);
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
      raw[ch] = (uint32_t)lroundf((float)sum / (float)(taken - 2));
    } else {
      uint32_t sum = 0;
      for (uint8_t i = 0; i < taken; i++) sum += buf[i][ch];
      raw[ch] = (uint32_t)lroundf((float)sum / (float)taken);
    }
  }
  return raw;
}

void AS7341Spectrometer::calibrate(const std::vector<uint32_t>& dark,
                                    const std::vector<uint32_t>& white) {
  if (dark.size() == N_CH) dark_ = dark;
  if (white.size() == N_CH) white_ = white;
}

void AS7341Spectrometer::computeVisReflectance(const std::vector<uint32_t>& raw,
                                                float R_vis[N_VIS]) const {
  bool haveCal = (dark_.size() == N_CH && white_.size() == N_CH);
  bool haveRaw = (raw.size() == N_CH);
  for (uint8_t i = 0; i < N_VIS; i++) {
    if (!haveCal || !haveRaw) { R_vis[i] = 0.0f; continue; }
    float denom = (float)white_[i] - (float)dark_[i];
    if (fabsf(denom) < 1e-6f) { R_vis[i] = 0.0f; continue; }
    float r = ((float)raw[i] - (float)dark_[i]) / denom;
    R_vis[i] = (r < 0.0f) ? 0.0f : r;  // kein oberes Clamping, spiegelt data/colorimeter.py
  }
}

void AS7341Spectrometer::computeXYZ(const float R_vis[N_VIS], float& X, float& Y, float& Z) const {
  spectrumToXYZ(VIS_CENTERS_NM, R_vis, N_VIS, X, Y, Z);
}

Spectrum AS7341Spectrometer::getSpectrum(const std::vector<uint32_t>& raw) {
  float R_vis[N_VIS];
  computeVisReflectance(raw, R_vis);

  Spectrum s;
  s.bands.resize(N_VIS);
  s.values.resize(N_VIS);
  for (uint8_t i = 0; i < N_VIS; i++) {
    s.bands[i] = Band{ VIS_CENTERS_NM[i], VIS_FWHM_NM[i] };
    s.values[i] = R_vis[i];
  }
  return s;
}

Lab AS7341Spectrometer::getColor(const std::vector<uint32_t>& raw) {
  float R_vis[N_VIS];
  computeVisReflectance(raw, R_vis);
  float X, Y, Z;
  computeXYZ(R_vis, X, Y, Z);
  return xyzToLab(X, Y, Z);
}
