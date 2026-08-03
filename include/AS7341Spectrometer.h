#pragma once
#include <Adafruit_AS7341.h>
#include "Spectrometer.h"

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
  static const uint8_t N_VIS = 8;   // erste 8 Eintraege sind die VIS-Baender (Clear=8, NIR=9)

  static const float VIS_CENTERS_NM[N_VIS];  // {415,445,480,515,555,590,630,680}
  static const float VIS_FWHM_NM[N_VIS];     // Bandbreite je Kanal lt. AS7341-Datenblatt
                                              // ("Optical Characteristics"), verifiziert.
  static const char* const MEASUREMENT_LABELS[N_CH];

  bool begin();  // as7341_.begin() + setATIME/ASTEP/GAIN. Kein NVS-Zugriff.

  Measurement performMeasurement(Precision precision,
                                  ProgressCallback onProgress = nullptr) override;
  const char* const* measurementLabels() const override { return MEASUREMENT_LABELS; }
  Spectrum getSpectrum(const Measurement& measurement,
                        const Measurement& whiteReference,
                        const Measurement& darkReference) const override;

private:
  void computeVisReflectance(const Measurement& measurement,
                              const Measurement& whiteReference,
                              const Measurement& darkReference,
                              float R_vis[N_VIS]) const;

  Adafruit_AS7341 as7341_;
};
