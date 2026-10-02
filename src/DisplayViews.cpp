#include "DisplayViews.h"
#include "AppConfig.h"
#include "ColorimetryTables.h"
#include <cmath>
#include <cstdio>
#include <cstring>

// The display cannot meaningfully show more bands than this -- a pure
// rendering limit for the bar width, not a sensor-specific assumption.
static const size_t MAX_DISPLAYABLE_BANDS = 16;

static void drawModeCorner(Adafruit_SSD1306& d, const char* modeLabel) {
  d.setTextSize(1);
  d.setTextColor(SSD1306_WHITE);
  d.setCursor(OLED_WIDTH - 6, 0);
  d.print(modeLabel);
}

static void renderNotCalibrated(Adafruit_SSD1306& d) {
  d.setCursor(0, 0);
  d.println("not calibrated");
  d.println("Mode long: White/");
  d.println("Dark select,");
  d.println("then trigger");
}

// Unlike "not calibrated": calibration is available, it just hasn't been
// measured in this mode yet since the last mode change (or boot).
static void renderNoMeasurementYet(Adafruit_SSD1306& d) {
  d.setCursor(0, 0);
  d.println("no measurement");
  d.println("Press trigger");
}

// Replaces the former LabHex view: label, L*/a*/b*, chroma/hue angle
// (C*/h, see labToLCh() -- the same information as a*/b*, just cylindrical
// and often more intuitive) and hex code, all on one screen.
static void renderColorInfo(Adafruit_SSD1306& d, const ViewContext& ctx) {
  d.clearDisplay();
  d.setTextColor(SSD1306_WHITE);
  d.setTextSize(1);

  if (!ctx.calibrated) {
    renderNotCalibrated(d);
  } else if (ctx.measurement.empty()) {
    renderNoMeasurementYet(d);
  } else {
    Spectrum spec = ctx.spectrometer.getSpectrum(ctx.measurement, ctx.whiteReference, ctx.darkReference, ctx.filterState);
    Lab lab = getColor(spec);
    LCh lch = labToLCh(lab);
    uint8_t r, g, b;
    labToSRGB255(lab, r, g, b);

    char line[24];
    d.setCursor(0, 0);
    d.println(ctx.lastLabel);

    d.setCursor(0, 16);
    snprintf(line, sizeof(line), "L* %6.2f", lab.L); d.println(line);
    snprintf(line, sizeof(line), "a* %6.2f", lab.a); d.println(line);
    snprintf(line, sizeof(line), "b* %6.2f", lab.b); d.println(line);

    snprintf(line, sizeof(line), "C* %5.1f h %5.1f", lch.C, lch.h); d.println(line);

    snprintf(line, sizeof(line), "#%02X%02X%02X", r, g, b);
    d.setTextSize(2);
    d.setCursor(0, 48);
    d.println(line);
  }

  drawModeCorner(d, ctx.modeLabel);
  d.display();
}

// Prints 'text' rotated an additional 90 degrees counterclockwise, as seen
// from the display already mounting-corrected via Rotation 2 (see
// main.cpp::setup(), display.setRotation(2)). leftX/bottomY are the
// left/bottom edge of the resulting bounding box in normal (upright) screen
// coordinates (0..127 / 0..63).
//
// Derivation: Rotation 1 produces, as seen from the already 180-degree
// mounting-corrected Rotation-2 view, exactly one additional 90-degree CCW
// rotation (worked out via the rotation transformations hard-wired into
// Adafruit_GFX). The resulting conversion from an upright anchor position
// to the cursor coordinates to be set in Rotation 1 is:
// cx = (OLED_HEIGHT-1) - bottomY, cy = leftX.
static void drawRotatedLabel(Adafruit_SSD1306& d, const char* text, int leftX, int bottomY, uint16_t color) {
  d.setRotation(1);
  d.setTextSize(1);
  d.setTextColor(color);
  d.setCursor((OLED_HEIGHT - 1) - bottomY, leftX);
  d.print(text);
  d.setRotation(2);  // back to the normal, mounting-corrected orientation
}

// Uses getSpectrum() (8 normalized VIS reflectance values) -- Clear/NIR/raw
// Measurement values are not relevant here (see Spectrometer.h). No numeric
// table area -- band center and value (as percent) are placed rotated
// directly below/inside the bar, so that as much height as possible remains
// for the bars themselves. White/dark reference measurements no longer run
// through this view (own screen in main.cpp), so there is no mode special
// case here.
static void renderSpectrum(Adafruit_SSD1306& d, const ViewContext& ctx) {
  d.clearDisplay();
  d.setTextColor(SSD1306_WHITE);
  d.setTextSize(1);

  if (ctx.measurement.empty()) {
    d.setCursor(0, 0);
    d.println("no measurement");
  } else {
    Spectrum spec = ctx.spectrometer.getSpectrum(ctx.measurement, ctx.whiteReference, ctx.darkReference, ctx.filterState);
    size_t n = spec.values.size();
    if (n > MAX_DISPLAYABLE_BANDS) n = MAX_DISPLAYABLE_BANDS;

    // Pre-format the wavelength labels to know their width -- in the rotated
    // state this becomes the height that must be reserved for the bar area.
    char wlText[MAX_DISPLAYABLE_BANDS][6];
    int maxWlWidth = 0;
    for (size_t i = 0; i < n; i++) {
      snprintf(wlText[i], sizeof(wlText[i]), "%d", (int)lroundf(spec.bands[i].center_nm));
      int w = (int)strlen(wlText[i]) * 6;
      if (w > maxWlWidth) maxWlWidth = w;
    }

    const int gap = 1;
    int barAreaH = OLED_HEIGHT - maxWlWidth - gap;
    if (barAreaH < 8) barAreaH = 8;  // safety net for extremely long labels

    // Reference line at reflectance=1.0 (white), but also scale larger if a
    // channel exceeds it.
    float maxV = 1.0f;
    for (size_t i = 0; i < n; i++) if (spec.values[i] > maxV) maxV = spec.values[i];

    int barW = OLED_WIDTH / (int)(n > 0 ? n : 1);
    const int margin = 2;

    for (size_t i = 0; i < n; i++) {
      int barX = (int)i * barW;
      int h = (int)((spec.values[i] / maxV) * barAreaH);
      if (h < 0) h = 0;
      if (h > barAreaH) h = barAreaH;
      int barTopY = barAreaH - h;
      if (h > 0) d.fillRect(barX, barTopY, barW > 2 ? barW - 2 : barW, h, SSD1306_WHITE);

      int labelLeftX = barX + (barW - 8) / 2;  // 8px = text height at textSize 1

      // Band center, centered below the bar, rotated.
      drawRotatedLabel(d, wlText[i], labelLeftX, OLED_HEIGHT - 1, SSD1306_WHITE);

      // Value as percent (reflectance is a ratio, "52%" is just as short as
      // ".52" and reads more unambiguously): inside the bar if it is large
      // enough (>50%), otherwise above it -- also rotated.
      char valText[6];
      snprintf(valText, sizeof(valText), "%d%%", (int)lroundf(spec.values[i] * 100.0f));
      int valW = (int)strlen(valText) * 6;

      if (spec.values[i] > 0.5f) {
        int bottomY = barTopY + margin + valW - 1;
        if (bottomY > barAreaH - 1) bottomY = barAreaH - 1;  // safety net
        drawRotatedLabel(d, valText, labelLeftX, bottomY, SSD1306_BLACK);
      } else {
        int bottomY = barTopY - margin;
        if (bottomY - valW + 1 < 0) bottomY = valW - 1;  // safety net
        drawRotatedLabel(d, valText, labelLeftX, bottomY, SSD1306_WHITE);
      }
    }
  }

  drawModeCorner(d, ctx.modeLabel);
  d.display();
}

const ViewRenderFn VIEW_RENDERERS[static_cast<uint8_t>(DisplayView::COUNT)] = {
  renderColorInfo, renderSpectrum
};
