#pragma once
#include <cstdint>
#include <vector>
#include <cmath>

// Ergebnis einer physischen Messung: Struktur und Bedeutung der Elemente
// sind ein Implementierungsdetail des jeweiligen Sensors (Reihenfolge,
// Anzahl, welcher Kanal an welcher Position steht). Generischer Code soll
// ein Measurement nur als opake Blackbox durchreichen (an getSpectrum()
// oder zur Persistierung) -- siehe Spectrometer::measurementLabels() fuer
// die einzige zulaessige Ausnahme (Debug/Analyse).
using Measurement = std::vector<float>;

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

// Sensor-UNABHAENGIGES Spektrum: N Baender (je eigenes Zentrum/Breite,
// aufsteigend nach Zentrum) mit kalibrierten Reflexionswerten (~0..1+).
// Bewusst nur das sichtbare Spektrum -- Kanaele wie Clear oder NIR, die kein
// "echtes" Band des sichtbaren Spektrums darstellen (NIR waere z.B. eine
// unsinnig breite Luecke zum letzten VIS-Band), gehoeren NICHT hierher,
// sondern bleiben Teil des sensor-spezifischen Measurement.
struct Spectrum {
  std::vector<Band> bands;
  std::vector<float> values;
  // Absolute Unsicherheit je Band, abgeleitet aus der Unsicherheit des dafuer
  // verwendeten NIR-Korrekturfaktors (siehe AS7341Spectrometer.cpp) -- 0.0, wo
  // kein Fehlermodell vorliegt. Vorerst nur intern mitgefuehrt (kein CSV-
  // Export), Grundlage fuer eine spaetere DeltaE-Unsicherheitsberechnung.
  std::vector<float> valueErrors;
};

enum class Precision : uint8_t { Single = 0, Precise = 1, COUNT = 2 };

// Welcher physische (Bandpass-/Cut-)Filter aktuell vor dem Sensor sitzt.
// Generisches Konzept (jeder Spectrometer koennte sowas unterstuetzen), auch
// wenn die konkreten Werte aktuell nur von AS7341Spectrometer befuellt werden
// -- gleiche Pragmatik wie bei Precision::Single/Precise.
enum class FilterState : uint8_t { None = 0, Filter650nm = 1, Filter700nm = 2, COUNT = 3 };

// current: wie viele Einzelproben bereits genommen wurden; maxEstimate: Obergrenze
// (bei Precision::Precise ggf. nicht ausgeschoepft, falls vorher konvergiert).
// Reiner Funktionszeiger (kein std::function) -- kein Heap-Bedarf.
using ProgressCallback = void (*)(uint8_t current, uint8_t maxEstimate);

// SensorError: der Sensor liefert ueberhaupt keine gueltigen Daten
// (Hardware-Fehler). NotConverged: nur bei Precision::Precise moeglich --
// die Zielpraezision wurde innerhalb des Sample-Budgets nicht erreicht (z.B.
// das Geraet wurde waehrend der Messung durchgehend bewegt). Beide Faelle
// geben ein leeres Measurement zurueck (nicht nutzbare Daten bleiben nicht
// nutzbar), unterscheiden sich aber in der Ursache -- fuer eine dem Nutzer
// gegenueber unterschiedliche Fehlermeldung ("Sensorfehler" vs. "Geraet ruhig
// halten und erneut versuchen").
enum class MeasurementStatus : uint8_t { Ok, SensorError, NotConverged };

// Telemetrie zu EINER performMeasurement()-Ausfuehrung, jenseits der reinen
// Messdaten -- optionaler Out-Parameter (Aufrufer, die nur wissen wollen
// "hat's geklappt", pruefen weiterhin einfach Measurement::empty()).
//
// sampleCount ist die Anzahl tatsaechlich gemittelter Einzelproben (bei
// Precision::Single immer 1). relSemWorst ist der schlechteste relative
// Standardfehler des Mittelwerts ueber alle Kanaele oberhalb der
// Rauschgrenze (siehe AS7341Spectrometer.cpp converged()) -- ein oberer
// Schrankwert, KEIN Wert je Kanal (der tatsaechliche relSEM einzelner
// Kanaele kann darunter liegen). NAN, wenn kein relSEM berechnet wurde: bei
// Precision::Single (dort wird gar nicht konvergiert); bei Precision::Precise,
// falls KEIN Kanal ueber der Rauschgrenze lag (z.B. eine sehr dunkle Probe/
// Dunkelmessung); UND bei Precision::Precise, falls WAEHREND einer ansonsten
// erfolgreichen Messung mindestens ein (aber nicht alle) Kanaele unterhalb
// der Rauschgrenze blieben -- die Messung selbst (Measurement) ist in diesem
// Fall trotzdem gueltig und nicht leer, nur die Praezisionsaussage entfaellt
// bewusst fuer die GESAMTE Messung (nicht nur den betroffenen Kanal), statt
// eine Zahl zu berichten, die Praezision fuer einen gar nicht beurteilten
// Kanal unterstellt. Bewusst NICHT 0.0, das waere eine falsche Aussage ueber
// tatsaechlich erreichte Praezision.
struct MeasurementTelemetry {
  MeasurementStatus status = MeasurementStatus::Ok;
  uint8_t sampleCount = 0;
  float relSemWorst = NAN;
};

class Spectrometer {
public:
  virtual ~Spectrometer() = default;

  // Eine physische Messung. Kapselt alles Sensor-Spezifische (I2C, SMUX-
  // Konfiguration, Register auslesen/umrechnen) -- das Ergebnis ist ein
  // opakes Measurement, keine Struktur darauf verlassen. onProgress (falls
  // gesetzt) wird waehrend der Messung wiederholt aufgerufen, damit die
  // Orchestrierung z.B. eine Fortschrittsanzeige zeichnen kann. outTelemetry
  // (falls gesetzt) liefert Status/Sample-Anzahl/relSEM -- siehe MeasurementTelemetry.
  virtual Measurement performMeasurement(Precision precision,
                                          ProgressCallback onProgress = nullptr,
                                          MeasurementTelemetry* outTelemetry = nullptr) = 0;

  // NUR fuer Debug-/Analysezwecke: ein Klartext-Label je Measurement-Element,
  // in derselben Reihenfolge und Laenge wie ein Measurement desselben Sensors.
  // Darf NICHT von anderem Code benutzt werden, um den INHALT eines
  // Measurements zu interpretieren (das waere ein Bruch der Abstraktion) --
  // einzig zur menschenlesbaren Beschriftung beim Debug-Export gedacht.
  virtual const char* const* measurementLabels() const = 0;

  // Reine Berechnung: baut ein sensor-unabhaengiges Spectrum aus einer
  // Messung plus Weiss-/Dunkelreferenzmessung (gleiches Measurement-Format).
  // Kein Hardwarezugriff, keine gespeicherte Kalibrierung -- jeder Aufruf ist
  // eine reine Funktion seiner drei Eingaben. Wie eine leere/fehlende
  // Referenz zu behandeln ist, entscheidet die jeweilige Implementierung.
  virtual Spectrum getSpectrum(const Measurement& measurement,
                                const Measurement& whiteReference,
                                const Measurement& darkReference,
                                FilterState filterState) const = 0;
};
