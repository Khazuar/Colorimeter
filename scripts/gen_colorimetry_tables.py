"""Generates the constants in include/ColorimetryTables.h.

Not part of the firmware, not compiled. Run once and manually transfer the
output into the respective .cpp file if the tables ever need to be
changed (e.g. different grid, different illuminant).

Requires: pip install colour-science numpy
"""
import numpy as np
import colour

# ---------------------------------------------------------------
# 1) CIE 1931 2-degree CMF + D50 SPD, 5nm grid 380-730nm (71 points)
# ---------------------------------------------------------------
shape = colour.SpectralShape(380, 730, 5)
cmfs = colour.MSDS_CMFS["CIE 1931 2 Degree Standard Observer"].copy().align(shape)
d50 = colour.SDS_ILLUMINANTS["D50"].copy().align(shape)
wavelengths = shape.range()

print("N_WL =", len(wavelengths))

print("\n// CIE_CMF[i] = { xbar, ybar, zbar }, 5nm-Raster 380-730nm")
print("const float CIE_CMF[CIE_N_WL][3] = {")
for wl in wavelengths:
    xb, yb, zb = cmfs[wl]
    print(f"  {{ {xb:.6f}f, {yb:.6f}f, {zb:.6f}f }}, // {int(wl)}nm")
print("};")

print("\n// CIE_D50_SPD[i], 5nm-Raster 380-730nm (relative Leistung)")
print("const float CIE_D50_SPD[CIE_N_WL] = {")
vals = [d50[wl] for wl in wavelengths]
for i in range(0, len(vals), 8):
    chunk = vals[i:i + 8]
    print("  " + ", ".join(f"{v:.6f}f" for v in chunk) + ",")
print("};")

# ---------------------------------------------------------------
# 2) D50 white point tristimulus (Y=100 scale)
# ---------------------------------------------------------------
wp_xy_D50 = colour.CCS_ILLUMINANTS["CIE 1931 2 Degree Standard Observer"]["D50"]
wp_XYZ_D50 = colour.xy_to_XYZ(wp_xy_D50) * 100.0
print("\n// D50-Weisspunkt (Y=100-Skala)")
print(f"Xn={wp_XYZ_D50[0]:.6f}, Yn={wp_XYZ_D50[1]:.6f}, Zn={wp_XYZ_D50[2]:.6f}")

# ---------------------------------------------------------------
# 3) XYZ(D50, Y=1) -> linear sRGB matrix (Bradford D50->D65 baked in)
# ---------------------------------------------------------------
sRGB = colour.RGB_COLOURSPACES["sRGB"]
XYZ_w_D50 = colour.xy_to_XYZ(wp_xy_D50)
XYZ_w_D65 = colour.xy_to_XYZ(sRGB.whitepoint)


def xyz_to_srgb_D50(XYZ_1):
    return colour.XYZ_to_RGB(
        np.array(XYZ_1), wp_xy_D50, sRGB.whitepoint,
        matrix_XYZ_to_RGB=sRGB.matrix_XYZ_to_RGB,
        chromatic_adaptation_transform="Bradford",
    )


I = np.eye(3)
cols = [xyz_to_srgb_D50(I[i]) for i in range(3)]
M = np.array(cols).T
print("\n// XYZ(D50, Y=1) -> linear sRGB-Matrix (Bradford D50->D65 fest eingerechnet)")
for row in M:
    print("  " + " ".join(f"{v:+.7f}f" for v in row))

white_XYZ1 = wp_XYZ_D50 / 100.0
print("Sanity-Check Weisspunkt -> linear rgb (sollte ~[1,1,1] sein):",
      xyz_to_srgb_D50(white_XYZ1))
