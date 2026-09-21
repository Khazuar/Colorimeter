#pragma once
#include <Preferences.h>
#include "Spectrometer.h"
#include "AppConfig.h"

// Duenner NVS-Wrapper fuer die Dunkel-/Weiss-Kalibrierung UND die
// Einstellungs-Hierarchie (RootSettings). Gehoert der Orchestrierung
// (main.cpp) -- der Spectrometer selbst fasst kein Flash an. Persistiert
// jeweils als JSON-String (siehe CalibrationStore.cpp und
// schema/settings.schema.json) statt als Binaer-Blob -- robust gegenueber
// hinzukommenden/fehlenden Feldern, Enum-Werte als Strings statt Zahlen-Index.
// Groesse der Rohwerte wird aus dem gespeicherten Blob selbst ermittelt
// (getBytesLength), die Orchestrierung muss also keine sensorspezifische
// Kanalanzahl kennen.
class CalibrationStore {
public:
  void begin();  // prefs_.begin("colorim", false)

  // false, falls nie gespeichert ODER JSON nicht lesbar (out/params fallen
  // dann auf ihre Defaults zurueck).
  bool loadDark(Measurement& out, AcquisitionParameters& params);
  bool loadWhite(Measurement& out, AcquisitionParameters& params);

  // params wird zusammen mit den Rohwerten gespeichert -- Referenz und die
  // bei ihrer Aufnahme aktiven Parameter gehoeren untrennbar zusammen (siehe
  // main.cpp::calibrationValidFor()).
  void saveDark(const Measurement& v, const AcquisitionParameters& params);
  void saveWhite(const Measurement& v, const AcquisitionParameters& params);

  // Die vollstaendige, aktuell im Settings-Baum gewaehlte Konfiguration --
  // unabhaengig davon, was gerade als Dark/White-Referenz kalibriert ist.
  // false, falls nie gespeichert ODER JSON nicht lesbar (out faellt dann auf
  // die RootSettings-Defaults zurueck).
  bool loadSettings(RootSettings& out);
  void saveSettings(const RootSettings& v);

private:
  bool load(const char* key, Measurement& out);
  void save(const char* key, const Measurement& v);

  Preferences prefs_;
};
