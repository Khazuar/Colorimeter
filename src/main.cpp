#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <cstring>
#include <cmath>

#include "AppConfig.h"
#include "Spectrometer.h"
#include "AS7341Spectrometer.h"
#include "CalibrationStore.h"
#include "Buttons.h"
#include "DisplayViews.h"

// ------------------------- Display -------------------------
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
bool displayOk = false;

// ------------------------- Sensor (nur ueber die Abstraktion nutzen) -------------------------
AS7341Spectrometer sensorImpl;
Spectrometer& spectrometer = sensorImpl;

// ------------------------- Kalibrierung: Sache der Orchestrierung -------------------------
CalibrationStore calStore;
std::vector<uint32_t> darkRef, whiteRef;
bool calibrated = false;

// ------------------------- letzte Messung / Anzeige-Zustand -------------------------
std::vector<uint32_t> lastRaw;
char lastLabel[16] = "";
uint16_t sampleCount = 0;
bool measuring = false;  // waehrend true: keine weitere Messung ausloesbar

MeasureMode currentMode = MeasureMode::Fast;
DisplayView currentView = DisplayView::ColorInfo;

// ------------------------- Messhistorie (nur RAM, nicht persistiert) -------------------------
// Jede Messung wird hier zusammen mit ihrem Modus gesammelt, damit sie in
// einem spaeteren Schritt vom Geraet abgerufen werden kann (z.B. ueber ein
// noch zu bauendes Serial-Kommando). Bewusst kein Flash-Zugriff -- das waere
// fuer eine reine "History" unnoetiger Verschleiss und Latenz.
struct MeasurementRecord {
  char label[16];
  MeasureMode mode;
  std::vector<uint32_t> raw;
};
std::vector<MeasurementRecord> history;

// ------------------------- Taster -------------------------
DebouncedButton triggerBtn(TRIGGER_PIN, DEBOUNCE_MS, LONG_PRESS_MS);
DebouncedButton modeBtn(MODE_PIN, DEBOUNCE_MS, LONG_PRESS_MS);

const char* modeLabel(MeasureMode m) {
  switch (m) {
    case MeasureMode::Precise: return "P";
    case MeasureMode::White:   return "W";
    case MeasureMode::Dark:    return "D";
    default:                   return "S";
  }
}

// White/Dark messen immer genau -- die Referenz ist Grundlage jeder spaeteren
// Reflexionsberechnung, Fehler dort pflanzen sich in jede Messung fort.
Precision precisionFor(MeasureMode m) {
  return (m == MeasureMode::Fast) ? Precision::Fast : Precision::Precise;
}

// White/Dark brauchen nur die Spektral-Ansicht (z.B. um zu sehen, ob die
// Weissreferenz ueber alle Baender gleichmaessig ist) -- die per Mode-Taste
// (kurz) gewaehlte View bleibt gemerkt, wirkt sich aber erst in Fast/Precise
// wieder aus.
DisplayView effectiveView() {
  if (currentMode == MeasureMode::White || currentMode == MeasureMode::Dark) return DisplayView::Spectrum;
  return currentView;
}

// Kopfzeile/Zeilen werden ausschliesslich aus der generischen Spectrum-Struktur
// gebildet (Bandzentren als Spaltennamen, Reflexionswerte als Zellen) -- keine
// Abhaengigkeit zu AS7341-spezifischen Kanalnamen. Fuer den Header genuegt ein
// leerer Rohvektor, da spec.bands unabhaengig von den Messwerten ist.
void printCsvHeader() {
  Spectrum spec = spectrometer.getSpectrum(std::vector<uint32_t>());
  Serial.print("label");
  for (size_t i = 0; i < spec.bands.size(); i++) {
    Serial.print(',');
    Serial.print((int)lroundf(spec.bands[i].center_nm));
    Serial.print("nm");
  }
  Serial.println();
}

void printCsvRow(const char* label, const Spectrum& spec) {
  Serial.print(label);
  for (size_t i = 0; i < spec.values.size(); i++) {
    Serial.print(',');
    Serial.print(spec.values[i], 4);
  }
  Serial.println();
}

void renderCurrentView() {
  if (!displayOk) return;
  ViewContext ctx{ spectrometer, lastRaw, calibrated, modeLabel(currentMode), lastLabel };
  VIEW_RENDERERS[static_cast<uint8_t>(effectiveView())](display, ctx);
}

// Als ProgressCallback an measureRawSpectrum() UEBERGEBBAR (gleiche Signatur)
// und direkt aufrufbar, um den Screen vor der ersten Probe schon zu zeigen.
void showMeasuringScreen(uint8_t current, uint8_t maxEstimate) {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(4, 8);
  display.println("MESSUNG");
  display.setTextSize(1);
  display.setCursor(0, 30);
  display.print("Modus: ");
  display.println(modeLabel(currentMode));

  int barX = 4, barY = 44, barW = OLED_WIDTH - 8, barH = 10;
  display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
  uint8_t capped = (current > maxEstimate) ? maxEstimate : current;
  int fillW = (maxEstimate > 0) ? (int)((uint32_t)capped * (barW - 2) / maxEstimate) : 0;
  if (fillW > 0) display.fillRect(barX + 1, barY + 1, fillW, barH - 2, SSD1306_WHITE);
  display.display();
}

// Kurzes optisches Feedback fuer einen tatsaechlich ausloesenden Tastendruck.
void flashBorder() {
  if (!displayOk) return;
  display.drawRect(0, 0, OLED_WIDTH, OLED_HEIGHT, SSD1306_WHITE);
  display.display();
  delay(80);
}

// Gemeinsamer Einstiegspunkt fuer Serial-Kommandos UND Trigger-Taste.
void performMeasurement(MeasureMode mode) {
  if (measuring) return;  // keine zweite Messung waehrend eine laeuft
  measuring = true;

  flashBorder();  // nur hier, also nur wenn tatsaechlich gestartet wird
  showMeasuringScreen(0, 1);

  Precision prec = precisionFor(mode);
  Serial.print("# messe (");
  Serial.print(prec == Precision::Fast ? "schnell" : "genau");
  Serial.println(")...");

  std::vector<uint32_t> raw = spectrometer.measureRawSpectrum(prec, showMeasuringScreen);
  if (raw.empty()) {
    Serial.println("# measurement failed");
    measuring = false;
    renderCurrentView();
    return;
  }
  lastRaw = raw;

  char lbl[16];
  if (mode == MeasureMode::Dark) {
    strncpy(lbl, "dark", sizeof(lbl));
  } else if (mode == MeasureMode::White) {
    strncpy(lbl, "white", sizeof(lbl));
  } else {
    snprintf(lbl, sizeof(lbl), "sample_%02u", (unsigned)(++sampleCount));
  }
  lbl[sizeof(lbl) - 1] = '\0';
  strncpy(lastLabel, lbl, sizeof(lastLabel));
  lastLabel[sizeof(lastLabel) - 1] = '\0';

  MeasurementRecord rec;
  strncpy(rec.label, lbl, sizeof(rec.label));
  rec.label[sizeof(rec.label) - 1] = '\0';
  rec.mode = mode;
  rec.raw = raw;
  history.push_back(rec);

  if (mode == MeasureMode::Dark)  { darkRef  = raw; calStore.saveDark(darkRef); }
  if (mode == MeasureMode::White) { whiteRef = raw; calStore.saveWhite(whiteRef); }
  if ((mode == MeasureMode::Dark || mode == MeasureMode::White)
      && !darkRef.empty() && !whiteRef.empty()) {
    spectrometer.calibrate(darkRef, whiteRef);
    calibrated = true;
  }

  // Erst NACH einem moeglichen calibrate()-Update auswerten, damit z.B. eine
  // frisch aufgenommene Weissreferenz sich sofort als ~1.0 auf allen Baendern
  // bestaetigt (statt gegen die alte Kalibrierung normiert zu werden).
  printCsvRow(lbl, spectrometer.getSpectrum(raw));

  measuring = false;
  renderCurrentView();
}

void cycleView() {
  uint8_t n = (static_cast<uint8_t>(currentView) + 1) % static_cast<uint8_t>(DisplayView::COUNT);
  currentView = static_cast<DisplayView>(n);
  renderCurrentView();
}

void cycleMode() {
  uint8_t n = (static_cast<uint8_t>(currentMode) + 1) % static_cast<uint8_t>(MeasureMode::COUNT);
  currentMode = static_cast<MeasureMode>(n);

  // Die zuletzt gezeigte Messung gehoert zum vorherigen Modus -- nach einem
  // Moduswechsel soll sie nicht weiter angezeigt werden (Views fallen dann
  // auf ihren "keine Messung"/"nicht kalibriert"-Hinweis zurueck).
  lastRaw.clear();
  lastLabel[0] = '\0';

  renderCurrentView();
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(I2C_HZ);

  triggerBtn.begin();
  modeBtn.begin();

  calStore.begin();
  bool haveDark  = calStore.loadDark(darkRef);
  bool haveWhite = calStore.loadWhite(whiteRef);
  if (haveDark && haveWhite) {
    spectrometer.calibrate(darkRef, whiteRef);
    calibrated = true;
  }

  displayOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (displayOk) {
    display.setRotation(2);  // Display ist 180 Grad verdreht montiert
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Colorimeter");
    display.println(calibrated ? "ready" : "need cal: d/w");
    display.display();  // sofort push -- kein "Schnee"-Frame mehr sichtbar
  } else {
    Serial.println("# SSD1306 nicht gefunden/initialisiert (Adresse 0x3C) - Display bleibt inaktiv");
  }

  if (!sensorImpl.begin()) {
    Serial.println("# AS7341 not found");
    while (true) delay(1000);
  }

  printCsvHeader();
  Serial.println("# bereit. m=messen (aktueller Modus)  d=Dunkelreferenz  w=Weissreferenz  r=Zaehler zuruecksetzen");
}

void loop() {
  if (triggerBtn.poll() == DebouncedButton::Event::Pressed) {
    performMeasurement(currentMode);
  }

  DebouncedButton::Event me = modeBtn.poll();
  if (me == DebouncedButton::Event::ShortRelease) {
    cycleView();
  } else if (me == DebouncedButton::Event::LongPress) {
    cycleMode();
  }

  if (!Serial.available()) return;
  char c = Serial.read();

  switch (c) {
    case 'm': performMeasurement(currentMode);         break;
    case 'd': performMeasurement(MeasureMode::Dark);   break;
    case 'w': performMeasurement(MeasureMode::White);  break;
    case 'r': sampleCount = 0; Serial.println("# Proben-Zaehler zurueckgesetzt"); break;
    case '\n': case '\r': case ' ': break;   // Zeilenenden/Leerzeichen ignorieren
    default:  Serial.print("# unbekannt: "); Serial.println(c);
  }
}
