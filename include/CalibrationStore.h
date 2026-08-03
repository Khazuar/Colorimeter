#pragma once
#include <Preferences.h>
#include "Spectrometer.h"

// Duenner NVS-Wrapper fuer die Dunkel-/Weiss-Kalibrierung. Gehoert der
// Orchestrierung (main.cpp) -- der Spectrometer selbst fasst kein Flash an.
// Groesse wird aus dem gespeicherten Blob selbst ermittelt (getBytesLength),
// die Orchestrierung muss also keine sensorspezifische Kanalanzahl kennen.
class CalibrationStore {
public:
  void begin();  // prefs_.begin("colorim", false)

  // false, falls nie gespeichert (out bleibt dann leer)
  bool loadDark(Measurement& out);
  bool loadWhite(Measurement& out);

  void saveDark(const Measurement& v);
  void saveWhite(const Measurement& v);

private:
  bool load(const char* key, Measurement& out);
  void save(const char* key, const Measurement& v);

  Preferences prefs_;
};
