/* Metal renderer HDR output bridge (Post Processing path).
 *
 * nPlayer's Metal renderer hardcodes BGRA8Unorm output and tone-maps HDR to
 * SDR. This unit rewrites the runtime-generated MSL for linear extended-range
 * output and configures the CAMetalLayer accordingly; the manifests carry the
 * frozen instruction and call-site patches. */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <math.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "renderer-pp-hdr-bindings.h"

#define NPA_EXPORT __attribute__((visibility("default")))

/* Frozen renderer layout: the transfer function sits at +0x1B8 and the
 * MetalVideoView at +0x1E0. */
#define NPA_RENDERER_TRANSFER 0x1B8
#define NPA_RENDERER_VIEW 0x1E0
#define NPA_RENDERER_DEVICE 0x1E8
#define NPA_RENDERER_LIBRARY 0x1F8
#define NPA_RENDERER_QUEUE 0x1F0
#define NPA_RENDERER_VERTEX_BUFFER 0x208
#define NPA_RENDERER_TEXTURE_Y 0x220
#define NPA_RENDERER_TEXTURE_CBCR 0x228
#define NPA_RENDERER_TEXTURE_CB 0x230
#define NPA_RENDERER_TEXTURE_CR 0x238

#define NPA_TRANSFER_PQ 16
#define NPA_TRANSFER_HLG 18
#define NPA_UTF8_ENCODING 4
#define NPA_EDR_MIN_NITS 0.005f
#define NPA_EDR_MAX_NITS 10000.0f
/* The shader yields cd/m^2 / 100, so y = 1 maps to 100 cd/m^2. */
#define NPA_EDR_OPTICAL_SCALE 100.0f

/* The generator emits one string per branch; these anchors are its stable
 * text, so a rewritten shader always has exactly one interpretation. */
static const char NPA_TAIL_START[] = "  rgb1 *= float3(1.0/trc_nom_peak);\n";
static const char NPA_TAIL_END[] = ", lessthanequal);\n";
static const char NPA_MATRIX_RGB1[] =
    "  float3 rgb1 = colorConversion.matrix * ycbcr1;\n";
static const char NPA_MATRIX_RGB2[] =
    "  float3 rgb2 = colorConversion.matrix * ycbcr2;\n";
/* The SDR branch carries BT.709 encoded values; the layer expects linear. */
static const char NPA_LINEAR_RGB1[] =
    "  float3 rgb1 = colorConversion.matrix * ycbcr1;\n"
    "  rgb1 = max(rgb1, float3(0.0));\n"
    "  rgb1 = pow((rgb1 + float3(0.055)) / float3(1.055), float3(2.4));\n";
static const char NPA_LINEAR_RGB2[] =
    "  float3 rgb2 = colorConversion.matrix * ycbcr2;\n"
    "  rgb2 = max(rgb2, float3(0.0));\n"
    "  rgb2 = pow((rgb2 + float3(0.055)) / float3(1.055), float3(2.4));\n";
/* HLG keeps its nominal [0, 12] range; PQ already yields nits / 100. */
static const char NPA_HLG_SCALE[] = "  rgb1 *= float3(trc_nom_peak);\n";

/* Optional scaling kernel, chosen by a shared preference. Zero keeps the
 * host's bilinear sampling; the selection is baked into the generated MSL so
 * the tap loops stay compile-time constants. */
#define NPA_PREF_SCALE_KERNEL "PPVideoScaleKernel"

#define NPA_SAMPLER_ANCHOR \
    "  constexpr sampler s(address::clamp_to_edge, filter::linear);\n"
#define NPA_TEXCOORD_ANCHOR "  float2 texcoord = in.texcoord;\n"
#define NPA_NAMESPACE_ANCHOR "using namespace metal;\n"

/* Footprint cap. Raising it sharpens large downscales and costs tap count
 * quadratically; the value is a measurement knob, not a correctness limit. */
#define NPA_SCALE_ST_MAX "2.0f"

/* Mitchell-Netravali family: one function covers B-Spline (1,0),
 * Catmull-Rom (0,0.5), Keys a=-0.75 (0,0.75) and Mitchell (1/3,1/3). */
static const char NPA_WEIGHT_CUBIC[] =
    "  const float ax = fabs(x);\n"
    "  if (ax < 1.0f) {\n"
    "    return ((12.0f - 9.0f * NP_B - 6.0f * NP_C) * ax * ax * ax\n"
    "          + (-18.0f + 12.0f * NP_B + 6.0f * NP_C) * ax * ax\n"
    "          + (6.0f - 2.0f * NP_B)) / 6.0f;\n"
    "  }\n"
    "  if (ax < 2.0f) {\n"
    "    return ((-NP_B - 6.0f * NP_C) * ax * ax * ax\n"
    "          + (6.0f * NP_B + 30.0f * NP_C) * ax * ax\n"
    "          + (-12.0f * NP_B - 48.0f * NP_C) * ax\n"
    "          + (8.0f * NP_B + 24.0f * NP_C)) / 6.0f;\n"
    "  }\n"
    "  return 0.0f;\n"
    "}\n";

/* Lanczos: sinc(x) * sinc(x / a), normalised so w(0) == 1. */
static const char NPA_WEIGHT_LANCZOS[] =
    "  const float ax = fabs(x);\n"
    "  if (ax < 1e-5f) {\n"
    "    return 1.0f;\n"
    "  }\n"
    "  if (ax >= NP_A) {\n"
    "    return 0.0f;\n"
    "  }\n"
    "  const float px = M_PI_F * ax;\n"
    "  return (NP_A * sin(px) * sin(px / NP_A)) / (px * px);\n"
    "}\n";

/* One pass over the source texels around the sample point. `st` stretches the
 * kernel to the pixel footprint, which is what makes it a low-pass when the
 * video is larger than the drawable; at 1.0 it is a plain 4x4 or 6x6 tap. */
static const char NPA_SCALE_FOOTPRINT[] =
    "\n"
    "static inline float2 npa_footprint(float2 dudx, float2 dudy, float2 size) {\n"
    "  const float2 a = dudx * size;\n"
    "  const float2 b = dudy * size;\n"
    "  return sqrt(a * a + b * b);\n"
    "}\n";

/* The wrappers must follow the tap loops, so they are appended last. The
 * separable variant only ever needs the scalar loop, which carries the final
 * vertical pass; NP_MODE is a compile-time constant either way. */
static const char NPA_SCALE_WRAPPERS[] =
    "\n"
    "static inline float npa_scale1(texture2d<float, access::sample> t, sampler sn,\n"
    "                              float2 uv, float2 dudx, float2 dudy) {\n"
    "  return npa_tap1(t, sn, uv, dudx, dudy);\n"
    "}\n"
    "\n"
    "static inline float2 npa_scale2(texture2d<float, access::sample> t, sampler sn,\n"
    "                               float2 uv, float2 dudx, float2 dudy) {\n"
    "  return npa_tap4(t, sn, uv, dudx, dudy).rg;\n"
    "}\n";

/* Horizontal pre-pass for a separable kernel: filter one plane of the source
 * into an intermediate of the same height. The tap loop is the same code with
 * NP_MODE 2, so both passes share one weight function and one fold rule. */
static const char NPA_SCALE_HORIZONTAL[] =
    "\n"
    "fragment float4 npaHorizontal(Varyings in [[ stage_in ]],\n"
    "                              texture2d<float, access::sample> src [[ texture(0) ]]) {\n"
    "  constexpr sampler sn(address::clamp_to_edge, filter::linear);\n"
    "  const float2 uv = in.texcoord;\n"
    "  return npa_tap4(src, sn, uv, dfdx(uv), dfdy(uv));\n"
    "}\n";

/* Tap geometry. `mode` selects which axes this pass filters: 0 is the whole 2D
 * footprint, 1 filters vertically only, 2 horizontally only. A separable
 * kernel runs mode 2 into an intermediate and then mode 1 into the drawable, so
 * each pass stretches the kernel by its own ratio rather than the combined 2D
 * footprint. Forcing the unused axis to st=1 and k=1 collapses it to a single
 * fetch per tap pair: at a texel centre the two weights are 1 and 0, so the
 * pairing fold keeps one of them. */
static const char NPA_SCALE_AXIS[] =
    "\n"
    "struct NpaAxis {\n"
    "  float2 size;\n"
    "  float2 st;\n"
    "  float2 pos;\n"
    "  float2 base;\n"
    "  int2 k;\n"
    "};\n"
    "\n"
    "static inline NpaAxis npa_axis(texture2d<float, access::sample> t,\n"
    "                               float2 uv, float2 dudx, float2 dudy, int mode) {\n"
    "  NpaAxis a;\n"
    "  a.size = float2(t.get_width(), t.get_height());\n"
    "  a.st = clamp(npa_footprint(dudx, dudy, a.size), 1.0f, NP_ST_MAX);\n"
    "  if (mode == 1) a.st.x = 1.0f;\n"
    "  if (mode == 2) a.st.y = 1.0f;\n"
    "  a.k = int2(ceil(NP_SUPPORT * a.st));\n"
    "  if (mode == 1) a.k.x = 1;\n"
    "  if (mode == 2) a.k.y = 1;\n"
    "  a.pos = uv * a.size - 0.5f;\n"
    "  a.base = floor(a.pos);\n"
    "  return a;\n"
    "}\n";

/* Tap pairing: fold each 2x2 block of taps into one bilinear fetch. The fold
 * places the fetch at w1/(w0+w1) and scales by w0+w1, which is algebraically
 * identical to the two separate taps. The hardware clamps that fraction to
 * [0,1], so a block whose weights change sign cannot be folded; it falls back
 * to four texels. The test is per block, not per kernel: Catmull-Rom pairs at
 * some footprints and not others.
 *
 * The horizontal weights do not depend on the vertical tap, so the whole row is
 * evaluated once instead of once per block.
 *
 * Sampling uses a linear sampler. At a texel centre it returns that texel
 * exactly, so the unfolded path is unaffected.
 *
 * The scalar loop reads the four unfolded texels with one gather instead of
 * four fetches. gather ignores the filter mode and returns the same 2x2 block
 * bilinear would have interpolated, in x, y, z, w order. A two-channel texture
 * cannot use it: gather only returns the first component, so that loop keeps
 * four fetches. */
static const char NPA_SCALE_TAP_R[] =
    "\n"
    "static inline float npa_tap1(texture2d<float, access::sample> t, sampler sn,\n"
    "                             float2 uv, float2 dudx, float2 dudy) {\n"
    "  const NpaAxis a = npa_axis(t, uv, dudx, dudy, NP_MODE);\n"
    "  float wx[2 * NP_MAX_K];\n"
    "  for (int i = 0; i < 2 * a.k.x; ++i) {\n"
    "    wx[i] = npa_kernel_w((a.pos.x - (a.base.x + float(1 - a.k.x + i))) / a.st.x);\n"
    "  }\n"
    "  float acc = 0.0f;\n"
    "  float wsum = 0.0f;\n"
    "  for (int j = 0; j < a.k.y; ++j) {\n"
    "    const int iy = 1 - a.k.y + 2 * j;\n"
    "    const float wy0 = npa_kernel_w((a.pos.y - (a.base.y + float(iy))) / a.st.y);\n"
    "    const float wy1 = npa_kernel_w((a.pos.y - (a.base.y + float(iy + 1))) / a.st.y);\n"
    "    const float sy = wy0 + wy1;\n"
    "    const bool foldY = wy0 * wy1 >= 0.0f && fabs(sy) > 1e-8f;\n"
    "    const float fy = foldY ? wy1 / sy : 0.0f;\n"
    "    const float2 row = a.base + float2(0.0f, float(iy)) + 0.5f;\n"
    "    for (int i = 0; i < a.k.x; ++i) {\n"
    "      const float wx0 = wx[2 * i];\n"
    "      const float wx1 = wx[2 * i + 1];\n"
    "      const float sx = wx0 + wx1;\n"
    "      const bool foldX = wx0 * wx1 >= 0.0f && fabs(sx) > 1e-8f;\n"
    "      const float2 at = row + float2(float(1 - a.k.x + 2 * i), 0.0f);\n"
    "      if (foldX) {\n"
    "        const float2 x0 = at + float2(wx1 / sx, 0.0f);\n"
    "        if (foldY) {\n"
    "          acc += t.sample(sn, (x0 + float2(0.0f, fy)) / a.size).r * (sx * sy);\n"
    "        } else {\n"
    "          acc += t.sample(sn, x0 / a.size).r * (sx * wy0);\n"
    "          acc += t.sample(sn, (x0 + float2(0.0f, 1.0f)) / a.size).r * (sx * wy1);\n"
    "        }\n"
    "      } else if (foldY) {\n"
    "        const float2 y0 = at + float2(0.0f, fy);\n"
    "        acc += t.sample(sn, y0 / a.size).r * (wx0 * sy);\n"
    "        acc += t.sample(sn, (y0 + float2(1.0f, 0.0f)) / a.size).r * (wx1 * sy);\n"
    "      } else {\n"
    /* MSL puts the four gathered texels counter-clockwise from the lower-left
     * of the queried location: (-,+) (+,-) ... i.e. x is the top-left texel of
     * the 2x2 block in a top-left origin texture, y top-right, z bottom-right,
     * w bottom-left. Ordering these wrong is a silent one-texel swap. */
    /* Gather picks the quad by frac(uv * size - 0.5), so querying at a texel
     * centre lands exactly on a quad boundary and the hardware may return the
     * neighbouring block. Half a texel further puts the query at the block
     * centre, where the choice is unambiguous. */
    "        const float4 g = t.gather(sn, (at + 0.5f) / a.size);\n"
    "        acc += g.x * (wx0 * wy1) + g.y * (wx1 * wy1)\n"
    "             + g.z * (wx1 * wy0) + g.w * (wx0 * wy0);\n"
    "      }\n"
    "      wsum += sx * sy;\n"
    "    }\n"
    "  }\n"
    "  return acc / max(wsum, 1e-5f);\n"
    "}\n";

/* The four-component loop. It covers the chroma plane, whose two channels
 * gather cannot return, and the horizontal pre-pass of a separable kernel,
 * which filters every channel of a plane. */
static const char NPA_SCALE_TAP_RG[] =
    "\n"
    "static inline float4 npa_tap4(texture2d<float, access::sample> t, sampler sn,\n"
    "                              float2 uv, float2 dudx, float2 dudy) {\n"
    "  const NpaAxis a = npa_axis(t, uv, dudx, dudy, NP_MODE);\n"
    "  float wx[2 * NP_MAX_K];\n"
    "  for (int i = 0; i < 2 * a.k.x; ++i) {\n"
    "    wx[i] = npa_kernel_w((a.pos.x - (a.base.x + float(1 - a.k.x + i))) / a.st.x);\n"
    "  }\n"
    "  float4 acc = float4(0.0f);\n"
    "  float wsum = 0.0f;\n"
    "  for (int j = 0; j < a.k.y; ++j) {\n"
    "    const int iy = 1 - a.k.y + 2 * j;\n"
    "    const float wy0 = npa_kernel_w((a.pos.y - (a.base.y + float(iy))) / a.st.y);\n"
    "    const float wy1 = npa_kernel_w((a.pos.y - (a.base.y + float(iy + 1))) / a.st.y);\n"
    "    const float sy = wy0 + wy1;\n"
    "    const bool foldY = wy0 * wy1 >= 0.0f && fabs(sy) > 1e-8f;\n"
    "    const float fy = foldY ? wy1 / sy : 0.0f;\n"
    "    const float2 row = a.base + float2(0.0f, float(iy)) + 0.5f;\n"
    "    for (int i = 0; i < a.k.x; ++i) {\n"
    "      const float wx0 = wx[2 * i];\n"
    "      const float wx1 = wx[2 * i + 1];\n"
    "      const float sx = wx0 + wx1;\n"
    "      const bool foldX = wx0 * wx1 >= 0.0f && fabs(sx) > 1e-8f;\n"
    "      const float2 at = row + float2(float(1 - a.k.x + 2 * i), 0.0f);\n"
    "      if (foldX) {\n"
    "        const float2 x0 = at + float2(wx1 / sx, 0.0f);\n"
    "        if (foldY) {\n"
    "          acc += t.sample(sn, (x0 + float2(0.0f, fy)) / a.size) * (sx * sy);\n"
    "        } else {\n"
    "          acc += t.sample(sn, x0 / a.size) * (sx * wy0);\n"
    "          acc += t.sample(sn, (x0 + float2(0.0f, 1.0f)) / a.size) * (sx * wy1);\n"
    "        }\n"
    "      } else if (foldY) {\n"
    "        const float2 y0 = at + float2(0.0f, fy);\n"
    "        acc += t.sample(sn, y0 / a.size) * (wx0 * sy);\n"
    "        acc += t.sample(sn, (y0 + float2(1.0f, 0.0f)) / a.size) * (wx1 * sy);\n"
    "      } else {\n"
    "        const float2 step = float2(1.0f) / a.size;\n"
    "        acc += t.sample(sn, at / a.size) * (wx0 * wy0);\n"
    "        acc += t.sample(sn, at / a.size + float2(step.x, 0.0f)) * (wx1 * wy0);\n"
    "        acc += t.sample(sn, at / a.size + float2(0.0f, step.y)) * (wx0 * wy1);\n"
    "        acc += t.sample(sn, at / a.size + step) * (wx1 * wy1);\n"
    "      }\n"
    "      wsum += sx * sy;\n"
    "    }\n"
    "  }\n"
    "  return acc / max(wsum, 1e-5f);\n"
    "}\n";

typedef struct {
    const char *parameters;
    const char *weight;
    float support;
    /* A separable variant runs one axis here and the other in a second pass, so
     * each pass stretches the kernel by its own ratio instead of the combined
     * 2D footprint. */
    int separable;
} NPAKernelSpec;

static const NPAKernelSpec NPA_KERNELS[] = {
    {NULL, NULL, 0.0f, 0},
    {"#define NP_B 0.0f\n#define NP_C 0.5f\n", NPA_WEIGHT_CUBIC, 2.0f, 0},
    {"#define NP_B 0.33333334f\n#define NP_C 0.33333334f\n", NPA_WEIGHT_CUBIC, 2.0f, 0},
    {"#define NP_B 0.0f\n#define NP_C 0.75f\n", NPA_WEIGHT_CUBIC, 2.0f, 0},
    {"#define NP_A 2.0f\n", NPA_WEIGHT_LANCZOS, 2.0f, 0},
    {"#define NP_A 3.0f\n", NPA_WEIGHT_LANCZOS, 3.0f, 0},
    {"#define NP_B 1.0f\n#define NP_C 0.0f\n", NPA_WEIGHT_CUBIC, 2.0f, 0},
    {"#define NP_A 3.0f\n", NPA_WEIGHT_LANCZOS, 3.0f, 1},
};

#define NPA_KERNEL_COUNT (sizeof(NPA_KERNELS) / sizeof(NPA_KERNELS[0]))

typedef id (*NPAOriginalShaderSourceFn)(void *renderer, int mode, int flag);
/* Same frozen lookup serves the draw wrapper; the two share the caller table
 * because each has its own call site. */
typedef void (*NPAOriginalDrawFn)(void *renderer);

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} NPABuffer;

typedef struct {
    id view;
    int transfer;
    int hdr;
} NPALayerContext;

static SEL npa_sel(const char *name)
{
    return sel_registerName(name);
}

static void npa_hdr_log(const char *message, int status)
{
    fprintf(stderr, "[npa-pp-hdr] %s (status=%d)\n", message, status);
}

/* One line per generated shader, so the active kernel is observable without a
 * debugger: the choice is otherwise only visible in the MSL text. */
static void npa_hdr_log_kernel(int kernel, int transfer, int hdr, int edr)
{
    fprintf(stderr, "[npa-pp-hdr] shader kernel=%d transfer=%d hdr=%d edr=%d\n",
            kernel, transfer, hdr, edr);
}

/* The original shader generator is reached through the frozen table; the
 * caller offset identifies the host build. */
static NPAOriginalShaderSourceFn npa_call_original_shader_source(void *caller)
{
    Dl_info info;
    if (!dladdr(caller, &info) || !info.dli_fbase) {
        npa_hdr_log("renderer caller image is unavailable", -1);
        return NULL;
    }
    uintptr_t base = (uintptr_t)info.dli_fbase;
    uintptr_t address = (uintptr_t)caller;
    for (size_t index = 0; index < sizeof(renderer_bindings) / sizeof(renderer_bindings[0]); index++) {
        const struct renderer_binding *binding = &renderer_bindings[index];
        if (address >= base && address - base == binding->caller) {
            return (NPAOriginalShaderSourceFn)(base + binding->original);
        }
    }
    npa_hdr_log("shader caller does not match a supported host", -1);
    return NULL;
}

static int npa_append(NPABuffer *buffer, const char *text, size_t length)
{
    if (buffer->length + length + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 2048;
        while (capacity < buffer->length + length + 1) {
            capacity *= 2;
        }
        char *data = realloc(buffer->data, capacity);
        if (!data) {
            return 0;
        }
        buffer->data = data;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 1;
}

static char *npa_replace_all(
    const char *source,
    const char *needle,
    const char *replacement,
    size_t *hits
)
{
    NPABuffer buffer = {0};
    const char *cursor = source;
    *hits = 0;
    for (;;) {
        const char *hit = strstr(cursor, needle);
        if (!hit) {
            break;
        }
        (*hits)++;
        if (!npa_append(&buffer, cursor, (size_t)(hit - cursor))
            || !npa_append(&buffer, replacement, strlen(replacement))) {
            free(buffer.data);
            return NULL;
        }
        cursor = hit + strlen(needle);
    }
    if (!npa_append(&buffer, cursor, strlen(cursor))) {
        free(buffer.data);
        return NULL;
    }
    return buffer.data;
}

/* Replace the whole [start, end] span, so the tone-map tail disappears in one
 * step instead of matching each of its lines. */
static char *npa_replace_span(
    const char *source,
    const char *start,
    const char *end,
    const char *replacement
)
{
    const char *first = strstr(source, start);
    const char *last = first ? strstr(first, end) : NULL;
    if (!last) {
        return NULL;
    }
    last += strlen(end);
    NPABuffer buffer = {0};
    if (!npa_append(&buffer, source, (size_t)(first - source))
        || !npa_append(&buffer, replacement, strlen(replacement))
        || !npa_append(&buffer, last, strlen(last))) {
        free(buffer.data);
        return NULL;
    }
    return buffer.data;
}

/* `required` separates the always-present anchors from the 3D-only ones. */
static int npa_sub(char **stage, const char *needle, const char *replacement, int required)
{
    size_t hits = 0;
    char *next = npa_replace_all(*stage, needle, replacement, &hits);
    if (!next) {
        return 0;
    }
    if (required && !hits) {
        free(next);
        return 0;
    }
    free(*stage);
    *stage = next;
    return 1;
}

static int npa_sub_span(char **stage, const char *start, const char *end, const char *replacement)
{
    char *next = npa_replace_span(*stage, start, end, replacement);
    if (!next) {
        return 0;
    }
    free(*stage);
    *stage = next;
    return 1;
}

/* Rewrite every `.sample(s, uv).r` / `.rg` into a kernel tap. The host only
 * ever samples through the one linear sampler `s`, so the pattern is
 * unambiguous; the argument is copied verbatim, which keeps the 3D branch's
 * texcoordAbove/texcoordBelow working. */
static char *npa_scale_samples(const char *source)
{
    NPABuffer out = {0};
    const char *cursor = source;
    for (;;) {
        const char *hit = strstr(cursor, ".sample(s,");
        if (!hit) {
            break;
        }
        /* Walk back over the receiver identifier. */
        const char *name_end = hit;
        const char *name_start = name_end;
        while (name_start > cursor
               && (name_start[-1] == '_' || name_start[-1] == '.'
                   || (name_start[-1] >= '0' && name_start[-1] <= '9')
                   || (name_start[-1] >= 'a' && name_start[-1] <= 'z')
                   || (name_start[-1] >= 'A' && name_start[-1] <= 'Z'))) {
            name_start--;
        }
        /* Find the argument and the closing paren. */
        const char *arg = hit + strlen(".sample(s,");
        while (*arg == ' ') {
            arg++;
        }
        const char *close = arg;
        int depth = 0;
        while (*close) {
            if (*close == '(') {
                depth++;
            } else if (*close == ')') {
                if (depth == 0) {
                    break;
                }
                depth--;
            }
            close++;
        }
        if (!*close) {
            free(out.data);
            return NULL;
        }
        const char *accessor = close + 1;
        const char *fn = NULL;
        size_t accessor_length = 0;
        if (accessor[0] == '.' && accessor[1] == 'r'
            && !(accessor[2] == 'g' || accessor[2] == 'b' || accessor[2] == 'a')) {
            fn = "npa_scale1";
            accessor_length = 2;
        } else if (accessor[0] == '.' && accessor[1] == 'r' && accessor[2] == 'g'
                   && !(accessor[3] == 'b' || accessor[3] == 'a')) {
            fn = "npa_scale2";
            accessor_length = 3;
        }
        if (!fn) {
            /* Not a scalar or rg accessor: leave it alone. */
            if (!npa_append(&out, cursor, (size_t)(hit + 1 - cursor))) {
                free(out.data);
                return NULL;
            }
            cursor = hit + 1;
            continue;
        }
        const size_t name_length = (size_t)(name_end - name_start);
        const size_t arg_length = (size_t)(close - arg);
        if (!npa_append(&out, cursor, (size_t)(name_start - cursor))
            || !npa_append(&out, fn, strlen(fn))
            || !npa_append(&out, "(", 1)
            || !npa_append(&out, name_start, name_length)
            || !npa_append(&out, ", sn, ", 6)
            || !npa_append(&out, arg, arg_length)
            || !npa_append(&out, ", npa_dudx, npa_dudy)",
                          strlen(", npa_dudx, npa_dudy)"))) {
            free(out.data);
            return NULL;
        }
        cursor = accessor + accessor_length;
    }
    if (!npa_append(&out, cursor, strlen(cursor))) {
        free(out.data);
        return NULL;
    }
    return out.data;
}

/* Bake one kernel into the shader. Selection happens once per generation, so
 * the tap bounds become compile-time constants and the GPU pays nothing for
 * the choice. */
static int npa_apply_scale(char **stage, int kernel)
{
    if (kernel <= 0 || (size_t)kernel >= NPA_KERNEL_COUNT) {
        return 0;
    }
    const NPAKernelSpec *spec = &NPA_KERNELS[kernel];

    char *samples = npa_scale_samples(*stage);
    if (!samples) {
        return 0;
    }
    free(*stage);
    *stage = samples;

    /* Derivatives are taken once, where control flow is still uniform: the 3D
     * branch reassigns texcoord, and dfdx inside divergent control flow is
     * undefined. */
    if (!npa_sub(stage, NPA_TEXCOORD_ANCHOR,
                 NPA_TEXCOORD_ANCHOR
                 "  float2 npa_dudx = dfdx(texcoord);\n"
                 "  float2 npa_dudy = dfdy(texcoord);\n", 1)) {
        return 0;
    }
    if (!npa_sub(stage, NPA_SAMPLER_ANCHOR,
                 NPA_SAMPLER_ANCHOR
                 "  constexpr sampler sn(address::clamp_to_edge, filter::linear);\n",
                 1)) {
        return 0;
    }

    NPABuffer helper = {0};
    char constants[128];
    snprintf(constants, sizeof(constants),
             "#define NP_SUPPORT %.1ff\n#define NP_ST_MAX %s\n"
             "#define NP_MAX_K %d\n#define NP_MODE %d\n",
             spec->support, NPA_SCALE_ST_MAX,
             (int)ceilf(spec->support * atof(NPA_SCALE_ST_MAX)),
             spec->separable ? 1 : 0);
    const char prologue[] = "\nstatic inline float npa_kernel_w(float x) {\n";
    int ok = npa_append(&helper, constants, strlen(constants))
             && npa_append(&helper, spec->parameters, strlen(spec->parameters))
             && npa_append(&helper, prologue, strlen(prologue))
             && npa_append(&helper, spec->weight, strlen(spec->weight))
             && npa_append(&helper, NPA_SCALE_FOOTPRINT, strlen(NPA_SCALE_FOOTPRINT));
    if (ok) {
        ok = npa_append(&helper, NPA_SCALE_AXIS, strlen(NPA_SCALE_AXIS))
             && npa_append(&helper, NPA_SCALE_TAP_R, strlen(NPA_SCALE_TAP_R))
             && npa_append(&helper, NPA_SCALE_TAP_RG, strlen(NPA_SCALE_TAP_RG))
             && npa_append(&helper, NPA_SCALE_WRAPPERS, strlen(NPA_SCALE_WRAPPERS));
    }
    if (ok && spec->separable) {
        /* The pre-pass filters horizontally, so it re-emits the geometry and
         * the four-component loop under different names with NP_MODE 2, then
         * restores the vertical mode for the fragment shader below. The
         * renames keep both definitions in one translation unit. */
        static const char pre[] =
            "\n#undef NP_MODE\n#define NP_MODE 2\n"
            "#define NpaAxis NpaAxisH\n#define npa_axis npa_h_axis\n"
            "#define npa_tap4 npa_h_tap4\n";
        static const char post[] =
            "\n#undef npa_tap4\n#undef npa_axis\n#undef NpaAxis\n"
            "#undef NP_MODE\n#define NP_MODE 1\n";
        ok = npa_append(&helper, pre, strlen(pre))
             && npa_append(&helper, NPA_SCALE_AXIS, strlen(NPA_SCALE_AXIS))
             && npa_append(&helper, NPA_SCALE_TAP_RG, strlen(NPA_SCALE_TAP_RG))
             && npa_append(&helper, post, strlen(post));
    }
    if (!ok) {
        free(helper.data);
        return 0;
    }
    /* The helpers must precede the shader bodies, so they go right after the
     * namespace declaration. */
    NPABuffer merged = {0};
    ok = npa_append(&merged, NPA_NAMESPACE_ANCHOR, strlen(NPA_NAMESPACE_ANCHOR))
         && npa_append(&merged, helper.data, helper.length);
    free(helper.data);
    if (!ok) {
        free(merged.data);
        return 0;
    }
    merged.data[merged.length] = '\0';
    int applied = npa_sub(stage, NPA_NAMESPACE_ANCHOR, merged.data, 1);
    free(merged.data);
    if (!applied) {
        return 0;
    }
    if (spec->separable) {
        /* The pre-pass takes Varyings, which the host declares after the
         * namespace, so this function goes in front of the fragment shader
         * instead of into the helper block. */
        static const char anchor[] = "fragment ";
        static const char replacement[] = "fragment ";
        NPABuffer spliced = {0};
        applied = npa_append(&spliced, NPA_SCALE_HORIZONTAL, strlen(NPA_SCALE_HORIZONTAL))
                  && npa_append(&spliced, replacement, strlen(replacement));
        if (applied) {
            spliced.data[spliced.length] = '\0';
            applied = npa_sub(stage, anchor, spliced.data, 1);
        }
        free(spliced.data);
    }
    return applied;
}

static int npa_scale_kernel_preference(void)
{
    id defaults = ((id (*)(id, SEL))objc_msgSend)(
        (id)objc_getClass("NSUserDefaults"), npa_sel("standardUserDefaults")
    );
    if (!defaults) {
        return 0;
    }
    id value = ((id (*)(id, SEL, id))objc_msgSend)(
        defaults, npa_sel("objectForKey:"),
        ((id (*)(id, SEL, const char *))objc_msgSend)(
            (id)objc_getClass("NSString"), npa_sel("stringWithUTF8String:"),
            NPA_PREF_SCALE_KERNEL
        )
    );
    if (!value) {
        return 0;
    }
    const long kernel = ((long (*)(id, SEL))objc_msgSend)(value, npa_sel("integerValue"));
    if (kernel <= 0 || (size_t)kernel >= NPA_KERNEL_COUNT) {
        return 0;
    }
    return (int)kernel;
}

/* Produce linear-light MSL for a floating-point extended-range drawable.
 * A missing anchor fails instead of emitting a shader with mixed encodings. */
static char *npa_rewrite_shader(
    const char *source,
    int transfer,
    int hdr,
    int edr,
    int kernel
)
{
    char *stage = strdup(source);
    if (!stage) {
        return NULL;
    }
    /* Both the fragment return type and every return value must widen. */
    if (!npa_sub(&stage, "half4", "float4", 1)
        || !npa_sub(&stage, "half3", "float3", 1)) {
        goto fail;
    }
    if (edr) {
        if (hdr) {
            /* The host tone-maps rgb1 only; a 3D return would mix a linear
             * left eye with an encoded right eye, so 3D keeps the original
             * shader. */
            if (strstr(stage, NPA_MATRIX_RGB2)) {
                npa_hdr_log("3D HDR output keeps the tone-mapped shader", transfer);
                goto fail;
            }
            const char *tail = transfer == NPA_TRANSFER_HLG ? NPA_HLG_SCALE : "";
            if (!npa_sub_span(&stage, NPA_TAIL_START, NPA_TAIL_END, tail)) {
                goto fail;
            }
        } else if (!npa_sub(&stage, NPA_MATRIX_RGB1, NPA_LINEAR_RGB1, 1)
                   || !npa_sub(&stage, NPA_MATRIX_RGB2, NPA_LINEAR_RGB2, 0)) {
            goto fail;
        }
    }
    if (kernel > 0 && !npa_apply_scale(&stage, kernel)) {
        goto fail;
    }
    return stage;
fail:
    free(stage);
    return NULL;
}

static int npa_edr_available(void)
{
    Class metadata = objc_getClass("CAEDRMetadata");
    if (!metadata) {
        return 0;
    }
    SEL selector = npa_sel("isAvailable");
    if (!class_respondsToSelector(object_getClass((id)metadata), selector)) {
        return 0;
    }
    return ((BOOL (*)(id, SEL))objc_msgSend)((id)metadata, selector) ? 1 : 0;
}

static id npa_edr_metadata(int transfer)
{
    Class metadata = objc_getClass("CAEDRMetadata");
    if (!metadata) {
        return NULL;
    }
    if (transfer == NPA_TRANSFER_HLG) {
        return ((id (*)(id, SEL))objc_msgSend)((id)metadata, npa_sel("HLGMetadata"));
    }
    if (transfer == NPA_TRANSFER_PQ) {
        return ((id (*)(id, SEL, float, float, float))objc_msgSend)(
            (id)metadata,
            npa_sel("HDR10MetadataWithMinLuminance:maxLuminance:opticalOutputScale:"),
            NPA_EDR_MIN_NITS,
            NPA_EDR_MAX_NITS,
            NPA_EDR_OPTICAL_SCALE
        );
    }
    return NULL;
}

/* Layer properties are UIKit-adjacent state, so they are only ever touched on
 * the main queue. The view is retained for the hop. A negative mode restores
 * the default SDR layer, which is what a shader that keeps the host tone
 * mapping needs. */
static void npa_configure_layer(void *context)
{
    NPALayerContext *config = context;
    id layer = ((id (*)(id, SEL))objc_msgSend)(config->view, npa_sel("metalLayer"));
    if (!layer) {
        npa_hdr_log("MetalVideoView has no metal layer", -1);
        goto done;
    }

    if (config->hdr < 0) {
        ((void (*)(id, SEL, CGColorSpaceRef))objc_msgSend)(
            layer, npa_sel("setColorspace:"), NULL
        );
        ((void (*)(id, SEL, BOOL))objc_msgSend)(
            layer, npa_sel("setWantsExtendedDynamicRangeContent:"), NO
        );
        ((void (*)(id, SEL, id))objc_msgSend)(layer, npa_sel("setEDRMetadata:"), (id)0);
        goto done;
    }

    CGColorSpaceRef space = CGColorSpaceCreateWithName(
        config->hdr ? kCGColorSpaceExtendedLinearITUR_2020
                    : kCGColorSpaceExtendedLinearSRGB
    );
    if (!space) {
        npa_hdr_log("extended linear colour space is unavailable", -1);
        goto done;
    }
    ((void (*)(id, SEL, CGColorSpaceRef))objc_msgSend)(
        layer, npa_sel("setColorspace:"), space
    );
    CGColorSpaceRelease(space);

    /* A previous video may have left HDR metadata on this layer; SDR content
     * must not keep it, or the system tone-maps it as HDR. */
    id metadata = npa_edr_metadata(config->transfer);
    ((void (*)(id, SEL, BOOL))objc_msgSend)(
        layer, npa_sel("setWantsExtendedDynamicRangeContent:"), YES
    );
    ((void (*)(id, SEL, id))objc_msgSend)(layer, npa_sel("setEDRMetadata:"), metadata);
    if (config->hdr && !metadata) {
        npa_hdr_log("no EDR metadata for this transfer function", config->transfer);
    }

done:
    ((void (*)(id, SEL))objc_msgSend)(config->view, npa_sel("release"));
    free(config);
}

static void npa_configure_layer_async(void *renderer, int transfer, int hdr)
{
    id view = *(id *)((char *)renderer + NPA_RENDERER_VIEW);
    if (!view) {
        npa_hdr_log("renderer has no video view yet", -1);
        return;
    }
    NPALayerContext *config = malloc(sizeof(*config));
    if (!config) {
        npa_hdr_log("layer configuration allocation failed", -1);
        return;
    }
    config->view = ((id (*)(id, SEL))objc_msgSend)(view, npa_sel("retain"));
    config->transfer = transfer;
    config->hdr = hdr;
    dispatch_async_f(dispatch_get_main_queue(), config, npa_configure_layer);
}

/* ---- Separable pre-pass -------------------------------------------------
 *
 * A separable kernel filters one axis into an intermediate and the other axis
 * into the drawable. The intermediate keeps the source plane layout (Y plus
 * one chroma plane) and the source pixel format, so the second pass is the
 * same shader with NP_MODE 1 and the intermediate is lossless.
 *
 * The pre-pass runs as its own command buffer on the renderer's queue, which
 * Metal executes in submission order, so the draw that follows sees a finished
 * intermediate without an extra wait.
 *
 * State is keyed on the renderer and rebuilt whenever a size, format or
 * library changes. The host rebuilds its library on every pixel-format change,
 * so the cached pipeline is dropped by comparing the library pointer. */

typedef struct {
    void *renderer;
    id library;
    id pipeline[3];
    uint64_t pipeline_format[3];
    id plane[3];
    int plane_count;
    int width;
    int height;
} NPAPrepass;

static NPAPrepass npa_prepass;
static id npa_prepass_saved[4];

static int npa_prepass_active(void)
{
    int kernel = npa_scale_kernel_preference();
    return kernel > 0 && (size_t)kernel < NPA_KERNEL_COUNT
           && NPA_KERNELS[kernel].separable;
}

static id npa_msg_id(id object, const char *selector)
{
    return ((id (*)(id, SEL))objc_msgSend)(object, npa_sel(selector));
}

static void npa_release(void *object)
{
    if (object) {
        ((void (*)(id, SEL))objc_msgSend)((id)object, npa_sel("release"));
    }
}

static void npa_prepass_reset(NPAPrepass *state)
{
    for (int index = 0; index < 3; index++) {
        npa_release(state->pipeline[index]);
        npa_release(state->plane[index]);
        state->pipeline[index] = NULL;
        state->plane[index] = NULL;
    }
    state->plane_count = 0;
    state->width = 0;
    state->height = 0;
}

/* One pipeline per pixel format: the attachment format is part of the pipeline
 * descriptor, and the source planes differ between 8- and 10-bit content. */
static id npa_prepass_pipeline(NPAPrepass *state, id device, uint64_t format)
{
    for (int index = 0; index < state->plane_count; index++) {
        if (state->pipeline[index] && state->pipeline_format[index] == format) {
            return state->pipeline[index];
        }
    }
    id function = ((id (*)(id, SEL, id))objc_msgSend)(
        state->library, npa_sel("newFunctionWithName:"),
        ((id (*)(id, SEL, const char *))objc_msgSend)(
            (id)objc_getClass("NSString"), npa_sel("stringWithUTF8String:"),
            "npaHorizontal"
        )
    );
    if (!function) {
        npa_hdr_log("separable pre-pass function is missing", -1);
        return NULL;
    }
    id vertex = ((id (*)(id, SEL, id))objc_msgSend)(
        state->library, npa_sel("newFunctionWithName:"),
        ((id (*)(id, SEL, const char *))objc_msgSend)(
            (id)objc_getClass("NSString"), npa_sel("stringWithUTF8String:"),
            "vertexPassthrough"
        )
    );
    id descriptor = npa_msg_id((id)objc_getClass("MTLRenderPipelineDescriptor"), "alloc");
    descriptor = npa_msg_id(descriptor, "init");
    if (!vertex || !descriptor) {
        npa_hdr_log("separable pre-pass pipeline could not be built", -1);
        npa_release(function);
        npa_release(vertex);
        npa_release(descriptor);
        return NULL;
    }
    ((void (*)(id, SEL, id))objc_msgSend)(descriptor, npa_sel("setLabel:"), NULL);
    ((void (*)(id, SEL, id))objc_msgSend)(descriptor, npa_sel("setVertexFunction:"), vertex);
    ((void (*)(id, SEL, id))objc_msgSend)(descriptor, npa_sel("setFragmentFunction:"), function);
    ((void (*)(id, SEL, unsigned long))objc_msgSend)(descriptor, npa_sel("setSampleCount:"), 1);
    id attachment = npa_msg_id(npa_msg_id(descriptor, "colorAttachments"), "objectAtIndexedSubscript:");
    if (attachment) {
        ((void (*)(id, SEL, unsigned long))objc_msgSend)(
            attachment, npa_sel("setPixelFormat:"), (unsigned long)format
        );
    }
    id error = NULL;
    id pipeline = ((id (*)(id, SEL, id, void **))objc_msgSend)(
        device, npa_sel("newRenderPipelineStateWithDescriptor:error:"), descriptor, (void **)&error
    );
    if (!pipeline) {
        npa_hdr_log("separable pre-pass pipeline compilation failed", (int)format);
    }
    npa_release(function);
    npa_release(vertex);
    npa_release(descriptor);

    /* Reuse the first free slot; there is at most one format per frame in
     * practice, and the array is only a small cache. */
    for (int index = 0; index < 3; index++) {
        if (!state->pipeline[index]) {
            state->pipeline[index] = pipeline;
            state->pipeline_format[index] = format;
            return pipeline;
        }
    }
    npa_hdr_log("separable pre-pass pipeline cache is full", -1);
    npa_release(pipeline);
    return NULL;
}

static id npa_prepass_texture(id device, uint64_t format, int width, int height)
{
    id descriptor = ((id (*)(id, SEL, unsigned long, unsigned long, unsigned long, unsigned long))objc_msgSend)(
        (id)objc_getClass("MTLTextureDescriptor"),
        npa_sel("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"),
        (unsigned long)format, (unsigned long)width, (unsigned long)height, 0UL
    );
    if (!descriptor) {
        return NULL;
    }
    /* The pre-pass renders into it and the draw samples it. */
    ((void (*)(id, SEL, unsigned long))objc_msgSend)(descriptor, npa_sel("setUsage:"), 5);
    return ((id (*)(id, SEL, id))objc_msgSend)(device, npa_sel("newTextureWithDescriptor:"), descriptor);
}

/* Returns 0 when the pre-pass could not run; the caller then falls back to the
 * host draw and logs, because the fragment shader is already vertical-only.
 * On success the renderer's texture slots point at the intermediates, and the
 * caller must restore them: the host writes the next frame into those slots
 * with replaceRegion at the source size, which would land inside a
 * differently sized intermediate. */
static int npa_prepass_run(void *renderer)
{
    NPAPrepass *state = &npa_prepass;
    if (state->renderer != renderer) {
        npa_prepass_reset(state);
        state->renderer = renderer;
    }

    id device = *(id *)((char *)renderer + NPA_RENDERER_DEVICE);
    id queue = *(id *)((char *)renderer + NPA_RENDERER_QUEUE);
    id library = *(id *)((char *)renderer + NPA_RENDERER_LIBRARY);
    id vertex_buffer = *(id *)((char *)renderer + NPA_RENDERER_VERTEX_BUFFER);
    id view = *(id *)((char *)renderer + NPA_RENDERER_VIEW);
    if (!device || !queue || !library || !vertex_buffer || !view) {
        npa_hdr_log("separable pre-pass is missing renderer state", -1);
        return 0;
    }
    if (state->library != library) {
        npa_prepass_reset(state);
        state->library = library;
    }

    /* Plane 0 is luma, plane 1 is either interleaved chroma or Cb, plane 2 is
     * Cr. The host clears all four slots on a format change, so an empty slot
     * means that layout is not in use. */
    id sources[3] = {
        *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_Y),
        *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_CBCR),
        *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_CR),
    };
    if (!sources[1]) {
        sources[1] = *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_CB);
    }
    int plane_count = 0;
    for (int index = 0; index < 3; index++) {
        if (sources[index]) {
            plane_count++;
        } else {
            break;
        }
    }
    if (plane_count < 2) {
        npa_hdr_log("separable pre-pass found no source planes", -1);
        return 0;
    }

    id layer = npa_msg_id(view, "metalLayer");
    CGSize drawable = {0, 0};
    if (layer) {
        ((CGSize (*)(id, SEL))objc_msgSend)(layer, npa_sel("drawableSize"));
    }
    if (drawable.width < 1.0 || drawable.height < 1.0) {
        npa_hdr_log("separable pre-pass has no drawable size", -1);
        return 0;
    }

    /* Luma keeps the source height and takes the drawable width; chroma keeps
     * its own subsampling, so the second pass sees an exactly 1:1 horizontal
     * footprint on both planes. */
    uint64_t formats[3] = {0, 0, 0};
    int sizes[3][2] = {{0, 0}, {0, 0}, {0, 0}};
    const int luma_width = (int)((unsigned long (*)(id, SEL))objc_msgSend)(
        sources[0], npa_sel("width")
    );
    for (int index = 0; index < plane_count; index++) {
        formats[index] = ((unsigned long (*)(id, SEL))objc_msgSend)(
            sources[index], npa_sel("pixelFormat")
        );
        const int source_width = (int)((unsigned long (*)(id, SEL))objc_msgSend)(
            sources[index], npa_sel("width")
        );
        const int source_height = (int)((unsigned long (*)(id, SEL))objc_msgSend)(
            sources[index], npa_sel("height")
        );
        if (source_width < 1 || source_height < 1) {
            npa_hdr_log("separable pre-pass source plane is empty", index);
            return 0;
        }
        const int scale = source_width == luma_width ? 1 : 2;
        sizes[index][0] = (int)(drawable.width / scale);
        sizes[index][1] = source_height;
        if (sizes[index][0] < 1) {
            sizes[index][0] = 1;
        }
    }

    if (state->plane_count != plane_count || state->width != sizes[0][0]
        || state->height != sizes[0][1]) {
        npa_prepass_reset(state);
        state->library = library;
        state->plane_count = plane_count;
        state->width = sizes[0][0];
        state->height = sizes[0][1];
        for (int index = 0; index < plane_count; index++) {
            state->plane[index] = npa_prepass_texture(
                device, formats[index], sizes[index][0], sizes[index][1]
            );
            if (!state->plane[index]) {
                npa_hdr_log("separable pre-pass texture allocation failed", index);
                npa_prepass_reset(state);
                state->library = library;
                return 0;
            }
        }
    }

    id command_buffer = npa_msg_id(queue, "commandBuffer");
    if (!command_buffer) {
        npa_hdr_log("separable pre-pass could not get a command buffer", -1);
        return 0;
    }
    const unsigned long vertex_count =
        ((unsigned long (*)(id, SEL))objc_msgSend)(vertex_buffer, npa_sel("length")) >> 4;

    for (int index = 0; index < plane_count; index++) {
        id pipeline = npa_prepass_pipeline(state, device, formats[index]);
        if (!pipeline) {
            return 0;
        }
        id descriptor = npa_msg_id((id)objc_getClass("MTLRenderPassDescriptor"), "renderPassDescriptor");
        if (!descriptor) {
            npa_hdr_log("separable pre-pass could not get a pass descriptor", -1);
            return 0;
        }
        id attachment = npa_msg_id(npa_msg_id(descriptor, "colorAttachments"), "objectAtIndexedSubscript:");
        ((void (*)(id, SEL, id))objc_msgSend)(attachment, npa_sel("setTexture:"), state->plane[index]);
        ((void (*)(id, SEL, unsigned long))objc_msgSend)(attachment, npa_sel("setLoadAction:"), 2);
        ((void (*)(id, SEL, unsigned long))objc_msgSend)(attachment, npa_sel("setStoreAction:"), 1);

        id encoder = ((id (*)(id, SEL, id))objc_msgSend)(
            command_buffer, npa_sel("renderCommandEncoderWithDescriptor:"), descriptor
        );
        if (!encoder) {
            npa_hdr_log("separable pre-pass could not get an encoder", -1);
            return 0;
        }
        ((void (*)(id, SEL, id))objc_msgSend)(encoder, npa_sel("setRenderPipelineState:"), pipeline);
        ((void (*)(id, SEL, id, unsigned long))objc_msgSend)(
            encoder, npa_sel("setFragmentTexture:atIndex:"), sources[index], 0
        );
        ((void (*)(id, SEL, id, unsigned long, unsigned long))objc_msgSend)(
            encoder, npa_sel("setVertexBuffer:offset:atIndex:"), vertex_buffer, 0, 0
        );
        ((void (*)(id, SEL, unsigned long, unsigned long, unsigned long, unsigned long))objc_msgSend)(
            encoder, npa_sel("drawPrimitives:vertexStart:vertexCount:instanceCount:"),
            3, 0, vertex_count, 1
        );
        ((void (*)(id, SEL))objc_msgSend)(encoder, npa_sel("endEncoding"));
    }
    /* Nothing waits on this buffer. Metal holds a committed buffer until its
     * work completes, and the queue runs buffers in submission order, so the
     * host draw that follows is already ordered after this one. Blocking here
     * would serialise the GPU with the CPU every frame. */
    ((void (*)(id, SEL))objc_msgSend)(command_buffer, npa_sel("commit"));

    /* The second pass samples the intermediates. The host overwrites these
     * slots on the next upload and never releases them, so they are swapped
     * without a retain and restored by the caller before returning. */
    npa_prepass_saved[0] = *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_Y);
    npa_prepass_saved[1] = *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_CBCR);
    npa_prepass_saved[2] = *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_CB);
    npa_prepass_saved[3] = *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_CR);
    *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_Y) = state->plane[0];
    *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_CBCR) =
        plane_count == 2 ? state->plane[1] : NULL;
    *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_CB) =
        plane_count == 3 ? state->plane[1] : NULL;
    *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_CR) =
        plane_count == 3 ? state->plane[2] : NULL;
    return 1;
}

static void npa_prepass_restore(void *renderer)
{
    *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_Y) = npa_prepass_saved[0];
    *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_CBCR) = npa_prepass_saved[1];
    *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_CB) = npa_prepass_saved[2];
    *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_CR) = npa_prepass_saved[3];
}

/* Draw wrapper. The separable variant needs its horizontal pass to land in the
 * intermediate before the host draw samples it, so the wrapper runs the
 * pre-pass and then hands off to the original draw. Everything else goes
 * straight to the original, which keeps the non-separable path byte-identical
 * to a build without this hook. */
NPA_EXPORT __attribute__((noinline)) void npa_renderer_pp_hdr_draw(void *renderer)
{
    NPAOriginalDrawFn draw = (NPAOriginalDrawFn)npa_call_original_shader_source(
        __builtin_return_address(0)
    );
    if (!draw) {
        return;
    }
    if (!renderer || !npa_prepass_active()) {
        draw(renderer);
        return;
    }
    /* A failed pre-pass leaves the source planes in place; the fragment shader
     * is vertical-only by then, so say so rather than shipping a silently
     * under-filtered frame. */
    if (!npa_prepass_run(renderer)) {
        npa_hdr_log("separable pre-pass did not run; frame is vertical-only", -1);
        draw(renderer);
        return;
    }
    draw(renderer);
    npa_prepass_restore(renderer);
}

NPA_EXPORT __attribute__((noinline)) id npa_renderer_pp_hdr_shader_source(
    void *renderer,
    int mode,
    int flag
)
{
    NPAOriginalShaderSourceFn original =
        npa_call_original_shader_source(__builtin_return_address(0));
    if (!original) {
        return NULL;
    }
    id source = original(renderer, mode, flag);
    if (!source || !renderer) {
        return source;
    }

    /* The scaling kernel is independent of the EDR fix: it is a sampling
     * choice, so it applies whether or not the layer can go extended-range. */
    const int kernel = npa_scale_kernel_preference();
    const int edr = npa_edr_available();
    if (!edr && kernel == 0) {
        /* Nothing to change: without CAEDRMetadata the layer stays SDR and the
         * host's tone-mapped shader is what that layer expects. */
        return source;
    }

    int transfer = *(int32_t *)((char *)renderer + NPA_RENDERER_TRANSFER);
    int hdr = transfer == NPA_TRANSFER_PQ || transfer == NPA_TRANSFER_HLG;
    const char *utf8 = ((const char *(*)(id, SEL))objc_msgSend)(
        source, npa_sel("UTF8String")
    );
    if (!utf8) {
        npa_hdr_log("shader source is not a UTF-8 string", -1);
        return source;
    }

    npa_hdr_log_kernel(kernel, transfer, hdr, edr);
    char *rewritten = npa_rewrite_shader(utf8, transfer, hdr, edr, kernel);
    if (!rewritten) {
        npa_hdr_log("shader was not rewritten; keeping the host shader", transfer);
        if (edr && hdr) {
            /* An earlier video may have left the layer extended-range. */
            npa_configure_layer_async(renderer, transfer, -1);
        }
        return source;
    }
    id result = ((id (*)(id, SEL, const void *, unsigned long, unsigned long))objc_msgSend)(
        (id)objc_getClass("NSMutableString"),
        npa_sel("stringWithBytes:length:encoding:"),
        rewritten,
        (unsigned long)strlen(rewritten),
        NPA_UTF8_ENCODING
    );
    free(rewritten);
    if (!result) {
        npa_hdr_log("rewritten shader string allocation failed", -1);
        return source;
    }
    if (edr) {
        npa_configure_layer_async(renderer, transfer, hdr);
    }
    return result;
}
