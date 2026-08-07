#pragma once
#include <cstddef>
#include <cstdint>
#include "AppConfig.h"
#include "Spectrometer.h"

struct MeasurementRecord {
  char label[16];
  MeasureMode mode;
  Measurement measurement;
  // Kontext zum Messzeitpunkt -- nicht repraesentativ fuer die tatsaechliche
  // LED-Temperatur (misst nur den ESP32-Die), aber ein greifbarer Hinweis bei
  // spaeterer Auswertung, falls Messreihen unerklaerte Abweichungen zeigen.
  float tempC = 0.0f;         // ESP32-interner Temperatursensor, Grad Celsius
  uint32_t sessionMs = 0;     // millis() zum Messzeitpunkt (Laufzeit seit diesem Boot)
  uint32_t uptimeS = 0;       // UptimeLogger::totalSeconds() zum Messzeitpunkt (LED-Alter, lebenslang)
};

// Persistiert die Messhistorie zeilenweise als CSV auf der (bisher
// ungenutzten) "spiffs"-Partition via LittleFS -- physisch getrennt von
// "nvs" (Kalibrierung) und "uptime". Jede Zeile ist eine minimale, verlustfreie
// Rohdaten-Serialisierung (Label, Modus, rohe Measurement-Werte); Spectrum/
// Lab/Hex werden wie bisher erst bei jedem Export frisch aus den ROHDATEN neu
// berechnet (appendCsvRow() bleibt unveraendert) -- das entspricht genau dem
// heutigen Verhalten, bei dem sich eine Neukalibrierung rueckwirkend auf alle
// Historien-Zeilen im Export auswirkt.
class HistoryStore {
public:
  bool begin();
  bool append(const MeasurementRecord& rec);
  size_t count() const { return count_; }
  void clear();

  using RecordVisitor = void (*)(const MeasurementRecord& rec, void* userData);
  void forEach(RecordVisitor visitor, void* userData) const;

  // Kanalzahl der ersten gespeicherten Zeile, oder 0 falls leer/nicht gemountet.
  size_t firstRecordChannelCount() const;

private:
  bool mounted_ = false;
  size_t count_ = 0;
};
