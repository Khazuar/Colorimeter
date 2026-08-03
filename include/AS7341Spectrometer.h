#pragma once
#include <Adafruit_AS7341.h>
#include "Spectrometer.h"

class AS7341Spectrometer : public Spectrometer {
public:
  // Feste Ausgabereihenfolge fuer measureRawSpectrum()/calibrate(): F1..F8, Clear, NIR.
  // readAllChannels() liefert intern 12 Rohslots aus zwei Integrationszyklen
  // (Zyklus 1: F1,F2,F3,F4,Clear,NIR auf Slot 0-5; Zyklus 2: F5,F6,F7,F8,Clear,NIR
  // auf Slot 6-11 -- verifiziert gegen Adafruit_AS7341::setup_F1F4_Clear_NIR()/
  // setup_F5F8_Clear_NIR()). Das erste (ueberzaehlige) Clear/NIR-Paar auf Slot 4/5
  // wird verworfen, das zweite auf Slot 10/11 verwendet. Wird intern in
  // measureRawSpectrum() aufgeloest -- niemand ausserhalb dieser Klasse sieht die
  // 12 Rohslots.
  static const uint8_t N_CH  = 10;  // Laenge aller measureRawSpectrum()/calibrate()-Vektoren
  static const uint8_t N_VIS = 8;   // erste 8 Eintraege sind die VIS-Baender (Clear=8, NIR=9)

  static const float VIS_CENTERS_NM[N_VIS];  // {415,445,480,515,555,590,630,680}
  static const float VIS_FWHM_NM[N_VIS];     // typische Bandbreite je Kanal lt. AS7341-
                                              // Datenblatt ("Optical Characteristics") --
                                              // vor dem produktiven Einsatz gegen die
                                              // Datenblatt-Tabelle verifizieren.

  bool begin();  // as7341_.begin() + setATIME/ASTEP/GAIN. Kein NVS-Zugriff.

  std::vector<uint32_t> measureRawSpectrum(Precision precision,
                                            ProgressCallback onProgress = nullptr) override;
  Spectrum getSpectrum(const std::vector<uint32_t>& raw) override;
  Lab getColor(const std::vector<uint32_t>& raw) override;
  Spectrum getRangeUtilization(const std::vector<uint32_t>& raw) override;
  void calibrate(const std::vector<uint32_t>& dark, const std::vector<uint32_t>& white) override;

private:
  void computeVisReflectance(const std::vector<uint32_t>& raw, float R_vis[N_VIS], float& R_nir) const;
  void computeXYZ(const float R_vis[N_VIS], float& X, float& Y, float& Z) const;

  Adafruit_AS7341 as7341_;
  std::vector<uint32_t> dark_;   // leer, bis calibrate() aufgerufen wurde
  std::vector<uint32_t> white_;
};
