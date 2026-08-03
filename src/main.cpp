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
Measurement darkRef, whiteRef;
bool calibrated = false;

// ------------------------- letzte Messung / Anzeige-Zustand -------------------------
Measurement lastMeasurement;
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
  Measurement measurement;
};
std::vector<MeasurementRecord> history;

// ------------------------- BLE-Export -------------------------
BleExporter bleExporter;
bool exportSending = false;
bool exportSentOk  = false;
bool exportHint    = false;  // "kein Handy verbunden"
size_t exportSentBytes = 0, exportTotalBytes = 0;

// "Normal" exportiert nur Spectrum/Lab/Hex; "Debug" haengt zusaetzlich das
// rohe Measurement (mit measurementLabels() beschriftet) an jede Zeile an.
// Per kurzem Mode-Druck im Export-Modus umschaltbar (siehe cycleView()).
// Ueber USB wird IMMER die Debug-Variante gesendet, unabhaengig von diesem
// Flag (siehe loop()) -- das Flag steuert nur den BLE-Export.
bool includeRawValues = false;

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

// Haengt eine einzelne CSV-Zeile an 'out' an. Gemeinsam genutzt von
// buildHistoryCsv() (voller Dump) und printCsvRow() (Live-Zeile), damit beide
// garantiert dasselbe Format erzeugen.
//
// computeSpectrum=false (fuer Dark/White-Referenzen und die weiter unten
// angehaengten "*_ref"-Zeilen): es gibt keine sinnvolle "Reflexion einer
// Referenz gegen sich selbst" mehr, seit getSpectrum() explizite Referenzen
// statt einer gespeicherten Kalibrierung nimmt -- die Spectrum/Lab/Hex-Spalten
// bleiben dann leer (aber vorhanden, gleiche Spaltenzahl wie jede andere
// Zeile). Das eigentliche Ergebnis dieser Referenz-Zeilen sind ihre rohen
// Measurement-Werte (nBandCols dient nur dazu, in diesem Fall die richtige
// Anzahl Leerspalten auszugeben).
void appendCsvRow(std::string& out, const char* label, const Measurement& measurement,
                   bool computeSpectrum, bool includeRaw, size_t nBandCols) {
  out += label;
  char buf[16];

  if (computeSpectrum) {
    Spectrum spec = spectrometer.getSpectrum(measurement, whiteRef, darkRef);
    for (size_t i = 0; i < spec.values.size(); i++) {
      out += ',';
      snprintf(buf, sizeof(buf), "%.4f", spec.values[i]);
      out += buf;
    }
    Lab lab = getColor(spec);
    uint8_t r, g, b;
    labToSRGB255(lab, r, g, b);
    snprintf(buf, sizeof(buf), ",%.2f", lab.L); out += buf;
    snprintf(buf, sizeof(buf), ",%.2f", lab.a); out += buf;
    snprintf(buf, sizeof(buf), ",%.2f", lab.b); out += buf;
    snprintf(buf, sizeof(buf), ",#%02X%02X%02X", r, g, b); out += buf;
  } else {
    for (size_t i = 0; i < nBandCols; i++) out += ',';
    out += ",,,,";
  }

  if (includeRaw) {
    for (size_t i = 0; i < measurement.size(); i++) {
      out += ',';
      snprintf(buf, sizeof(buf), "%.1f", measurement[i]);
      out += buf;
    }
  }
  out += '\n';
}

// Baut die komplette Messhistorie als CSV. includeRaw haengt zusaetzlich die
// rohen Measurement-Spalten an (Spaltennamen aus measurementLabels() --
// einzige Stelle im Code, die diese Labels benutzt). Die aktuell gueltige
// Kalibrierreferenz wird (nur im Debug/Raw-Modus, sonst gaebe es nichts
// Sinnvolles zu zeigen) immer als eigene Zeile mitgeschickt, auch wenn sie
// nicht in dieser Sitzung neu gemessen, sondern aus dem Flash geladen wurde.
std::string buildHistoryCsv(bool includeRaw) {
  std::string out;

  Spectrum headerSpec = spectrometer.getSpectrum(Measurement(), Measurement(), Measurement());
  size_t nBandCols = headerSpec.bands.size();

  out += "label";
  for (size_t i = 0; i < nBandCols; i++) {
    out += ',';
    out += std::to_string((int)lroundf(headerSpec.bands[i].center_nm));
    out += "nm";
  }
  out += ",L,a,b,hex";
  if (includeRaw) {
    const char* const* labels = spectrometer.measurementLabels();
    size_t n = !darkRef.empty()  ? darkRef.size()
             : !whiteRef.empty() ? whiteRef.size()
             : !history.empty() ? history[0].measurement.size()
                                 : 0;
    for (size_t i = 0; i < n; i++) { out += ','; out += labels[i]; }
  }
  out += '\n';

  if (includeRaw) {
    if (!darkRef.empty())  appendCsvRow(out, "dark_ref",  darkRef,  false, true, nBandCols);
    if (!whiteRef.empty()) appendCsvRow(out, "white_ref", whiteRef, false, true, nBandCols);
  }

  for (const MeasurementRecord& rec : history) {
    bool isRef = (rec.mode == MeasureMode::Dark || rec.mode == MeasureMode::White);
    appendCsvRow(out, rec.label, rec.measurement, !isRef, includeRaw, nBandCols);
  }
  return out;
}

// Live-Zeile nach jeder Einzelmessung. Nutzt denselben appendCsvRow() wie der
// volle Dump, ueber USB IMMER mit Rohwerten (siehe includeRawValues-Kommentar
// weiter oben).
void printCsvRow(const char* label, MeasureMode mode, const Measurement& measurement) {
  bool isRef = (mode == MeasureMode::Dark || mode == MeasureMode::White);
  Spectrum headerSpec = spectrometer.getSpectrum(Measurement(), Measurement(), Measurement());
  std::string row;
  appendCsvRow(row, label, measurement, !isRef, /*includeRaw=*/true, headerSpec.bands.size());
  Serial.print(row.c_str());
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

  display.setCursor(0, 10);
  display.println(includeRawValues ? "Modus: Debug" : "Modus: Normal");

  display.setCursor(0, 20);
  if (!bleExporter.isActive()) {
    display.println("Trigger: BLE an");
  } else if (bleExporter.isConnected()) {
    display.println("BLE: verbunden");
  } else {
    display.println("BLE: warte...");
  }

  char line[24];
  snprintf(line, sizeof(line), "%u Messungen", (unsigned)history.size());
  display.setCursor(0, 30);
  display.println(line);

  if (exportSending) {
    display.setCursor(0, 40);
    display.println("sende...");
    int barX = 4, barY = 50, barW = OLED_WIDTH - 8, barH = 10;
    display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
    int fillW = exportTotalBytes
                  ? (int)((uint64_t)exportSentBytes * (barW - 2) / exportTotalBytes)
                  : 0;
    if (fillW > 0) display.fillRect(barX + 1, barY + 1, fillW, barH - 2, SSD1306_WHITE);
  } else if (exportHint) {
    display.setCursor(0, 42);
    display.println("Kein Handy");
    display.println("verbunden!");
  } else if (exportSentOk) {
    display.setTextSize(2);
    display.setCursor(0, 46);
    display.println("gesendet");
  }

  display.display();
}

// Eigener Screen fuer White/Dark: zeigt die aktuelle Referenz als rohes
// Measurement (ueber measurementLabels() beschriftet) statt sie als
// kalibrierte Reflexion darzustellen -- seit getSpectrum() explizite
// Referenzen statt einer gespeicherten Kalibrierung nimmt, gibt es fuer "die
// Referenz gegen sich selbst" keine sinnvolle Reflexion mehr (siehe
// appendCsvRow()-Kommentar). Zeigt bewusst IMMER die aktuell guenstige
// Referenz (auch aus dem Flash geladen, nicht nur frisch gemessen).
void renderReferenceStatus() {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  bool isWhite = (currentMode == MeasureMode::White);
  display.setCursor(0, 0);
  display.println(isWhite ? "Weiss-Referenz" : "Dunkel-Referenz");

  const Measurement& ref = isWhite ? whiteRef : darkRef;
  if (ref.empty()) {
    display.setCursor(0, 16);
    display.println("keine Messung");
    display.println("Trigger druecken");
  } else {
    const char* const* labels = spectrometer.measurementLabels();
    char line[27];
    int y = 16;
    size_t i = 0;
    for (; i + 1 < ref.size(); i += 2) {
      snprintf(line, sizeof(line), "%-4.4s %5.0f %-4.4s %5.0f",
               labels[i], ref[i], labels[i + 1], ref[i + 1]);
      display.setCursor(0, y);
      display.print(line);
      y += 8;
    }
    if (i < ref.size()) {
      snprintf(line, sizeof(line), "%-4.4s %5.0f", labels[i], ref[i]);
      display.setCursor(0, y);
      display.print(line);
    }
  }

  display.display();
}

void renderCurrentView() {
  if (!displayOk) return;
  if (currentMode == MeasureMode::Export) {
    renderExportStatus();
    return;
  }
  if (currentMode == MeasureMode::White || currentMode == MeasureMode::Dark) {
    renderReferenceStatus();
    return;
  }

  ViewContext ctx{ spectrometer, lastMeasurement, whiteRef, darkRef, calibrated, modeLabel(currentMode), lastLabel };
  VIEW_RENDERERS[static_cast<uint8_t>(currentView)](display, ctx);
}

// Als ProgressCallback an performMeasurement() UEBERGEBBAR (gleiche Signatur)
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

// Fortschritts-Drosselung fuer den BLE-Export: performMeasurement() ruft
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

// Gemeinsamer Einstiegspunkt fuer den Trigger-Taster in allen Mess-Modi.
void performMeasurement(MeasureMode mode) {
  if (busy) return;  // keine zweite Messung waehrend eine laeuft
  busy = true;

  flashBorder();  // nur hier, also nur wenn tatsaechlich gestartet wird
  showMeasuringScreen(0, 1);

  Precision prec = precisionFor(mode);
  Measurement measurement = spectrometer.performMeasurement(prec, showMeasuringScreen);
  if (measurement.empty()) {
    busy = false;
    renderCurrentView();
    return;
  }
  lastMeasurement = measurement;

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
  rec.measurement = measurement;
  history.push_back(rec);

  if (mode == MeasureMode::Dark)  { darkRef  = measurement; calStore.saveDark(darkRef); }
  if (mode == MeasureMode::White) { whiteRef = measurement; calStore.saveWhite(whiteRef); }
  calibrated = !darkRef.empty() && !whiteRef.empty();

  printCsvRow(lbl, mode, measurement);

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

  std::string csv = buildHistoryCsv(includeRawValues);
  bool ok = bleExporter.send(csv, onExportProgress);

  exportSending = false;
  exportSentOk = ok;
  busy = false;
  renderExportStatus();
}

// Kurzer Mode-Druck: in Fast/Precise/Export-Modus View bzw. Export-Variante
// wechseln. In White/Dark gibt es (seit dem eigenen Referenz-Screen) nichts
// zum Umschalten -- Aufruf bleibt dort ein harmloses No-op.
void cycleView() {
  if (currentMode == MeasureMode::Export) {
    includeRawValues = !includeRawValues;
    renderCurrentView();
    return;
  }
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
  lastMeasurement.clear();
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
  calibrated = haveDark && haveWhite;

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
  // Historie dumpen (IMMER mit Rohwerten, unabhaengig vom BLE-Umschalt-Flag).
  // Danach haengt performMeasurement() weiterhin jede neue Messung live als
  // einzelne Zeile an. Diese Pruefung steht bewusst VOR der Tasterabfrage:
  // faellt ein neuer Verbindungsaufbau in denselben loop()-Durchlauf wie ein
  // Trigger-Druck, spiegelt der Dump die Historie VOR dieser Messung, die
  // neue Zeile erscheint danach einmalig live -- vermeidet ein sonst
  // moegliches kosmetisches Duplikat.
  bool nowSerialConnected = (bool)Serial;
  if (nowSerialConnected && !serialWasConnected) {
    Serial.print(buildHistoryCsv(/*includeRaw=*/true).c_str());
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
