#pragma once
#include <Preferences.h>
#include <cstdint>
#include <vector>

// Duenner NVS-Wrapper fuer die Dunkel-/Weiss-Kalibrierung. Gehoert der
// Orchestrierung (main.cpp) -- der Spectrometer selbst fasst kein Flash an.
// Groesse wird aus dem gespeicherten Blob selbst ermittelt (getBytesLength),
// die Orchestrierung muss also keine sensorspezifische Kanalanzahl kennen.
class CalibrationStore {
public:
  void begin();  // prefs_.begin("colorim", false)

  // false, falls nie gespeichert (out bleibt dann leer)
  bool loadDark(std::vector<uint32_t>& out);
  bool loadWhite(std::vector<uint32_t>& out);

  void saveDark(const std::vector<uint32_t>& v);
  void saveWhite(const std::vector<uint32_t>& v);

private:
  bool load(const char* key, std::vector<uint32_t>& out);
  void save(const char* key, const std::vector<uint32_t>& v);

  Preferences prefs_;
};
