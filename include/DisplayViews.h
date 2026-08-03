#pragma once
#include <Adafruit_SSD1306.h>
#include <cstdint>
#include <vector>
#include "Spectrometer.h"
#include "AppConfig.h"

// Views arbeiten nur gegen die abstrakte Spectrometer&-Referenz plus vom
// Aufrufer bereitgestellten Kontext -- bewusst KEIN Zugriff auf AS7341-
// spezifische Interna (kein #include "AS7341Spectrometer.h" hier), damit
// dieselben Views unveraendert mit einem zukuenftigen AS7343Spectrometer
// funktionieren.
enum class DisplayView : uint8_t { ColorInfo = 0, Spectrum = 1, COUNT = 2 };

struct ViewContext {
  Spectrometer& spectrometer;
  const std::vector<uint32_t>& raw;  // letzte Rohmessung (leer, falls noch keine erfolgt ist)
  bool calibrated;                   // von der Orchestrierung getrackt
  const char* modeLabel;             // "F"/"P"/"W"/"D" fuer die Ecken-Anzeige
  const char* lastLabel;             // z.B. "sample_03", "" falls noch keine Messung
  MeasureMode mode;                  // fuer Spectrum-View: White/Dark zeigen Wertebereich-Nutzung
                                      // statt selbstbezueglicher Reflexion, siehe DisplayViews.cpp
};

using ViewRenderFn = void (*)(Adafruit_SSD1306& d, const ViewContext& ctx);

extern const ViewRenderFn VIEW_RENDERERS[static_cast<uint8_t>(DisplayView::COUNT)];
