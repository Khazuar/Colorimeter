#pragma once
#include <Adafruit_AS7341.h>
#include "Spectrometer.h"
#include "AppConfig.h"

class AS7341Spectrometer : public Spectrometer {
public:
  // Feste Measurement-Reihenfolge: F1..F8, Clear, NIR.
  // readAllChannels() liefert intern 12 Rohslots aus zwei Integrationszyklen
  // (Zyklus 1: F1,F2,F3,F4,Clear,NIR auf Slot 0-5; Zyklus 2: F5,F6,F7,F8,Clear,NIR
  // auf Slot 6-11 -- verifiziert gegen Adafruit_AS7341::setup_F1F4_Clear_NIR()/
  // setup_F5F8_Clear_NIR()). Das erste (ueberzaehlige) Clear/NIR-Paar auf Slot 4/5
  // wird verworfen, das zweite auf Slot 10/11 verwendet. Wird intern in
  // performMeasurement() aufgeloest -- niemand ausserhalb dieser Klasse sieht die
  // 12 Rohslots (auch measurementLabels() ist rein zur Beschriftung gedacht,
  // nicht zur Interpretation durch generischen Code).
  static const uint8_t N_CH  = 10;  // Laenge jedes Measurement dieses Sensors

  static const char* const MEASUREMENT_LABELS[N_CH];

  // Rohkanaele mit einem Mittelwert darunter liefern keinen verlaesslichen
  // relSEM und zaehlen deshalb nicht zur Abbruchentscheidung der Sample-
  // Schleife (siehe converged() in der .cpp) -- reine Effizienz-/Berichts-
  // Heuristik, KEINE Aussage ueber Verwertbarkeit (die trifft checkValidity()
  // separat, ueber einen Limit-of-Detection-Test gegen die Dunkelreferenz,
  // siehe dort). Oeffentlich, weil main.cpp diesen Wert konzeptuell kennen
  // koennen muss (z.B. fuer Fehlermeldungen).
  static constexpr float NOISE_FLOOR_COUNTS = 50.0f;

  bool begin();  // NUR as7341_.begin() (I2C-Inbetriebnahme). Kein NVS-Zugriff,
                 // keine Annahme ueber Gain/ATIME/ASTEP -- siehe applySettings().

  // Schreibt Gain/ATIME/ASTEP auf den Sensor -- beliebig oft wiederholbar,
  // im Gegensatz zu begin() (das nur einmal beim Boot laeuft). Wird von der
  // Orchestrierung (main.cpp) sowohl beim Start (geladene Einstellungen)
  // als auch bei jeder Aenderung im Settings-Modus aufgerufen.
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
  // Rohkanal-Indizes, die fuer den gegebenen Filter tatsaechlich als VIS-Band
  // ins Spektrum eingehen (siehe BANDS_*-Tabellen) -- NUR fuer checkValidity(),
  // deshalb privat (kein Interface-Bestandteil, siehe Spectrometer.h-Kommentar).
  // BEWUSST ohne NIR/Clear: NIR waere unter einem IR-Cut-Filter strukturell
  // dunkel und liesse sich durch nichts ueber die Rauschgrenze heben; Clear
  // wird durch die VIS-Kanaele bereits indirekt abgedeckt.
  uint8_t relevantChannels(FilterState fs, uint8_t outIdx[N_CH]) const;

  // Berechnet die kalibrierte Reflexion der je nach FilterState nutzbaren
  // VIS-Kanaele (siehe die BANDS_*-Tabellen in der .cpp) direkt in 'out'.
  // Intern wird dabei auch die NIR-Reflexion bestimmt (gleiche Formel), um
  // optisches Uebersprechen von NIR-Licht herauszurechnen -- die Staerke
  // dieser Korrektur ist filterabhaengig. Das Ergebnis ist weiterhin nur
  // R_vis; NIR selbst fliesst nicht in das (sensor-unabhaengige) Spectrum ein.
  void computeReflectance(const Measurement& measurement,
                          const Measurement& whiteReference,
                          const Measurement& darkReference,
                          FilterState filterState,
                          Spectrum& out) const;

  Adafruit_AS7341 as7341_;
};
