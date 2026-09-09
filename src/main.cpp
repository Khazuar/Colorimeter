#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <cstring>
#include <cmath>
#include <string>
#include <algorithm>

#include "AppConfig.h"
#include "Spectrometer.h"
#include "AS7341Spectrometer.h"
#include "CalibrationStore.h"
#include "Buttons.h"
#include "DisplayViews.h"
#include "BleExporter.h"
#include "ColorimetryTables.h"
#include "UptimeLogger.h"
#include "HistoryStore.h"
#include "DigitEditor.h"

// ------------------------- Display -------------------------
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
bool displayOk = false;

// ------------------------- Sensor (nur ueber die Abstraktion nutzen) -------------------------
AS7341Spectrometer sensorImpl;
Spectrometer& spectrometer = sensorImpl;

// ------------------------- Kalibrierung: Sache der Orchestrierung -------------------------
CalibrationStore calStore;
Measurement darkRef, whiteRef;
AcquisitionSettings darkRefSettings;   // eingefroren MIT darkRef, siehe CalibrationStore
AcquisitionSettings whiteRefSettings;  // eingefroren MIT whiteRef
bool calibrated = false;  // "Referenz passt zu den AKTUELL gewaehlten Einstellungen" -- siehe calibrationValidFor()

// Aktuell im Settings-Modus gewaehlte Einstellungen (siehe DisplayMode::Settings).
AcquisitionSettings currentSettings;

// Liefert true, wenn sowohl Dark- als auch White-Referenz vorhanden sind UND
// beide unter GENAU dem angegebenen Einstellungs-Buendel (Filter+Gain+ATIME+
// ASTEP) aufgenommen wurden. Zentrale Stelle fuer die Regel "weicht auch nur
// eine Einstellung ab, gilt die Referenz als nicht vorhanden" -- wird fuer die
// Live-Anzeige (gegen lastMeasurementSettings), den globalen "ready/need cal"-
// Status (gegen currentSettings) UND den CSV-Export (gegen rec.settings jeder
// Zeile) gleichermassen benutzt.
bool calibrationValidFor(const AcquisitionSettings& s) {
  return !darkRef.empty() && !whiteRef.empty()
      && darkRefSettings == s && whiteRefSettings == s;
}

// ------------------------- Betriebszeit-Logging (dedizierte NVS-Partition) -------------------------
UptimeLogger uptimeLogger;

// ------------------------- letzte Messung / Anzeige-Zustand -------------------------
Measurement lastMeasurement;
char lastLabel[16] = "";
// Einstellungen, die zum Zeitpunkt VON lastMeasurement tatsaechlich aktiv
// waren -- eingefroren, NICHT die live im Settings-Modus editierbaren
// currentSettings (siehe renderCurrentView() fuer die Begruendung dieser
// Asymmetrie).
AcquisitionSettings lastMeasurementSettings;
bool busy = false;  // waehrend true: keine weitere Messung/kein weiterer Export ausloesbar

DisplayMode currentDisplayMode = DisplayMode::Fast;
DisplayView currentView = DisplayView::ColorInfo;

// Innerhalb des Calibration-DisplayMode per kurzem Mode-Druck gewaehlte
// Referenz -- langer Trigger-Druck misst dann genau diese. Bewusst ein
// eigenes, main.cpp-lokales Enum statt SampleKind mitzubenutzen: das eine ist
// fluechtiger UI-Zustand ("welche Seite sehe ich gerade"), das andere ein
// Datenmodell-Konzept ("was ist das fuer ein gespeicherter Messwert") --
// obwohl beide White/Dark kennen, sind es unterschiedliche Fragen.
enum class CalibrationTarget : uint8_t { White = 0, Dark = 1 };
CalibrationTarget calibrationTarget = CalibrationTarget::White;

// ------------------------- Messhistorie (persistiert, siehe HistoryStore) -------------------------
// Jede Messung wird hier zusammen mit ihrem Modus gesammelt -- Grundlage fuer
// den Serial- und BLE-Export (buildHistoryCsv()). Ueberlebt Neustarts/
// Stromausfaelle (LittleFS auf der "spiffs"-Partition), siehe HistoryStore.h.
HistoryStore historyStore;

// ------------------------- BLE-Export -------------------------
BleExporter bleExporter;
bool exportSending = false;
bool exportSentOk  = false;
bool exportHint    = false;  // "kein Handy verbunden"
size_t exportSentBytes = 0, exportTotalBytes = 0;

// Der Export-Modus hat drei per kurzem Mode-Druck durchschaltbare "Seiten"
// (siehe cycleView()): Normal/Debug steuern wie bisher, ob der BLE-Export
// zusaetzlich das rohe Measurement (mit measurementLabels() beschriftet) an
// jede Zeile anhaengt -- ueber USB wird IMMER die Debug-Variante gesendet,
// unabhaengig davon (siehe loop()). Clear ist eine eigene Seite zum Loeschen
// der persistierten Historie (siehe renderExportClear() / loop()-Trigger-
// Dispatch) -- bewusst per LANGEM statt kurzem Trigger-Druck ausgeloest.
enum class ExportPage : uint8_t { Normal = 0, Debug = 1, Clear = 2, COUNT = 3 };
ExportPage exportPage = ExportPage::Normal;

// ------------------------- Settings-Modus -------------------------
// Kurzer Mode-Druck waehlt (ausserhalb einer Bearbeitung), WELCHE Einstellung
// angezeigt wird. Jede Einstellung wird ueber denselben DigitEditor bedient
// (siehe DigitEditor.h): langer Trigger-Druck startet die Bearbeitung, kurzer
// Trigger-Druck erhoeht die aktuelle Ziffer, kurzer Mode-Druck verringert sie,
// langer Trigger/Mode-Druck wechselt zur naechsten/vorigen Ziffer -- am
// jeweiligen Ende (letzte Ziffer vorwaerts bzw. erste rueckwaerts) wird der
// zusammengesetzte Wert uebernommen (siehe loop()). Enum-artige Einstellungen
// (Filter, Gain) sind dabei "einstellige Zahlen": digitCount=1, die Ziffer
// ist der Options-Index.
const char* filterStateUiLabel(FilterState fs) {
  switch (fs) {
    case FilterState::Filter650nm: return "650nm";
    case FilterState::Filter700nm: return "700nm";
    default:                       return "kein Filter";
  }
}
const char* filterStateCsvLabel(FilterState fs) {
  switch (fs) {
    case FilterState::Filter650nm: return "650nm";
    case FilterState::Filter700nm: return "700nm";
    default:                       return "none";
  }
}

// Reihenfolge == as7341_gain_t (siehe Adafruit_AS7341.h), verifiziert.
static const char* const GAIN_LABELS[AS7341_GAIN_COUNT] = {
  "0.5X", "1X", "2X", "4X", "8X", "16X", "32X", "64X", "128X", "256X", "512X"
};
const char* gainCsvLabel(as7341_gain_t g) {
  uint8_t i = static_cast<uint8_t>(g);
  return (i < AS7341_GAIN_COUNT) ? GAIN_LABELS[i] : "?";
}

struct SettingDescriptor {
  const char* name;
  uint8_t digitCount;                             // 1 = enum-artig (Filter, Gain)
  uint8_t digitCycleLen[DigitEditor::MAX_DIGITS];  // Zyklus-Laenge je Ziffernposition
  uint32_t maxValue;                               // Clamp des Endwerts (siehe DigitEditor::assembledValue())
  const char* (*valueLabel)(uint32_t value);       // nur bei digitCount==1, sonst nullptr (Ziffern direkt gerendert)
  uint32_t (*getValue)();
  void (*setValue)(uint32_t value);                // einmalig beim Abschluss der Bearbeitung aufgerufen
};

// Gemeinsamer Abschluss fuer jede Einstellungsaenderung: persistieren +
// calibrated neu bewerten (kann durch eine reine Einstellungsaenderung sofort
// kippen, ganz ohne neue Messung).
void commitCurrentSettings() {
  calStore.saveSettings(currentSettings);
  calibrated = calibrationValidFor(currentSettings);
}

const char* filterSettingLabel(uint32_t v) { return filterStateUiLabel(static_cast<FilterState>(v)); }
uint32_t getFilterSetting() { return static_cast<uint32_t>(currentSettings.filterState); }
void setFilterSetting(uint32_t v) {
  currentSettings.filterState = static_cast<FilterState>(v);
  commitCurrentSettings();
}

const char* gainSettingLabel(uint32_t v) { return (v < AS7341_GAIN_COUNT) ? GAIN_LABELS[v] : "?"; }
uint32_t getGainSetting() { return static_cast<uint32_t>(currentSettings.gain); }
void setGainSetting(uint32_t v) {
  currentSettings.gain = static_cast<as7341_gain_t>(v);
  sensorImpl.applySettings(currentSettings);
  commitCurrentSettings();
}

uint32_t getATimeSetting() { return currentSettings.atime; }
void setATimeSetting(uint32_t v) {
  currentSettings.atime = static_cast<uint8_t>(v);
  sensorImpl.applySettings(currentSettings);
  commitCurrentSettings();
}

uint32_t getAStepSetting() { return currentSettings.astep; }
void setAStepSetting(uint32_t v) {
  currentSettings.astep = static_cast<uint16_t>(v);
  sensorImpl.applySettings(currentSettings);
  commitCurrentSettings();
}

const SettingDescriptor SETTINGS[] = {
  { "Filter", 1, {3},           2,     filterSettingLabel, getFilterSetting, setFilterSetting },
  { "Gain",   1, {AS7341_GAIN_COUNT}, AS7341_GAIN_COUNT - 1, gainSettingLabel, getGainSetting, setGainSetting },
  // ATIME (uint8_t, max 255): 3 Dezimalstellen, fuehrende Ziffer 0-2.
  { "ATIME",  3, {3, 10, 10},   255,   nullptr, getATimeSetting, setATimeSetting },
  // ASTEP (uint16_t, max 65535): 5 Dezimalstellen, fuehrende Ziffer 0-6.
  { "ASTEP",  5, {7, 10, 10, 10, 10}, 65535, nullptr, getAStepSetting, setAStepSetting },
};
const uint8_t SETTINGS_COUNT = sizeof(SETTINGS) / sizeof(SETTINGS[0]);
uint8_t currentSettingIndex = 0;

// Bearbeitungszustand: solange editingActive, hijacken Trigger/Mode ihre
// sonstige Bedeutung (Messen/Moduswechsel) zugunsten der Ziffernbearbeitung
// -- siehe loop().
bool editingActive = false;
DigitEditor editor;

// ------------------------- Taster -------------------------
DebouncedButton triggerBtn(TRIGGER_PIN, DEBOUNCE_MS, LONG_PRESS_MS);
DebouncedButton modeBtn(MODE_PIN, DEBOUNCE_MS, LONG_PRESS_MS);

// ------------------------- Serial (reiner Datenexport, siehe loop()) -------------------------
bool serialWasConnected = false;

// Periodischer Auto-Refresh des Info-Screens (siehe loop()/renderInfoStatus()).
uint32_t lastInfoRenderMs = 0;

const char* displayModeLabel(DisplayMode m) {
  switch (m) {
    case DisplayMode::Precise:     return "P";
    case DisplayMode::Calibration: return "K";  // in der Praxis nie direkt angezeigt, siehe activeModeLabel()
    case DisplayMode::Export:      return "E";
    case DisplayMode::Settings:    return "C";
    case DisplayMode::Info:        return "I";
    default:                       return "S";  // Fast
  }
}
const char* calibrationTargetLabel(CalibrationTarget t) {
  return (t == CalibrationTarget::White) ? "W" : "D";
}

// Liefert das fuer die aktuelle Anzeige (z.B. "MESSUNG"-Screen) passende
// Modus-Kuerzel -- im Calibration-Modus das der aktuell gewaehlten Referenz
// (White/Dark), sonst das des DisplayMode selbst. Noetig, weil
// showMeasuringScreen() als ProgressCallback eine feste Signatur hat und
// daher nicht direkt wissen kann, welche Referenz calibrationTarget gerade meint.
const char* activeModeLabel() {
  return (currentDisplayMode == DisplayMode::Calibration)
      ? calibrationTargetLabel(calibrationTarget)
      : displayModeLabel(currentDisplayMode);
}

// Haengt eine einzelne CSV-Zeile an 'out' an. Gemeinsam genutzt von
// buildHistoryCsv() (voller Dump) und printCsvRow() (Live-Zeile), damit beide
// garantiert dasselbe Format erzeugen.
//
// Kontext-Spalten (Temperatur/Laufzeit/Betriebszeit/Einstellungen) fuer eine
// CSV-Zeile -- siehe MeasurementRecord-Kommentar in HistoryStore.h. ctx==nullptr
// fuer die "*_ref"-Zeilen weiter unten (die zeigen die AKTUELL geladene
// Kalibrierung, nicht ein konkretes Messereignis -- fuer sie gibt es keinen
// sinnvollen Zeitpunkt/Temperatur/Einstellungsstand, die Spalten bleiben dort leer).
struct MeasurementContext {
  float tempC;
  uint32_t sessionMs;
  uint32_t uptimeS;
  AcquisitionSettings settings;
};

// "674/45nm" -- Center/FWHM, angelehnt an uebliche Bandpassfilter-Notation
// (z.B. "680/52 BrightLine"). Noetig, weil verschiedene FilterStates
// unterschiedliche Baender liefern -- ein reiner "674nm"-Name waere nicht
// mehr eindeutig genug.
std::string bandColumnName(const Band& b) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%d/%dnm", (int)lroundf(b.center_nm), (int)lroundf(b.fwhm_nm));
  return buf;
}

// computeSpectrum=false (fuer Dark/White-Referenzen, die weiter unten
// angehaengten "*_ref"-Zeilen, ODER eine Messung, deren Filter nicht zur
// aktuell geladenen Kalibrierung passt -- siehe calibrationValidFor()): es
// gibt keine sinnvolle abgeleitete Reflexion -- die Spectrum/Lab/Hex-Spalten
// bleiben dann leer (aber vorhanden, `bandColumns.size()` Leerspalten). Die
// rohen Measurement-Werte (falls includeRaw) bleiben davon unberuehrt.
void appendCsvRow(std::string& out, const char* label, const Measurement& measurement,
                   FilterState filterState, bool computeSpectrum, bool includeRaw,
                   const std::vector<Band>& bandColumns,
                   const MeasurementContext* ctx = nullptr) {
  out += label;
  char buf[16];

  if (ctx) {
    snprintf(buf, sizeof(buf), ",%.1f", ctx->tempC); out += buf;
    snprintf(buf, sizeof(buf), ",%.1f", ctx->sessionMs / 1000.0f); out += buf;
    snprintf(buf, sizeof(buf), ",%lu", (unsigned long)ctx->uptimeS); out += buf;
    out += ',';
    out += filterStateCsvLabel(ctx->settings.filterState);
    out += ',';
    out += gainCsvLabel(ctx->settings.gain);
    snprintf(buf, sizeof(buf), ",%u", ctx->settings.atime); out += buf;
    snprintf(buf, sizeof(buf), ",%u", ctx->settings.astep); out += buf;
  } else {
    out += ",,,,,,,";
  }

  if (computeSpectrum) {
    Spectrum spec = spectrometer.getSpectrum(measurement, whiteRef, darkRef, filterState);
    for (const Band& col : bandColumns) {
      out += ',';
      int idx = -1;
      for (size_t k = 0; k < spec.bands.size(); k++) {
        if (spec.bands[k].center_nm == col.center_nm && spec.bands[k].fwhm_nm == col.fwhm_nm) { idx = (int)k; break; }
      }
      if (idx >= 0) { snprintf(buf, sizeof(buf), "%.4f", spec.values[idx]); out += buf; }
    }
    Lab lab = getColor(spec);
    uint8_t r, g, b;
    labToSRGB255(lab, r, g, b);
    snprintf(buf, sizeof(buf), ",%.2f", lab.L); out += buf;
    snprintf(buf, sizeof(buf), ",%.2f", lab.a); out += buf;
    snprintf(buf, sizeof(buf), ",%.2f", lab.b); out += buf;
    snprintf(buf, sizeof(buf), ",#%02X%02X%02X", r, g, b); out += buf;
  } else {
    for (size_t i = 0; i < bandColumns.size(); i++) out += ',';
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

// HistoryStore::forEach()-Visitor: sammelt die Vereinigungsmenge aller
// vorkommenden Baender (nur von Datensaetzen, deren Filter zur aktuell
// geladenen Kalibrierung passt -- alle anderen bekommen ohnehin keine echten
// Spectrum-Spalten, ihre Baender "verdienen" also keine Kopfzeilen-Spalte).
struct BandCollectCtx {
  std::vector<Band>* cols;
};
void collectBandsVisitor(const MeasurementRecord& rec, void* userData) {
  BandCollectCtx* c = reinterpret_cast<BandCollectCtx*>(userData);
  bool isRef = (rec.kind != SampleKind::Regular);
  if (isRef || !calibrationValidFor(rec.settings)) return;
  Spectrum spec = spectrometer.getSpectrum(rec.measurement, whiteRef, darkRef, rec.settings.filterState);
  for (const Band& b : spec.bands) {
    bool known = false;
    for (const Band& existing : *c->cols) {
      if (existing.center_nm == b.center_nm && existing.fwhm_nm == b.fwhm_nm) { known = true; break; }
    }
    if (!known) c->cols->push_back(b);
  }
}

// HistoryStore::forEach()-Visitor: haengt einen persistierten Datensatz per
// appendCsvRow() an 'out' an -- appendCsvRow() bleibt dabei unveraendert und
// berechnet Spectrum/Lab/Hex weiterhin frisch gegen die AKTUELL geladene
// Kalibrierung, nicht gegen eine zum Messzeitpunkt eingefrorene. Zeilen, deren
// Filter nicht zur Kalibrierung passt, bekommen keine abgeleiteten Spalten
// (computeSpectrum=false), behalten aber ihre Rohwerte.
struct CsvBuildCtx {
  std::string* out;
  bool includeRaw;
  const std::vector<Band>* bandColumns;
};
void appendRecordToCsv(const MeasurementRecord& rec, void* userData) {
  CsvBuildCtx* ctx = reinterpret_cast<CsvBuildCtx*>(userData);
  bool isRef = (rec.kind != SampleKind::Regular);
  bool computeSpectrum = !isRef && calibrationValidFor(rec.settings);
  MeasurementContext mctx{ rec.tempC, rec.sessionMs, rec.uptimeS, rec.settings };
  appendCsvRow(*ctx->out, rec.label, rec.measurement, rec.settings.filterState, computeSpectrum,
               ctx->includeRaw, *ctx->bandColumns, &mctx);
}

// Baut die komplette Messhistorie als CSV. includeRaw haengt zusaetzlich die
// rohen Measurement-Spalten an (Spaltennamen aus measurementLabels() --
// einzige Stelle im Code, die diese Labels benutzt). Die aktuell gueltige
// Kalibrierreferenz wird (nur im Debug/Raw-Modus, sonst gaebe es nichts
// Sinnvolles zu zeigen) immer als eigene Zeile mitgeschickt, auch wenn sie
// nicht in dieser Sitzung neu gemessen, sondern aus dem Flash geladen wurde.
//
// Zwei Durchlaeufe durch die Historie: der erste bestimmt die Vereinigungsmenge
// aller vorkommenden Baender (unterschiedliche FilterStates koennen strukturell
// unterschiedliche Baender liefern -- keine feste Spaltenzahl mehr moeglich),
// der zweite gibt die eigentlichen Zeilen aus.
std::string buildHistoryCsv(bool includeRaw) {
  std::string out;

  std::vector<Band> bandColumns;
  BandCollectCtx collectCtx{ &bandColumns };
  historyStore.forEach(collectBandsVisitor, &collectCtx);
  std::sort(bandColumns.begin(), bandColumns.end(), [](const Band& a, const Band& b) {
    return a.center_nm < b.center_nm;
  });

  out += "label,temp_c,session_s,uptime_s,filter,gain,atime,astep";
  for (const Band& b : bandColumns) {
    out += ',';
    out += bandColumnName(b);
  }
  out += ",L,a,b,hex";
  if (includeRaw) {
    const char* const* labels = spectrometer.measurementLabels();
    size_t n = !darkRef.empty()  ? darkRef.size()
             : !whiteRef.empty() ? whiteRef.size()
                                 : historyStore.firstRecordChannelCount();
    for (size_t i = 0; i < n; i++) { out += ','; out += labels[i]; }
  }
  out += '\n';

  // Kumulierte Betriebszeit (Alterungs-Tracking der durchgehend an bleibenden
  // Beleuchtungs-LED) -- immer mitgeschickt, auch im "Normal"-Export, da es
  // nur ein einzelner Wert ist, keine Kalibrierung/Reflexion voraussetzt.
  char uptimeLine[32];
  snprintf(uptimeLine, sizeof(uptimeLine), "uptime_seconds,%lu\n",
           (unsigned long)uptimeLogger.totalSeconds());
  out += uptimeLine;
  snprintf(uptimeLine, sizeof(uptimeLine), "measurement_count,%lu\n",
           (unsigned long)uptimeLogger.measurementCount());
  out += uptimeLine;

  if (includeRaw) {
    if (!darkRef.empty())  appendCsvRow(out, "dark_ref",  darkRef,  FilterState::None, false, true, bandColumns);
    if (!whiteRef.empty()) appendCsvRow(out, "white_ref", whiteRef, FilterState::None, false, true, bandColumns);
  }

  CsvBuildCtx ctx{ &out, includeRaw, &bandColumns };
  historyStore.forEach(appendRecordToCsv, &ctx);
  return out;
}

// Live-Zeile nach jeder Einzelmessung. Nutzt denselben appendCsvRow() wie der
// volle Dump, ueber USB IMMER mit Rohwerten (siehe ExportPage-Kommentar weiter
// oben). Braucht KEINE Spalten-Vereinigungsmenge (nur eine Zeile, nichts womit
// sie sich abgleichen muesste) -- nutzt einfach die eigenen Baender. Kann
// daher, je nach Filter-Status/Kalibrierungs-Uebereinstimmung, unterschiedlich
// viele/benannte Spalten haben als andere Zeilen -- unausweichliche Folge der
// Filter-Abhaengigkeit, kein Bug.
void printCsvRow(const MeasurementRecord& rec) {
  bool isRef = (rec.kind != SampleKind::Regular);
  bool computeSpectrum = !isRef && calibrationValidFor(rec.settings);
  std::vector<Band> bandColumns;
  if (computeSpectrum) {
    bandColumns = spectrometer.getSpectrum(rec.measurement, whiteRef, darkRef, rec.settings.filterState).bands;
  }
  std::string row;
  MeasurementContext ctx{ rec.tempC, rec.sessionMs, rec.uptimeS, rec.settings };
  appendCsvRow(row, rec.label, rec.measurement, rec.settings.filterState, computeSpectrum,
               /*includeRaw=*/true, bandColumns, &ctx);
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
  display.println(exportPage == ExportPage::Debug ? "Modus: Debug" : "Modus: Normal");

  display.setCursor(0, 20);
  if (!bleExporter.isActive()) {
    display.println("Trigger: BLE an");
  } else if (bleExporter.isConnected()) {
    display.println("BLE: verbunden");
  } else {
    display.println("BLE: warte...");
  }

  char line[24];
  snprintf(line, sizeof(line), "%u Messungen", (unsigned)historyStore.count());
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

// Gemeinsamer Screen fuer den Calibration-Modus (Weiss- ODER Dunkelreferenz,
// je nach calibrationTarget -- kurzer Mode-Druck wechselt dazwischen): zeigt
// die gewaehlte Referenz als rohes Measurement (ueber measurementLabels()
// beschriftet) statt sie als kalibrierte Reflexion darzustellen -- seit
// getSpectrum() explizite Referenzen statt einer gespeicherten Kalibrierung
// nimmt, gibt es fuer "die Referenz gegen sich selbst" keine sinnvolle
// Reflexion mehr (siehe appendCsvRow()-Kommentar).
//
// Passt der Filter der gespeicherten Referenz NICHT zum aktuell gewaehlten
// (siehe calibrationValidFor()), gilt sie hier als nicht vorhanden -- exakt
// dasselbe Verhalten wie beim Messen/Exportieren, keine Sonderbehandlung
// (kein Warnhinweis, sie wird schlicht nicht angezeigt).
void renderReferenceStatus() {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  bool isWhite = (calibrationTarget == CalibrationTarget::White);
  display.setCursor(0, 0);
  display.println(isWhite ? "Weiss-Referenz" : "Dunkel-Referenz");

  const Measurement& ref = isWhite ? whiteRef : darkRef;
  const AcquisitionSettings& refSettings = isWhite ? whiteRefSettings : darkRefSettings;
  bool refValid = !ref.empty() && (refSettings == currentSettings);

  if (!refValid) {
    display.setCursor(0, 16);
    display.println("keine Messung");
    display.println("Trigger halten");
  } else {
    // Bewusst OHNE Filter/Gain/ATIME/ASTEP-Zusammenfassung hier -- nahm zu viel
    // Platz weg, bot fuer diesen Screen kaum Mehrwert (der Abgleich selbst
    // passiert weiterhin unsichtbar ueber refValid oben).
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

// Reiner Statusbildschirm fuer Info-Modus -- Betriebszeit im hh:mm-Format
// (Stundenanteil bewusst nicht auf 2 Stellen begrenzt, da er ueber die
// Geraete-Lebensdauer durchaus dreistellig werden kann), der lebenslange
// Messzaehler aus UptimeLogger, sowie die aktuelle ESP32-Die-Temperatur (kein
// Ersatz fuer eine echte LED-Temperaturmessung, aber ein greifbarer Hinweis
// bei spaeterer Auswertung unerklaerter Abweichungen). Wird waehrend des
// Aufenthalts in diesem Modus alle 10s automatisch neu gezeichnet (siehe
// loop()), damit die Temperatur/Betriebszeit sichtbar mitlaeuft.
void renderInfoStatus() {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("Info");

  uint32_t totalSec = uptimeLogger.totalSeconds();
  uint32_t hh = totalSec / 3600;
  uint32_t mm = (totalSec % 3600) / 60;
  char line[24];
  snprintf(line, sizeof(line), "Betrieb: %lu:%02lu", (unsigned long)hh, (unsigned long)mm);
  display.setCursor(0, 20);
  display.println(line);

  snprintf(line, sizeof(line), "Messungen: %lu", (unsigned long)uptimeLogger.measurementCount());
  display.setCursor(0, 32);
  display.println(line);

  snprintf(line, sizeof(line), "Temp: %.1f C", temperatureRead());
  display.setCursor(0, 44);
  display.println(line);

  display.display();
}

// Dritte "Seite" des Export-Modus: loescht die persistierte Historie, aber
// nur nach LANGEM Trigger-Druck (siehe loop()) -- ein kurzer Druck hier tut
// bewusst nichts, daher der Hinweistext. Nach dem Loeschen zeigt dieselbe
// Seite direkt "0 Messungen" -- das ist die Erfolgsrueckmeldung.
void renderExportClear() {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("Verlauf loeschen?");

  char line[24];
  snprintf(line, sizeof(line), "%u Messungen", (unsigned)historyStore.count());
  display.setCursor(0, 16);
  display.println(line);

  display.setCursor(0, 32);
  display.println("Trigger halten");
  display.setCursor(0, 42);
  display.println("zum Loeschen");

  display.display();
}

// Statusbildschirm fuer den Settings-Modus: Name der aktuell per kurzem
// Mode-Druck gewaehlten Einstellung (siehe SETTINGS-Registry weiter oben),
// grosser Wertetext darunter -- entweder im Browsing-Zustand (kein Cursor)
// oder waehrend der Bearbeitung (aktive Ziffer/Option invertiert
// dargestellt, siehe editingActive/editor).
void renderSettingsStatus() {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("Einstellungen");

  const SettingDescriptor& s = SETTINGS[currentSettingIndex];
  display.setCursor(0, 20);
  display.print(s.name);
  display.println(":");

  display.setTextSize(2);

  if (!editingActive) {
    display.setCursor(0, 34);
    if (s.valueLabel) {
      display.println(s.valueLabel(s.getValue()));
    } else {
      char buf[8];
      snprintf(buf, sizeof(buf), "%lu", (unsigned long)s.getValue());
      display.println(buf);
    }
  } else if (s.digitCount == 1) {
    // Enum-artig: die einzige "Ziffer" ist der ganze Optionswert -- als
    // Ganzes invertiert darstellen (nur eine Position, immer aktiv).
    const char* label = s.valueLabel(editor.digitAt(0));
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(label, 0, 34, &x1, &y1, &w, &h);
    display.fillRect(0, 34, w + 4, h + 4, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(2, 36);
    display.println(label);
    display.setTextColor(SSD1306_WHITE);
  } else {
    // Mehrstellig: jede Ziffer einzeln zeichnen, die am Cursor invertiert.
    const int digitW = 14;
    int x = 0;
    for (uint8_t i = 0; i < editor.digitCount(); i++) {
      char ch[2] = { (char)('0' + editor.digitAt(i)), '\0' };
      if (i == editor.cursor()) {
        display.fillRect(x, 34, digitW, 18, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
        display.setCursor(x + 3, 36);
        display.print(ch);
        display.setTextColor(SSD1306_WHITE);
      } else {
        display.setCursor(x + 3, 36);
        display.print(ch);
      }
      x += digitW;
    }
  }

  display.display();
}

void renderCurrentView() {
  if (!displayOk) return;
  if (currentDisplayMode == DisplayMode::Export) {
    if (exportPage == ExportPage::Clear) {
      renderExportClear();
    } else {
      renderExportStatus();
    }
    return;
  }
  if (currentDisplayMode == DisplayMode::Calibration) {
    renderReferenceStatus();
    return;
  }
  if (currentDisplayMode == DisplayMode::Settings) {
    renderSettingsStatus();
    return;
  }
  if (currentDisplayMode == DisplayMode::Info) {
    renderInfoStatus();
    return;
  }

  // Effektive Referenz gegen lastMeasurementSettings (NICHT gegen
  // currentSettings!) -- ein Rohmesswert wurde physisch mit einem bestimmten
  // Filter/Gain/ATIME/ASTEP aufgenommen, ein spaeterer Wechsel im Settings-
  // Modus macht ihn nicht nachtraeglich "mit anderen Einstellungen gemessen".
  // Solange aber noch GAR KEINE Messung vorliegt (lastMeasurement leer), gibt
  // es nichts einzufrieren -- lastMeasurementSettings stuende dann noch auf
  // seinen Boot-Defaults, was faelschlich als Mismatch gegen eine tatsaechlich
  // gueltige Kalibrierung fuer ANDERE Einstellungen durchschlagen wuerde
  // ("nicht kalibriert" direkt nach dem Start, obwohl eine passende Referenz
  // gespeichert ist). In diesem Fall daher gegen currentSettings pruefen (die
  // eigentlich relevante Frage: "waere eine JETZT gestartete Messung gueltig
  // kalibriert").
  const AcquisitionSettings& calCheckSettings = lastMeasurement.empty() ? currentSettings : lastMeasurementSettings;
  bool haveMatchingCal = calibrationValidFor(calCheckSettings);
  static const Measurement emptyRef;
  const Measurement& effDark  = haveMatchingCal ? darkRef  : emptyRef;
  const Measurement& effWhite = haveMatchingCal ? whiteRef : emptyRef;

  ViewContext ctx{ spectrometer, lastMeasurement, effWhite, effDark, haveMatchingCal,
                   displayModeLabel(currentDisplayMode), lastLabel, calCheckSettings.filterState };
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
  display.println(activeModeLabel());

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

// Eigener Screen fuer einen fehlgeschlagenen Messversuch (leeres Measurement
// von spectrometer.performMeasurement()) -- bleibt stehen, bis die naechste
// Aktion (erneuter Trigger-/Mode-Druck) einen regulaeren Re-Render ausloest,
// gleiches Muster wie z.B. das "gesendet"/"Kein Handy verbunden"-Feedback im
// Export-Modus. Gilt fuer alle performMeasurement()-Aufrufer gleichermassen
// (Fast/Precise/Calibration) -- die Ursache ist unabhaengig davon relevant.
void renderMeasurementError(MeasurementStatus status) {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("Fehler");

  display.setCursor(0, 20);
  if (status == MeasurementStatus::NotConverged) {
    display.println("Messung zu stark");
    display.println("verrauscht");
    display.println("Geraet ruhig halten");
    display.println("und erneut versuchen");
  } else {
    display.println("Sensor antwortet");
    display.println("nicht");
  }

  display.display();
}

// Gemeinsamer Einstiegspunkt fuer den Trigger-Taster in allen Mess-Modi.
// precision/kind werden vom Aufrufer (loop()) bestimmt, nicht hier -- diese
// Funktion kennt keinen DisplayMode mehr, nur noch "wie genau messen" und
// "was fuer ein Messwert ist das".
void performMeasurement(Precision precision, SampleKind kind) {
  if (busy) return;  // keine zweite Messung waehrend eine laeuft
  busy = true;

  flashBorder();  // nur hier, also nur wenn tatsaechlich gestartet wird
  showMeasuringScreen(0, 1);

  MeasurementStatus status = MeasurementStatus::Ok;
  Measurement measurement = spectrometer.performMeasurement(precision, showMeasuringScreen, &status);
  if (measurement.empty()) {
    if (status == MeasurementStatus::NotConverged) {
      Serial.println("# Messung nicht konvergiert -- Geraet ruhig halten und erneut versuchen");
    } else {
      Serial.println("# Sensorfehler bei der Messung");
    }
    busy = false;
    renderMeasurementError(status);
    return;
  }
  lastMeasurement = measurement;
  lastMeasurementSettings = currentSettings;

  // Vor der Beschriftung inkrementieren: die Sample-Nummer ist der neue,
  // lebenslange Zaehlerstand -- so laufen die Nummern ueber Reboots/Sessions
  // hinweg durch, statt bei jedem Neustart wieder bei 1 anzufangen. Dark/White
  // verbrauchen dabei ebenfalls eine Nummer (zaehlen als Messung), tauchen aber
  // nicht als "sample_NN" auf -- entstehende Luecken in der Sample-Numerierung
  // sind bewusst in Kauf genommen.
  uptimeLogger.recordMeasurement();

  char lbl[16];
  if (kind == SampleKind::Dark) {
    strncpy(lbl, "dark", sizeof(lbl));
  } else if (kind == SampleKind::White) {
    strncpy(lbl, "white", sizeof(lbl));
  } else {
    snprintf(lbl, sizeof(lbl), "sample_%02u", (unsigned)uptimeLogger.measurementCount());
  }
  lbl[sizeof(lbl) - 1] = '\0';
  strncpy(lastLabel, lbl, sizeof(lastLabel));
  lastLabel[sizeof(lastLabel) - 1] = '\0';

  MeasurementRecord rec;
  strncpy(rec.label, lbl, sizeof(rec.label));
  rec.label[sizeof(rec.label) - 1] = '\0';
  rec.kind = kind;
  rec.measurement = measurement;
  // Kontext zum Messzeitpunkt -- siehe MeasurementRecord-Kommentar in
  // HistoryStore.h: kein Ersatz fuer eine echte LED-Temperaturmessung, aber
  // ein greifbarer Hinweis bei spaeterer Auswertung unerklaerter Abweichungen.
  rec.tempC = temperatureRead();
  rec.sessionMs = millis();
  rec.uptimeS = uptimeLogger.totalSeconds();
  rec.settings = currentSettings;
  if (!historyStore.append(rec)) {
    Serial.println("# history append failed (Flash voll?)");
  }

  if (kind == SampleKind::Dark) {
    darkRef = measurement;
    darkRefSettings = currentSettings;  // Einstellungen zum Aufnahmezeitpunkt einfrieren
    calStore.saveDark(darkRef, darkRefSettings);
  }
  if (kind == SampleKind::White) {
    whiteRef = measurement;
    whiteRefSettings = currentSettings;
    calStore.saveWhite(whiteRef, whiteRefSettings);
  }
  calibrated = calibrationValidFor(currentSettings);

  printCsvRow(rec);

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

  std::string csv = buildHistoryCsv(exportPage == ExportPage::Debug);
  bool ok = bleExporter.send(csv, onExportProgress);

  exportSending = false;
  exportSentOk = ok;
  busy = false;
  renderExportStatus();
}

// Kurzer Mode-Druck: in Fast/Precise/Export-Modus View bzw. Export-Seite
// wechseln, in Settings die zu editierende Einstellung wechseln, in
// Calibration zwischen Weiss-/Dunkelreferenz wechseln.
void cycleView() {
  if (currentDisplayMode == DisplayMode::Export) {
    uint8_t n = (static_cast<uint8_t>(exportPage) + 1) % static_cast<uint8_t>(ExportPage::COUNT);
    exportPage = static_cast<ExportPage>(n);
    renderCurrentView();
    return;
  }
  if (currentDisplayMode == DisplayMode::Settings) {
    currentSettingIndex = (currentSettingIndex + 1) % SETTINGS_COUNT;
    renderCurrentView();
    return;
  }
  if (currentDisplayMode == DisplayMode::Calibration) {
    calibrationTarget = (calibrationTarget == CalibrationTarget::White) ? CalibrationTarget::Dark : CalibrationTarget::White;
    renderCurrentView();
    return;
  }
  uint8_t n = (static_cast<uint8_t>(currentView) + 1) % static_cast<uint8_t>(DisplayView::COUNT);
  currentView = static_cast<DisplayView>(n);
  renderCurrentView();
}

void cycleMode() {
  DisplayMode previous = currentDisplayMode;
  uint8_t n = (static_cast<uint8_t>(currentDisplayMode) + 1) % static_cast<uint8_t>(DisplayMode::COUNT);
  currentDisplayMode = static_cast<DisplayMode>(n);
  // Sicherheitsnetz: ein voller Moduswechsel sollte nie mitten in einer
  // Settings-Bearbeitung passieren (loop() blockt cycleMode() waehrend
  // editingActive bereits ab), aber ein sauberer Reset hier kostet nichts.
  editingActive = false;

  if (previous == DisplayMode::Export && currentDisplayMode != DisplayMode::Export) {
    bleExporter.end();  // no-op, falls BLE in diesem Aufenthalt nie aktiviert wurde
  }
  if (currentDisplayMode == DisplayMode::Export && previous != DisplayMode::Export) {
    // BLE wird bewusst NICHT hier gestartet, siehe performExport().
    exportSending = false;
    exportSentOk = false;
    exportHint = false;
    exportSentBytes = exportTotalBytes = 0;
    // Verhindert, dass man nach einem Modus-Rundgang unbemerkt wieder auf der
    // Loeschen-Seite landet.
    exportPage = ExportPage::Normal;
  }
  if (currentDisplayMode == DisplayMode::Settings && previous != DisplayMode::Settings) {
    // Gleiche Ueberlegung wie bei exportPage oben.
    currentSettingIndex = 0;
  }
  if (currentDisplayMode == DisplayMode::Calibration && previous != DisplayMode::Calibration) {
    // Gleiche Ueberlegung -- nicht unbemerkt auf "Dark" landen.
    calibrationTarget = CalibrationTarget::White;
  }

  // Die zuletzt gezeigte Messung gehoert zum vorherigen Modus -- nach einem
  // Moduswechsel soll sie nicht weiter angezeigt werden (Views fallen dann
  // auf ihren "keine Messung"/"nicht kalibriert"-Hinweis zurueck).
  lastMeasurement.clear();
  lastLabel[0] = '\0';

  renderCurrentView();

  // Startpunkt fuer den periodischen 10s-Refresh im Info-Modus (siehe loop())
  // -- verhindert ein sofortiges, redundantes zweites Neuzeichnen direkt nach
  // dem obigen renderCurrentView().
  if (currentDisplayMode == DisplayMode::Info) lastInfoRenderMs = millis();
}

void setup() {
  // Thermik: 160->80MHz halbiert die aktive Rechenleistung praktisch ohne
  // Risiko -- auf dem C3 haengt der APB-Takt (u.a. I2C/USB-Timing) NICHT von
  // der CPU-Frequenz ab (anders als bei Frequenzen < 80MHz, die deshalb WLAN/
  // BT/USB brechen koennen und hier bewusst nicht angefasst werden).
  setCpuFrequencyMhz(80);

  // Display so frueh wie moeglich initialisieren -- VOR dem 2s-USB-CDC-Delay
  // und den NVS/LittleFS-Reads weiter unten, damit so schnell wie physikalisch
  // moeglich (begrenzt nur noch durch die ESP32-eigene Bootloader-Zeit) etwas
  // auf dem Bildschirm erscheint, statt mehrere hundert ms bis >2s leer zu
  // bleiben. Das Display selbst hat kein nichtfluechtiges Speicher (GDDRAM ist
  // reines SRAM im Controller) -- "so frueh wie moeglich zeigen" ist die
  // einzige Stellschraube, "vor dem ESP anzeigen" ist auf dieser Hardware
  // schlicht nicht moeglich.
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(I2C_HZ);
  displayOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (displayOk) {
    display.setRotation(2);  // Display ist 180 Grad verdreht montiert
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Colorimeter");
    display.println("startet ...");
    display.display();  // sofort push -- kein "Schnee"-Frame mehr sichtbar
  }

  Serial.begin(115200);
  delay(2000);

  triggerBtn.begin();
  modeBtn.begin();

  calStore.begin();
  calStore.loadDark(darkRef, darkRefSettings);
  calStore.loadWhite(whiteRef, whiteRefSettings);
  calStore.loadSettings(currentSettings);
  calibrated = calibrationValidFor(currentSettings);

  uptimeLogger.begin();
  historyStore.begin();  // nicht fatal bei Fehlschlag -- Kernfunktion laeuft ohne Historie weiter

  if (displayOk) {
    // Ersetzt den "startet ..."-Text von oben, sobald Kalibrierstatus etc.
    // tatsaechlich bekannt sind.
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Colorimeter");
    display.println(calibrated ? "ready" : "need cal: d/w");
    display.display();
  } else {
    // Reine Boot-Diagnose auf einem Fehlerpfad -- vermischt sich nie mit dem
    // eigentlichen CSV-Export (der erst spaeter/live in loop() beginnt).
    Serial.println("# SSD1306 nicht gefunden/initialisiert (Adresse 0x3C) - Display bleibt inaktiv");
  }

  if (!sensorImpl.begin()) {
    Serial.println("# AS7341 not found");
    while (true) delay(1000);
  }
  sensorImpl.applySettings(currentSettings);  // Hardware von Anfang an zum geladenen Zustand passend
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

  // White/Dark (ueberschreibt die Kalibrierreferenz) und die Export-
  // Clear-Seite (loescht die Historie) verlangen einen LANGEN statt kurzen
  // Trigger-Druck -- gleiche "haltbewusst statt versehentlich"-Absicherung
  // wie der bestehende lange Mode-Druck fuer den Moduswechsel. Fast/Precise
  // und das Ausloesen des BLE-Sendens bleiben bei sofortigem, kurzem Druck,
  // da sie haeufig und unkritisch sind.
  DebouncedButton::Event te = triggerBtn.poll();
  if (currentDisplayMode == DisplayMode::Export) {
    if (exportPage == ExportPage::Clear) {
      if (te == DebouncedButton::Event::LongPress) {
        flashBorder();
        historyStore.clear();
        renderCurrentView();
      }
    } else if (te == DebouncedButton::Event::Pressed) {
      performExport();
    }
  } else if (currentDisplayMode == DisplayMode::Calibration) {
    if (te == DebouncedButton::Event::LongPress) {
      SampleKind kind = (calibrationTarget == CalibrationTarget::White) ? SampleKind::White : SampleKind::Dark;
      performMeasurement(Precision::Precise, kind);  // Referenzmessungen immer Precise, wie bisher
    }
  } else if (currentDisplayMode == DisplayMode::Settings) {
    // Ausserhalb einer Bearbeitung startet nur ein LANGER Trigger-Druck die
    // Bearbeitung der aktuell gewaehlten Einstellung (verhindert
    // versehentliches Hineinrutschen) -- ein kurzer Druck tut dann nichts.
    // Waehrend der Bearbeitung: kurz = aktuelle Ziffer +1, lang = naechste
    // Ziffer (auf der letzten Ziffer: Bearbeitung abschliessen + speichern).
    // Siehe DigitEditor.h / Kontext-Abschnitt im Plan fuer die volle
    // Tasten-Zuordnung (Mode-Taste spiegelbildlich, siehe unten).
    //
    // WICHTIG: hier absichtlich ShortRelease statt Pressed fuer "+1" --
    // Pressed feuert sofort beim Herunterdruecken, UNABHAENGIG davon, wie
    // lange danach gehalten wird. Mit Pressed wuerde ein langer Druck also
    // erst ein Pressed (faelschlich +1) UND anschliessend ein LongPress
    // (naechste Ziffer) ausloesen -- genau der gemeldete Fehler. ShortRelease
    // feuert dagegen nur beim Loslassen, und nur, wenn die Lang-Druck-
    // Schwelle waehrend des Haltens NICHT ueberschritten wurde (siehe
    // Buttons.h) -- exakt wie die Mode-Taste es bereits macht.
    const SettingDescriptor& s = SETTINGS[currentSettingIndex];
    if (!editingActive) {
      if (te == DebouncedButton::Event::LongPress) {
        editor.begin(s.digitCount, s.digitCycleLen, s.getValue());
        editingActive = true;
        renderCurrentView();
      }
    } else if (te == DebouncedButton::Event::ShortRelease) {
      editor.incrementCurrentDigit();
      renderCurrentView();
    } else if (te == DebouncedButton::Event::LongPress) {
      if (editor.advanceDigit()) {  // true = letzte Ziffer ueberschritten -> fertig
        s.setValue(editor.assembledValue(s.maxValue));
        editingActive = false;
      }
      renderCurrentView();
    }
  } else if (currentDisplayMode != DisplayMode::Info) {
    // Fast oder Precise. Info ist ein reiner Statusbildschirm -- Trigger
    // loest dort bewusst keine (sinnlose) Messung aus.
    if (te == DebouncedButton::Event::Pressed) {
      Precision prec = (currentDisplayMode == DisplayMode::Fast) ? Precision::Fast : Precision::Precise;
      performMeasurement(prec, SampleKind::Regular);
    }
  }

  // Waehrend einer Settings-Bearbeitung uebernimmt die Mode-Taste
  // spiegelbildlich zur Trigger-Taste die Ziffern-Navigation (kurz = -1,
  // lang = vorige Ziffer bzw. auf der ersten Ziffer: Bearbeitung
  // abschliessen + speichern -- bewusst symmetrisch zum Trigger-Abschluss,
  // kein separater Abbrechen-Pfad, siehe Plan) -- ihre sonstige Bedeutung
  // (View/Modus wechseln) ist so lange blockiert.
  DebouncedButton::Event me = modeBtn.poll();
  if (currentDisplayMode == DisplayMode::Settings && editingActive) {
    const SettingDescriptor& s = SETTINGS[currentSettingIndex];
    if (me == DebouncedButton::Event::ShortRelease) {
      editor.decrementCurrentDigit();
      renderCurrentView();
    } else if (me == DebouncedButton::Event::LongPress) {
      if (editor.retreatDigit()) {  // true = erste Ziffer unterschritten -> fertig
        s.setValue(editor.assembledValue(s.maxValue));
        editingActive = false;
      }
      renderCurrentView();
    }
  } else if (me == DebouncedButton::Event::ShortRelease) {
    cycleView();
  } else if (me == DebouncedButton::Event::LongPress) {
    cycleMode();
  }

  // Info ist ein rein passiver Statusbildschirm (kein Tastendruck loest dort
  // ein Neuzeichnen aus) -- deshalb hier per Zeitgeber alle 10s aufgefrischt,
  // damit Temperatur/Betriebszeit sichtbar mitlaufen.
  if (currentDisplayMode == DisplayMode::Info && (millis() - lastInfoRenderMs) >= 10000UL) {
    lastInfoRenderMs = millis();
    renderCurrentView();
  }

  // Export: der BLE-Verbindungsstatus aendert sich asynchron im Bluedroid-
  // Callback (siehe BleExporter::onConnect()/onDisconnect()), nicht durch
  // einen Tastendruck hier. Event-getrieben statt periodisch gepollt --
  // takeConnectionChanged() liefert nur GENAU DANN true, wenn sich seit dem
  // letzten Aufruf tatsaechlich etwas geaendert hat.
  if (currentDisplayMode == DisplayMode::Export && bleExporter.takeConnectionChanged()) {
    renderCurrentView();
  }

  bleExporter.loop();
  uptimeLogger.loop();

  // Thermik: ohne dieses delay() spinnt die Idle-Schleife (reine
  // Tasterabfrage) mit 100% Duty-Cycle, praktisch die ganze Zeit, in der das
  // Geraet nur auf einen Tastendruck wartet. delay() gibt an den FreeRTOS-
  // Idle-Task ab (WFI) -- 1ms ist gegenueber dem 40ms-Debounce vernachlaessigbar.
  delay(1);
}
