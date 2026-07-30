"""
AS7341 Farb-Pipeline: Rohcounts -> Reflexion -> XYZ -> Lab.

Bootstrap OHNE Farbtarget: statt einer erratenen ams-Matrix wird das Spektrum
aus den 8 VIS-Kanaelen rekonstruiert und gegen D50 + CIE-1931-2deg integriert.
Das ist genau die "Spektralrekonstruktion", die ams als Alternative zur Matrix
nennt - und es braucht keine geheimen Koeffizienten.

Sobald du bekannte Referenzen (Farbfelder mit Soll-Lab) hast, ersetzt du den
Bootstrap durch fit_matrix() -> dann wird die aktuelle LED automatisch mit
einkalibriert.

Abhaengigkeiten:  pip install colour-science pandas numpy
CSV-Format (aus der Firmware):
    label,F1_415nm,F2_445nm,F3_480nm,F4_515nm,F5_555nm,F6_590nm,F7_630nm,F8_680nm,Clear,NIR_910nm
    dark,...      <- Dunkelfalle
    white,...     <- Weissreferenz
    sample_01,... <- Proben
"""

import numpy as np
import pandas as pd
import colour

# --- AS7341 VIS-Kanaele: Spaltenname -> Mittenwellenlaenge (nm) ---
VIS = {
    "F1_415nm": 415, "F2_445nm": 445, "F3_480nm": 480, "F4_515nm": 515,
    "F5_555nm": 555, "F6_590nm": 590, "F7_630nm": 630, "F8_680nm": 680,
}
VIS_COLS = list(VIS.keys())
CENTERS  = np.array(list(VIS.values()), dtype=float)

CMFS   = colour.MSDS_CMFS["CIE 1931 2 Degree Standard Observer"]
ILLUM  = colour.SDS_ILLUMINANTS["D50"]
WP_XY  = colour.CCS_ILLUMINANTS["CIE 1931 2 Degree Standard Observer"]["D50"]
GRID   = np.arange(380, 731, 1.0)   # Rekonstruktions-Raster


# ---------------------------------------------------------------------------
# 1) Einlesen + Dunkel/Weiss-Normierung
# ---------------------------------------------------------------------------
def load(csv_path):
    df = pd.read_csv(csv_path, comment="#")
    df["label"] = df["label"].astype(str)
    return df


def reference_vectors(df):
    """Mittelt alle dark- bzw. white-Zeilen zu je einem Vektor (alle Kanaele)."""
    chan = VIS_COLS + ["Clear", "NIR_910nm"]
    dark  = df[df["label"] == "dark"][chan].mean().values
    white = df[df["label"] == "white"][chan].mean().values
    if np.any(np.isnan(dark)):  raise ValueError("keine 'dark'-Zeile gefunden")
    if np.any(np.isnan(white)): raise ValueError("keine 'white'-Zeile gefunden")
    return dark, white, chan


def reflectance(row_vals, dark, white):
    """Kanalweise Reflexion R_i = (S-D)/(W-D), auf >=0 geklemmt."""
    denom = (white - dark)
    denom[denom == 0] = np.nan
    R = (row_vals - dark) / denom
    return np.clip(R, 0.0, None)


# ---------------------------------------------------------------------------
# 2) Bootstrap: 8 VIS-Reflexionen -> Spektrum -> XYZ -> Lab
# ---------------------------------------------------------------------------
def vis_reflectance_to_XYZ(R_vis):
    """R_vis: 8 Reflexionswerte an den Bandmitten -> XYZ (Y=100 fuer Weiss)."""
    # lineare Interpolation zwischen den Bandmitten, flache Extrapolation aussen
    R_grid = np.interp(GRID, CENTERS, R_vis)
    sd = colour.SpectralDistribution(dict(zip(GRID, R_grid)))
    # Integration-Methode ist robust gegen beliebige Raster (kein E308-Zwang)
    XYZ = colour.sd_to_XYZ(sd, CMFS, ILLUM, method="Integration")
    return XYZ  # Skala: perfekter Diffusor -> Y=100


def XYZ_to_Lab(XYZ):
    return colour.XYZ_to_Lab(XYZ / 100.0, WP_XY)


def XYZ_to_sRGB255(XYZ):
    rgb = colour.XYZ_to_sRGB(XYZ / 100.0, illuminant=WP_XY)
    return np.clip(rgb, 0, 1) * 255


# ---------------------------------------------------------------------------
# 3) Spaeter: eigene Matrix fitten (wenn Referenzen vorhanden)
# ---------------------------------------------------------------------------
def fit_matrix(R, XYZ_ref):
    """
    R:       (N,8) Reflexionsvektoren deiner Referenzfelder
    XYZ_ref: (N,3) bekannte XYZ (Y=100-Skala) derselben Felder
    liefert  M (9,3):  XYZ ~ [R, 1] @ M   (mit Bias-Term)
    Fuer mehr Genauigkeit: R vorher um Wurzel-Polynom-Terme erweitern
    (Finlayson root-polynomial) und dann genauso linear loesen.
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
# Ablauf
# ---------------------------------------------------------------------------
def process(csv_path):
    df = load(csv_path)
    dark, white, chan = reference_vectors(df)
    idx_nir   = chan.index("NIR_910nm")
    idx_clear = chan.index("Clear")

    # Diagnose: interner Streulicht-/NIR-Rest der Weissreferenz (Info, kein Farb-Input)
    print(f"# Weiss-NIR/Clear-Verhaeltnis: {white[idx_nir]/white[idx_clear]:.3f} "
          f"(hoch -> NIR-Leckage/IR-Anteil pruefen)\n")

    print("label       L*     a*     b*    (sRGB Vorschau)")
    for _, r in df.iterrows():
        if r["label"] in ("dark", "white"):
            continue
        vals = r[chan].values.astype(float)
        R = reflectance(vals, dark, white)          # alle Kanaele
        R_vis = R[[chan.index(c) for c in VIS_COLS]]  # nur die 8 VIS
        XYZ = vis_reflectance_to_XYZ(R_vis)
        L, a, b = XYZ_to_Lab(XYZ)
        rgb = XYZ_to_sRGB255(XYZ).astype(int)
        print(f"{r['label']:<10} {L:6.1f} {a:6.1f} {b:6.1f}   rgb{tuple(rgb)}")


if __name__ == "__main__":
    import sys
    process(sys.argv[1] if len(sys.argv) > 1 else "messung.csv")