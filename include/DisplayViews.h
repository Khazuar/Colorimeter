#pragma once
#include <Adafruit_SSD1306.h>
#include <cstdint>
#include "Spectrometer.h"

// Views arbeiten nur gegen die abstrakte Spectrometer&-Referenz plus vom
// Aufrufer bereitgestellten Kontext -- bewusst KEIN Zugriff auf AS7341-
// spezifische Interna (kein #include "AS7341Spectrometer.h" hier), damit
// dieselben Views unveraendert mit einem zukuenftigen AS7343Spectrometer
// funktionieren. White/Dark-Referenzmessungen laufen NICHT mehr ueber diese
// Views (eigener Screen in main.cpp, siehe renderReferenceStatus()) -- hier
// gibt es daher auch keinen Modus-Sonderfall mehr.
enum class DisplayView : uint8_t { ColorInfo = 0, Spectrum = 1, COUNT = 2 };

struct ViewContext {
  Spectrometer& spectrometer;
  const Measurement& measurement;       // letzte Messung (leer, falls noch keine erfolgt ist)
  const Measurement& whiteReference;
  const Measurement& darkReference;
  bool calibrated;                      // von der Orchestrierung getrackt
  const char* modeLabel;                // "S"/"P"/"W"/"D"/"E" fuer die Ecken-Anzeige
  const char* lastLabel;                // z.B. "sample_03", "" falls noch keine Messung
  // Filter, der zum Zeitpunkt VON 'measurement' tatsaechlich eingesetzt war
  // (NICHT notwendigerweise der aktuell im Settings-Modus gewaehlte) -- siehe
  // main.cpp::renderCurrentView() fuer die Begruendung dieser Asymmetrie
  // gegenueber whiteReference/darkReference.
  FilterState filterState;
};

using ViewRenderFn = void (*)(Adafruit_SSD1306& d, const ViewContext& ctx);

extern const ViewRenderFn VIEW_RENDERERS[static_cast<uint8_t>(DisplayView::COUNT)];
