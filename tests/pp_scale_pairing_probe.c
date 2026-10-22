/* Verify that the paired tap loop returns the same value as the plain one.
 *
 * The shader folds each 2x2 block into one bilinear fetch at
 * f = w1 / (w0 + w1), scaled by (w0 + w1); the hardware clamps f to [0,1], so a
 * block whose weights change sign is fetched four times instead. This probe
 * mirrors both paths on a synthetic image and compares the results.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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

static float bspline(float x) { return cubic(x, 1.0f, 0.0f); }
static float mitchell(float x) { return cubic(x, 1.0f / 3.0f, 1.0f / 3.0f); }
static float catmull(float x) { return cubic(x, 0.0f, 0.5f); }
static float keys075(float x) { return cubic(x, 0.0f, 0.75f); }

static float lanczos(float x, float a)
{
    const float ax = fabsf(x);
    if (ax < 1e-5f) return 1.0f;
    if (ax >= a) return 0.0f;
    const float px = (float)M_PI * ax;
    return (a * sinf(px) * sinf(px / a)) / (px * px);
}

static float spline36(float x)
{
    const float ax = fabsf(x);
    if (ax < 1.0f) {
        return 1.0f + ax * ax * (-3.0f / 209.0f + ax * (-453.0f / 209.0f
               + ax * (13.0f / 11.0f)));
    }
    if (ax < 2.0f) {
        const float t = ax - 1.0f;
        return t * (-156.0f / 209.0f + t * (270.0f / 209.0f + t * (-6.0f / 11.0f)));
    }
    if (ax < 3.0f) {
        const float t = ax - 2.0f;
        return t * (26.0f / 209.0f + t * (-45.0f / 209.0f + t * (1.0f / 11.0f)));
    }
    return 0.0f;
}

static float spline64(float x)
{
    const float ax = fabsf(x);
    if (ax < 1.0f) {
        return 1.0f + ax * ax * (-3.0f / 2911.0f + ax * (-6387.0f / 2911.0f
               + ax * (49.0f / 41.0f)));
    }
    if (ax < 2.0f) {
        const float t = ax - 1.0f;
        return t * (-2328.0f / 2911.0f + t * (4032.0f / 2911.0f + t * (-24.0f / 41.0f)));
    }
    if (ax < 3.0f) {
        const float t = ax - 2.0f;
        return t * (582.0f / 2911.0f + t * (-1008.0f / 2911.0f + t * (6.0f / 41.0f)));
    }
    if (ax < 4.0f) {
        const float t = ax - 3.0f;
        return t * (-97.0f / 2911.0f + t * (168.0f / 2911.0f + t * (-1.0f / 41.0f)));
    }
    return 0.0f;
}

static float lanczos2(float x) { return lanczos(x, 2.0f); }
static float lanczos3(float x) { return lanczos(x, 3.0f); }

typedef float (*Kernel)(float);

#define WIDTH 64
#define HEIGHT 64
static float image[HEIGHT][WIDTH];

static float texel(int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= WIDTH) x = WIDTH - 1;
    if (y >= HEIGHT) y = HEIGHT - 1;
    return image[y][x];
}

/* Bilinear fetch in texel space, matching the hardware unit. */
static float bilinear(float x, float y)
{
    const float fx = x - 0.5f;
    const float fy = y - 0.5f;
    const int x0 = (int)floorf(fx);
    const int y0 = (int)floorf(fy);
    const float tx = fx - (float)x0;
    const float ty = fy - (float)y0;
    const float a = texel(x0, y0);
    const float b = texel(x0 + 1, y0);
    const float c = texel(x0, y0 + 1);
    const float d = texel(x0 + 1, y0 + 1);
    return (a * (1.0f - tx) + b * tx) * (1.0f - ty)
           + (c * (1.0f - tx) + d * tx) * ty;
}



/* Folding must be an identity: the single bilinear fetch at w1/(w0+w1),
 * scaled by w0+w1, has to equal the two separate taps. This is the property
 * the shader relies on, and it is checkable per pass without modelling the
 * intermediate. */
static void compare(const char *name, Kernel k, float support, float st, float tolerance)
{
    float worst = 0.0f;
    for (int y = 4; y < HEIGHT - 4; y += 7) {
        for (int x = 4; x < WIDTH - 4; x += 7) {
            const float u = ((float)x + 0.37f) / (float)WIDTH;
            const float v = ((float)y + 0.61f) / (float)HEIGHT;
            for (int axis = 0; axis < 2; axis++) {
                const float p = (axis ? v * HEIGHT : u * WIDTH) - 0.5f;
                const float base = floorf(p);
                const int n = (int)ceilf(support * st);
                const float keepx = (floorf(u * WIDTH - 0.5f) + 0.5f) / WIDTH;
                const float keepy = (floorf(v * HEIGHT - 0.5f) + 0.5f) / HEIGHT;
                float folded = 0.0f;
                float plain = 0.0f;
                float wsum = 0.0f;
                for (int j = 0; j < n; j++) {
                    const int i0 = 1 - n + 2 * j;
                    const float w0 = k((p - (base + (float)i0)) / st);
                    const float w1 = k((p - (base + (float)(i0 + 1))) / st);
                    const float s = w0 + w1;
                    const float a0 = base + (float)i0 + 0.5f;
                    const float a1 = a0 + 1.0f;
                    /* Unfolded: two taps at the texel centres. */
                    if (axis) {
                        plain += bilinear(keepx * WIDTH, a0) * w0;
                        plain += bilinear(keepx * WIDTH, a1) * w1;
                    } else {
                        plain += bilinear(a0, keepy * HEIGHT) * w0;
                        plain += bilinear(a1, keepy * HEIGHT) * w1;
                    }
                    /* Folded: one fetch, only where the hardware can place it. */
                    if (w0 * w1 >= 0.0f && fabsf(s) > 1e-8f) {
                        const float f = a0 + w1 / s;
                        if (axis) {
                            folded += bilinear(keepx * WIDTH, f) * s;
                        } else {
                            folded += bilinear(f, keepy * HEIGHT) * s;
                        }
                    } else {
                        if (axis) {
                            folded += bilinear(keepx * WIDTH, a0) * w0;
                            folded += bilinear(keepx * WIDTH, a1) * w1;
                        } else {
                            folded += bilinear(a0, keepy * HEIGHT) * w0;
                            folded += bilinear(a1, keepy * HEIGHT) * w1;
                        }
                    }
                    wsum += s;
                }
                const float d = fabsf(folded - plain) / fmaxf(fabsf(wsum), 1e-5f);
                if (d > worst) worst = d;
            }
        }
    }
    printf("%-10s st=%.2f  max |folded - plain| = %.6f\n", name, st, worst);
    assert(worst <= tolerance);
}

int main(void)
{
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            /* A sharp edge plus a gradient: catches sign errors and clamping. */
            image[y][x] = (x < WIDTH / 2)
                ? 0.15f + 0.5f * (float)x / (float)WIDTH
                : 0.85f - 0.3f * (float)y / (float)HEIGHT;
        }
    }

    /* Every kernel on the same code path; the fold only fires where it is
     * exact, so all of them must agree with the plain sum. */
    for (float st = 1.0f; st <= 2.0f; st += 0.25f) {
        compare("bspline", bspline, 2.0f, st, 2e-6f);
        compare("mitchell", mitchell, 2.0f, st, 2e-6f);
        compare("catmull", catmull, 2.0f, st, 2e-6f);
        compare("keys075", keys075, 2.0f, st, 2e-6f);
        compare("lanczos2", lanczos2, 2.0f, st, 2e-6f);
        compare("lanczos3", lanczos3, 3.0f, st, 2e-6f);
        compare("spline36", spline36, 3.0f, st, 2e-6f);
        compare("spline64", spline64, 4.0f, st, 2e-6f);
    }
    printf("folding matches the plain taps for every kernel\n");
    return 0;
}
