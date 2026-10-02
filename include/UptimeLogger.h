#pragma once
#include <cstdint>
#include <Preferences.h>

// Cumulative operating time (ever since, across reboots/power losses) in
// seconds, persisted on a DEDICATED NVS partition ("uptime", see
// partitions.csv) -- separate from the calibration partition, so that the
// frequent writes here don't also wear out its flash sector. Committed at
// most once per minute, regardless of how often loop() runs.
//
// Also carries a lifetime measurement counter (every successful
// performMeasurement(), regardless of mode) on the same partition -- same
// "frequently written operational telemetry" profile as the uptime, hence
// deliberately no separate module/partition for it.
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
