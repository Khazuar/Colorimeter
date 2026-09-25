#pragma once
#include <string>
#include <vector>
#include "AppConfig.h"

struct TipCatalog;  // siehe MeasurementTip::isPlausible() weiter unten

// Ergebnis von MeasurementTip::isPlausible() -- ob eine neue Weissmessung zu
// den bisher fuer diese Spitze (und aktuelle whiteReferenceGeneration)
// gesammelten Fingerabdruecken passt. PlausibleViaFallback = plausibel, aber
// nur anhand einer aus Fremddaten geschaetzten Streuung beurteilt (zu wenige
// eigene Fingerabdruecke fuer eine direkte Schaetzung, siehe isPlausible()) --
// fuer die UI heute gleichbedeutend mit Plausible (nur Implausible/
// Indeterminate unterbrechen den Messablauf, siehe main.cpp).
enum class PlausibilityResult : uint8_t { Plausible, PlausibleViaFallback, Implausible, Indeterminate };

// Ergebnis von MeasurementTip::whiteFingerprintStats() -- je Kanal Mittelwert
// und ABSOLUTER Standardfehler des Mittelwerts (SEM, gleiche Einheit/
// Groessenordnung wie 'mean' -- bewusst NICHT relativ/prozentual: die
// normalisierten Werte selbst reichen ueber viele Groessenordnungen, ein
// Prozentwert allein macht die absolute Groessenordnung nicht sichtbar, siehe
// main.cpp::renderWhiteFingerprintStats()) ueber die eigenen Fingerabdruecke
// EINER Spitze+Generation+Sensor-Kombination (siehe isPlausible() fuer die
// Begruendung, warum nur diese untereinander vergleichbar sind). 'mean'/'sem'
// sind leer, wenn n == 0; 'sem' bleibt zusaetzlich leer bei n == 1
// (Standardfehler dort nicht definiert) -- Anzeige-Code (main.cpp) muss beide
// Faelle abfangen.
struct WhiteFingerprintStats {
  size_t n = 0;
  std::vector<float> mean;
  std::vector<float> sem;
};

// Ein Fingerabdruck: normalisierte Rohmessung (siehe Spectrometer::normalize())
// + Zeitpunkt (main.cpp::uptimeLogger, gleiche Konvention wie
// HistoryStore::MeasurementRecord::uptimeS) + die zum Messzeitpunkt aktiven
// SensorSettings (eingefroren, NICHT die live editierbaren -- gleiche
// Asymmetrie wie AppConfig.h::RootSettings vs. main.cpp::lastMeasurementSettings)
// fuer spaetere Ausreisser-Diagnose (z.B. "war der Gain hier ungewoehnlich
// niedrig" statt vorschnell auf eine physische Veraenderung der Spitze zu
// schliessen) + der zum Messzeitpunkt gueltige Stand von
// TipCatalog::whiteReferenceGeneration (siehe dort) -- damit spaeter erkennbar
// ist, ob dieser Fingerabdruck noch mit der AKTUELL benutzten physischen
// Weissreferenz vergleichbar ist oder von einer frueheren stammt -- sowie
// die sensorId (siehe Spectrometer::sensorId()) des Sensors, der ihn erzeugt
// hat: 'normalized' ist ein opakes, sensorspezifisches Measurement (siehe
// Spectrometer.h), zwei Fingerabdruecke sind nur bei UEBEREINSTIMMENDER
// sensorId ueberhaupt vergleichbar (ein reiner Laengenvergleich waere keine
// echte Garantie, siehe MeasurementTip::isPlausible()).
struct WhiteFingerprint {
  uint32_t uptimeS = 0;
  uint32_t generation = 0;
  std::string sensorId;
  SensorSettings sensor;
  Measurement normalized;
};

// Aeltester Eintrag faellt raus, sobald ein neuer dazukommt und die Liste
// bereits MAX_WHITE_FINGERPRINTS_PER_TIP Eintraege haette (siehe
// main.cpp::appendWhiteFingerprint()).
static const size_t MAX_WHITE_FINGERPRINTS_PER_TIP = 20;

// Ab wie vielen eigenen Fingerabdruecken der AKTUELLEN Generation
// MeasurementTip::isPlausible() Mittelwert UND Streuung direkt daraus schaetzt
// (statt auf eine Fremd-Stichprobe fuer die Streuung auszuweichen). 5 statt
// weniger, weil eine Stichproben-Standardabweichung bei nur 2-3
// Freiheitsgraden noch sehr instabil ist.
static const uint8_t MIN_FINGERPRINTS_FOR_DIRECT_STATS = 5;

// Ab wie vielen (ueber mehrere vergleichbare Gruppen gepoolten) Eintraegen
// eine Fallback-Stichprobe ueberhaupt als Grundlage fuer eine
// Streuungsschaetzung benutzt wird -- siehe isPlausible().
static const uint8_t MIN_FINGERPRINTS_FOR_FALLBACK_STATS = 5;

// Eine bekannte Messspitze: eindeutiger Name (aktuell autogeneriert,
// "Messspitze N") + das OpticalSettings, das beim Aktivieren dieser Spitze
// automatisch geladen/angewendet wird (siehe main.cpp::activateTip()) + die
// letzten Weissreferenz-"Fingerabdruecke" dieser Spitze (siehe
// WhiteFingerprint). Weitere Meta-/Diagnosedaten (kein Teil von
// OpticalSettings) kommen hier kuenftig als weitere Felder dazu.
struct MeasurementTip {
  std::string name;
  OpticalSettings optical;
  std::vector<WhiteFingerprint> whiteFingerprints;

  // Prueft, ob 'candidate' (eine gerade aufgenommene, noch nicht angehaengte
  // Weissmessung, siehe main.cpp::buildWhiteFingerprint()) zu den bisherigen
  // Fingerabdruecken DIESER Spitze passt. 'catalog' wird nur fuer den
  // Fallback-Fall gebraucht (Streuungsschaetzung aus Fingerabdruecken anderer
  // Spitzen) -- siehe TipCatalog.cpp fuer die volle Herleitung.
  PlausibilityResult isPlausible(const WhiteFingerprint& candidate, const TipCatalog& catalog) const;

  // Mittelwert + relativer Standardfehler je Kanal ueber die eigenen
  // Fingerabdruecke der gegebenen Generation/sensorId (typischerweise die
  // aktuelle TipCatalog::whiteReferenceGeneration und der Sensor, der gerade
  // misst) -- fuer den "Weiss-Fingerabdruck"-Anzeige-Screen (main.cpp).
  WhiteFingerprintStats whiteFingerprintStats(const std::string& sensorId, uint32_t generation) const;
};

// Alle bekannten Messspitzen PLUS welche davon aktiv ist -- bewusst EIN
// zusammenhaengendes, atomar persistiertes Dokument (CalibrationStore::
// save/loadTips(), schema/tips.schema.json): "welche Spitze aktiv ist" ist
// eine Eigenschaft DES KATALOGS (welche der hier gelisteten Optionen gerade
// gewaehlt ist), keine RootSettings-Einstellung -- RootSettings kennt das
// Konzept "Spitze" gar nicht. Invariante (von main.cpp aufrechterhalten):
// tips ist NIE leer, 'active' bezeichnet IMMER einen existierenden Eintrag.
struct TipCatalog {
  std::string active;
  std::vector<MeasurementTip> tips;

  // Erhoeht sich NUR ueber die UI-Aktion "Fingerabdruecke invalidieren" (siehe
  // main.cpp::invalidateFingerprints()) -- z.B. wenn das physische
  // Weissreferenz-Material gewechselt wird und dadurch ALLE bisherigen
  // Fingerabdruecke (ueber alle Spitzen hinweg, deshalb hier am Katalog statt
  // je Spitze) nicht mehr mit kuenftigen vergleichbar sind. Ein
  // WhiteFingerprint mit einem KLEINEREN gespeicherten generation-Wert als
  // dieser hier gilt als veraltet. Reine Datengrundlage -- die eigentliche
  // Erkennung/Reaktion auf veraltete Fingerabdruecke ist bewusst noch nicht
  // Teil dieses Schritts.
  uint32_t whiteReferenceGeneration = 0;

  MeasurementTip* find(const std::string& name) {
    for (auto& t : tips) if (t.name == name) return &t;
    return nullptr;
  }
  const MeasurementTip* find(const std::string& name) const {
    for (auto& t : tips) if (t.name == name) return &t;
    return nullptr;
  }
  MeasurementTip* activeTip() { return find(active); }
  const MeasurementTip* activeTip() const { return find(active); }
};
