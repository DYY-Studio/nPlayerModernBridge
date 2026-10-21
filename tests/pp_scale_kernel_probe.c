/* Self-check for the scaling kernels.
 *
 * The MSL is generated, not linked, so this probe mirrors the exact weight
 * functions the bridge emits and asserts the properties the shader relies on:
 * the kernel is normalised at the origin, its support matches NP_SUPPORT, the
 * tap window covers every non-zero weight, and the cubic family reproduces
 * the published (B,C) members.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>

static float cubic(float x, float b, float c)
{
    const float ax = fabsf(x);
    if (ax < 1.0f) {
        return ((12.0f - 9.0f * b - 6.0f * c) * ax * ax * ax
                + (-18.0f + 12.0f * b + 6.0f * c) * ax * ax
                + (6.0f - 2.0f * b)) / 6.0f;
    }
    if (ax < 2.0f) {
        return ((-b - 6.0f * c) * ax * ax * ax
                + (6.0f * b + 30.0f * c) * ax * ax
                + (-12.0f * b - 48.0f * c) * ax
                + (8.0f * b + 24.0f * c)) / 6.0f;
    }
    return 0.0f;
}

static float lanczos(float x, float a)
{
    const float ax = fabsf(x);
    if (ax < 1e-5f) {
        return 1.0f;
    }
    if (ax >= a) {
        return 0.0f;
    }
    const float px = (float)M_PI * ax;
    return (a * sinf(px) * sinf(px / a)) / (px * px);
}

/* B-Spline and Mitchell are smoothing kernels: their weight at the origin is
 * below one (2/3 and 8/9) by construction, so they are not interpolating. The
 * tap loop divides by the weight sum, which is what makes both families output
 * a normalised result; only Catmull-Rom and Keys pass through the samples. */
static void check_support(const char *name, float (*fn)(float), float support)
{
    assert(fn(0.0f) > 0.0f);
    /* Outside the support every tap must be zero, or the loop wastes work and
     * the normalisation drifts. */
    assert(fabsf(fn(support)) < 1e-6f);
    assert(fabsf(fn(support + 0.5f)) < 1e-6f);
    assert(fabsf(fn(-support - 0.5f)) < 1e-6f);
    printf("%-12s w(0)=%.4f support=%.1f ok\n", name, fn(0.0f), support);
}

static float catmull(float x) { return cubic(x, 0.0f, 0.5f); }
static float bspline(float x) { return cubic(x, 1.0f, 0.0f); }
static float mitchell(float x) { return cubic(x, 1.0f / 3.0f, 1.0f / 3.0f); }
static float keys075(float x) { return cubic(x, 0.0f, 0.75f); }
static float lanczos2(float x) { return lanczos(x, 2.0f); }
static float lanczos3(float x) { return lanczos(x, 3.0f); }

/* The generated loop spans i in [1 - ceil(S*st), ceil(S*st)]. Every weight
 * outside that window is zero, and the window is symmetric about 0.5. */
static void check_taps(const char *name, float (*fn)(float), float support, float st)
{
    const int k = (int)ceilf(support * st);
    for (int i = 1 - k; i <= k; i++) {
        const float w = fn((0.5f - (float)i) / st);
        assert(isfinite(w));
    }
    const float outside_left = fn((0.5f - (float)(1 - k - 1)) / st);
    const float outside_right = fn((0.5f - (float)(k + 1)) / st);
    assert(fabsf(outside_left) < 1e-6f);
    assert(fabsf(outside_right) < 1e-6f);
    printf("%-12s st=%.2f taps/axis=%d\n", name, st, 2 * k);
}

int main(void)
{
    /* Published members of the Mitchell-Netravali family. */
    assert(fabsf(bspline(0.5f) - 23.0f / 48.0f) < 1e-5f);
    assert(fabsf(catmull(0.5f) - 9.0f / 16.0f) < 1e-5f);
    assert(fabsf(mitchell(1.0f) - 1.0f / 18.0f) < 1e-5f);
    /* Catmull-Rom interpolates: w(1) is exactly zero. */
    assert(fabsf(catmull(1.0f)) < 1e-6f);
    /* B-Spline is the all-positive member: no negative lobe at any tap. */
    for (float x = 0.0f; x < 2.0f; x += 0.01f) {
        assert(bspline(x) >= -1e-6f);
    }
    /* Catmull-Rom and Keys a=-0.75 ring: their first side lobe goes negative. */
    assert(catmull(1.5f) < 0.0f);
    assert(keys075(1.5f) < 0.0f);

    check_support("bspline", bspline, 2.0f);
    check_support("mitchell", mitchell, 2.0f);
    check_support("catmull", catmull, 2.0f);
    check_support("keys075", keys075, 2.0f);
    check_support("lanczos2", lanczos2, 2.0f);
    check_support("lanczos3", lanczos3, 3.0f);
    /* The interpolating members must hit 1.0 at the origin. */
    assert(fabsf(catmull(0.0f) - 1.0f) < 1e-6f);
    assert(fabsf(keys075(0.0f) - 1.0f) < 1e-6f);
    assert(fabsf(lanczos2(0.0f) - 1.0f) < 1e-6f);
    assert(fabsf(lanczos3(0.0f) - 1.0f) < 1e-6f);

    /* Both the upscale and the two downscale footprints. */
    for (float st = 1.0f; st <= 2.0f; st += 0.5f) {
        check_taps("bspline", bspline, 2.0f, st);
        check_taps("catmull", catmull, 2.0f, st);
        check_taps("lanczos2", lanczos2, 2.0f, st);
        check_taps("lanczos3", lanczos3, 3.0f, st);
    }
    return 0;
}
