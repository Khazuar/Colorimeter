"""
AS7341 color pipeline: raw counts -> reflectance -> XYZ -> Lab.

Bootstrap WITHOUT a color target: instead of a guessed ams matrix, the spectrum
is reconstructed from the 8 VIS channels and integrated against D50 + CIE 1931
2-degree. This is exactly the "spectral reconstruction" that ams names as an
alternative to the matrix - and it needs no secret coefficients.

As soon as you have known references (color patches with target Lab values),
replace the bootstrap with fit_matrix() -> then the current LED is
automatically calibrated in as well.

Dependencies:  pip install colour-science pandas numpy
CSV format (from the firmware):
    label,F1_415nm,F2_445nm,F3_480nm,F4_515nm,F5_555nm,F6_590nm,F7_630nm,F8_680nm,Clear,NIR_910nm
    dark,...      <- dark trap
    white,...     <- white reference
    sample_01,... <- samples
"""

import numpy as np
import pandas as pd
import colour

# --- AS7341 VIS channels: column name -> center wavelength (nm) ---
VIS = {
    "F1_415nm": 415, "F2_445nm": 445, "F3_480nm": 480, "F4_515nm": 515,
    "F5_555nm": 555, "F6_590nm": 590, "F7_630nm": 630, "F8_680nm": 680,
}
VIS_COLS = list(VIS.keys())
CENTERS  = np.array(list(VIS.values()), dtype=float)

CMFS   = colour.MSDS_CMFS["CIE 1931 2 Degree Standard Observer"]
ILLUM  = colour.SDS_ILLUMINANTS["D50"]
WP_XY  = colour.CCS_ILLUMINANTS["CIE 1931 2 Degree Standard Observer"]["D50"]
GRID   = np.arange(380, 731, 1.0)   # reconstruction grid


# ---------------------------------------------------------------------------
# 1) Reading + dark/white normalization
# ---------------------------------------------------------------------------
def load(csv_path):
    df = pd.read_csv(csv_path, comment="#")
    df["label"] = df["label"].astype(str)
    return df


def reference_vectors(df):
    """Averages all dark and white rows respectively into one vector each (all channels)."""
    chan = VIS_COLS + ["Clear", "NIR_910nm"]
    dark  = df[df["label"] == "dark"][chan].mean().values
    white = df[df["label"] == "white"][chan].mean().values
    if np.any(np.isnan(dark)):  raise ValueError("keine 'dark'-Zeile gefunden")
    if np.any(np.isnan(white)): raise ValueError("keine 'white'-Zeile gefunden")
    return dark, white, chan


def reflectance(row_vals, dark, white):
    """Per-channel reflectance R_i = (S-D)/(W-D), clamped to >=0."""
    denom = (white - dark)
    denom[denom == 0] = np.nan
    R = (row_vals - dark) / denom
    return np.clip(R, 0.0, None)


# ---------------------------------------------------------------------------
# 2) Bootstrap: 8 VIS reflectances -> spectrum -> XYZ -> Lab
# ---------------------------------------------------------------------------
def vis_reflectance_to_XYZ(R_vis):
    """R_vis: 8 reflectance values at the band centers -> XYZ (Y=100 for white)."""
    # linear interpolation between the band centers, flat extrapolation outside
    R_grid = np.interp(GRID, CENTERS, R_vis)
    sd = colour.SpectralDistribution(dict(zip(GRID, R_grid)))
    # Integration method is robust against arbitrary grids (no E308 requirement)
    XYZ = colour.sd_to_XYZ(sd, CMFS, ILLUM, method="Integration")
    return XYZ  # scale: perfect diffuser -> Y=100


def XYZ_to_Lab(XYZ):
    return colour.XYZ_to_Lab(XYZ / 100.0, WP_XY)


def XYZ_to_sRGB255(XYZ):
    rgb = colour.XYZ_to_sRGB(XYZ / 100.0, illuminant=WP_XY)
    return np.clip(rgb, 0, 1) * 255


# ---------------------------------------------------------------------------
# 3) Later: fit your own matrix (once references are available)
# ---------------------------------------------------------------------------
def fit_matrix(R, XYZ_ref):
    """
    R:       (N,8) reflectance vectors of your reference patches
    XYZ_ref: (N,3) known XYZ (Y=100 scale) of the same patches
    returns  M (9,3):  XYZ ~ [R, 1] @ M   (with bias term)
    For more accuracy: extend R beforehand with root-polynomial terms
    (Finlayson root-polynomial) and then solve it the same way linearly.
    """
    A = np.hstack([R, np.ones((len(R), 1))])
    M, *_ = np.linalg.lstsq(A, XYZ_ref, rcond=None)
    return M


def apply_matrix(R, M):
    A = np.hstack([np.atleast_2d(R), np.ones((len(np.atleast_2d(R)), 1))])
    return A @ M


def delta_E_report(Lab_pred, Lab_ref):
    dE = colour.delta_E(Lab_pred, Lab_ref, method="CIE 2000")
    return dict(mean=float(np.mean(dE)), max=float(np.max(dE)),
                rms=float(np.sqrt(np.mean(dE**2))))


# ---------------------------------------------------------------------------
# Process flow
# ---------------------------------------------------------------------------
def process(csv_path):
    df = load(csv_path)
    dark, white, chan = reference_vectors(df)
    idx_nir   = chan.index("NIR_910nm")
    idx_clear = chan.index("Clear")

    # Diagnostic: internal stray-light/NIR residual of the white reference (info only, not a color input)
    print(f"# Weiss-NIR/Clear-Verhaeltnis: {white[idx_nir]/white[idx_clear]:.3f} "
          f"(hoch -> NIR-Leckage/IR-Anteil pruefen)\n")

    print("label       L*     a*     b*    (sRGB Vorschau)")
    for _, r in df.iterrows():
        if r["label"] in ("dark", "white"):
            continue
        vals = r[chan].values.astype(float)
        R = reflectance(vals, dark, white)          # all channels
        R_vis = R[[chan.index(c) for c in VIS_COLS]]  # only the 8 VIS
        XYZ = vis_reflectance_to_XYZ(R_vis)
        L, a, b = XYZ_to_Lab(XYZ)
        rgb = XYZ_to_sRGB255(XYZ).astype(int)
        print(f"{r['label']:<10} {L:6.1f} {a:6.1f} {b:6.1f}   rgb{tuple(rgb)}")


if __name__ == "__main__":
    import sys
    process(sys.argv[1] if len(sys.argv) > 1 else "messung.csv")