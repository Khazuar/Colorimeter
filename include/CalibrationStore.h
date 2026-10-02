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
  void begin();  // prefs_.begin("colorim", false) + LittleFS.begin() (fuer loadTips/saveTips, siehe dort)

  // false, falls nie gespeichert ODER JSON nicht lesbar (out/optical fallen
  // dann auf ihre Defaults zurueck). outSem (absoluter Standardfehler je
  // Kanal, siehe MeasurementTelemetry::semPerChannel) bleibt leer, wenn nie
  // gespeichert (z.B. eine Referenz von vor dieser Erweiterung) -- kein
  // Ladefehler, checkValidity() faellt dann auf 0 fuer diesen Anteil zurueck.
  bool loadDark(Measurement& out, Measurement& outSem, OpticalSettings& optical);
  bool loadWhite(Measurement& out, Measurement& outSem, OpticalSettings& optical);

  // optical wird zusammen mit den Rohwerten gespeichert -- Referenz und die
  // bei ihrer Aufnahme aktiven Einstellungen gehoeren untrennbar zusammen
  // (siehe main.cpp::calibrationValidFor()).
  void saveDark(const Measurement& v, const Measurement& sem, const OpticalSettings& optical);
  void saveWhite(const Measurement& v, const Measurement& sem, const OpticalSettings& optical);

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
