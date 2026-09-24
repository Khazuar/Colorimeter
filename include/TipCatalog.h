#pragma once
#include <string>
#include <vector>
#include "AppConfig.h"

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
// Weissreferenz vergleichbar ist oder von einer frueheren stammt.
struct WhiteFingerprint {
  uint32_t uptimeS = 0;
  uint32_t generation = 0;
  SensorSettings sensor;
  Measurement normalized;
};

// Aeltester Eintrag faellt raus, sobald ein neuer dazukommt und die Liste
// bereits MAX_WHITE_FINGERPRINTS_PER_TIP Eintraege haette (siehe
// main.cpp::addWhiteFingerprint()).
static const size_t MAX_WHITE_FINGERPRINTS_PER_TIP = 20;

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
