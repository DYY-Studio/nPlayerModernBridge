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

/* The loop the shader used before pairing. */
static float plain(Kernel k, float support, float u, float v, float st)
{
    const float px = u * WIDTH - 0.5f;
    const float py = v * HEIGHT - 0.5f;
    const float bx = floorf(px);
    const float by = floorf(py);
    const int kx = (int)ceilf(support * st);
    float acc = 0.0f;
    float wsum = 0.0f;
    for (int j = 1 - kx; j <= kx; j++) {
        const float wy = k((py - (by + (float)j)) / st);
        for (int i = 1 - kx; i <= kx; i++) {
            const float wx = k((px - (bx + (float)i)) / st);
            acc += texel((int)bx + i, (int)by + j) * (wx * wy);
            wsum += wx * wy;
        }
    }
    return acc / fmaxf(wsum, 1e-5f);
}

/* The paired loop now in the shader. Horizontal weights are hoisted out of
 * the vertical loop, and each block fetches once, twice or four times. */
static float paired(Kernel k, float support, float u, float v, float st)
{
    const float px = u * WIDTH - 0.5f;
    const float py = v * HEIGHT - 0.5f;
    const float bx = floorf(px);
    const float by = floorf(py);
    const int kx = (int)ceilf(support * st);
    float wx[2 * 8];
    for (int i = 0; i < 2 * kx; i++) {
        wx[i] = k((px - (bx + (float)(1 - kx + i))) / st);
    }
    float acc = 0.0f;
    float wsum = 0.0f;
    for (int j = 0; j < kx; j++) {
        const int iy = 1 - kx + 2 * j;
        const float wy0 = k((py - (by + (float)iy)) / st);
        const float wy1 = k((py - (by + (float)(iy + 1))) / st);
        const float sy = wy0 + wy1;
        const int foldY = wy0 * wy1 >= 0.0f && fabsf(sy) > 1e-8f;
        const float fy = foldY ? wy1 / sy : 0.0f;
        for (int i = 0; i < kx; i++) {
            const float wx0 = wx[2 * i];
            const float wx1 = wx[2 * i + 1];
            const float sx = wx0 + wx1;
            const int foldX = wx0 * wx1 >= 0.0f && fabsf(sx) > 1e-8f;
            const float ax = bx + (float)(1 - kx + 2 * i) + 0.5f;
            const float ay = by + (float)iy + 0.5f;
            if (foldX) {
                const float x0 = ax + wx1 / sx;
                if (foldY) {
                    acc += bilinear(x0, ay + fy) * (sx * sy);
                } else {
                    acc += bilinear(x0, ay) * (sx * wy0);
                    acc += bilinear(x0, ay + 1.0f) * (sx * wy1);
                }
            } else if (foldY) {
                const float y0 = ay + fy;
                acc += bilinear(ax, y0) * (wx0 * sy);
                acc += bilinear(ax + 1.0f, y0) * (wx1 * sy);
            } else {
                const int ix = (int)floorf(ax);
                const int iy0 = (int)floorf(ay);
                acc += texel(ix, iy0) * (wx0 * wy0);
                acc += texel(ix + 1, iy0) * (wx1 * wy0);
                acc += texel(ix, iy0 + 1) * (wx0 * wy1);
                acc += texel(ix + 1, iy0 + 1) * (wx1 * wy1);
            }
            wsum += sx * sy;
        }
    }
    return acc / fmaxf(wsum, 1e-5f);
}

static void compare(const char *name, Kernel k, float support, float st, float tolerance)
{
    float worst = 0.0f;
    for (int y = 4; y < HEIGHT - 4; y += 7) {
        for (int x = 4; x < WIDTH - 4; x += 7) {
            const float u = ((float)x + 0.37f) / (float)WIDTH;
            const float v = ((float)y + 0.61f) / (float)HEIGHT;
            const float a = plain(k, support, u, v, st);
            const float b = paired(k, support, u, v, st);
            const float d = fabsf(a - b);
            if (d > worst) worst = d;
        }
    }
    printf("%-10s st=%.2f  max |plain - paired| = %.6f\n", name, st, worst);
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
    }
    printf("paired loop matches the plain loop for every kernel\n");
    return 0;
}
