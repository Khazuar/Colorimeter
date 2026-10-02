#pragma once
#include <Adafruit_SSD1306.h>
#include <cstdint>
#include "Spectrometer.h"

// Views work only against the abstract Spectrometer& reference plus the
// context provided by the caller -- deliberately NO access to AS7341-
// specific internals (no #include "AS7341Spectrometer.h" here), so that
// the same views work unchanged with a future AS7343Spectrometer. White/
// dark reference measurements NO LONGER run through these views (own
// screen in main.cpp, see renderReferenceStatus()) -- so there is also no
// mode special case here anymore.
enum class DisplayView : uint8_t { ColorInfo = 0, Spectrum = 1, COUNT = 2 };

struct ViewContext {
  Spectrometer& spectrometer;
  const Measurement& measurement;       // last measurement (empty if none has occurred yet)
  const Measurement& whiteReference;
  const Measurement& darkReference;
  bool calibrated;                      // tracked by the orchestration
  const char* modeLabel;                // "S"/"P"/"W"/"D"/"E" for the corner display
  const char* lastLabel;                // e.g. "sample_03", "" if no measurement yet
  // Filter that was actually in use at the time OF 'measurement' (NOT
  // necessarily the one currently selected in Settings mode) -- see
  // main.cpp::renderCurrentView() for the rationale behind this asymmetry
  // relative to whiteReference/darkReference.
  FilterState filterState;
};

using ViewRenderFn = void (*)(Adafruit_SSD1306& d, const ViewContext& ctx);

extern const ViewRenderFn VIEW_RENDERERS[static_cast<uint8_t>(DisplayView::COUNT)];
