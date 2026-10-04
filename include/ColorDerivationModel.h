#pragma once
#include "Spectrometer.h"

// Interface for deriving a Lab color from a sensor-independent Spectrum.
// Different implementations can use different underlying math (see
// DefaultColorDerivationModel for the one currently in use) and may later
// be restricted to specific OpticalSettings -- first step toward a
// user-selectable list of models in the settings tree, see Plan/context.
// Deliberately a single method for now; nothing beyond that is needed yet.
class ColorDerivationModel {
public:
  virtual ~ColorDerivationModel() = default;
  virtual Lab deriveColor(const Spectrum& spectrum) const = 0;
};
