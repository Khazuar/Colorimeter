#pragma once
#include <Adafruit_AS7341.h>
#include <cstdint>

// ------------------------- I2C / Bus -------------------------
static const uint8_t  SDA_PIN = 6;
static const uint8_t  SCL_PIN = 7;
static const uint32_t I2C_HZ  = 100000;

// ------------------------- OLED (SSD1306, 0.96", 128x64, I2C) ---------
static const uint8_t OLED_WIDTH  = 128;
static const uint8_t OLED_HEIGHT = 64;
static const uint8_t OLED_ADDR   = 0x3C;

// ------------------------- AS7341 Timing -------------------------
// EINGEFROREN: identisch fuer Dunkel, Weiss und alle Proben verwenden!
static const uint8_t       AS_ATIME = 100;
static const uint16_t      AS_ASTEP = 999;                 // ~281 ms Integration
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

// BLE-Export (Nordic UART Service) -- Geraetename ist app-weit relevant (main.cpp
// startet/stoppt BleExporter damit); Chunk-Groesse/MTU/Delays bleiben rein interne
// Implementierungsdetails von BleExporter.cpp.
static constexpr char BLE_DEVICE_NAME[] = "Colorimeter";
