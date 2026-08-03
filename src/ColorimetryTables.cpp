#include "ColorimetryTables.h"
#include <cmath>
#include <vector>

// Generiert mit colour-science (CIE 1931 2 Degree Standard Observer, D50),
// 5nm-Raster 380-730nm. Siehe ColorimetryTables.h fuer die Quelle.
const float CIE_CMF[CIE_N_WL][3] = {
  { 0.001368f, 0.000039f, 0.006450f }, // 380nm
  { 0.002236f, 0.000064f, 0.010550f }, // 385nm
  { 0.004243f, 0.000120f, 0.020050f }, // 390nm
  { 0.007650f, 0.000217f, 0.036210f }, // 395nm
  { 0.014310f, 0.000396f, 0.067850f }, // 400nm
  { 0.023190f, 0.000640f, 0.110200f }, // 405nm
  { 0.043510f, 0.001210f, 0.207400f }, // 410nm
  { 0.077630f, 0.002180f, 0.371300f }, // 415nm
  { 0.134380f, 0.004000f, 0.645600f }, // 420nm
  { 0.214770f, 0.007300f, 1.039050f }, // 425nm
  { 0.283900f, 0.011600f, 1.385600f }, // 430nm
  { 0.328500f, 0.016840f, 1.622960f }, // 435nm
  { 0.348280f, 0.023000f, 1.747060f }, // 440nm
  { 0.348060f, 0.029800f, 1.782600f }, // 445nm
  { 0.336200f, 0.038000f, 1.772110f }, // 450nm
  { 0.318700f, 0.048000f, 1.744100f }, // 455nm
  { 0.290800f, 0.060000f, 1.669200f }, // 460nm
  { 0.251100f, 0.073900f, 1.528100f }, // 465nm
  { 0.195360f, 0.090980f, 1.287640f }, // 470nm
  { 0.142100f, 0.112600f, 1.041900f }, // 475nm
  { 0.095640f, 0.139020f, 0.812950f }, // 480nm
  { 0.057950f, 0.169300f, 0.616200f }, // 485nm
  { 0.032010f, 0.208020f, 0.465180f }, // 490nm
  { 0.014700f, 0.258600f, 0.353300f }, // 495nm
  { 0.004900f, 0.323000f, 0.272000f }, // 500nm
  { 0.002400f, 0.407300f, 0.212300f }, // 505nm
  { 0.009300f, 0.503000f, 0.158200f }, // 510nm
  { 0.029100f, 0.608200f, 0.111700f }, // 515nm
  { 0.063270f, 0.710000f, 0.078250f }, // 520nm
  { 0.109600f, 0.793200f, 0.057250f }, // 525nm
  { 0.165500f, 0.862000f, 0.042160f }, // 530nm
  { 0.225750f, 0.914850f, 0.029840f }, // 535nm
  { 0.290400f, 0.954000f, 0.020300f }, // 540nm
  { 0.359700f, 0.980300f, 0.013400f }, // 545nm
  { 0.433450f, 0.994950f, 0.008750f }, // 550nm
  { 0.512050f, 1.000000f, 0.005750f }, // 555nm
  { 0.594500f, 0.995000f, 0.003900f }, // 560nm
  { 0.678400f, 0.978600f, 0.002750f }, // 565nm
  { 0.762100f, 0.952000f, 0.002100f }, // 570nm
  { 0.842500f, 0.915400f, 0.001800f }, // 575nm
  { 0.916300f, 0.870000f, 0.001650f }, // 580nm
  { 0.978600f, 0.816300f, 0.001400f }, // 585nm
  { 1.026300f, 0.757000f, 0.001100f }, // 590nm
  { 1.056700f, 0.694900f, 0.001000f }, // 595nm
  { 1.062200f, 0.631000f, 0.000800f }, // 600nm
  { 1.045600f, 0.566800f, 0.000600f }, // 605nm
  { 1.002600f, 0.503000f, 0.000340f }, // 610nm
  { 0.938400f, 0.441200f, 0.000240f }, // 615nm
  { 0.854450f, 0.381000f, 0.000190f }, // 620nm
  { 0.751400f, 0.321000f, 0.000100f }, // 625nm
  { 0.642400f, 0.265000f, 0.000050f }, // 630nm
  { 0.541900f, 0.217000f, 0.000030f }, // 635nm
  { 0.447900f, 0.175000f, 0.000020f }, // 640nm
  { 0.360800f, 0.138200f, 0.000010f }, // 645nm
  { 0.283500f, 0.107000f, 0.000000f }, // 650nm
  { 0.218700f, 0.081600f, 0.000000f }, // 655nm
  { 0.164900f, 0.061000f, 0.000000f }, // 660nm
  { 0.121200f, 0.044580f, 0.000000f }, // 665nm
  { 0.087400f, 0.032000f, 0.000000f }, // 670nm
  { 0.063600f, 0.023200f, 0.000000f }, // 675nm
  { 0.046770f, 0.017000f, 0.000000f }, // 680nm
  { 0.032900f, 0.011920f, 0.000000f }, // 685nm
  { 0.022700f, 0.008210f, 0.000000f }, // 690nm
  { 0.015840f, 0.005723f, 0.000000f }, // 695nm
  { 0.011359f, 0.004102f, 0.000000f }, // 700nm
  { 0.008111f, 0.002929f, 0.000000f }, // 705nm
  { 0.005790f, 0.002091f, 0.000000f }, // 710nm
  { 0.004109f, 0.001484f, 0.000000f }, // 715nm
  { 0.002899f, 0.001047f, 0.000000f }, // 720nm
  { 0.002049f, 0.000740f, 0.000000f }, // 725nm
  { 0.001440f, 0.000520f, 0.000000f }, // 730nm
};

const float CIE_D50_SPD[CIE_N_WL] = {
  24.488000f, 27.179000f, 29.871000f, 39.589000f, 49.308000f, 52.910000f, 56.513000f, 58.273000f,
  60.034000f, 58.926000f, 57.818000f, 66.321000f, 74.825000f, 81.036000f, 87.247000f, 88.930000f,
  90.612000f, 90.990000f, 91.368000f, 93.238000f, 95.109000f, 93.536000f, 91.963000f, 93.843000f,
  95.724000f, 96.169000f, 96.613000f, 96.871000f, 97.129000f, 99.614000f, 102.099000f, 101.427000f,
  100.755000f, 101.536000f, 102.317000f, 101.159000f, 100.000000f, 98.868000f, 97.735000f, 98.327000f,
  98.918000f, 96.208000f, 93.499000f, 95.593000f, 97.688000f, 98.478000f, 99.269000f, 99.155000f,
  99.042000f, 97.382000f, 95.722000f, 97.290000f, 98.857000f, 97.262000f, 95.667000f, 96.929000f,
  98.190000f, 100.597000f, 103.003000f, 101.068000f, 99.133000f, 93.257000f, 87.381000f, 89.492000f,
  91.604000f, 92.246000f, 92.889000f, 84.872000f, 76.854000f, 81.683000f, 86.511000f,
};

void spectrumToXYZ(const float* wavelengths_nm, const float* values, int n,
                    float& X, float& Y, float& Z) {
  X = Y = Z = 0.0f;
  float k = 0.0f;
  for (int i = 0; i < CIE_N_WL; i++) {
    float wl = (float)(CIE_WL_MIN + i * CIE_WL_STEP);

    // Linear interpolieren, flach extrapoliert ausserhalb [wavelengths_nm[0], wavelengths_nm[n-1]]
    float R;
    if (wl <= wavelengths_nm[0]) {
      R = values[0];
    } else if (wl >= wavelengths_nm[n - 1]) {
      R = values[n - 1];
    } else {
      int j = 0;
      while (j + 1 < n && wavelengths_nm[j + 1] < wl) j++;
      float x0 = wavelengths_nm[j], x1 = wavelengths_nm[j + 1];
      float y0 = values[j], y1 = values[j + 1];
      float t = (wl - x0) / (x1 - x0);
      R = y0 + t * (y1 - y0);
    }

    float illum = CIE_D50_SPD[i];
    float xb = CIE_CMF[i][0], yb = CIE_CMF[i][1], zb = CIE_CMF[i][2];
    X += R * illum * xb;
    Y += R * illum * yb;
    Z += R * illum * zb;
    k += illum * yb;
  }
  float scale = 100.0f / k;   // perfekter Diffusor (R=1 ueberall) -> Y=100
  X *= scale; Y *= scale; Z *= scale;
}

static inline float f_lab(float t) {
  const float d = 6.0f / 29.0f;
  return (t > d * d * d) ? cbrtf(t) : (t / (3.0f * d * d) + 4.0f / 29.0f);
}

Lab xyzToLab(float X, float Y, float Z) {
  float fx = f_lab(X / D50_XN), fy = f_lab(Y / D50_YN), fz = f_lab(Z / D50_ZN);
  return Lab{ 116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz) };
}

static inline float finv_lab(float t) {
  const float d = 6.0f / 29.0f;
  return (t > d) ? t * t * t : 3.0f * d * d * (t - 4.0f / 29.0f);
}

void labToXYZ(const Lab& lab, float& X, float& Y, float& Z) {
  float fy = (lab.L + 16.0f) / 116.0f;
  float fx = fy + lab.a / 500.0f;
  float fz = fy - lab.b / 200.0f;
  X = D50_XN * finv_lab(fx);
  Y = D50_YN * finv_lab(fy);
  Z = D50_ZN * finv_lab(fz);
}

static inline float srgbEncode(float c) {
  c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
  return (c <= 0.0031308f) ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

// XYZ(D50, Y=1-Skala) -> linear sRGB, Bradford-adaptierte Matrix (verifiziert
// per colour-science: D50-Weiss bildet auf [1,1,1] linear-RGB ab).
void xyzToSRGB255(float X, float Y, float Z, uint8_t& r, uint8_t& g, uint8_t& b) {
  float Xr = X / 100.0f, Yr = Y / 100.0f, Zr = Z / 100.0f;
  float rl =  3.1337773f * Xr - 1.6171927f * Yr - 0.4906675f * Zr;
  float gl = -0.9784631f * Xr + 1.9160784f * Yr + 0.0333878f * Zr;
  float bl =  0.0720232f * Xr - 0.2290022f * Yr + 1.4054279f * Zr;
  r = (uint8_t)roundf(srgbEncode(rl) * 255.0f);
  g = (uint8_t)roundf(srgbEncode(gl) * 255.0f);
  b = (uint8_t)roundf(srgbEncode(bl) * 255.0f);
}

void labToSRGB255(const Lab& lab, uint8_t& r, uint8_t& g, uint8_t& b) {
  float X, Y, Z;
  labToXYZ(lab, X, Y, Z);
  xyzToSRGB255(X, Y, Z, r, g, b);
}

Lab getColor(const Spectrum& spectrum) {
  size_t n = spectrum.bands.size();
  std::vector<float> wavelengths(n);
  for (size_t i = 0; i < n; i++) wavelengths[i] = spectrum.bands[i].center_nm;

  float X, Y, Z;
  spectrumToXYZ(wavelengths.data(), spectrum.values.data(), (int)n, X, Y, Z);
  return xyzToLab(X, Y, Z);
}
