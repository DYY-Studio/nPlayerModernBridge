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

/* The wrappers must follow npa_tap, so they are appended last. */
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

/* Tap pairing: fold each 2x2 block of taps into one bilinear fetch. The fold
 * places the fetch at w1/(w0+w1) and scales by w0+w1, which is algebraically
 * identical to the two separate taps. The hardware clamps that fraction to
 * [0,1], so a block whose weights change sign cannot be folded; it falls back
 * to four fetches. The test is per block, not per kernel: Catmull-Rom pairs at
 * some footprints and not others.
 *
 * Sampling uses a linear sampler. At a texel centre it returns that texel
 * exactly, so the unfolded path is unaffected. */
static const char NPA_SCALE_TAP_PAIR[] =
    "\n"
    "static inline float4 npa_tap(texture2d<float, access::sample> t, sampler sn,\n"
    "                             float2 uv, float2 dudx, float2 dudy) {\n"
    "  const float2 size = float2(t.get_width(), t.get_height());\n"
    "  const float2 st = clamp(npa_footprint(dudx, dudy, size), 1.0f, NP_ST_MAX);\n"
    "  const int2 k = int2(ceil(NP_SUPPORT * st));\n"
    "  const float2 pos = uv * size - 0.5f;\n"
    "  const float2 base = floor(pos);\n"
    "  float4 acc = float4(0.0f);\n"
    "  float wsum = 0.0f;\n"
    "  for (int j = 0; j < k.y; ++j) {\n"
    "    const int iy = 1 - k.y + 2 * j;\n"
    "    const float wy0 = npa_kernel_w((pos.y - (base.y + float(iy))) / st.y);\n"
    "    const float wy1 = npa_kernel_w((pos.y - (base.y + float(iy + 1))) / st.y);\n"
    "    const float sy = wy0 + wy1;\n"
    "    const bool foldY = wy0 * wy1 >= 0.0f && fabs(sy) > 1e-8f;\n"
    "    const float fy = foldY ? wy1 / sy : 0.0f;\n"
    "    for (int i = 0; i < k.x; ++i) {\n"
    "      const int ix = 1 - k.x + 2 * i;\n"
    "      const float wx0 = npa_kernel_w((pos.x - (base.x + float(ix))) / st.x);\n"
    "      const float wx1 = npa_kernel_w((pos.x - (base.x + float(ix + 1))) / st.x);\n"
    "      const float sx = wx0 + wx1;\n"
    "      const bool foldX = wx0 * wx1 >= 0.0f && fabs(sx) > 1e-8f;\n"
    "      if (foldX && foldY) {\n"
    "        const float fx = wx1 / sx;\n"
    "        const float2 at = base + float2(float(ix) + fx, float(iy) + fy) + 0.5f;\n"
    "        acc += t.sample(sn, at / size) * (sx * sy);\n"
    "        wsum += sx * sy;\n"
    "      } else {\n"
    "        const float2 corner = base + float2(float(ix), float(iy)) + 0.5f;\n"
    "        const float2 step = float2(1.0f) / size;\n"
    "        acc += t.sample(sn, corner / size) * (wx0 * wy0);\n"
    "        acc += t.sample(sn, corner / size + float2(step.x, 0.0f)) * (wx1 * wy0);\n"
    "        acc += t.sample(sn, corner / size + float2(0.0f, step.y)) * (wx0 * wy1);\n"
    "        acc += t.sample(sn, corner / size + step) * (wx1 * wy1);\n"
    "        wsum += (wx0 + wx1) * (wy0 + wy1);\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  return acc / max(wsum, 1e-5f);\n"
    "}\n";

typedef struct {
    const char *parameters;
    const char *weight;
    float support;
} NPAKernelSpec;

static const NPAKernelSpec NPA_KERNELS[] = {
    {NULL, NULL, 0.0f},
    {"#define NP_B 0.0f\n#define NP_C 0.5f\n", NPA_WEIGHT_CUBIC, 2.0f},
    {"#define NP_B 0.33333334f\n#define NP_C 0.33333334f\n", NPA_WEIGHT_CUBIC, 2.0f},
    {"#define NP_B 0.0f\n#define NP_C 0.75f\n", NPA_WEIGHT_CUBIC, 2.0f},
    {"#define NP_A 2.0f\n", NPA_WEIGHT_LANCZOS, 2.0f},
    {"#define NP_A 3.0f\n", NPA_WEIGHT_LANCZOS, 3.0f},
    {"#define NP_B 1.0f\n#define NP_C 0.0f\n", NPA_WEIGHT_CUBIC, 2.0f},
};

#define NPA_KERNEL_COUNT (sizeof(NPA_KERNELS) / sizeof(NPA_KERNELS[0]))

typedef id (*NPAOriginalShaderSourceFn)(void *renderer, int mode, int flag);

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
             "#define NP_SUPPORT %.1ff\n#define NP_ST_MAX %s\n",
             spec->support, NPA_SCALE_ST_MAX);
    const char prologue[] = "\nstatic inline float npa_kernel_w(float x) {\n";
    int ok = npa_append(&helper, constants, strlen(constants))
             && npa_append(&helper, spec->parameters, strlen(spec->parameters))
             && npa_append(&helper, prologue, strlen(prologue))
             && npa_append(&helper, spec->weight, strlen(spec->weight))
             && npa_append(&helper, NPA_SCALE_FOOTPRINT, strlen(NPA_SCALE_FOOTPRINT));
    if (ok) {
        ok = npa_append(&helper, NPA_SCALE_TAP_PAIR, strlen(NPA_SCALE_TAP_PAIR))
             && npa_append(&helper, NPA_SCALE_WRAPPERS, strlen(NPA_SCALE_WRAPPERS));
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
    const int applied = npa_sub(stage, NPA_NAMESPACE_ANCHOR, merged.data, 1);
    free(merged.data);
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
