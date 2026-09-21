#pragma once
#include <Preferences.h>
#include "Spectrometer.h"
#include "AppConfig.h"
#include "TipCatalog.h"

// Duenner NVS-Wrapper fuer die Dunkel-/Weiss-Kalibrierung, die Einstellungs-
// Hierarchie (RootSettings) UND den Messspitzen-Katalog (TipCatalog). Gehoert
// der Orchestrierung (main.cpp) -- der Spectrometer selbst fasst kein Flash
// an. Persistiert jeweils als JSON-String (siehe CalibrationStore.cpp und
// schema/settings.schema.json bzw. schema/tips.schema.json) statt als
// Binaer-Blob -- robust gegenueber hinzukommenden/fehlenden Feldern,
// Enum-Werte als Strings statt Zahlen-Index. Groesse der Rohwerte wird aus
// dem gespeicherten Blob selbst ermittelt (getBytesLength), die
// Orchestrierung muss also keine sensorspezifische Kanalanzahl kennen.
class CalibrationStore {
public:
  void begin();  // prefs_.begin("colorim", false)

  // false, falls nie gespeichert ODER JSON nicht lesbar (out/optical fallen
  // dann auf ihre Defaults zurueck).
  bool loadDark(Measurement& out, OpticalSettings& optical);
  bool loadWhite(Measurement& out, OpticalSettings& optical);

  // optical wird zusammen mit den Rohwerten gespeichert -- Referenz und die
  // bei ihrer Aufnahme aktiven Einstellungen gehoeren untrennbar zusammen
  // (siehe main.cpp::calibrationValidFor()).
  void saveDark(const Measurement& v, const OpticalSettings& optical);
  void saveWhite(const Measurement& v, const OpticalSettings& optical);

  // Die vollstaendige, aktuell im Settings-Baum gewaehlte Konfiguration --
  // unabhaengig davon, was gerade als Dark/White-Referenz kalibriert ist.
  // false, falls nie gespeichert ODER JSON nicht lesbar (out faellt dann auf
  // die RootSettings-Defaults zurueck).
  bool loadSettings(RootSettings& out);
  void saveSettings(const RootSettings& v);

  // Der Messspitzen-Katalog (main.cpp::tipCatalog). false, falls nie
  // gespeichert ODER JSON ungueltig (out wird dann ein LEERER Katalog --
  // main.cpp::setup() bootstrapped in diesem Fall die allererste Spitze).
  bool loadTips(TipCatalog& out);
  void saveTips(const TipCatalog& v);

private:
  bool load(const char* key, Measurement& out);
  void save(const char* key, const Measurement& v);

  Preferences prefs_;
};
