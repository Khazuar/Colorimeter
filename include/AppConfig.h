#pragma once
#include <Adafruit_AS7341.h>
#include <cstdint>
#include "Spectrometer.h"  // FilterState (Teil von RootSettings/AcquisitionParameters, siehe unten)

// ------------------------- I2C / Bus -------------------------
static const uint8_t  SDA_PIN = 6;
static const uint8_t  SCL_PIN = 7;
static const uint32_t I2C_HZ  = 100000;

// ------------------------- OLED (SSD1306, 0.96", 128x64, I2C) ---------
static const uint8_t OLED_WIDTH  = 128;
static const uint8_t OLED_HEIGHT = 64;
static const uint8_t OLED_ADDR   = 0x3C;

// ------------------------- AS7341 Timing -------------------------
// Nur noch Startwerte fuer den allerersten Boot (vorher fest verdrahtet) --
// zur Laufzeit im Settings-Baum einstellbar, siehe SensorSettings unten.
static const uint8_t       AS_ATIME = 100;
static const uint16_t      AS_ASTEP = 999;                 // ~281 ms Integration bei diesem Startwert
static const as7341_gain_t AS_GAIN  = AS7341_GAIN_512X;    // Weiss bleibt unter Vollausschlag

// ------------------------- Taster -------------------------
static const uint8_t  TRIGGER_PIN   = 3;
static const uint8_t  MODE_PIN      = 9;
static const uint32_t DEBOUNCE_MS   = 40;
static const uint32_t LONG_PRESS_MS = 600;

// Welcher Top-Level-Bildschirm gerade aktiv ist (per Mode-Taste lang
// durchgeschaltet: Measure -> Calibration -> Export -> Settings -> Info -> Measure ...).
// Reines UI-/Orchestrierungs-Konzept -- hat NICHTS mit der Sensor-Messqualitaet
// zu tun (das ist Spectrometer::Precision) und NICHTS mit der Art eines
// gespeicherten Messwerts (das ist SampleKind, siehe unten).
//
// Measure fasst ALLE physischen Messmodi (aktuell: Precise, Single) in einem
// Top-Level-Eintrag zusammen, um die Hauptnavigation nicht mit jedem neuen,
// selten genutzten Messmodus weiter wachsen zu lassen. Measure hat dafuer eine
// eigene, zweistufige Sub-Navigation (main.cpp::MeasurePage): eine
// Auswahl-Seite (Cursor per kurzem Mode-Druck ueber main.cpp::MEASUREMENT_MODES
// bewegt, Trigger loest die gewaehlte Messung aus) und eine Ergebnis-Seite
// (identisch zur bisherigen Fast/Precise-Ansicht, siehe DisplayViews.h; kurzer
// Mode-Druck wechselt dort wie bisher die Ergebnis-Ansicht, langer Mode-Druck
// kehrt zur Auswahl-Seite zurueck OHNE den Top-Level-DisplayMode zu wechseln --
// siehe main.cpp::cycleMode()).
//
// Calibration fasst Weiss- und Dunkelreferenz in einem Bildschirm zusammen
// (kurzer Mode-Druck wechselt in main.cpp zwischen main.cpp::CalibrationTarget
// White/Dark, langer Trigger-Druck misst die jeweils gewaehlte Referenz immer
// mit Precision::Precise). Export misst nicht physisch: Trigger sendet dort
// stattdessen die Messhistorie per BLE. Settings misst nicht: Trigger rotiert
// dort den Wert der aktuell gewaehlten Einstellung (z.B. FilterState), Mode
// (kurz) wechselt, WELCHE Einstellung editiert wird. Info misst ueberhaupt
// nicht (Trigger ist dort ein No-Op): reiner Statusbildschirm fuer
// Betriebszeit/Messzaehler aus UptimeLogger.
enum class DisplayMode : uint8_t { Measure = 0, Calibration = 1, Export = 2, Settings = 3, Info = 4, COUNT = 5 };

// Was ein gespeicherter/exportierter Messwert repraesentiert
// (HistoryStore::MeasurementRecord) -- unabhaengig davon, mit welchem
// DisplayMode/welcher Precision er aufgenommen wurde. Regular = normale
// Precise/Single-Probe (aus dem Measure-DisplayMode); Dark/White =
// Referenzaufnahmen aus dem Calibration-DisplayMode. Bewusst ein eigenes Enum
// statt DisplayMode mitzubenutzen: welcher Bildschirm gerade aktiv ist und was
// ein Messwert bedeutet sind zwei unabhaengige Fragen.
enum class SampleKind : uint8_t { Regular = 0, Dark = 1, White = 2, COUNT = 3 };

// Anzahl bekannter as7341_gain_t-Werte (0.5X..512X, siehe Adafruit_AS7341.h)
// -- einzige Quelle fuer Bounds-Checks (HistoryStore::parseLine()) und die
// Settings-Registry/Label-Tabelle (main.cpp).
static const uint8_t AS7341_GAIN_COUNT = 11;

// Gain/ATIME/ASTEP als Block -- siehe schema/settings.schema.json ("sensor").
// Wird UNVERAENDERT als Ganzes sowohl in RootSettings als auch in
// AcquisitionParameters verwendet (siehe deren Kommentare unten). AS_ATIME/
// AS_ASTEP/AS_GAIN dienen hier nur noch als Startwerte fuer den allerersten Boot.
struct SensorSettings {
  as7341_gain_t gain = AS_GAIN;
  uint8_t atime = AS_ATIME;
  uint16_t astep = AS_ASTEP;
  bool operator==(const SensorSettings& o) const {
    return gain == o.gain && atime == o.atime && astep == o.astep;
  }
  bool operator!=(const SensorSettings& o) const { return !(*this == o); }
};

struct AcquisitionParameters;  // fwd, siehe unten

// Die tatsaechliche Einstellungs-Hierarchie: UI-Baum-Wurzel UND das, was als
// EIN JSON-Dokument persistiert wird (CalibrationStore::save/loadSettings()).
// Siehe schema/settings.schema.json ("Settings") -- BEIDE (dieses Struct und
// die Schema-Datei) bei Aenderungen zusammen pflegen. filterState bleibt
// bewusst ein eigenstaendiges Geschwisterfeld statt Teil von SensorSettings
// (fuer Filter gibt es eigene Plaene). Kuenftige, nicht Aufnahme-bezogene
// Einstellungen werden hier als weitere Geschwisterfelder ergaenzt -- NICHT in
// AcquisitionParameters (das ist kein Einstellungs-Knoten, siehe dort).
struct RootSettings {
  FilterState filterState = FilterState::None;
  SensorSettings sensor;
  bool operator==(const RootSettings& o) const {
    return filterState == o.filterState && sensor == o.sensor;
  }
  bool operator!=(const RootSettings& o) const { return !(*this == o); }

  // Bewusst eine kleine Kopiermethode statt Cast/Vererbung -- macht explizit,
  // dass hier ein purpose-cut Parameter-Buendel aus dem aktuellen
  // Einstellungsstand HERAUSKOPIERT wird, kein struktureller Zusammenhang.
  AcquisitionParameters toAcquisitionParameters() const;
};

// KEIN Einstellungs-Knoten -- ein reines Parameter-Buendel fuer
// AS7341Spectrometer::applySettings()/main.cpp::calibrationValidFor(), das
// lange, sich wiederholende Parameterlisten vermeidet. Wird nie selbst
// editiert oder im Settings-Baum navigiert, sondern aus
// RootSettings::toAcquisitionParameters() heraus zusammenkopiert bzw. pro
// Messung/Referenz eingefroren (siehe HistoryStore::MeasurementRecord::settings,
// main.cpp darkRefSettings/whiteRefSettings) -- das sind Snapshots "womit wurde
// das gemessen", keine Einstellungen im Sinne der Baum-Hierarchie. Hat
// zufaellig heute dieselben Felder wie RootSettings (weil aktuell
// ausschliesslich Aufnahme-relevante Einstellungen existieren) -- das ist
// keine strukturelle Garantie.
struct AcquisitionParameters {
  FilterState filterState = FilterState::None;
  SensorSettings sensor;
  bool operator==(const AcquisitionParameters& o) const {
    return filterState == o.filterState && sensor == o.sensor;
  }
  bool operator!=(const AcquisitionParameters& o) const { return !(*this == o); }
};

inline AcquisitionParameters RootSettings::toAcquisitionParameters() const {
  AcquisitionParameters p;
  p.filterState = filterState;
  p.sensor = sensor;
  return p;
}

// BLE-Export (Nordic UART Service) -- Geraetename ist app-weit relevant (main.cpp
// startet/stoppt BleExporter damit); Chunk-Groesse/MTU/Delays bleiben rein interne
// Implementierungsdetails von BleExporter.cpp.
static constexpr char BLE_DEVICE_NAME[] = "Colorimeter";
