#include <unity.h>
#include <cmath>
#include "AS7341Math.h"
#include "ColorimetryTables.h"

void setUp() {}
void tearDown() {}

// --- 0.1 ADC-Vollausschlag ------------------------------------------------

static void test_full_scale_everyday_settings() {
  TEST_ASSERT_EQUAL_UINT32(15251u, adcFullScale(150, 100));  // v4 ohne Polfilter
  TEST_ASSERT_EQUAL_UINT32(40401u, adcFullScale(200, 200));  // v4 mit Polfilter
}

static void test_full_scale_capped() {
  TEST_ASSERT_EQUAL_UINT32(65535u, adcFullScale(100, 999));  // Firmware-Default
  TEST_ASSERT_EQUAL_UINT32(65535u, adcFullScale(255, 65535));
}

static void test_full_scale_minimum() {
  TEST_ASSERT_EQUAL_UINT32(1u, adcFullScale(0, 0));
}

// --- 0.2 Neutrale Grau-Rekonstruktion ---------------------------------------
// Bandgeometrie (Zentrum/FWHM) wie in AS7341Spectrometer.cpp, BANDS_*.

static const Band BANDS_NONE[]  = {{415,26},{445,30},{480,36},{515,39},{555,39},{590,40},{630,50}};
static const Band BANDS_700NM[] = {{415,26},{445,30},{480,36},{515,39},{555,39},{590,40},{630,50},{674,45}};
static const Band BANDS_650NM[] = {{415,26},{445,30},{480,36},{515,39},{555,39},{590,40}};

static void assertNeutral(const Band* bands, int n) {
  const float levels[] = {1.0f, 0.5f, 0.1f};
  float values[8];
  for (float level : levels) {
    for (int i = 0; i < n; i++) values[i] = level;
    float X, Y, Z;
    spectrumToXYZ(bands, values, n, X, Y, Z);
    Lab lab = xyzToLab(X, Y, Z);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, lab.a);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, lab.b);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 100.0f * level, Y);
  }
}

static void test_gray_neutral_no_filter() { assertNeutral(BANDS_NONE, 7); }
static void test_gray_neutral_700nm()     { assertNeutral(BANDS_700NM, 8); }
static void test_gray_neutral_650nm()     { assertNeutral(BANDS_650NM, 6); }

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_full_scale_everyday_settings);
  RUN_TEST(test_full_scale_capped);
  RUN_TEST(test_full_scale_minimum);
  RUN_TEST(test_gray_neutral_no_filter);
  RUN_TEST(test_gray_neutral_700nm);
  RUN_TEST(test_gray_neutral_650nm);
  return UNITY_END();
}
