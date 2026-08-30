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

  // false, falls nie gespeichert (out bleibt dann leer, filterState unveraendert)
  bool loadDark(Measurement& out, FilterState& filterState);
  bool loadWhite(Measurement& out, FilterState& filterState);

  // filterState wird zusammen mit den Rohwerten gespeichert -- Referenz und
  // ihr Filter-Tag gehoeren untrennbar zusammen (siehe main.cpp::calibrationValidFor()).
  void saveDark(const Measurement& v, FilterState filterState);
  void saveWhite(const Measurement& v, FilterState filterState);

  // Die aktuell im Settings-Modus gewaehlte Einstellung -- unabhaengig davon,
  // was gerade als Dark/White-Referenz kalibriert ist. false, falls nie
  // gespeichert (out faellt dann auf FilterState::None zurueck).
  bool loadFilterState(FilterState& out);
  void saveFilterState(FilterState v);

private:
  bool load(const char* key, Measurement& out);
  void save(const char* key, const Measurement& v);

  Preferences prefs_;
};
