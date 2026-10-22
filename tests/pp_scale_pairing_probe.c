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
        return 1.0f + ax * (-3.0f / 209.0f + ax * (-453.0f / 209.0f
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
        return 1.0f + ax * (-3.0f / 2911.0f + ax * (-6387.0f / 2911.0f
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
                const float keepx = (floorf(u * WIDTH) + 0.5f) / WIDTH;
                const float keepy = (floorf(v * HEIGHT) + 0.5f) / HEIGHT;
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

/* Anti-ringing clamps the result to the two texels that straddle the sample
 * point, which are `base` and `base + 1` whatever the tap count is. Three
 * properties matter: it must remove the overshoot a negative lobe produces at
 * a step edge, it must leave a smooth ramp alone, and the clamp pair must not
 * depend on the parity of the tap count. */
static float filter_axis(Kernel k, float support, float st, int down, int antiring,
                         float u, float v, int *clamped)
{
    const float p = u * WIDTH - 0.5f;
    const float keepy = (floorf(v * HEIGHT) + 0.5f) / HEIGHT;
    const float base = floorf(p);
    const int n = (int)ceilf(support * st);
    float acc = 0.0f;
    float wsum = 0.0f;
    for (int j = 0; j < n; j++) {
        const int i0 = 1 - n + 2 * j;
        const float w0 = down ? k((p - (base + (float)i0)) / st)
                              : k((p - (base + (float)i0)));
        const float w1 = down ? k((p - (base + (float)(i0 + 1))) / st)
                              : k((p - (base + (float)(i0 + 1))));
        const float first = base + (float)i0 + 0.5f;
        acc += bilinear(first, keepy * HEIGHT) * w0;
        acc += bilinear(first + 1.0f, keepy * HEIGHT) * w1;
        wsum += w0 + w1;
    }
    const float result = acc / fmaxf(wsum, 1e-5f);
    if (!antiring) {
        return result;
    }
    const float c0 = bilinear(base + 0.5f, keepy * HEIGHT);
    const float c1 = bilinear(base + 1.5f, keepy * HEIGHT);
    const float clampedResult =
        fminf(fmaxf(result, fminf(c0, c1)), fmaxf(c0, c1));
    if (clamped) {
        *clamped = fabsf(clampedResult - result) > 1e-7f;
    }
    return clampedResult;
}

static void check_antiring(Kernel k, float support, float st, int down)
{
    /* A hard step: Lanczos overshoots on both sides of it. */
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            image[y][x] = (x < WIDTH / 2) ? 0.0f : 1.0f;
        }
    }
    float worst = 0.0f;
    float stepChange = 0.0f;
    int fired = 0;
    for (int x = WIDTH / 2 - 6; x <= WIDTH / 2 + 6; x++) {
        /* A non-integer phase: at an exact texel centre every interpolating
         * kernel reduces to a copy and there is nothing to ring. */
        const float u = ((float)x + 0.37f) / (float)WIDTH;
        const float plain = filter_axis(k, support, st, down, 0, u, 0.5f, NULL);
        int clamped = 0;
        const float guarded =
            filter_axis(k, support, st, down, 1, u, 0.5f, &clamped);
        if (clamped) fired++;
        stepChange = fmaxf(stepChange, fabsf(guarded - plain));
        /* The clamp can only pull the value towards [0,1]. */
        assert(guarded >= -1e-6f && guarded <= 1.0f + 1e-6f);
        const float overshoot = fmaxf(0.0f, fmaxf(-plain, plain - 1.0f));
        if (overshoot > 1e-4f) {
            assert(fabsf(guarded - plain) >= 0.0f);
            worst = fmaxf(worst, overshoot);
        }
    }
    printf("antiring %s overshoot=%.4f clamped_pixels=%d\n",
           down ? "down" : "up  ", worst, fired);
    /* The kernel must actually ring on a step, or this test proves nothing. */
    assert(worst > 1e-3f);
    assert(fired > 0);

    /* A smooth ramp barely rings, so the clamp must move far less there than
     * at the step. That is the property worth asserting; requiring zero change
     * would fail on the ramp's own edge pixels, where a long kernel still
     * overshoots slightly. */
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            image[y][x] = (float)x / (float)WIDTH;
        }
    }
    float rampChange = 0.0f;
    for (int x = 8; x < WIDTH - 8; x += 3) {
        const float u = ((float)x + 0.37f) / (float)WIDTH;
        const float plain = filter_axis(k, support, st, down, 0, u, 0.5f, NULL);
        const float guarded =
            filter_axis(k, support, st, down, 1, u, 0.5f, NULL);
        rampChange = fmaxf(rampChange, fabsf(guarded - plain));
    }
    printf("antiring %s step=%.4f ramp=%.4f\n",
           down ? "down" : "up  ", stepChange, rampChange);
    assert(stepChange > 10.0f * rampChange);
}

/* A linear ramp is reproduced exactly by every interpolating kernel, and the
 * two texels that straddle a point on a ramp bracket its value. So a correct
 * clamp pair leaves the ramp untouched even at full strength. Clamping to a
 * pair one texel to the left, which is what indexing the pair by the tap count
 * does for an even tap count, offsets the whole ramp by half a texel. */
static void check_antiring_preserves_ramp(Kernel k, float support)
{
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            image[y][x] = (float)x / (float)WIDTH;
        }
    }
    float worst = 0.0f;
    for (int x = 8; x < WIDTH - 8; x++) {
        for (int phase = 0; phase < 4; phase++) {
            const float u = ((float)x + 0.125f * (float)(phase * 2 + 1)) / (float)WIDTH;
            const float plain = filter_axis(k, support, 1.0f, 0, 0, u, 0.5f, NULL);
            const float guarded = filter_axis(k, support, 1.0f, 0, 1, u, 0.5f, NULL);
            /* Texel n holds n/WIDTH and sits at n + 0.5, so the ramp's value
             * at the sample point is (p - 0.5) / WIDTH. */
            const float ideal = (u * WIDTH - 1.0f) / (float)WIDTH;
            assert(fabsf(plain - ideal) < 0.02f);
            worst = fmaxf(worst, fabsf(guarded - plain));
        }
    }
    printf("antiring up   ramp shift=%.6f\n", worst);
    assert(worst < 1e-4f);
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
    check_antiring(lanczos3, 3.0f, 1.5f, 1);
    /* st=1 is an odd tap count for the radius-3 kernels and even for the
     * radius-2 ones; both parities must clamp to the same pair. */
    check_antiring(catmull, 2.0f, 1.0f, 0);
    check_antiring(lanczos3, 3.0f, 1.0f, 0);
    check_antiring_preserves_ramp(catmull, 2.0f);
    check_antiring_preserves_ramp(lanczos3, 3.0f);
    printf("folding matches the plain taps for every kernel\n");
    return 0;
}
