#include "UptimeLogger.h"
#include <Arduino.h>

static const uint32_t FLUSH_INTERVAL_MS = 60000UL;  // write once/minute -- see the plan for the
                                                      // flash-wear calculation behind this.

void UptimeLogger::begin() {
  prefs_.begin("uptime", false, "uptime");  // dedicated partition (label == partition name)
  totalSeconds_ = prefs_.getULong("seconds", 0);
  measurementCount_ = prefs_.getULong("meas_count", 0);
  lastFlushMillis_ = millis();
}

void UptimeLogger::loop() {
  uint32_t now = millis();
  uint32_t elapsedMs = now - lastFlushMillis_;  // overflow-safe (unsigned wraparound),
                                                 // also across the millis() overflow after ~49.7 days
  if (elapsedMs < FLUSH_INTERVAL_MS) return;

  uint32_t elapsedWholeSec = elapsedMs / 1000;
  totalSeconds_ += elapsedWholeSec;
  lastFlushMillis_ += elapsedWholeSec * 1000UL;  // deliberately do NOT discard the remaining <1s milliseconds
  prefs_.putULong("seconds", totalSeconds_);
}

void UptimeLogger::recordMeasurement() {
  // No batching needed: even in the fastest case (Single mode), a measurement
  // takes noticeably longer than one second including display/debouncing,
  // which naturally limits the write rate to well under 1/s -- even with
  // constant continuous pressing, the cycle limit would only be reached after
  // days of uninterrupted spamming.
  measurementCount_++;
  prefs_.putULong("meas_count", measurementCount_);
}
