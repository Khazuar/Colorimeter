#include "ColorNames.h"
#include <cmath>

// Lab-Werte (D50) generiert mit scripts/gen_colorimetry_tables.py aus sRGB-
// Referenzfarben (sRGB -> linear -> XYZ(D65) -> Bradford-Adaption D65->D50 ->
// Lab(D50)), damit sie zur selben Weisspunkt-Kette wie die Messwerte passen.
const NamedColor NAMED_COLORS[] = {
  { "White",   { 100.000f,  0.008f,   0.004f } },
  { "Black",   {   0.000f,  0.000f,   0.000f } },
  { "Gray",    {  53.585f,  0.005f,   0.002f } },
  { "Red",     {  54.287f, 80.825f,  69.913f } },
  { "Orange",  {  70.209f, 39.796f,  76.101f } },
  { "Yellow",  {  88.712f,  0.519f,  87.379f } },
  { "Green",   {  50.775f,-47.200f,  32.718f } },
  { "Cyan",    {  66.127f,-39.000f, -11.516f } },
  { "Blue",    {  31.210f, 31.460f, -77.889f } },
  { "Purple",  {  33.024f, 47.490f, -44.679f } },
  { "Magenta", {  46.136f, 68.848f, -16.850f } },
  { "Pink",    {  72.239f, 37.497f,  -0.619f } },
  { "Brown",   {  33.958f, 15.283f,  25.261f } },
  { "Beige",   {  75.232f,  6.935f,  24.683f } },
  { "Olive",   {  45.206f, -8.065f,  45.507f } },
  { "Navy",    {  13.863f, 14.539f, -38.182f } },
};
const size_t N_NAMED_COLORS = sizeof(NAMED_COLORS) / sizeof(NAMED_COLORS[0]);

const char* nearestColorName(const Lab& c, float* outDeltaE) {
  size_t bestIdx = 0;
  float bestSq = -1.0f;
  for (size_t i = 0; i < N_NAMED_COLORS; i++) {
    float dL = c.L - NAMED_COLORS[i].lab.L;
    float da = c.a - NAMED_COLORS[i].lab.a;
    float db = c.b - NAMED_COLORS[i].lab.b;
    float sq = dL * dL + da * da + db * db;
    if (bestSq < 0.0f || sq < bestSq) { bestSq = sq; bestIdx = i; }
  }
  if (outDeltaE) *outDeltaE = sqrtf(bestSq);
  return NAMED_COLORS[bestIdx].name;
}
