#pragma once
#include <string>
#include <vector>
#include "AppConfig.h"

// Eine bekannte Messspitze: eindeutiger Name (aktuell autogeneriert,
// "Messspitze N") + das OpticalSettings, das beim Aktivieren dieser Spitze
// automatisch geladen/angewendet wird (siehe main.cpp::activateTip()).
// Weitere Meta-/Diagnosedaten (kein Teil von OpticalSettings) kommen hier
// kuenftig als weitere Felder dazu.
struct MeasurementTip {
  std::string name;
  OpticalSettings optical;
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
