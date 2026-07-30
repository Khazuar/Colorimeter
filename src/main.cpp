#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_AS7341.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ------------------------- User-Konfiguration -------------------------
static const uint8_t  SDA_PIN = 6;      // an deine Verdrahtung anpassen
static const uint8_t  SCL_PIN = 7;
static const uint32_t I2C_HZ  = 100000;

// ------------------------- OLED (SSD1306, 0.96", 128x64, I2C) ---------
static const uint8_t  OLED_WIDTH  = 128;
static const uint8_t  OLED_HEIGHT = 64;
static const uint8_t  OLED_ADDR   = 0x3C;
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
bool displayOk = false;

// EINGEFROREN: identisch fuer Dunkel, Weiss und alle Proben verwenden!
static const uint8_t  AS_ATIME = 100;
static const uint16_t AS_ASTEP = 999;                 // ~281 ms Integration
static const as7341_gain_t AS_GAIN = AS7341_GAIN_256X; // Ausgesucht, so dass Weiss unter Vollausschlag bleibt

// Messungen, die pro Trigger gemittelt werden. Rauschen sinkt ~mit sqrt(N).
// Jede readAllChannels() braucht zwei Integrationszyklen -> N=16 dauert einige
// Sekunden. Fuer schnelleres Erfassen ggf. auf 8 reduzieren.
static const uint8_t  N_AVG = 16;

Adafruit_AS7341 as7341;

struct Band { uint8_t idx; const char* name; const char* shortName; };
static const Band BANDS[] = {
  { 0,  "F1_415nm", "415" }, { 1,  "F2_445nm", "445" }, { 2,  "F3_480nm", "480" }, { 3,  "F4_515nm", "515" },
  { 6,  "F5_555nm", "555" }, { 7,  "F6_590nm", "590" }, { 8,  "F7_630nm", "630" }, { 9,  "F8_680nm", "680" },
  { 10, "Clear",    "Clr" }, { 11, "NIR_910nm", "NIR" },
};
static const size_t N_BANDS = sizeof(BANDS) / sizeof(BANDS[0]);

uint16_t sampleCount = 0;

// Zeigt die gemittelten Kanalwerte einer Messung an: Label in Zeile 1,
// danach die Baender zu je zweit pro Zeile (passt auf 128x64 bei Textsize 1).
void showResults(const char* label, const uint32_t* acc, uint8_t taken) {
  if (!displayOk) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(label);

  char line[24];
  for (size_t i = 0; i < N_BANDS; i += 2) {
    float v1 = (float)acc[BANDS[i].idx] / taken;
    if (i + 1 < N_BANDS) {
      float v2 = (float)acc[BANDS[i + 1].idx] / taken;
      snprintf(line, sizeof(line), "%-4s%5.0f %-4s%5.0f",
               BANDS[i].shortName, v1, BANDS[i + 1].shortName, v2);
    } else {
      snprintf(line, sizeof(line), "%-4s%5.0f", BANDS[i].shortName, v1);
    }
    display.println(line);
  }
  display.display();
}

// Nimmt N_AVG Messungen, mittelt kanalweise (uint32-Akku, kein Overflow),
// schreibt eine CSV-Zeile: label,F1,...,NIR und zeigt das Ergebnis an.
void measureAveraged(const char* label) {
  Serial.print("# messe ("); Serial.print(N_AVG);
  Serial.println(" Messungen, bitte still halten)...");

  uint32_t acc[12] = {0};
  uint8_t  taken = 0;
  for (uint8_t n = 0; n < N_AVG; n++) {
    uint16_t r[12];
    if (!as7341.readAllChannels(r)) { Serial.println("# read error"); continue; }
    for (uint8_t i = 0; i < 12; i++) acc[i] += r[i];
    taken++;
  }
  if (taken == 0) { Serial.println("# measurement failed"); return; }

  Serial.print(label);
  for (size_t i = 0; i < N_BANDS; i++) {
    Serial.print(',');
    Serial.print((float)acc[BANDS[i].idx] / taken, 2);
  }
  Serial.println();

  showResults(label, acc, taken);
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(I2C_HZ);

  displayOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!displayOk) {
    Serial.println("# SSD1306 nicht gefunden/initialisiert (Adresse 0x3C) - Display bleibt inaktiv");
  }

  if (!as7341.begin()) { Serial.println("# AS7341 not found"); while (true) delay(1000); }
  as7341.setATIME(AS_ATIME);
  as7341.setASTEP(AS_ASTEP);
  as7341.setGain(AS_GAIN);

  // CSV-Header (label-Spalte + Kanaele)
  Serial.print("label");
  for (size_t i = 0; i < N_BANDS; i++) { Serial.print(','); Serial.print(BANDS[i].name); }
  Serial.println();

  Serial.println("# bereit. m=Probe messen  d=Dunkelreferenz  w=Weissreferenz  r=Zaehler zuruecksetzen");
}

void loop() {
  if (!Serial.available()) return;
  char c = Serial.read();

  switch (c) {
    case 'm': {
      char lbl[16];
      snprintf(lbl, sizeof(lbl), "sample_%02u", (unsigned)(++sampleCount));
      measureAveraged(lbl);
      break;
    }
    case 'd': measureAveraged("dark");  break;
    case 'w': measureAveraged("white"); break;
    case 'r': sampleCount = 0; Serial.println("# Proben-Zaehler zurueckgesetzt"); break;
    case '\n': case '\r': case ' ': break;   // Zeilenenden/Leerzeichen ignorieren
    default:  Serial.print("# unbekannt: "); Serial.println(c);
  }
}