#pragma once
#include <Preferences.h>
#include "Spectrometer.h"
#include "AppConfig.h"

// Duenner NVS-Wrapper fuer die Dunkel-/Weiss-Kalibrierung UND die
// Aufnahme-Einstellungen (Filter/Gain/ATIME/ASTEP). Gehoert der
// Orchestrierung (main.cpp) -- der Spectrometer selbst fasst kein Flash an.
// Groesse der Rohwerte wird aus dem gespeicherten Blob selbst ermittelt
// (getBytesLength), die Orchestrierung muss also keine sensorspezifische
// Kanalanzahl kennen.
class CalibrationStore {
public:
  void begin();  // prefs_.begin("colorim", false)

  // false, falls nie gespeichert (out bleibt dann leer, settings unveraendert)
  bool loadDark(Measurement& out, AcquisitionSettings& settings);
  bool loadWhite(Measurement& out, AcquisitionSettings& settings);

  // settings wird zusammen mit den Rohwerten gespeichert -- Referenz und ihre
  // Aufnahme-Einstellungen gehoeren untrennbar zusammen (siehe
  // main.cpp::calibrationValidFor()).
  void saveDark(const Measurement& v, const AcquisitionSettings& settings);
  void saveWhite(const Measurement& v, const AcquisitionSettings& settings);

  // Die aktuell im Settings-Modus gewaehlten Einstellungen -- unabhaengig
  // davon, was gerade als Dark/White-Referenz kalibriert ist. false, falls
  // nie gespeichert (out faellt dann auf die AcquisitionSettings-Defaults zurueck).
  bool loadSettings(AcquisitionSettings& out);
  void saveSettings(const AcquisitionSettings& v);

private:
  bool load(const char* key, Measurement& out);
  void save(const char* key, const Measurement& v);

  Preferences prefs_;
};
