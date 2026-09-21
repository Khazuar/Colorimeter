#pragma once
#include <Adafruit_AS7341.h>
#include "Spectrometer.h"

// Text-Repraesentation von FilterState/Gain -- EINE gemeinsame Quelle fuer
// sowohl den CSV-Export (main.cpp) als auch die JSON-Persistenz
// (CalibrationStore.cpp), damit beide niemals auseinanderlaufen. Die
// *FromCsvLabel()-Richtung ist robust: unbekannter/leerer String -> false,
// 'out' bleibt unveraendert (Aufrufer faellt dann auf einen Struct-Default
// zurueck, siehe CalibrationStore.cpp) -- absichtlich kein Rückfall auf einen
// stillschweigend geratenen Wert.

const char* filterStateCsvLabel(FilterState fs);                  // "none" | "650nm" | "700nm"
bool filterStateFromCsvLabel(const char* s, FilterState& out);

const char* gainCsvLabel(as7341_gain_t g);                        // "0.5X" .. "512X"
bool gainFromCsvLabel(const char* s, as7341_gain_t& out);
