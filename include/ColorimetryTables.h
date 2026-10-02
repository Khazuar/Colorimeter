#pragma once
#include <cstdint>
#include "Spectrometer.h"

// CIE 1931 2-degree standard observer + D50 illuminant, 5nm grid 380-730nm.
// Values were generated with the Python package "colour-science" (colour.MSDS_CMFS["CIE 1931
// 2 Degree Standard Observer"], colour.SDS_ILLUMINANTS["D50"]), not
// typed by hand -- see scripts/gen_colorimetry_tables.py.
static const int CIE_WL_MIN  = 380;
static const int CIE_WL_MAX  = 730;
static const int CIE_WL_STEP = 5;
static const int CIE_N_WL    = (CIE_WL_MAX - CIE_WL_MIN) / CIE_WL_STEP + 1; // 71

extern const float CIE_CMF[CIE_N_WL][3];   // x-bar, y-bar, z-bar
extern const float CIE_D50_SPD[CIE_N_WL];  // D50 relative spectral power distribution

// D50 white point, Y=100 scale (from colour.CCS_ILLUMINANTS[...]["D50"] via xy_to_XYZ)
static const float D50_XN = 96.429568f;
static const float D50_YN = 100.000000f;
static const float D50_ZN = 82.510460f;

// Integrates R(lambda) against CIE_CMF*CIE_D50_SPD into XYZ, normalized so that
// a perfect diffuser (R=1 everywhere) maps to Y=100. R(lambda) is
// reconstructed at each CIE_CMF sample point from ALL bands (weight
// per band = a Gaussian curve around its center_nm with a width derived
// from fwhm_nm) instead of interpolating linearly between band centers --
// the AS7341 per-band sensitivity curves are themselves nearly Gaussian
// according to the datasheet, which reproduces the actual physical band shape
// instead of replacing it with straight line segments. In the overlapping core
// region of the bands this yields a true weighted average; outside the
// bands it is strictly normalized, so R(lambda) there holds the value of the
// nearest band (see spectrumToXYZ). A constant spectrum therefore
// yields an exactly neutral color.
void spectrumToXYZ(const Band* bands, const float* values, int n,
                    float& X, float& Y, float& Z);

Lab  xyzToLab(float X, float Y, float Z);
void xyzToSRGB255(float X, float Y, float Z, uint8_t& r, uint8_t& g, uint8_t& b);

// Inverse transform for the hex/RGB display -- operates only on Lab,
// i.e. sensor-independent.
void labToXYZ(const Lab& lab, float& X, float& Y, float& Z);
void labToSRGB255(const Lab& lab, uint8_t& r, uint8_t& g, uint8_t& b);

// Cylindrical representation of Lab (CIELCh): C* = chroma (color
// saturation, distance from the neutral axis), h = hue angle in degrees [0,360) (0=+a*/
// red, 90=+b*/yellow, 180=-a*/green, 270=-b*/blue). A pure conversion of the same
// information as a*/b*, not a new measurement -- often more
// intuitive for display than the cartesian a*/b* values.
struct LCh {
  float C;
  float h;
};
LCh labToLCh(const Lab& lab);

// Derives a Lab color from a sensor-independent Spectrum (Gaussian-
// weighted CIE integration over band center +- width, see spectrumToXYZ).
// Deliberately NOT part of the Spectrometer abstraction -- generic
// color science, depends on no sensor details, only on the (already
// calibrated) Spectrum.
Lab getColor(const Spectrum& spectrum);
