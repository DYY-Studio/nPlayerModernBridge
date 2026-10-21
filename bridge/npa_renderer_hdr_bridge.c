/* Metal renderer HDR output bridge (Post Processing path).
 *
 * nPlayer's Metal renderer hardcodes BGRA8Unorm output and tone-maps HDR to
 * SDR. This unit owns the runtime shader generator call so it can later emit
 * linear extended-range MSL; the manifests carry the frozen instruction and
 * call-site patches. */
#include <dlfcn.h>
#include <objc/objc.h>
#include <stdint.h>
#include <stdio.h>

#include "renderer-hdr-bindings.h"

#define NPA_EXPORT __attribute__((visibility("default")))

typedef id (*NPAOriginalShaderSourceFn)(void *renderer, int mode, int flag);

static void npa_hdr_log(const char *message, int status)
{
    fprintf(stderr, "[npa-hdr] %s (status=%d)\n", message, status);
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

NPA_EXPORT __attribute__((noinline)) id npa_renderer_shader_source(
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
    return original(renderer, mode, flag);
}
