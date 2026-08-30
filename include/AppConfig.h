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

// Messmodus: reines Orchestrierungs-Konzept, gehoert nicht in den Spectrometer.
// Per Mode-Taste (lang) durchgeschaltet: Fast -> Precise -> White -> Dark -> Export -> Settings -> Info -> Fast ...
// White/Dark messen dabei immer mit Precision::Precise (siehe main.cpp precisionFor()),
// unabhaengig vom Namen -- die Referenz ist Grundlage jeder spaeteren Reflexionsberechnung.
// Export misst nicht physisch: Trigger sendet dort stattdessen die Messhistorie per BLE.
// Settings misst nicht: Trigger rotiert dort den Wert der aktuell gewaehlten Einstellung
// (z.B. FilterState), Mode (kurz) wechselt, WELCHE Einstellung editiert wird.
// Info misst ueberhaupt nicht (Trigger ist dort ein No-Op): reiner Statusbildschirm fuer
// Betriebszeit/Messzaehler aus UptimeLogger.
enum class MeasureMode : uint8_t { Fast = 0, Precise = 1, White = 2, Dark = 3, Export = 4, Settings = 5, Info = 6, COUNT = 7 };

// BLE-Export (Nordic UART Service) -- Geraetename ist app-weit relevant (main.cpp
// startet/stoppt BleExporter damit); Chunk-Groesse/MTU/Delays bleiben rein interne
// Implementierungsdetails von BleExporter.cpp.
static constexpr char BLE_DEVICE_NAME[] = "Colorimeter";
