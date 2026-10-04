#include "DefaultColorDerivationModel.h"
#include "ColorimetryTables.h"
#include <cmath>

// FWHM (full width at half maximum) of a normal distribution ->
// standard deviation: FWHM = 2*sqrt(2*ln2)*sigma.
static const float FWHM_TO_SIGMA = 1.0f / 2.35482f;

void spectrumToXYZ(const Band* bands, const float* values, int n,
                    float& X, float& Y, float& Z) {
  X = Y = Z = 0.0f;
  float k = 0.0f;
  for (int i = 0; i < CIE_N_WL; i++) {
    float wl = (float)(CIE_WL_MIN + i * CIE_WL_STEP);

    // Gaussian-weighted sum over all bands (see header comment) instead of
    // linear interpolation between band centers -- each band contributes to R(wl)
    // according to its own (nearly Gaussian) sensitivity curve,
    // not just the two numerically nearest ones. Strictly
    // normalized (division by the sum of weights): R(wl) is therefore at
    // EVERY sample point a true weighted average of the band values, and
    // a constant spectrum (R=c in all bands) stays exactly c everywhere
    // -- an ideal gray yields a*=b*=0. Outside the bands this holds the
    // value of the nearest band instead of decaying toward 0. Previously
    // the denominator was floored at a minimum of 1; that pulled R toward 0 at
    // the edges (especially 650-700nm without F8, < 405nm) and measurably
    // distorted the color (perfect white with no filter: a* -4.7; with the
    // 700nm filter: b* +1.7), because the standard observer curves are not
    // yet negligible there.
    float weightedSum = 0.0f, weightTotal = 0.0f;
    for (int j = 0; j < n; j++) {
      float sigma = bands[j].fwhm_nm * FWHM_TO_SIGMA;
      if (sigma <= 0.0f) continue;
      float d = (wl - bands[j].center_nm) / sigma;
      float w = expf(-0.5f * d * d);
      weightedSum += w * values[j];
      weightTotal += w;
    }
    // Far outside all bands the weights become tiny, but their ratio
    // remains exact -- hence NO minimum threshold (a threshold like
    // 1e-12 would e.g. incorrectly set R to 0 at 730nm in 650nm mode). Only
    // when all weights underflow to exactly 0 is the value of the
    // nearest band held.
    float R;
    if (weightTotal > 0.0f) {
      R = weightedSum / weightTotal;
    } else {
      int nearest = -1;
      float bestDist = 0.0f;
      for (int j = 0; j < n; j++) {
        if (bands[j].fwhm_nm <= 0.0f) continue;
        float dist = fabsf(wl - bands[j].center_nm);
        if (nearest < 0 || dist < bestDist) { nearest = j; bestDist = dist; }
      }
      R = (nearest >= 0) ? values[nearest] : 0.0f;
    }

    float illum = CIE_D50_SPD[i];
    float xb = CIE_CMF[i][0], yb = CIE_CMF[i][1], zb = CIE_CMF[i][2];
    X += R * illum * xb;
    Y += R * illum * yb;
    Z += R * illum * zb;
    k += illum * yb;
  }
  float scale = 100.0f / k;   // perfect diffuser (R=1 everywhere) -> Y=100
  X *= scale; Y *= scale; Z *= scale;
}

Lab DefaultColorDerivationModel::deriveColor(const Spectrum& spectrum) const {
  float X, Y, Z;
  spectrumToXYZ(spectrum.bands.data(), spectrum.values.data(), (int)spectrum.bands.size(), X, Y, Z);
  return xyzToLab(X, Y, Z);
}
