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
// schliessen).
struct WhiteFingerprint {
  uint32_t uptimeS = 0;
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
