/* Exercise the shader rewrite on the exact literal text the host generator
 * emits, and on a source that no longer matches the anchors. */
#include <assert.h>
#include <dlfcn.h>
#include <string.h>

/* The bridge only touches CoreAnimation from the layer hop, which this probe
 * never reaches; the loader hooks keep the frozen table path unused. */
static void *probe_caller;
static void *probe_base;
static int probe_dladdr(const void *address, Dl_info *info)
{
    (void)address;
    info->dli_fbase = probe_base;
    return 1;
}
#define dladdr probe_dladdr
#define __builtin_return_address(level) probe_caller
#include "../bridge/npa_renderer_pp_hdr_bridge.c"
#undef dladdr
#undef __builtin_return_address

static const char PQ_SOURCE[] =
    "fragment half4 fragmentShader(Varyings in [[ stage_in ]],\n"
    "  float3 ycbcr1 = float3(0.0);\n"
    "  ycbcr1.r += colorControl.x;  // brightness\n"
    "  float3 rgb1 = colorConversion.matrix * ycbcr1;\n"
    "  rgb1 = clamp(rgb1, 0.0, 1.0);\n"
    "  const float MP_REF_WHITE = 100;\n"
    "  const float trc_nom_peak = 10000.0 / MP_REF_WHITE;\n"
    "  rgb1 = pow(rgb1, float3(1.0 / PQ_M2));\n"
    "  rgb1 *= float3(10000.0 / MP_REF_WHITE);\n"
    "  rgb1 *= float3(1.0/trc_nom_peak);\n"
    "  const float src_peak = trc_nom_peak;\n"
    "  float sig_orig = sig;\n"
    "  rgb1 *= float3(sig / sig_orig);\n"
    "  rgb1 = clamp(rgb1, 0.0, 1.0);\n"
    "  rgb1 *= float3(trc_nom_peak);\n"
    "  const float3 lessthanequal = float3(0.0031308 <= rgb1.r, 0.0031308 <= rgb1.g, 0.0031308 <= rgb1.b);\n"
    "  rgb1 = mix(rgb1 * float3(12.92), float3(1.055) * pow(rgb1, float3(1.0/2.4)) - float3(0.055), lessthanequal);\n"
    "  return half4(half3(rgb1), 1.0);\n";

static const char HLG_SOURCE[] =
    "fragment half4 fragmentShader(Varyings in [[ stage_in ]],\n"
    "  float3 rgb1 = colorConversion.matrix * ycbcr1;\n"
    "  rgb1 = clamp(rgb1, 0.0, 1.0);\n"
    "  const float trc_nom_peak = 12.0;\n"
    "  rgb1 = mix(float3(4.0) * rgb1 * rgb1, exp((rgb1 - float3(HLG_C)) * float3(1.0/HLG_A)) + float3(HLG_B), lessthan);\n"
    "  rgb1 *= float3(1.0/trc_nom_peak);\n"
    "  const float src_peak = trc_nom_peak;\n"
    "  float sig_orig = sig;\n"
    "  rgb1 *= float3(sig / sig_orig);\n"
    "  rgb1 = clamp(rgb1, 0.0, 1.0);\n"
    "  rgb1 *= float3(trc_nom_peak);\n"
    "  const float3 lessthanequal = float3(0.0031308 <= rgb1.r, 0.0031308 <= rgb1.g, 0.0031308 <= rgb1.b);\n"
    "  rgb1 = mix(rgb1 * float3(12.92), float3(1.055) * pow(rgb1, float3(1.0/2.4)) - float3(0.055), lessthanequal);\n"
    "  return half4(half3(rgb1), 1.0);\n";

int main(void)
{
    /* PQ: the decoded nits/100 stay as-is, so no Hable tail and no sRGB OETF. */
    char *pq = npa_rewrite_shader(PQ_SOURCE, NPA_TRANSFER_PQ, 1, 1, 0);
    assert(pq);
    assert(strstr(pq, "rgb1 *= float3(10000.0 / MP_REF_WHITE);"));
    assert(!strstr(pq, "sig_orig"));
    assert(!strstr(pq, "lessthanequal"));
    assert(!strstr(pq, "1.0/trc_nom_peak"));
    assert(strstr(pq, "return float4(float3(rgb1), 1.0);"));
    assert(!strstr(pq, "half"));
    free(pq);

    /* HLG: scene linear gains its nominal 12x, still without tone mapping. */
    char *hlg = npa_rewrite_shader(HLG_SOURCE, NPA_TRANSFER_HLG, 1, 1, 0);
    assert(hlg);
    assert(strstr(hlg, "  rgb1 *= float3(trc_nom_peak);\n  return float4("));
    assert(!strstr(hlg, "sig_orig"));
    assert(!strstr(hlg, "lessthanequal"));
    free(hlg);

    /* SDR: BT.709 values become linear after each matrix product. */
    static const char SDR_SOURCE[] =
        "fragment half4 fragmentShader(Varyings in [[ stage_in ]],\n"
        "  float3 rgb1 = colorConversion.matrix * ycbcr1;\n"
        "  float3 rgb2 = colorConversion.matrix * ycbcr2;\n"
        "  return half4(half3(rgb1), 1.0);\n";
    char *sdr = npa_rewrite_shader(SDR_SOURCE, 0, 0, 1, 0);
    assert(sdr);
    assert(strstr(sdr, "float3 rgb1 = colorConversion.matrix * ycbcr1;\n"
                       "  rgb1 = max(rgb1, float3(0.0));\n"
                       "  rgb1 = pow((rgb1 + float3(0.055)) / float3(1.055), float3(2.4));\n"));
    assert(strstr(sdr, "float3 rgb2 = colorConversion.matrix * ycbcr2;\n"
                       "  rgb2 = max(rgb2, float3(0.0));\n"
                       "  rgb2 = pow((rgb2 + float3(0.055)) / float3(1.055), float3(2.4));\n"));
    assert(strstr(sdr, "return float4(float3(rgb1), 1.0);"));
    free(sdr);

    /* A 3D HDR shader mixes two eyes, so it keeps the host tone mapping. */
    static const char PQ_3D_SOURCE[] =
        "fragment half4 fragmentShader(Varyings in [[ stage_in ]],\n"
        "  float3 rgb1 = colorConversion.matrix * ycbcr1;\n"
        "  rgb1 *= float3(1.0/trc_nom_peak);\n"
        "  rgb1 *= float3(trc_nom_peak);\n"
        "  rgb1 = mix(rgb1, rgb1, lessthanequal);\n"
        "  float3 rgb2 = colorConversion.matrix * ycbcr2;\n"
        "  return half4(half3(rgb1), 1.0);\n";
    assert(!npa_rewrite_shader(PQ_3D_SOURCE, NPA_TRANSFER_PQ, 1, 1, 0));

    /* A host whose text no longer matches must fail instead of mixing curves. */
    assert(!npa_rewrite_shader("fragment float4 f() { return float4(0.0); }\n", 0, 0, 1, 0));
    assert(!npa_rewrite_shader(
        "fragment half4 fragmentShader() {\n  rgb1 *= float3(1.0/trc_nom_peak);\n",
        NPA_TRANSFER_HLG, 1, 1, 0
    ));
    return 0;
}
