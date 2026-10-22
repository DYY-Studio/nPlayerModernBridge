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
#define NPA_PREF_SCALE_KERNEL_UP "PPVideoScaleKernelUpscale"
/* Downscaling anti-ringing keeps its original key so the existing switch
 * keeps working; upscaling anti-ringing is a separate decision. */
#define NPA_PREF_ANTIRING "PPVideoAntiRinging"
#define NPA_PREF_ANTIRING_UP "PPVideoAntiRingingUpscale"
#define NPA_PREF_ANTIALIASING "PPVideoAntiAliasing"

#define NPA_SAMPLER_ANCHOR \
    "  constexpr sampler s(address::clamp_to_edge, filter::linear);\n"
#define NPA_TEXCOORD_ANCHOR "  float2 texcoord = in.texcoord;\n"
#define NPA_NAMESPACE_ANCHOR "using namespace metal;\n"

/* Anti-aliasing strength for downscaling, as libplacebo models it: the kernel
 * is stretched by the footprint ratio so it low-passes the pixels it skips.
 * Skipping the stretch is libplacebo's skip_anti_aliasing, and the only cost
 * of the stronger settings is tap count, which grows linearly with the ratio. */
#define NPA_AA_SKIP 0
#define NPA_AA_BALANCE 1
#define NPA_AA_QUALITY 2
/* Not a quality ceiling: the footprint comes from dfdx, and a degenerate
 * derivative would otherwise ask for thousands of taps. 16 covers 8K to 480p. */
#define NPA_AA_QUALITY_CAP 16.0f

static float npa_st_cap(int antialiasing)
{
    switch (antialiasing) {
    case NPA_AA_BALANCE: return 2.0f;
    case NPA_AA_QUALITY: return NPA_AA_QUALITY_CAP;
    default: return 1.0f;
    }
}

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

/* Spline36 / Spline64: the piecewise cubics from Avisynth, as in zimg. Each
 * segment is a cubic in the offset from its own left edge, and the segment
 * index is the integer part of |x|. Both interpolate, so w(0) == 1. */
static const char NPA_WEIGHT_SPLINE36[] =
    "  const float ax = fabs(x);\n"
    "  if (ax < 1.0f) {\n"
    "    return 1.0f + ax * (-3.0f / 209.0f + ax * (-453.0f / 209.0f\n"
    "           + ax * (13.0f / 11.0f)));\n"
    "  }\n"
    "  if (ax < 2.0f) {\n"
    "    const float t = ax - 1.0f;\n"
    "    return t * (-156.0f / 209.0f + t * (270.0f / 209.0f\n"
    "           + t * (-6.0f / 11.0f)));\n"
    "  }\n"
    "  if (ax < 3.0f) {\n"
    "    const float t = ax - 2.0f;\n"
    "    return t * (26.0f / 209.0f + t * (-45.0f / 209.0f\n"
    "           + t * (1.0f / 11.0f)));\n"
    "  }\n"
    "  return 0.0f;\n"
    "}\n";

static const char NPA_WEIGHT_SPLINE64[] =
    "  const float ax = fabs(x);\n"
    "  if (ax < 1.0f) {\n"
    "    return 1.0f + ax * (-3.0f / 2911.0f + ax * (-6387.0f / 2911.0f\n"
    "           + ax * (49.0f / 41.0f)));\n"
    "  }\n"
    "  if (ax < 2.0f) {\n"
    "    const float t = ax - 1.0f;\n"
    "    return t * (-2328.0f / 2911.0f + t * (4032.0f / 2911.0f\n"
    "           + t * (-24.0f / 41.0f)));\n"
    "  }\n"
    "  if (ax < 3.0f) {\n"
    "    const float t = ax - 2.0f;\n"
    "    return t * (582.0f / 2911.0f + t * (-1008.0f / 2911.0f\n"
    "           + t * (6.0f / 41.0f)));\n"
    "  }\n"
    "  if (ax < 4.0f) {\n"
    "    const float t = ax - 3.0f;\n"
    "    return t * (-97.0f / 2911.0f + t * (168.0f / 2911.0f\n"
    "           + t * (-1.0f / 41.0f)));\n"
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

/* One axis per pass. A separable kernel filters horizontally into an
 * intermediate, then vertically into the drawable, so the tap loop is
 * one-dimensional: the other axis is not filtered here at all.
 *
 * That is only sound because the intermediate keeps the source height and the
 * source plane's pixel format, while its width matches the drawable. Both
 * passes therefore land on a texel centre along the axis they do not filter,
 * where bilinear returns that texel exactly. Snapping is explicit so a rounding
 * difference cannot smear the untouched axis.
 *
 * Folding: a tap pair folds into one bilinear fetch at w1/(w0+w1) scaled by
 * w0+w1, which is algebraically the same two taps. The hardware clamps that
 * fraction to [0,1], so a pair whose weights change sign cannot fold; it costs
 * two fetches instead of one. The test is per pair, not per kernel. */
static const char NPA_SCALE_AXIS[] =
    "\n"
    "struct NpaAxis {\n"
    "  float2 size;\n"
    "  float st;\n"
    "  float pos;\n"
    "  float base;\n"
    "  int k;\n"
    "  bool down;\n"
    "  bool antiring;\n"
    "};\n"
    "\n"
    /* Direction and widening are separate decisions. The footprint says which
     * kernel applies, because that is a property of the geometry. Widening is
     * the anti-aliasing strength, and turning it off must not change which
     * kernel is chosen: with st pinned to 1 a footprint test on st would pick
     * the upscaling kernel for every downscale. */
    "static inline NpaAxis npa_axis(texture2d<float, access::sample> t,\n"
    "                               float2 uv, float2 dudx, float2 dudy) {\n"
    "  NpaAxis a;\n"
    "  a.size = float2(t.get_width(), t.get_height());\n"
    "  const float2 footprint = npa_footprint(dudx, dudy, a.size);\n"
    "  const float f = NP_HORIZONTAL ? footprint.x : footprint.y;\n"
    "  a.down = f > 1.0f;\n"
    "  a.st = a.down ? min(f, NP_ST_CAP) : 1.0f;\n"
    "  a.k = int(ceil(npa_support(a.down) * a.st));\n"
    "  a.antiring = a.down ? (NP_ANTIRING_DOWN != 0) : (NP_ANTIRING_UP != 0);\n"
    "  const float at = NP_HORIZONTAL ? uv.x * a.size.x : uv.y * a.size.y;\n"
    "  a.pos = at - 0.5f;\n"
    "  a.base = floor(a.pos);\n"
    "  return a;\n"
    "}\n"
    "\n"
    "static inline float4 npa_tap(texture2d<float, access::sample> t, sampler sn,\n"
    "                             float2 uv, float2 dudx, float2 dudy) {\n"
    "  const NpaAxis a = npa_axis(t, uv, dudx, dudy);\n"
    /* The unfiltered axis stays on a texel centre so bilinear cannot blur it. */
    "  const float2 keep = (floor(uv * a.size) + 0.5f) / a.size;\n"
    "  float4 acc = float4(0.0f);\n"
    "  float wsum = 0.0f;\n"
    "  for (int j = 0; j < a.k; ++j) {\n"
    "    const int i0 = 1 - a.k + 2 * j;\n"
    "    const float w0 = npa_kernel_w((a.pos - (a.base + float(i0))) / a.st, a.down);\n"
    "    const float w1 = npa_kernel_w((a.pos - (a.base + float(i0 + 1))) / a.st, a.down);\n"
    "    const float s = w0 + w1;\n"
    "    const float first = a.base + float(i0) + 0.5f;\n"
    "    const float2 at0 = NP_HORIZONTAL ? float2(first / a.size.x, keep.y)\n"
    "                                     : float2(keep.x, first / a.size.y);\n"
    "    if (w0 * w1 >= 0.0f && fabs(s) > 1e-8f) {\n"
    "      const float folded = first + w1 / s;\n"
    "      const float2 at = NP_HORIZONTAL ? float2(folded / a.size.x, keep.y)\n"
    "                                      : float2(keep.x, folded / a.size.y);\n"
    "      acc += t.sample(sn, at) * s;\n"
    "    } else {\n"
    "      const float second = first + 1.0f;\n"
    "      const float2 at1 = NP_HORIZONTAL ? float2(second / a.size.x, keep.y)\n"
    "                                       : float2(keep.x, second / a.size.y);\n"
    "      acc += t.sample(sn, at0) * w0;\n"
    "      acc += t.sample(sn, at1) * w1;\n"
    "    }\n"
    "    wsum += s;\n"
    "  }\n"
    "  float4 result = acc / max(wsum, 1e-5f);\n"
    /* Anti-ringing clamps to the two texels that straddle the sample point, as
     * libplacebo does. Those are a.base and a.base + 1 whatever the tap count
     * is: the loop pairs texels at even offsets, so an odd k straddles the
     * boundary between two pairs and no single pair brackets the point. */
    "  if (a.antiring) {\n"
    "    const float2 c0 = NP_HORIZONTAL ? float2((a.base + 0.5f) / a.size.x, keep.y)\n"
    "                                    : float2(keep.x, (a.base + 0.5f) / a.size.y);\n"
    "    const float2 c1 = NP_HORIZONTAL ? float2((a.base + 1.5f) / a.size.x, keep.y)\n"
    "                                    : float2(keep.x, (a.base + 1.5f) / a.size.y);\n"
    "    const float4 t0 = t.sample(sn, c0);\n"
    "    const float4 t1 = t.sample(sn, c1);\n"
    "    result = clamp(result, min(t0, t1), max(t0, t1));\n"
    "  }\n"
    "  return result;\n"
    "}\n";

/* The fragment shader only ever filters vertically; the pre-pass filters
 * horizontally. Both are compiled from this one source, so the caller renames
 * the loop for the pre-pass and NP_HORIZONTAL selects the axis. */
static const char NPA_SCALE_WRAPPERS[] =
    "\n"
    "static inline float npa_scale1(texture2d<float, access::sample> t, sampler sn,\n"
    "                              float2 uv, float2 dudx, float2 dudy) {\n"
    "  return npa_tap(t, sn, uv, dudx, dudy).r;\n"
    "}\n"
    "\n"
    "static inline float2 npa_scale2(texture2d<float, access::sample> t, sampler sn,\n"
    "                               float2 uv, float2 dudx, float2 dudy) {\n"
    "  return npa_tap(t, sn, uv, dudx, dudy).rg;\n"
    "}\n";

static const char NPA_SCALE_HORIZONTAL[] =
    "\n"
    "fragment float4 npaHorizontal(Varyings in [[ stage_in ]],\n"
    "                              texture2d<float, access::sample> src [[ texture(0) ]]) {\n"
    "  constexpr sampler sn(address::clamp_to_edge, filter::linear);\n"
    "  const float2 uv = in.texcoord;\n"
    "  return npa_h_tap(src, sn, uv, dfdx(uv), dfdy(uv));\n"
    "}\n";

typedef struct {
    const char *parameters;
    const char *weight;
    float support;
    /* Anti-ringing clamps to the two centre texels. That only removes anything
     * for a kernel with negative lobes; an all-positive kernel cannot ring, so
     * the clamp would cost fetches and change nothing. */
    int rings;
} NPAKernelSpec;

/* Every kernel runs as two passes: one axis into an intermediate, the other
 * into the drawable. A single 2D pass costs k^2 tap blocks while two passes
 * cost k per pass, so the separable form is never slower and wins quickly as
 * the footprint grows. */
static const NPAKernelSpec NPA_KERNELS[] = {
    {NULL, NULL, 0.0f, 0},
    {"#define NP_B 0.0f\n#define NP_C 0.5f\n", NPA_WEIGHT_CUBIC, 2.0f, 1},
    {"#define NP_B 0.33333334f\n#define NP_C 0.33333334f\n", NPA_WEIGHT_CUBIC, 2.0f, 1},
    {"#define NP_B 0.0f\n#define NP_C 0.75f\n", NPA_WEIGHT_CUBIC, 2.0f, 1},
    {"#define NP_A 2.0f\n", NPA_WEIGHT_LANCZOS, 2.0f, 1},
    {"#define NP_A 3.0f\n", NPA_WEIGHT_LANCZOS, 3.0f, 1},
    {"#define NP_B 1.0f\n#define NP_C 0.0f\n", NPA_WEIGHT_CUBIC, 2.0f, 0},
    {"", NPA_WEIGHT_SPLINE36, 3.0f, 1},
    {"", NPA_WEIGHT_SPLINE64, 4.0f, 1},
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
static void npa_hdr_log_kernel(int kernel, int kernel_up, int antiring,
                               int antiring_up, int antialiasing,
                               int transfer, int hdr, int edr)
{
    fprintf(stderr,
            "[npa-pp-hdr] shader down=%d up=%d antiring=%d/%d aa=%d"
            " transfer=%d hdr=%d edr=%d\n",
            kernel, kernel_up, antiring, antiring_up, antialiasing,
            transfer, hdr, edr);
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
static int npa_apply_scale(char **stage, int kernel, int kernel_up,
                           int antiring_down, int antiring_up, int antialiasing)
{
    if (kernel <= 0 || (size_t)kernel >= NPA_KERNEL_COUNT) {
        return 0;
    }
    const NPAKernelSpec *spec = &NPA_KERNELS[kernel];
    const NPAKernelSpec *spec_up = &NPA_KERNELS[kernel_up];

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
    char constants[256];
    /* Both kernels are baked, because the drawable can be resized while the
     * shader stays: the choice has to happen at sample time. The support is
     * the larger of the two so the tap loop covers whichever is picked.
     *
     * NP_ST_CAP is the anti-aliasing strength, not a kernel property: 1 keeps
     * the filter at its base width, 2 is the historical cap, and the quality
     * setting only exists to bound a degenerate derivative. */
    snprintf(constants, sizeof(constants),
             "#define NP_ST_CAP %.1ff\n"
             "#define NP_SUPPORT_DOWN %.1ff\n#define NP_SUPPORT_UP %.1ff\n"
             "#define NP_ANTIRING_DOWN %d\n#define NP_ANTIRING_UP %d\n"
             "#define NP_HORIZONTAL 0\n",
             npa_st_cap(antialiasing), spec->support, spec_up->support,
             antiring_down && spec->rings ? 1 : 0,
             antiring_up && spec_up->rings ? 1 : 0);
    int ok = npa_append(&helper, constants, strlen(constants));

    /* One weight function per kernel under its own name, plus the dispatcher
     * the tap loop calls. A disabled side still compiles; it is never taken. */
    static const char names[2][12] = {"npa_w_down", "npa_w_up"};
    const NPAKernelSpec *specs[2] = {spec, spec_up};
    for (int side = 0; ok && side < 2; side++) {
        char open[96];
        snprintf(open, sizeof(open), "\nstatic inline float %s(float x) {\n", names[side]);
        ok = npa_append(&helper, open, strlen(open))
             && npa_append(&helper, specs[side]->parameters, strlen(specs[side]->parameters))
             && npa_append(&helper, specs[side]->weight, strlen(specs[side]->weight));
    }
    /* st > 1 is a downscale: the source is larger than the drawable. */
    static const char dispatch[] =
        "\n/* down is the axis direction, not the widening amount, so pinning the\n"
        " * anti-aliasing strength cannot change which kernel is chosen. */\n"
        "static inline float npa_kernel_w(float x, bool down) {\n"
        "  return down ? npa_w_down(x) : npa_w_up(x);\n"
        "}\n"
        "\nstatic inline float npa_support(bool down) {\n"
        "  return down ? NP_SUPPORT_DOWN : NP_SUPPORT_UP;\n"
        "}\n";
    if (ok) {
        ok = npa_append(&helper, dispatch, strlen(dispatch))
             && npa_append(&helper, NPA_SCALE_FOOTPRINT, strlen(NPA_SCALE_FOOTPRINT));
    }
    if (ok) {
        /* The pre-pass filters the other axis, so the loop is emitted twice
         * with NP_HORIZONTAL flipped and the second copy renamed. */
        static const char pre[] =
            "\n#undef NP_HORIZONTAL\n#define NP_HORIZONTAL 1\n"
            "#define NpaAxis NpaAxisH\n#define npa_axis npa_h_axis\n"
            "#define npa_tap npa_h_tap\n";
        static const char post[] =
            "\n#undef npa_tap\n#undef npa_axis\n#undef NpaAxis\n"
            "#undef NP_HORIZONTAL\n#define NP_HORIZONTAL 0\n";
        /* Vertical definitions first: the wrappers below bind to them, and the
         * horizontal copy is renamed so both stay in one translation unit. */
        ok = npa_append(&helper, NPA_SCALE_AXIS, strlen(NPA_SCALE_AXIS))
             && npa_append(&helper, NPA_SCALE_WRAPPERS, strlen(NPA_SCALE_WRAPPERS))
             && npa_append(&helper, pre, strlen(pre))
             && npa_append(&helper, NPA_SCALE_AXIS, strlen(NPA_SCALE_AXIS))
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
    {
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

/* Reads one integer preference. A missing key yields 0, which every caller
 * treats as "off" or "keep the host behaviour". */
static int npa_int_preference(const char *key)
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
            (id)objc_getClass("NSString"), npa_sel("stringWithUTF8String:"), key
        )
    );
    if (!value) {
        return 0;
    }
    return (int)((long (*)(id, SEL))objc_msgSend)(value, npa_sel("integerValue"));
}

/* Zero means the host's bilinear sampling; anything else indexes NPA_KERNELS. */
static int npa_kernel_or_zero(int kernel)
{
    return (kernel > 0 && (size_t)kernel < NPA_KERNEL_COUNT) ? kernel : 0;
}

static int npa_scale_kernel_preference(void)
{
    return npa_kernel_or_zero(npa_int_preference(NPA_PREF_SCALE_KERNEL));
}

/* The upscaling kernel falls back to the downscaling choice, so a single
 * setting still governs both directions until the two are set apart. */
static int npa_scale_kernel_up_preference(void)
{
    const int up = npa_kernel_or_zero(npa_int_preference(NPA_PREF_SCALE_KERNEL_UP));
    return up ? up : npa_scale_kernel_preference();
}

/* Produce linear-light MSL for a floating-point extended-range drawable.
 * A missing anchor fails instead of emitting a shader with mixed encodings. */
static char *npa_rewrite_shader(
    const char *source,
    int transfer,
    int hdr,
    int edr,
    int kernel,
    int kernel_up,
    int antiring_down,
    int antiring_up,
    int antialiasing
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
    if (kernel > 0
        && !npa_apply_scale(&stage, kernel, kernel_up,
                            antiring_down, antiring_up, antialiasing)) {
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
 * same sampling shader. UNorm intermediates retain the source precision and
 * clamp out-of-range values; they are not lossless for negative-lobe filters.
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
    return npa_scale_kernel_preference() > 0;
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
    ((void (*)(id, SEL, id))objc_msgSend)(descriptor, npa_sel("setVertexFunction:"), vertex);
    ((void (*)(id, SEL, id))objc_msgSend)(descriptor, npa_sel("setFragmentFunction:"), function);
    ((void (*)(id, SEL, unsigned long))objc_msgSend)(descriptor, npa_sel("setSampleCount:"), 1);
    id attachment = ((id (*)(id, SEL, unsigned long))objc_msgSend)(
        npa_msg_id(descriptor, "colorAttachments"), npa_sel("objectAtIndexedSubscript:"), 0UL
    );
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

/* Returns 0 when the pre-pass could not run; the caller skips the frame and
 * logs, because the fragment shader is already vertical-only.
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
    /* Two layouts share these slots: NV12/P010 put chroma in one interleaved
     * texture, planar 4:4:4 / 4:2:0 use separate Cb and Cr. The host clears
     * every slot on a format change, so the populated ones identify the
     * layout: interleaved fills CbCr and leaves Cb/Cr empty. */
    id sources[3];
    int plane_count;
    const id luma = *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_Y);
    const id chroma = *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_CBCR);
    if (chroma) {
        sources[0] = luma;
        sources[1] = chroma;
        plane_count = 2;
    } else {
        sources[0] = luma;
        sources[1] = *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_CB);
        sources[2] = *(id *)((char *)renderer + NPA_RENDERER_TEXTURE_CR);
        plane_count = 3;
    }
    if (!sources[0] || !sources[1] || (plane_count == 3 && !sources[2])) {
        npa_hdr_log("separable pre-pass found no source planes", -1);
        return 0;
    }

    id layer = npa_msg_id(view, "metalLayer");
    CGSize drawable = {0, 0};
    if (layer) {
        drawable = ((CGSize (*)(id, SEL))objc_msgSend)(layer, npa_sel("drawableSize"));
    }
    if (drawable.width < 1.0 || drawable.height < 1.0) {
        npa_hdr_log("separable pre-pass has no drawable size", -1);
        return 0;
    }

    /* Every plane takes the drawable width, completing chroma reconstruction
     * horizontally here. Source heights are kept for the vertical pass. */
    uint64_t formats[3] = {0, 0, 0};
    int sizes[3][2] = {{0, 0}, {0, 0}, {0, 0}};
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
        sizes[index][0] = (int)(drawable.width + 0.5);
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
        id attachment = ((id (*)(id, SEL, unsigned long))objc_msgSend)(
            npa_msg_id(descriptor, "colorAttachments"), npa_sel("objectAtIndexedSubscript:"), 0UL
        );
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
    /* A vertical-only shader cannot safely draw the unfiltered source planes. */
    if (!npa_prepass_run(renderer)) {
        npa_hdr_log("separable pre-pass failed; frame skipped", -1);
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
    const int kernel_up = npa_scale_kernel_up_preference();
    const int antiring_down = npa_int_preference(NPA_PREF_ANTIRING) != 0;
    const int antiring_up = npa_int_preference(NPA_PREF_ANTIRING_UP) != 0;
    /* npa_st_cap folds anything outside the three settings to Skip, so the raw
     * value is passed through and the log shows what was actually stored. */
    const int antialiasing = npa_int_preference(NPA_PREF_ANTIALIASING);
    const int edr = npa_edr_available();
    if (!edr && kernel == 0 && kernel_up == 0) {
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

    npa_hdr_log_kernel(kernel, kernel_up, antiring_down, antiring_up,
                       antialiasing, transfer, hdr, edr);
    char *rewritten = npa_rewrite_shader(utf8, transfer, hdr, edr, kernel,
                                         kernel_up, antiring_down, antiring_up,
                                         antialiasing);
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
