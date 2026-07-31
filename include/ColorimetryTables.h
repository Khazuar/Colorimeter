#pragma once
#include <cstdint>
#include "Spectrometer.h"

// CIE 1931 2-Grad-Normalbeobachter + D50-Illuminant, 5nm-Raster 380-730nm.
// Werte wurden mit dem Python-Paket "colour-science" (colour.MSDS_CMFS["CIE 1931
// 2 Degree Standard Observer"], colour.SDS_ILLUMINANTS["D50"]) generiert, nicht
// von Hand abgetippt -- siehe scripts/gen_colorimetry_tables.py.
static const int CIE_WL_MIN  = 380;
static const int CIE_WL_MAX  = 730;
static const int CIE_WL_STEP = 5;
static const int CIE_N_WL    = (CIE_WL_MAX - CIE_WL_MIN) / CIE_WL_STEP + 1; // 71

extern const float CIE_CMF[CIE_N_WL][3];   // x-bar, y-bar, z-bar
extern const float CIE_D50_SPD[CIE_N_WL];  // D50 relative spektrale Leistungsverteilung

// D50-Weisspunkt, Y=100-Skala (aus colour.CCS_ILLUMINANTS[...]["D50"] via xy_to_XYZ)
static const float D50_XN = 96.429568f;
static const float D50_YN = 100.000000f;
static const float D50_ZN = 82.510460f;

// Integriert R(lambda) (linear interpoliert an CIE_CMF-Stuetzstellen) gegen
// CIE_CMF*CIE_D50_SPD zu XYZ, normalisiert so dass ein perfekter Diffusor (R=1
// ueberall) auf Y=100 abgebildet wird.
void spectrumToXYZ(const float* wavelengths_nm, const float* values, int n,
                    float& X, float& Y, float& Z);

Lab  xyzToLab(float X, float Y, float Z);
void xyzToSRGB255(float X, float Y, float Z, uint8_t& r, uint8_t& g, uint8_t& b);

// Ruecktransformation fuer die Hex/RGB-Anzeige -- arbeitet nur auf dem von
// getColor() gelieferten Lab, also sensor-unabhaengig.
void labToXYZ(const Lab& lab, float& X, float& Y, float& Z);
void labToSRGB255(const Lab& lab, uint8_t& r, uint8_t& g, uint8_t& b);
