#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <cstring>
#include <cmath>
#include <string>

#include "AppConfig.h"
#include "Spectrometer.h"
#include "AS7341Spectrometer.h"
#include "CalibrationStore.h"
#include "Buttons.h"
#include "DisplayViews.h"
#include "BleExporter.h"
#include "ColorimetryTables.h"

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
bool busy = false;  // waehrend true: keine weitere Messung/kein weiterer Export ausloesbar

MeasureMode currentMode = MeasureMode::Fast;
DisplayView currentView = DisplayView::ColorInfo;

// ------------------------- Messhistorie (nur RAM, nicht persistiert) -------------------------
// Jede Messung wird hier zusammen mit ihrem Modus gesammelt -- Grundlage fuer
// den Serial- und BLE-Export (buildHistoryCsv()).
struct MeasurementRecord {
  char label[16];
  MeasureMode mode;
  std::vector<uint32_t> raw;
};
std::vector<MeasurementRecord> history;

// ------------------------- BLE-Export -------------------------
BleExporter bleExporter;
bool exportSending = false;
bool exportSentOk  = false;
bool exportHint    = false;  // "kein Handy verbunden"
size_t exportSentBytes = 0, exportTotalBytes = 0;

// ------------------------- Taster -------------------------
DebouncedButton triggerBtn(TRIGGER_PIN, DEBOUNCE_MS, LONG_PRESS_MS);
DebouncedButton modeBtn(MODE_PIN, DEBOUNCE_MS, LONG_PRESS_MS);

// ------------------------- Serial (reiner Datenexport, siehe loop()) -------------------------
bool serialWasConnected = false;

const char* modeLabel(MeasureMode m) {
  switch (m) {
    case MeasureMode::Precise: return "P";
    case MeasureMode::White:   return "W";
    case MeasureMode::Dark:    return "D";
    case MeasureMode::Export:  return "E";
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

// Baut die komplette Messhistorie als CSV (Header + eine Zeile je
// MeasurementRecord), neu berechnet aus den Rohwerten gegen die AKTUELLE
// Kalibrierung -- der Spectrometer bleibt bewusst zustandslos (siehe
// Spectrometer.h): Reflexionswerte werden nie zwischengespeichert, sondern
// bei jedem Abruf frisch aus raw + aktueller Kalibrierung berechnet. Genutzt
// vom einmaligen Serial-Dump bei neu erkanntem Terminal UND vom BLE-Export.
std::string buildHistoryCsv() {
  std::string out;

  // Aktuell gueltige Kalibrierreferenz roh mitschicken -- auch wenn sie nicht
  // in dieser Sitzung neu gemessen, sondern beim Boot aus dem Flash geladen
  // wurde. Ohne das sind die Reflexionswerte aller folgenden Zeilen nicht
  // nachvollziehbar (man kann (raw-dark)/(white-dark) nicht nachrechnen).
  // Bewusst VOR dem eigentlichen Header und mit ALLEN Rohkanaelen (nicht nur
  // den 8 VIS-Baendern) -- stuende das nach dem Header, koennte man die
  // Rohzaehlwerte leicht mit den dort benannten Reflexionsspalten verwechseln.
  auto appendRawRow = [&out](const char* label, const std::vector<uint32_t>& raw) {
    if (raw.empty()) return;  // noch nie kalibriert -- keine Zeile ausgeben
    out += label;
    for (size_t i = 0; i < raw.size(); i++) {
      out += ',';
      out += std::to_string(raw[i]);
    }
    out += '\n';
  };
  appendRawRow("dark_raw", darkRef);
  appendRawRow("white_raw", whiteRef);

  Spectrum headerSpec = spectrometer.getSpectrum(std::vector<uint32_t>());
  out += "label";
  for (size_t i = 0; i < headerSpec.bands.size(); i++) {
    out += ',';
    out += std::to_string((int)lroundf(headerSpec.bands[i].center_nm));
    out += "nm";
  }
  out += ",NIR,L,a,b,hex";
  out += '\n';

  char buf[16];
  for (const MeasurementRecord& rec : history) {
    out += rec.label;
    // Dark/White gegen die eigene (gerade erst gesetzte) Kalibrierung zu
    // normieren waere sinnlos selbstbezueglich (immer 0.0000/1.0000) --
    // fuer diese Zeilen stattdessen dieselbe kalibrierungsfreie
    // Wertebereich-Nutzung wie in der Display-Spektrum-Ansicht zeigen.
    Spectrum spec = (rec.mode == MeasureMode::White || rec.mode == MeasureMode::Dark)
                      ? spectrometer.getRangeUtilization(rec.raw)
                      : spectrometer.getSpectrum(rec.raw);
    for (size_t i = 0; i < spec.values.size(); i++) {
      out += ',';
      snprintf(buf, sizeof(buf), "%.4f", spec.values[i]);
      out += buf;
    }
    snprintf(buf, sizeof(buf), ",%.4f", spec.nir);
    out += buf;

    Lab lab = spectrometer.getColor(rec.raw);
    uint8_t r, g, b;
    labToSRGB255(lab, r, g, b);
    snprintf(buf, sizeof(buf), ",%.2f", lab.L); out += buf;
    snprintf(buf, sizeof(buf), ",%.2f", lab.a); out += buf;
    snprintf(buf, sizeof(buf), ",%.2f", lab.b); out += buf;
    snprintf(buf, sizeof(buf), ",#%02X%02X%02X", r, g, b); out += buf;
    out += '\n';
  }
  return out;
}

// Nimmt 'raw' statt einer schon berechneten Spectrum/Lab entgegen, damit
// diese Zeile (live nach jeder Einzelmessung) und buildHistoryCsv() (voller
// Dump) garantiert dasselbe Spaltenformat erzeugen. 'mode' entscheidet wie
// dort zwischen kalibrierter Reflexion und (fuer White/Dark) kalibrierungs-
// freier Wertebereich-Nutzung.
void printCsvRow(const char* label, MeasureMode mode, const std::vector<uint32_t>& raw) {
  Spectrum spec = (mode == MeasureMode::White || mode == MeasureMode::Dark)
                    ? spectrometer.getRangeUtilization(raw)
                    : spectrometer.getSpectrum(raw);
  Serial.print(label);
  for (size_t i = 0; i < spec.values.size(); i++) {
    Serial.print(',');
    Serial.print(spec.values[i], 4);
  }
  Serial.print(','); Serial.print(spec.nir, 4);

  Lab lab = spectrometer.getColor(raw);
  uint8_t r, g, b;
  labToSRGB255(lab, r, g, b);
  Serial.print(','); Serial.print(lab.L, 2);
  Serial.print(','); Serial.print(lab.a, 2);
  Serial.print(','); Serial.print(lab.b, 2);
  char hex[8];
  snprintf(hex, sizeof(hex), ",#%02X%02X%02X", r, g, b);
  Serial.print(hex);
  Serial.println();
}

// Eigene, einfache Statusanzeige fuer den Export-Modus -- braucht keinen
// Spectrometer, passt deshalb nicht zu DisplayViews.cpp (das Views bewusst
// nur gegen die Sensor-Abstraktion arbeiten laesst).
void renderExportStatus() {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("Export-Modus");

  display.setCursor(0, 14);
  if (!bleExporter.isActive()) {
    display.println("Trigger: BLE an");
  } else if (bleExporter.isConnected()) {
    display.println("BLE: verbunden");
  } else {
    display.println("BLE: warte...");
  }

  char line[24];
  snprintf(line, sizeof(line), "%u Messungen", (unsigned)history.size());
  display.setCursor(0, 26);
  display.println(line);

  if (exportSending) {
    display.setCursor(0, 38);
    display.println("sende...");
    int barX = 4, barY = 48, barW = OLED_WIDTH - 8, barH = 10;
    display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
    int fillW = exportTotalBytes
                  ? (int)((uint64_t)exportSentBytes * (barW - 2) / exportTotalBytes)
                  : 0;
    if (fillW > 0) display.fillRect(barX + 1, barY + 1, fillW, barH - 2, SSD1306_WHITE);
  } else if (exportHint) {
    display.setCursor(0, 40);
    display.println("Kein Handy");
    display.println("verbunden!");
  } else if (exportSentOk) {
    display.setTextSize(2);
    display.setCursor(0, 44);
    display.println("gesendet");
  }

  display.display();
}

void renderCurrentView() {
  if (!displayOk) return;
  if (currentMode == MeasureMode::Export) {
    renderExportStatus();
    return;
  }

  // In White/Dark soll die Spektrum-Ansicht die AKTUELLE Referenz zeigen,
  // auch wenn sie nicht gerade eben (in dieser Sitzung) neu gemessen wurde
  // (z.B. aus dem Flash geladen) -- lastRaw wird bei jedem Moduswechsel
  // geleert und waere hier sonst leer, bis erneut gemessen wird.
  const std::vector<uint32_t>& rawForView =
      (currentMode == MeasureMode::White) ? whiteRef :
      (currentMode == MeasureMode::Dark)  ? darkRef  :
      lastRaw;

  ViewContext ctx{ spectrometer, rawForView, calibrated, modeLabel(currentMode), lastLabel, currentMode };
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

// Fortschritts-Drosselung fuer den BLE-Export: measureRawSpectrum() ruft
// seinen ProgressCallback nur ~8-16x auf, ein BLE-Versand kann dagegen
// 50-200+ Haeppchen brauchen -- ungedrosselt wuerde jedes Haeppchen einen
// vollen I2C-Display-Redraw ausloesen und den Export spuerbar verlangsamen.
void onExportProgress(size_t sent, size_t total) {
  static uint32_t lastRedrawMs = 0;
  uint32_t now = millis();
  if (sent < total && (now - lastRedrawMs) < 150) return;  // ~7 Hz
  lastRedrawMs = now;
  exportSentBytes = sent;
  exportTotalBytes = total;
  renderExportStatus();
}

// Kurzes optisches Feedback fuer einen tatsaechlich ausloesenden Tastendruck.
void flashBorder() {
  if (!displayOk) return;
  display.drawRect(0, 0, OLED_WIDTH, OLED_HEIGHT, SSD1306_WHITE);
  display.display();
  delay(80);
}

void performMeasurement(MeasureMode mode) {
  if (busy) return;  // keine zweite Messung waehrend eine laeuft
  busy = true;

  flashBorder();  // nur hier, also nur wenn tatsaechlich gestartet wird
  showMeasuringScreen(0, 1);

  Precision prec = precisionFor(mode);

  std::vector<uint32_t> raw = spectrometer.measureRawSpectrum(prec, showMeasuringScreen);
  if (raw.empty()) {
    busy = false;
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
  printCsvRow(lbl, mode, raw);

  busy = false;
  renderCurrentView();
}

// Sendet die komplette Messhistorie als CSV per BLE-Notify (Nordic UART
// Service) an ein verbundenes Handy. Nutzt denselben busy-Guard wie
// performMeasurement(), da beide Vorgaenge blockierend sind und sich nicht
// ueberlappen duerfen.
//
// BLE wird bewusst NICHT beim Betreten des Export-Modus gestartet, sondern
// erst hier, beim ersten Trigger-Druck -- so kann man den Modus per Mode-
// Taste "durchklicken", ohne den Funk zwangslaeufig einzuschalten.
void performExport() {
  if (busy) return;

  if (!bleExporter.isActive()) {
    busy = true;
    flashBorder();
    bleExporter.begin(BLE_DEVICE_NAME);
    exportSending = false;
    exportSentOk = false;
    exportHint = false;
    exportSentBytes = exportTotalBytes = 0;
    busy = false;
    renderExportStatus();
    return;  // dieser Druck aktiviert nur BLE, sendet noch nichts
  }

  if (!bleExporter.isConnected()) {
    exportHint = true;
    exportSentOk = false;
    renderExportStatus();
    return;
  }

  busy = true;
  flashBorder();
  exportHint = false;
  exportSentOk = false;
  exportSending = true;
  exportSentBytes = 0;
  exportTotalBytes = 0;
  renderExportStatus();

  std::string csv = buildHistoryCsv();
  bool ok = bleExporter.send(csv, onExportProgress);

  exportSending = false;
  exportSentOk = ok;
  busy = false;
  renderExportStatus();
}

void cycleView() {
  uint8_t n = (static_cast<uint8_t>(currentView) + 1) % static_cast<uint8_t>(DisplayView::COUNT);
  currentView = static_cast<DisplayView>(n);
  renderCurrentView();
}

void cycleMode() {
  MeasureMode previous = currentMode;
  uint8_t n = (static_cast<uint8_t>(currentMode) + 1) % static_cast<uint8_t>(MeasureMode::COUNT);
  currentMode = static_cast<MeasureMode>(n);

  if (previous == MeasureMode::Export && currentMode != MeasureMode::Export) {
    bleExporter.end();  // no-op, falls BLE in diesem Aufenthalt nie aktiviert wurde
  }
  if (currentMode == MeasureMode::Export && previous != MeasureMode::Export) {
    // BLE wird bewusst NICHT hier gestartet, siehe performExport().
    exportSending = false;
    exportSentOk = false;
    exportHint = false;
    exportSentBytes = exportTotalBytes = 0;
  }

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
    // Reine Boot-Diagnose auf einem Fehlerpfad -- vermischt sich nie mit dem
    // eigentlichen CSV-Export (der erst spaeter/live in loop() beginnt).
    Serial.println("# SSD1306 nicht gefunden/initialisiert (Adresse 0x3C) - Display bleibt inaktiv");
  }

  if (!sensorImpl.begin()) {
    Serial.println("# AS7341 not found");
    while (true) delay(1000);
  }
}

void loop() {
  // Serial ist reiner, passiver Datenexport: sobald ein Terminal neu
  // angeschlossen wird (USB-CDC-Verbindungsstatus), einmalig die komplette
  // Historie dumpen. Danach haengt performMeasurement() weiterhin jede neue
  // Messung live als einzelne Zeile an. Diese Pruefung steht bewusst VOR der
  // Tasterabfrage: faellt ein neuer Verbindungsaufbau in denselben
  // loop()-Durchlauf wie ein Trigger-Druck, spiegelt der Dump die Historie
  // VOR dieser Messung, die neue Zeile erscheint danach einmalig live --
  // vermeidet ein sonst moegliches kosmetisches Duplikat.
  bool nowSerialConnected = (bool)Serial;
  if (nowSerialConnected && !serialWasConnected) {
    Serial.print(buildHistoryCsv().c_str());
  }
  serialWasConnected = nowSerialConnected;

  if (triggerBtn.poll() == DebouncedButton::Event::Pressed) {
    if (currentMode == MeasureMode::Export) {
      performExport();
    } else {
      performMeasurement(currentMode);
    }
  }

  DebouncedButton::Event me = modeBtn.poll();
  if (me == DebouncedButton::Event::ShortRelease) {
    cycleView();
  } else if (me == DebouncedButton::Event::LongPress) {
    cycleMode();
  }

  bleExporter.loop();
}
