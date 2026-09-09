#include "UptimeLogger.h"
#include <Arduino.h>

static const uint32_t FLUSH_INTERVAL_MS = 60000UL;  // 1x/Minute schreiben -- siehe Plan fuer die
                                                      // Flash-Verschleiss-Rechnung dahinter.

void UptimeLogger::begin() {
  prefs_.begin("uptime", false, "uptime");  // dedizierte Partition (Label == Partitionsname)
  totalSeconds_ = prefs_.getULong("seconds", 0);
  measurementCount_ = prefs_.getULong("meas_count", 0);
  lastFlushMillis_ = millis();
}

void UptimeLogger::loop() {
  uint32_t now = millis();
  uint32_t elapsedMs = now - lastFlushMillis_;  // ueberlaufsicher (unsigned Wraparound),
                                                 // auch ueber den millis()-Ueberlauf nach ~49,7 Tagen
  if (elapsedMs < FLUSH_INTERVAL_MS) return;

  uint32_t elapsedWholeSec = elapsedMs / 1000;
  totalSeconds_ += elapsedWholeSec;
  lastFlushMillis_ += elapsedWholeSec * 1000UL;  // Restmillisekunden < 1s bewusst NICHT verwerfen
  prefs_.putULong("seconds", totalSeconds_);
}

void UptimeLogger::recordMeasurement() {
  // Kein Batching noetig: eine Messung dauert selbst im schnellsten Fall (Single-Modus)
  // spuerbar laenger als eine Sekunde inkl. Anzeige/Entprellung, das begrenzt die
  // Schreibrate natuerlich auf weit unter 1/s -- selbst bei staendigem Dauerdruecken
  // waere das Zyklenlimit erst nach Tagen ununterbrochenen Spammens erreicht.
  measurementCount_++;
  prefs_.putULong("meas_count", measurementCount_);
}
