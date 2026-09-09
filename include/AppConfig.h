#pragma once
#include <Adafruit_AS7341.h>
#include <cstdint>
#include "Spectrometer.h"  // FilterState (Teil von AcquisitionSettings, siehe unten)

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
// zur Laufzeit im Settings-Modus einstellbar, siehe AcquisitionSettings unten.
static const uint8_t       AS_ATIME = 100;
static const uint16_t      AS_ASTEP = 999;                 // ~281 ms Integration bei diesem Startwert
static const as7341_gain_t AS_GAIN  = AS7341_GAIN_512X;    // Weiss bleibt unter Vollausschlag

// ------------------------- Taster -------------------------
static const uint8_t  TRIGGER_PIN   = 3;
static const uint8_t  MODE_PIN      = 9;
static const uint32_t DEBOUNCE_MS   = 40;
static const uint32_t LONG_PRESS_MS = 600;

// Welcher Top-Level-Bildschirm gerade aktiv ist (per Mode-Taste lang
// durchgeschaltet: Fast -> Precise -> Calibration -> Export -> Settings -> Info -> Fast ...).
// Reines UI-/Orchestrierungs-Konzept -- hat NICHTS mit der Sensor-Messqualitaet
// zu tun (das ist Spectrometer::Precision) und NICHTS mit der Art eines
// gespeicherten Messwerts (das ist SampleKind, siehe unten). Fast/Precise
// zeigen dieselbe Live-Ansicht (siehe DisplayViews.h), messen aber mit
// unterschiedlicher Precision (siehe main.cpp, Trigger-Dispatch in loop()).
// Calibration fasst Weiss- und Dunkelreferenz in einem Bildschirm zusammen
// (kurzer Mode-Druck wechselt in main.cpp zwischen main.cpp::CalibrationTarget
// White/Dark, langer Trigger-Druck misst die jeweils gewaehlte Referenz immer
// mit Precision::Precise). Export misst nicht physisch: Trigger sendet dort
// stattdessen die Messhistorie per BLE. Settings misst nicht: Trigger rotiert
// dort den Wert der aktuell gewaehlten Einstellung (z.B. FilterState), Mode
// (kurz) wechselt, WELCHE Einstellung editiert wird. Info misst ueberhaupt
// nicht (Trigger ist dort ein No-Op): reiner Statusbildschirm fuer
// Betriebszeit/Messzaehler aus UptimeLogger.
enum class DisplayMode : uint8_t { Fast = 0, Precise = 1, Calibration = 2, Export = 3, Settings = 4, Info = 5, COUNT = 6 };

// Was ein gespeicherter/exportierter Messwert repraesentiert
// (HistoryStore::MeasurementRecord) -- unabhaengig davon, mit welchem
// DisplayMode/welcher Precision er aufgenommen wurde. Regular = normale
// Fast/Precise-Probe; Dark/White = Referenzaufnahmen aus dem
// Calibration-DisplayMode. Bewusst ein eigenes Enum statt DisplayMode
// mitzubenutzen: welcher Bildschirm gerade aktiv ist und was ein Messwert
// bedeutet sind zwei unabhaengige Fragen.
enum class SampleKind : uint8_t { Regular = 0, Dark = 1, White = 2, COUNT = 3 };

// Anzahl bekannter as7341_gain_t-Werte (0.5X..512X, siehe Adafruit_AS7341.h)
// -- einzige Quelle fuer Bounds-Checks (HistoryStore::parseLine()) und die
// Settings-Registry/Label-Tabelle (main.cpp).
static const uint8_t AS7341_GAIN_COUNT = 11;

// Buendel aller Sensor-/Aufnahme-Einstellungen, die eine Messung beeinflussen
// -- als Ganzes verglichen (siehe main.cpp::calibrationValidFor()), pro
// Messung mitgespeichert/exportiert und live im Settings-Modus editierbar.
// AS_ATIME/AS_ASTEP/AS_GAIN dienen hier nur noch als Startwerte.
struct AcquisitionSettings {
  FilterState filterState = FilterState::None;
  as7341_gain_t gain = AS_GAIN;
  uint8_t atime = AS_ATIME;
  uint16_t astep = AS_ASTEP;
  bool operator==(const AcquisitionSettings& o) const {
    return filterState == o.filterState && gain == o.gain && atime == o.atime && astep == o.astep;
  }
  bool operator!=(const AcquisitionSettings& o) const { return !(*this == o); }
};

// BLE-Export (Nordic UART Service) -- Geraetename ist app-weit relevant (main.cpp
// startet/stoppt BleExporter damit); Chunk-Groesse/MTU/Delays bleiben rein interne
// Implementierungsdetails von BleExporter.cpp.
static constexpr char BLE_DEVICE_NAME[] = "Colorimeter";
