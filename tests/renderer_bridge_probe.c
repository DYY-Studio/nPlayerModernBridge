/* Execute the production YUV420P8 path with controlled loader answers.
 * The original renderers are real typed functions; no fabricated code is executed. */
#include <assert.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdint.h>

static void *probe_caller;
static void *probe_base;
static int loader_ok = 1;
static int probe_dladdr(const void *address, Dl_info *info)
{
    assert(address == probe_caller);
    info->dli_fbase = probe_base;
    return loader_ok;
}
static const struct mach_header *probe_image_header(uint32_t index)
{
    (void)index;
    return NULL; /* Old dyld-index lookup must not work in this host. */
}
#define dladdr probe_dladdr
#define _dyld_get_image_header probe_image_header
#define __builtin_return_address(level) probe_caller
#include "../bridge/npa_renderer_highbit_dylib.c"
#undef dladdr
#undef _dyld_get_image_header
#undef __builtin_return_address

static NPAFFmpegVideoFrame *expected_frame;
static CVPixelBufferPoolRef expected_pool;
static int selected;
static CVPixelBufferRef original_standard(NPAFFmpegVideoFrame *frame, CVPixelBufferPoolRef pool)
{
    assert(frame == expected_frame && pool == expected_pool);
    selected = 1;
    return (CVPixelBufferRef)(uintptr_t)0x1234;
}
static CVPixelBufferRef original_plus(NPAFFmpegVideoFrame *frame, CVPixelBufferPoolRef pool)
{
    assert(frame == expected_frame && pool == expected_pool);
    selected = 2;
    return (CVPixelBufferRef)(uintptr_t)0x1234;
}

int main(void)
{
    NPAFFmpegVideoFrame frame = {0};
    frame.pixel_format = AV_PIX_FMT_YUV420P;
    expected_frame = &frame;
    expected_pool = (CVPixelBufferPoolRef)(uintptr_t)0x5678;
    NPAOriginalPixelBufferFn functions[] = {original_standard, original_plus};
    for (size_t i = 0; i < 2; i++) {
        probe_base = (void *)((uintptr_t)functions[i] - renderer_bindings[i].original);
        probe_caller = (void *)((uintptr_t)probe_base + renderer_bindings[i].caller);
        selected = 0;
        assert(npa_renderer_create_pixel_buffer(&frame, expected_pool) == (CVPixelBufferRef)(uintptr_t)0x1234);
        assert(selected == (int)i + 1);
    }
    selected = 0;
    probe_caller = (void *)((uintptr_t)probe_caller + 4);
    assert(!npa_renderer_create_pixel_buffer(&frame, expected_pool));
    assert(!selected);
    loader_ok = 0;
    assert(!npa_renderer_create_pixel_buffer(&frame, expected_pool));
    loader_ok = 1;
    probe_base = NULL;
    assert(!npa_renderer_create_pixel_buffer(&frame, expected_pool));
    return 0;
}
