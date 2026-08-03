#pragma once
#include <cstdint>
#include <vector>

struct Lab {
  float L;
  float a;
  float b;
};

// Ein einzelnes Band als Zentrum + Halbwertsbreite (FWHM), so wie optische
// Bandpassfilter (z.B. im AS7341-Datenblatt) tatsaechlich charakterisiert
// werden: die Empfindlichkeit faellt zu beiden Seiten des Zentrums hin ab
// (naeherungsweise glockenfoermig) statt scharf an einer Kante abzuschneiden;
// fwhm_nm ist die Breite, bei der die Empfindlichkeit noch 50% des Maximums
// betraegt. Baender muessen weder lueckenlos noch ueberlappungsfrei sein --
// jeder Sensor hat seine eigene Filtercharakteristik, das darf die Abstraktion
// nicht einschraenken.
struct Band {
  float center_nm;
  float fwhm_nm;
};

// bands:  N Baender (je eigenes Zentrum/Breite, Reihenfolge aufsteigend nach Zentrum)
// values: N Messwerte (kalibrierte Reflexion, ~0..1+), values[i] gehoert zu bands[i]
// nir:    Sonderfall NIR-Kanal (bei AS7341 910nm) -- genauso dark/white-korrigiert
//         wie die Baender, aber bewusst NICHT Teil von bands/values: NIR ist kein
//         "Band" im Sinne dieser Abstraktion (kein sichtbares Spektrum, wuerde bei
//         einer Fensterkonstruktion wie die Baender eine unsinnig grosse Luecke zu
//         F8 aufreissen), aber als einzelner, kalibrierter Messwert weiterhin nuetzlich.
struct Spectrum {
  std::vector<Band> bands;
  std::vector<float> values;
  float nir = 0.0f;
};

enum class Precision : uint8_t { Fast, Precise };

// current: wie viele Einzelproben bereits genommen wurden; maxEstimate: Obergrenze
// (bei Precision::Precise ggf. nicht ausgeschoepft, falls vorher konvergiert).
// Reiner Funktionszeiger (kein std::function) -- kein Heap-Bedarf.
using ProgressCallback = void (*)(uint8_t current, uint8_t maxEstimate);

class Spectrometer {
public:
  virtual ~Spectrometer() = default;

  // Eine physische Messung, unverarbeitete Rohzaehlwerte. Nuetzlich eigenstaendig
  // fuer Diagnose/Debug UND als gemeinsame Eingabe fuer getSpectrum()/getColor(),
  // damit eine Messung genau einmal die Hardware anspricht, aus deren Ergebnis
  // beliebig viele Ableitungen berechnet werden koennen. onProgress (falls gesetzt)
  // wird waehrend der Messung wiederholt aufgerufen, damit die Orchestrierung z.B.
  // eine Fortschrittsanzeige zeichnen kann.
  virtual std::vector<uint32_t> measureRawSpectrum(Precision precision,
                                                     ProgressCallback onProgress = nullptr) = 0;

  // Reine Berechnungen auf einem gegebenen Rohspektrum, kein Hardwarezugriff.
  virtual Spectrum getSpectrum(const std::vector<uint32_t>& raw) = 0;
  virtual Lab getColor(const std::vector<uint32_t>& raw) = 0;

  // Nutzung des Sensor-Wertebereichs, UNABHAENGIG von der Dark/White-
  // Kalibrierung (fiktiver Dunkelwert 0, fiktiver Weisswert = Sensor-eigener
  // Referenzpunkt fuer "guter Ausschlag"). Sinnvoll z.B. waehrend einer
  // Dark/White-Referenzmessung selbst: dort waere getSpectrum() sinnlos
  // selbstbezueglich (die gerade gesetzte Referenz gegen sich selbst normiert
  // ergibt immer 0%/100%), aber "wie sehr wird der Wertebereich ausgenutzt"
  // bleibt eine ehrliche, kalibrierungsfreie Diagnose.
  virtual Spectrum getRangeUtilization(const std::vector<uint32_t>& raw) = 0;

  // Setzt Dunkel-/Weiss-Referenzrohwerte (gleiches Format wie getRawSpectrum()).
  // Woher diese Werte kommen und ob/wie sie ueber einen Neustart hinweg persistiert
  // werden, ist Sache der Orchestrierung -- diese Klasse liest/schreibt kein Flash.
  virtual void calibrate(const std::vector<uint32_t>& dark,
                          const std::vector<uint32_t>& white) = 0;
};
