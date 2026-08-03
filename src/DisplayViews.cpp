#include "DisplayViews.h"
#include "AppConfig.h"
#include "ColorimetryTables.h"
#include "ColorNames.h"
#include <cmath>
#include <cstdio>
#include <cstring>

// Display kann nicht sinnvoll mehr Baender als das anzeigen -- reine
// Darstellungsgrenze fuer die Balkenbreite, keine sensorspezifische Annahme.
static const size_t MAX_DISPLAYABLE_BANDS = 16;

static void drawModeCorner(Adafruit_SSD1306& d, const char* modeLabel) {
  d.setTextSize(1);
  d.setTextColor(SSD1306_WHITE);
  d.setCursor(OLED_WIDTH - 6, 0);
  d.print(modeLabel);
}

static void renderNotCalibrated(Adafruit_SSD1306& d) {
  d.setCursor(0, 0);
  d.println("nicht kalibriert");
  d.println("Mode lang: White/");
  d.println("Dark waehlen,");
  d.println("dann Trigger");
}

// Anders als "nicht kalibriert": Kalibrierung liegt vor, es wurde nur seit
// dem letzten Moduswechsel (oder Boot) noch nicht in diesem Modus gemessen.
static void renderNoMeasurementYet(Adafruit_SSD1306& d) {
  d.setCursor(0, 0);
  d.println("keine Messung");
  d.println("Trigger druecken");
}

// Ersetzt die fruehere LabHex- und NearestName-View: Label, L*/a*/b*, naechster
// Farbname+DeltaE und Hex-Code, alles auf einem Screen.
static void renderColorInfo(Adafruit_SSD1306& d, const ViewContext& ctx) {
  d.clearDisplay();
  d.setTextColor(SSD1306_WHITE);
  d.setTextSize(1);

  if (!ctx.calibrated) {
    renderNotCalibrated(d);
  } else if (ctx.measurement.empty()) {
    renderNoMeasurementYet(d);
  } else {
    Spectrum spec = ctx.spectrometer.getSpectrum(ctx.measurement, ctx.whiteReference, ctx.darkReference);
    Lab lab = getColor(spec);
    uint8_t r, g, b;
    labToSRGB255(lab, r, g, b);
    float dE;
    const char* name = nearestColorName(lab, &dE);

    char line[24];
    d.setCursor(0, 0);
    d.println(ctx.lastLabel);

    d.setCursor(0, 16);
    snprintf(line, sizeof(line), "L* %6.2f", lab.L); d.println(line);
    snprintf(line, sizeof(line), "a* %6.2f", lab.a); d.println(line);
    snprintf(line, sizeof(line), "b* %6.2f", lab.b); d.println(line);

    snprintf(line, sizeof(line), "%s dE%.1f", name, dE);
    d.println(line);

    snprintf(line, sizeof(line), "#%02X%02X%02X", r, g, b);
    d.setTextSize(2);
    d.setCursor(0, 48);
    d.println(line);
  }

  drawModeCorner(d, ctx.modeLabel);
  d.display();
}

// Druckt 'text' um zusaetzliche 90 Grad gegen den Uhrzeigersinn gedreht, aus
// Sicht der bereits ueber Rotation 2 montagekorrigierten Anzeige (siehe
// main.cpp::setup(), display.setRotation(2)). leftX/bottomY sind linke/untere
// Kante der resultierenden Boundingbox in normalen (aufrechten) Bildschirm-
// koordinaten (0..127 / 0..63).
//
// Herleitung: Rotation 1 liefert, aus Sicht der bereits um 180 Grad montage-
// korrigierten Rotation-2-Ansicht, exakt eine zusaetzliche 90-Grad-CCW-Drehung
// (durchgerechnet ueber die in Adafruit_GFX fest verdrahteten Rotations-
// Transformationen). Die daraus folgende Umrechnung von aufrechter Anker-
// position zu den in Rotation 1 zu setzenden Cursor-Koordinaten ist:
// cx = (OLED_HEIGHT-1) - bottomY, cy = leftX.
static void drawRotatedLabel(Adafruit_SSD1306& d, const char* text, int leftX, int bottomY, uint16_t color) {
  d.setRotation(1);
  d.setTextSize(1);
  d.setTextColor(color);
  d.setCursor((OLED_HEIGHT - 1) - bottomY, leftX);
  d.print(text);
  d.setRotation(2);  // zurueck zur normalen, montagekorrigierten Ausrichtung
}

// Nutzt getSpectrum() (8 normierte VIS-Reflexionswerte) -- Clear/NIR/rohe
// Measurement-Werte sind hier nicht relevant (siehe Spectrometer.h). Kein
// Zahlen-Tabellenbereich -- Bandzentrum und Wert (als Prozent) stehen
// gedreht direkt unter/im Balken, damit moeglichst viel Hoehe fuer die
// Balken selbst bleibt. White/Dark-Referenzmessungen laufen nicht mehr ueber
// diese View (eigener Screen in main.cpp), daher hier kein Modus-Sonderfall.
static void renderSpectrum(Adafruit_SSD1306& d, const ViewContext& ctx) {
  d.clearDisplay();
  d.setTextColor(SSD1306_WHITE);
  d.setTextSize(1);

  if (ctx.measurement.empty()) {
    d.setCursor(0, 0);
    d.println("keine Messung");
  } else {
    Spectrum spec = ctx.spectrometer.getSpectrum(ctx.measurement, ctx.whiteReference, ctx.darkReference);
    size_t n = spec.values.size();
    if (n > MAX_DISPLAYABLE_BANDS) n = MAX_DISPLAYABLE_BANDS;

    // Wellenlaengen-Labels vorab formatieren, um deren Breite zu kennen --
    // im gedrehten Zustand wird daraus die Hoehe, die fuer den Balkenbereich
    // reserviert werden muss.
    char wlText[MAX_DISPLAYABLE_BANDS][6];
    int maxWlWidth = 0;
    for (size_t i = 0; i < n; i++) {
      snprintf(wlText[i], sizeof(wlText[i]), "%d", (int)lroundf(spec.bands[i].center_nm));
      int w = (int)strlen(wlText[i]) * 6;
      if (w > maxWlWidth) maxWlWidth = w;
    }

    const int gap = 1;
    int barAreaH = OLED_HEIGHT - maxWlWidth - gap;
    if (barAreaH < 8) barAreaH = 8;  // Sicherheitsnetz bei extrem langen Labels

    // Referenzlinie bei Reflexion=1.0 (Weiss), aber auch groesser skalieren
    // falls ein Kanal darueber liegt.
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

      int labelLeftX = barX + (barW - 8) / 2;  // 8px = Texthoehe bei textSize 1

      // Bandzentrum, zentriert unter dem Balken, gedreht.
      drawRotatedLabel(d, wlText[i], labelLeftX, OLED_HEIGHT - 1, SSD1306_WHITE);

      // Wert als Prozent (Reflexion ist ein Verhaeltnis, "52%" ist genauso
      // kurz wie ".52" und eindeutiger lesbar): im Balken, wenn er gross
      // genug ist (>50%), sonst darueber -- ebenfalls gedreht.
      char valText[6];
      snprintf(valText, sizeof(valText), "%d%%", (int)lroundf(spec.values[i] * 100.0f));
      int valW = (int)strlen(valText) * 6;

      if (spec.values[i] > 0.5f) {
        int bottomY = barTopY + margin + valW - 1;
        if (bottomY > barAreaH - 1) bottomY = barAreaH - 1;  // Sicherheitsnetz
        drawRotatedLabel(d, valText, labelLeftX, bottomY, SSD1306_BLACK);
      } else {
        int bottomY = barTopY - margin;
        if (bottomY - valW + 1 < 0) bottomY = valW - 1;  // Sicherheitsnetz
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
