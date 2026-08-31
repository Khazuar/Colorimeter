#include "AS7341Spectrometer.h"
#include "AppConfig.h"
#include <cmath>

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
// n=4 hatte ~41% relative Unsicherheit der SD-Schaetzung selbst
// (1/sqrt(2*(n-1))) -- der allererste converged()-Check konnte dadurch rein
// zufaellig zu frueh positiv ausfallen. n=8 (~27%) ist spuerbar robuster,
// weiteres Erhoehen bringt abnehmenden Ertrag -- TODO tunen.
static const uint8_t PRECISE_MIN_SAMPLES    = 8;
static const uint8_t PRECISE_MAX_SAMPLES    = 32;    // Cap, ersetzt frueheres festes N_AVG=16
static const float   PRECISE_TARGET_REL_SEM = 0.01f; // 1% rel. Standardfehler d. Mittelwerts -- TODO tunen
static const float   NOISE_FLOOR_COUNTS     = 50.0f;  // Kanaele darunter zaehlen nicht zur Konvergenzpruefung

// Schlechtester relativer Standardfehler des Mittelwerts ueber alle Kanaele mit
// Signal oberhalb NOISE_FLOOR_COUNTS (sonst dominiert das Rauschen sehr dunkler
// Kanaele den relativen Fehler, ohne etwas ueber die Messqualitaet auszusagen).
// anyChannelEvaluated meldet, ob ueberhaupt ein Kanal oberhalb der
// Rauschgrenze lag -- der Rueckgabewert ist bedeutungslos, wenn nicht (worst
// bleibt bei seinem Initialwert 0.0). performMeasurement() behandelt diesen
// Fall (z.B. sehr dunkle Probe/Dunkelmessung) explizit separat, siehe dort.
static bool converged(const uint16_t buf[][AS7341Spectrometer::N_CH], uint8_t taken, bool& anyChannelEvaluated) {
  float worst = 0.0f;
  anyChannelEvaluated = false;
  for (uint8_t ch = 0; ch < AS7341Spectrometer::N_CH; ch++) {
    float mean = 0.0f;
    for (uint8_t i = 0; i < taken; i++) mean += buf[i][ch];
    mean /= taken;
    if (mean < NOISE_FLOOR_COUNTS) continue;
    anyChannelEvaluated = true;

    float varSum = 0.0f;
    for (uint8_t i = 0; i < taken; i++) { float d = buf[i][ch] - mean; varSum += d * d; }
    float stddev = sqrtf(varSum / (taken - 1));
    float relSEM = (stddev / mean) / sqrtf((float)taken);
    if (relSEM > worst) worst = relSEM;
  }
  return worst <= PRECISE_TARGET_REL_SEM;
}

Measurement AS7341Spectrometer::performMeasurement(Precision precision, ProgressCallback onProgress,
                                                    MeasurementStatus* outStatus) {
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
  uint8_t consecutiveConverged = 0;
  bool stoppedShortNoSignal = false;  // siehe "kein Kanal evaluiert"-Kurzschluss unten

  for (uint8_t n = 0; n < maxSamples; n++) {
    if (onProgress) onProgress(taken, maxSamples);
    uint16_t r[12];
    if (!as7341_.readAllChannels(r)) continue;
    for (uint8_t i = 0; i < N_CH; i++) buf[taken][i] = r[SRC_IDX[i]];
    taken++;

    if (precision == Precision::Precise && taken >= minSamples) {
      bool anyChannelEvaluated;
      bool isConverged = converged(buf, taken, anyChannelEvaluated);
      if (!anyChannelEvaluated) {
        // Bewusste Kurzschluss-Entscheidung: kein Kanal hat Signal oberhalb
        // der Rauschgrenze (z.B. sehr dunkle Probe/Dunkelmessung) -- die
        // relative Praezisionsschwelle ist fuer Kanaele ohne Signal nicht
        // aussagekraeftig, mehr Samples aendern daran systematisch nichts.
        // Deshalb sofortiger Abbruch bei minSamples, ohne die sonst uebliche
        // 2-von-2-Bestaetigung (siehe unten) abzuwarten.
        stoppedShortNoSignal = true;
        break;
      }
      if (isConverged) {
        // Zwei aufeinanderfolgende Treffer verlangt statt nur einem --
        // mildert "optional stopping"-Bias ab (ein einzelner zufaellig
        // guenstiger Zwischenwert wuerde die Messung sonst vorzeitig
        // optimistisch verzerrt beenden). Bei Cap 32 ist der praktische
        // Schaden eines einzelnen Treffers begrenzt, die Korrektur ist aber
        // billig genug, um sie trotzdem mitzunehmen -- TODO tunen.
        consecutiveConverged++;
        if (consecutiveConverged >= 2) break;
      } else {
        consecutiveConverged = 0;
      }
    }
  }
  if (onProgress) onProgress(taken, maxSamples);

  if (taken == 0) {
    // Sensor liefert ueberhaupt keine gueltigen Daten -- Hardware-Fehler.
    if (outStatus) *outStatus = MeasurementStatus::SensorError;
    return Measurement();
  }

  bool converged_enough = (precision != Precision::Precise)
                        || stoppedShortNoSignal
                        || (consecutiveConverged >= 2);
  if (!converged_enough) {
    // maxSamples ausgeschoepft, ohne dass die Zielpraezision (zwei
    // aufeinanderfolgende converged()-Treffer) bestaetigt wurde -- z.B. eine
    // andauernde Stoerung waehrend der Messung (Geraet wird bewegt). Explizit
    // vom Sensorfehler-Fall oben unterscheidbar, siehe MeasurementStatus.
    if (outStatus) *outStatus = MeasurementStatus::NotConverged;
    return Measurement();
  }

  if (outStatus) *outStatus = MeasurementStatus::Ok;

  // Schlichter Mittelwert ueber alle gesammelten Samples -- keine
  // Ausreisser-Trimmung mehr (siehe Kontext: die adaptive Stichprobenziehung
  // selbst daempft kurze Stoerungen bereits, ohne die von converged()
  // zertifizierte Praezision auf ungetrimmten Daten zu unterlaufen).
  Measurement m(N_CH);
  for (uint8_t ch = 0; ch < N_CH; ch++) {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < taken; i++) sum += buf[i][ch];
    m[ch] = (float)sum / (float)taken;
  }
  return m;
}

// Optisches Uebersprechen: NIR-Licht beeinflusst die VIS-Kanaele
// unterschiedlich stark -- WELCHE Kanaele ueberhaupt auswertbar sind und mit
// welchem Faktor sie korrigiert werden muessen, haengt vom eingesetzten
// IR-Cut-Filter ab (empirisch bestimmt per Vergleichsmessungen derselben
// Proben mit/ohne 650nm- bzw. 700nm-Filter). channelIndex verweist auf den
// zugehoerigen rohen F1..F8-Slot (0..7) -- die Rohkanal-Erfassung selbst
// bleibt davon unberuehrt.
struct VisBandDef {
  uint8_t channelIndex;
  float center_nm;
  float fwhm_nm;
  float nirFactor;
  float nirFactorErr;  // Unsicherheit von nirFactor -- Basis von Spectrum::valueErrors
};

// Kein Filter: F1-F7 korrigierbar, F8 hat einen Korrekturfaktor unbekannter
// Groesse -> komplett weggelassen statt unkorrigiert auszugeben.
static const VisBandDef BANDS_NONE[] = {
  { 0, 415.0f, 26.0f, 0.355f, 0.060f },  // F1
  { 1, 445.0f, 30.0f, 0.109f, 0.019f },  // F2
  { 2, 480.0f, 36.0f, 0.089f, 0.012f },  // F3
  { 3, 515.0f, 39.0f, 0.036f, 0.007f },  // F4
  { 4, 555.0f, 39.0f, 0.057f, 0.011f },  // F5
  { 5, 590.0f, 40.0f, 0.0f,   0.12f  },  // F6
  { 6, 630.0f, 50.0f, 0.0f,   0.0f   },  // F7
};

// 700nm-Filter: F1-F7 mit praeziseren Faktoren; F8 UMDEFINIERT als eigenes
// 674nm/45nm-Band statt als "abgeschnittener 680nm/52nm-Kanal" verworfen --
// der 700nm-Cut-Filter macht diesen engeren, effektiv genutzten
// Empfindlichkeitsbereich per Konstruktion NIR-frei (daher Faktor 0).
static const VisBandDef BANDS_700NM[] = {
  { 0, 415.0f, 26.0f, 0.125f, 0.045f },  // F1
  { 1, 445.0f, 30.0f, 0.032f, 0.017f },  // F2
  { 2, 480.0f, 36.0f, 0.024f, 0.013f },  // F3
  { 3, 515.0f, 39.0f, 0.009f, 0.011f },  // F4
  { 4, 555.0f, 39.0f, 0.026f, 0.014f },  // F5
  { 5, 590.0f, 40.0f, 0.0f,   0.14f  },  // F6
  { 6, 630.0f, 50.0f, 0.0f,   0.07f  },  // F7
  { 7, 674.0f, 45.0f, 0.0f,   0.03f  },  // F8, umdefiniert, NIR-frei per Konstruktion, Unsicherheit geometrisch bedingt (keine saubere Sigmoid Form)
};

// 650nm-Filter: blockt bereits ab 650nm -> F1-F6 per Konstruktion NIR-frei
// (Faktor 0), F7/F8 entfallen (der Filter schneidet bereits in ihren
// eigentlichen Empfindlichkeitsbereich, "nicht nutzbar").
// Die Unsicherheit "0.0f" ist eine Annahme, die nicht weiter geprüft wurde. 
// Es fehlt an verfügbaren Methoden, diese Unsicherheit zu charakterisieren. 
// Ziemlich sicher ist der verbleibende "echte" NIR-Anteil aber vernachlässigbar. 
// Verbleibender Roh-Anzeigewert ist durch VIZ-Übersprechen in den NIR-Kanal bedingt.
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

void AS7341Spectrometer::computeReflectance(const Measurement& measurement,
                                             const Measurement& whiteReference,
                                             const Measurement& darkReference,
                                             FilterState filterState,
                                             Spectrum& out) const {
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
      return (r < 0.0f) ? 0.0f : r;  // erneut clampen -- die NIR-Korrektur kann ins Negative ziehen
    };
    out.values[i] = correct(def.nirFactor);

    // Fehler-Range: halbe Spannweite der Korrektur bei +-1 Sigma des Faktors
    // -- eine einfache numerische Sensitivitaetsabschaetzung statt einer
    // analytisch hergeleiteten Ableitung; bei ohnehin nur grob bekannten
    // Unsicherheiten ausreichend und weniger fehleranfaellig.
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
