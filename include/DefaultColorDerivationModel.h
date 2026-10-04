#pragma once
#include "ColorDerivationModel.h"

// Integrates R(lambda) against CIE_CMF*CIE_D50_SPD (see ColorimetryTables.h)
// into XYZ, normalized so that a perfect diffuser (R=1 everywhere) maps to
// Y=100. R(lambda) is reconstructed at each CIE_CMF sample point from ALL
// bands (weight per band = a Gaussian curve around its center_nm with a
// width derived from fwhm_nm) instead of interpolating linearly between
// band centers -- per-band sensitivity curves of typical sensors are
// themselves nearly Gaussian, which reproduces the actual physical band
// shape instead of replacing it with straight line segments. In the
// overlapping core region of the bands this yields a true weighted average;
// outside the bands it is strictly normalized, so R(lambda) there holds the
// value of the nearest band (see the .cpp). A constant spectrum therefore
// yields an exactly neutral color. This Gaussian band-shape assumption is
// this model's own; it does not belong in ColorimetryTables, which stays
// generic CIE math with no sensor/band-shape assumptions of any kind.
void spectrumToXYZ(const Band* bands, const float* values, int n,
                    float& X, float& Y, float& Z);

// The original (and always available/applicable) color derivation:
// Gaussian-weighted CIE 1931/D50 integration over the spectrum's bands (see
// spectrumToXYZ above), followed by the generic XYZ->Lab conversion
// (ColorimetryTables::xyzToLab). Meant as the always-selectable fallback
// once multiple ColorDerivationModels exist. Sensor-independent: operates
// only on the already sensor-abstracted Spectrum (bands + values), not on
// any concrete sensor's raw channels. Not a singleton itself -- instantiated
// exactly once by ColorDerivationModelRegistry at boot (see main.cpp::setup()).
class DefaultColorDerivationModel : public ColorDerivationModel {
public:
  Lab deriveColor(const Spectrum& spectrum) const override;
};
