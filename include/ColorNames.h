#pragma once
#include <cstddef>
#include "Spectrometer.h"

struct NamedColor {
  const char* name;
  Lab lab;
};

extern const NamedColor NAMED_COLORS[];
extern const size_t N_NAMED_COLORS;

// Naechstgelegene benannte Farbe per quadrierter euklidischer Distanz in Lab
// (Delta-E76). outDeltaE (falls != nullptr) erhaelt die tatsaechliche (nicht
// quadrierte) Distanz.
const char* nearestColorName(const Lab& c, float* outDeltaE = nullptr);
