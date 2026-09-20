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

  // Rohkanaele mit einem Mittelwert darunter liefern keinen verlaesslichen
  // relSEM (siehe converged() in der .cpp). Oeffentlich, weil der Belichtungs-
  // Assistent (main.cpp) seinen Ziel-Signalpegel als Vielfaches davon festlegt.
  static constexpr float NOISE_FLOOR_COUNTS = 50.0f;

  static const char* const MEASUREMENT_LABELS[N_CH];

  bool begin();  // NUR as7341_.begin() (I2C-Inbetriebnahme). Kein NVS-Zugriff,
                 // keine Annahme ueber Gain/ATIME/ASTEP -- siehe applySettings().

  // Schreibt Gain/ATIME/ASTEP auf den Sensor -- beliebig oft wiederholbar,
  // im Gegensatz zu begin() (das nur einmal beim Boot laeuft). Wird von der
  // Orchestrierung (main.cpp) sowohl beim Start (geladene Einstellungen)
  // als auch bei jeder Aenderung im Settings-Modus aufgerufen.
  void applySettings(const AcquisitionSettings& settings);

  // Fuellt outIdx mit den Rohkanal-Indizes, die fuer den gegebenen Filter
  // tatsaechlich als VIS-Band ins Spektrum eingehen (siehe die BANDS_*-Tabellen
  // in der .cpp), und liefert deren Anzahl zurueck. BEWUSST ohne NIR/Clear:
  // NIR waere unter einem IR-Cut-Filter (650/700 nm) strukturell dunkel und
  // liesse sich durch keine Belichtungserhoehung ueber die Rauschgrenze heben.
  // Fuer den Belichtungs-Assistenten (main.cpp), der nur diese Kanaele auf
  // Rauschabstand prueft.
  uint8_t relevantChannels(FilterState fs, uint8_t outIdx[N_CH]) const;

  Measurement performMeasurement(Precision precision,
                                  ProgressCallback onProgress = nullptr,
                                  MeasurementTelemetry* outTelemetry = nullptr) override;
  const char* const* measurementLabels() const override { return MEASUREMENT_LABELS; }
  Spectrum getSpectrum(const Measurement& measurement,
                        const Measurement& whiteReference,
                        const Measurement& darkReference,
                        FilterState filterState) const override;

private:
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
