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
