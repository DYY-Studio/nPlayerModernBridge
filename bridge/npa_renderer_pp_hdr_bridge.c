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

/* Produce linear-light MSL for a floating-point extended-range drawable.
 * A missing anchor fails instead of emitting a shader with mixed encodings. */
static char *npa_rewrite_shader(const char *source, int transfer, int hdr)
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
    if (hdr) {
        /* The host tone-maps rgb1 only; a 3D return would mix a linear left
         * eye with an encoded right eye, so 3D keeps the original shader. */
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
    if (!npa_edr_available()) {
        /* Without CAEDRMetadata the layer stays SDR: keep the tone-mapped
         * shader and only benefit from the 16-bit drawable. */
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

    char *rewritten = npa_rewrite_shader(utf8, transfer, hdr);
    if (!rewritten) {
        npa_hdr_log("shader was not rewritten; keeping the host shader", transfer);
        if (hdr) {
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
    npa_configure_layer_async(renderer, transfer, hdr);
    return result;
}
