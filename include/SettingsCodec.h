#pragma once
#include <Adafruit_AS7341.h>
#include "Spectrometer.h"

// Text representation of FilterState/Gain -- ONE shared source for
// both the CSV export (main.cpp) and the JSON persistence
// (CalibrationStore.cpp), so that the two never drift apart. The
// *FromCsvLabel() direction is robust: unknown/empty string -> false,
// 'out' stays unchanged (caller then falls back to a struct default,
// see CalibrationStore.cpp) -- deliberately no silent fallback to a
// guessed value.

const char* filterStateCsvLabel(FilterState fs);                  // "none" | "650nm" | "700nm"
bool filterStateFromCsvLabel(const char* s, FilterState& out);

const char* gainCsvLabel(as7341_gain_t g);                        // "0.5X" .. "512X"
bool gainFromCsvLabel(const char* s, as7341_gain_t& out);
