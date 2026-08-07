#pragma once
#include <cstdint>
#include <Preferences.h>

// Kumulierte Betriebszeit (seit je, ueber Reboots/Stromausfaelle hinweg) in
// Sekunden, persistiert auf einer DEDIZIERTEN NVS-Partition ("uptime", siehe
// partitions.csv) -- getrennt von der Kalibrierungs-Partition, damit die
// haeufigen Schreibvorgaenge hier nicht deren Flash-Sektor mitverschleissen.
// Committed hoechstens 1x/Minute, unabhaengig davon wie oft loop() laeuft.
//
// Traegt ausserdem einen lebenslangen Messzaehler (jede erfolgreiche
// performMeasurement(), unabhaengig vom Modus) auf derselben Partition --
// selbes "haeufig geschriebene Betriebstelemetrie"-Profil wie die Betriebszeit,
// daher bewusst kein eigenes Modul/keine eigene Partition dafuer.
class UptimeLogger {
public:
  void begin();
  void loop();
  uint32_t totalSeconds() const { return totalSeconds_; }

  void recordMeasurement();
  uint32_t measurementCount() const { return measurementCount_; }

private:
  Preferences prefs_;
  uint32_t totalSeconds_ = 0;
  uint32_t lastFlushMillis_ = 0;
  uint32_t measurementCount_ = 0;
};
