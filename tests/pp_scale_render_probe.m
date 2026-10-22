/* Exercise the production pre-pass and generated MSL on a real Metal queue. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <assert.h>
#include "../bridge/npa_renderer_pp_hdr_bridge.c"

@interface ScaleProbeView : NSObject {
@public
    CAMetalLayer *layer;
}
- (id)metalLayer;
@end
@implementation ScaleProbeView
- (id)metalLayer { return layer; }
@end

static const char SOURCE[] =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct Vertex { packed_float2 position; packed_float2 texcoord; };\n"
    "struct Varyings { float4 position [[position]]; float2 texcoord; };\n"
    "vertex Varyings vertexPassthrough(const device Vertex *v [[buffer(0)]], uint i [[vertex_id]]) {\n"
    "  Varyings o; o.position=float4(float2(v[i].position),0,1); o.texcoord=v[i].texcoord; return o;\n}\n"
    "fragment float4 fragmentShader(Varyings in [[stage_in]],\n"
    "  texture2d<float> textureY [[texture(0)]], texture2d<float> textureCbCr [[texture(1)]]) {\n"
    NPA_SAMPLER_ANCHOR NPA_TEXCOORD_ANCHOR
    "  return float4(textureY.sample(s, texcoord).r, textureCbCr.sample(s, texcoord).rg, 1);\n}\n";

static id<MTLTexture> texture(id<MTLDevice> device, MTLPixelFormat format,
                              int width, int height, MTLTextureUsage usage)
{
    MTLTextureDescriptor *desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = usage;
    return [device newTextureWithDescriptor:desc];
}

static int check_activation(id<MTLDevice> device)
{
    char *source = strdup(SOURCE);
    assert(npa_apply_scale(&source, 5, 5, 0, 0, NPA_AA_QUALITY));
    NSError *error = nil;
    id<MTLLibrary> on = [device newLibraryWithSource:[NSString stringWithUTF8String:source]
                                           options:nil error:&error];
    free(source);
    id<MTLLibrary> off = [device newLibraryWithSource:[NSString stringWithUTF8String:SOURCE]
                                            options:nil error:&error];
    if (!on || !off) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
    union { void *alignment; unsigned char bytes[0x240]; } renderer = {0};
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSString *key = @NPA_PREF_SCALE_KERNEL;
    id old = [[defaults objectForKey:key] retain];
    *(id *)(renderer.bytes + NPA_RENDERER_LIBRARY) = on;
    [defaults setInteger:0 forKey:key];
    int stays_on = npa_prepass_active(renderer.bytes);
    *(id *)(renderer.bytes + NPA_RENDERER_LIBRARY) = off;
    [defaults setInteger:5 forKey:key];
    int stays_off = !npa_prepass_active(renderer.bytes);
    if (old) [defaults setObject:old forKey:key]; else [defaults removeObjectForKey:key];
    [old release]; [on release]; [off release];
    printf("library activation ignores pending preferences: on=%d off=%d\n", stays_on, stays_off);
    return !stays_on || !stays_off;
}

static int check_weights(id<MTLDevice> device)
{
    /* libplacebo filters.c, sampled at 0.5, 1.5, 2.5, 3.5 and divided by w(0).
     * Cubic's /6 versus upstream's /p0 is a constant that cancels in the sum. */
    static const float expected[][4] = {
        {0},
        {0.5625f, -0.0625f, 0, 0},
        {0.6015625f, -0.0390625f, 0, 0},
        {0.59375f, -0.09375f, 0, 0},
        {0.57315917f, -0.06368435f, 0, 0},
        {0.60792710f, -0.13509491f, 0.02431708f, 0},
        {0.71875f, 0.03125f, 0, 0},
        {0.59868421f, -0.11842105f, 0.01973684f, 0},
        {0.60035211f, -0.12676056f, 0.03169014f, -0.00528169f},
    };
    int failed = 0;
    for (int kernel = 1; kernel < (int)NPA_KERNEL_COUNT; kernel++) {
        char *source = strdup(SOURCE);
        assert(npa_apply_scale(&source, kernel, kernel, 0, 0, NPA_AA_QUALITY));
        NSString *msl = [[NSString stringWithUTF8String:source] stringByAppendingString:
            @"\nfragment float4 weightProbe(Varyings in [[stage_in]]) { return float4(npa_w_down(in.position.x) / npa_w_down(0.0f)); }\n"];
        free(source);
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:msl options:nil error:&error];
        if (!library) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [library newFunctionWithName:@"vertexPassthrough"];
        pd.fragmentFunction = [library newFunctionWithName:@"weightProbe"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!pipeline) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
        const float vertices[][4] = {{-1,-1,0,1}, {3,-1,2,1}, {-1,3,0,-1}};
        id<MTLBuffer> vb = [device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
        id<MTLTexture> output = texture(device, MTLPixelFormatRGBA32Float, 4, 1, MTLTextureUsageRenderTarget);
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = output;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:vb offset:0 atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (cb.error) { fprintf(stderr, "%s\n", cb.error.description.UTF8String); return 1; }
        float result[16];
        [output getBytes:result bytesPerRow:sizeof(result) fromRegion:MTLRegionMake2D(0,0,4,1) mipmapLevel:0];
        for (int x = 0; x < 4; x++) {
            if (!isfinite(result[4*x]) || fabsf(result[4*x] - expected[kernel][x]) > 2e-6f
                || (result[4*x] < -1e-6f && !NPA_KERNELS[kernel].rings)) {
                fprintf(stderr, "FAIL: kernel=%d x=%.1f got=%g expected=%g rings=%d\n",
                        kernel, x + 0.5, result[4*x], expected[kernel][x], NPA_KERNELS[kernel].rings);
                failed = 1;
            }
        }
        [pd.vertexFunction release]; [pd.fragmentFunction release]; [pd release];
        [pipeline release]; [vb release]; [output release]; [queue release]; [library release];
    }
    return failed;
}

/* Keep one compiled library and drawable while switching chroma layout/format. */
static int check_plane_changes(id<MTLDevice> device, void *renderer)
{
    id *slot = (id *)((char *)renderer + NPA_RENDERER_TEXTURE_CBCR);
    id original = *slot;
    int failed = 0;
    const MTLPixelFormat formats[] = {MTLPixelFormatRG32Float, MTLPixelFormatRG16Float};
    for (int i = 0; i < 2; i++) {
        id<MTLTexture> changed = texture(device, formats[i], 128, 8, MTLTextureUsageShaderRead);
        *slot = changed;
        if (!npa_prepass_run(renderer)) return 1;
        id<MTLTexture> intermediate = *slot;
        if (intermediate.height != 8 || intermediate.pixelFormat != formats[i]) {
            fprintf(stderr, "FAIL: plane change %d cached height=%lu format=%lu\n", i,
                    (unsigned long)intermediate.height, (unsigned long)intermediate.pixelFormat);
            failed = 1;
        }
        npa_prepass_restore(renderer);
        *slot = original;
        [changed release];
    }
    id<MTLCommandQueue> queue = *(id *)((char *)renderer + NPA_RENDERER_QUEUE);
    id<MTLCommandBuffer> completion = [queue commandBuffer];
    [completion commit]; [completion waitUntilCompleted];
    return failed || completion.error != nil;
}

static int check_render(id<MTLDevice> device, int kernel, int antiring,
                        int source_width, int output_width, int ramp,
                        float scale_x, float scale_y)
{
    char *source = strdup(SOURCE);
    assert(npa_apply_scale(&source, kernel, kernel, antiring, antiring, NPA_AA_QUALITY));
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:
        [NSString stringWithUTF8String:source] options:nil error:&error];
    free(source);
    if (!library) {
        fprintf(stderr, "%s\n", error.description.UTF8String);
        return 1;
    }

    const int height = 8;
    id<MTLTexture> y = texture(device, MTLPixelFormatR32Float, source_width, height,
                              MTLTextureUsageShaderRead);
    id<MTLTexture> uv = texture(device, MTLPixelFormatRG32Float, source_width / 2, height / 2,
                               MTLTextureUsageShaderRead);
    float *pixels = calloc(source_width * height, sizeof(float));
    for (int row = 0; row < height; row++)
        for (int x = 0; x < source_width; x++)
            pixels[row * source_width + x] = ramp ? 0.5f : x % 2;
    [y replaceRegion:MTLRegionMake2D(0, 0, source_width, height) mipmapLevel:0
           withBytes:pixels bytesPerRow:source_width * sizeof(float)];
    for (int row = 0; row < height / 2; row++)
        for (int x = 0; x < source_width / 2; x++) {
            pixels[row * source_width + 2 * x] = ramp ? (float)x / (source_width / 2) : 0.5f;
            pixels[row * source_width + 2 * x + 1] = 0.5f;
        }
    [uv replaceRegion:MTLRegionMake2D(0, 0, source_width / 2, height / 2) mipmapLevel:0
            withBytes:pixels bytesPerRow:source_width * sizeof(float)];
    free(pixels);

    float vertices[][4] = {
        {-1,-1,0,1}, {1,-1,1,1}, {-1,1,0,0},
        {-1,1,0,0}, {1,-1,1,1}, {1,1,1,0}
    };
    for (int i = 0; i < 6; i++) {
        vertices[i][0] *= scale_x;
        vertices[i][1] *= scale_y;
    }
    id<MTLBuffer> vb = [device newBufferWithBytes:vertices length:sizeof(vertices)
                                       options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    ScaleProbeView *view = [ScaleProbeView new];
    view->layer = [CAMetalLayer new];
    view->layer.drawableSize = CGSizeMake(output_width, height);
    union { void *alignment; unsigned char bytes[0x240]; } renderer = {0};
    *(id *)(renderer.bytes + NPA_RENDERER_DEVICE) = device;
    *(id *)(renderer.bytes + NPA_RENDERER_QUEUE) = queue;
    *(id *)(renderer.bytes + NPA_RENDERER_LIBRARY) = library;
    *(id *)(renderer.bytes + NPA_RENDERER_VERTEX_BUFFER) = vb;
    *(id *)(renderer.bytes + NPA_RENDERER_VIEW) = view;
    *(id *)(renderer.bytes + NPA_RENDERER_TEXTURE_Y) = y;
    *(id *)(renderer.bytes + NPA_RENDERER_TEXTURE_CBCR) = uv;
    if (!npa_prepass_run(renderer.bytes)) {
        fprintf(stderr, "FAIL: horizontal pre-pass did not run\n");
        return 1;
    }

    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [library newFunctionWithName:@"vertexPassthrough"];
    pd.fragmentFunction = [library newFunctionWithName:@"fragmentShader"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!pipeline) {
        fprintf(stderr, "%s\n", error.description.UTF8String);
        return 1;
    }
    id<MTLTexture> output = texture(device, MTLPixelFormatRGBA32Float, output_width, height,
                                   MTLTextureUsageRenderTarget);
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = output;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
    [encoder setVertexBuffer:vb offset:0 atIndex:0];
    [encoder setFragmentTexture:*(id *)(renderer.bytes + NPA_RENDERER_TEXTURE_Y) atIndex:0];
    [encoder setFragmentTexture:*(id *)(renderer.bytes + NPA_RENDERER_TEXTURE_CBCR) atIndex:1];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [encoder endEncoding];
    npa_prepass_restore(renderer.bytes);
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.error) {
        fprintf(stderr, "%s\n", cb.error.description.UTF8String);
        return 1;
    }

    float *result = calloc(output_width * height * 4, sizeof(float));
    [output getBytes:result bytesPerRow:output_width * 4 * sizeof(float)
         fromRegion:MTLRegionMake2D(0, 0, output_width, height) mipmapLevel:0];
    float max_error = 0;
    for (int row = 2; row < height - 2; row++)
        for (int x = 16; x < output_width - 16; x++) {
            float u = ((x + 0.5f) / output_width - 0.5f) / scale_x + 0.5f;
            float v = ((row + 0.5f) / height - 0.5f) / scale_y + 0.5f;
            if (u < 0.1f || u > 0.9f || v < 0.1f || v > 0.9f) continue;
            float expected = ramp ? (u * source_width / 2 - 0.5f)
                                      / (source_width / 2)
                                  : source_width == output_width ? x % 2 : 0.5f;
            float actual = result[(row * output_width + x) * 4 + (ramp ? 1 : 0)];
            if (!isfinite(actual)) { free(result); return 1; }
            max_error = fmaxf(max_error, fabsf(actual - expected));
        }
    free(result);
    printf("kernel=%d ar=%d %d->%d %s geometry=%g/%g max_error=%g\n", kernel, antiring,
           source_width, output_width, ramp ? "chroma ramp" : "luma stripes", scale_x, scale_y, max_error);
    int cache_failed = 0;
    if (source_width == 256 && output_width == 128 && !ramp)
        cache_failed = check_plane_changes(device, renderer.bytes);
    npa_prepass_reset(&npa_prepass);
    [pd.vertexFunction release];
    [pd.fragmentFunction release];
    [pd release]; [pipeline release]; [output release];
    [view->layer release]; [view release];
    [y release]; [uv release]; [vb release]; [queue release]; [library release];
    return cache_failed || max_error > 2e-4f;
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { fprintf(stderr, "Metal device required\n"); return 2; }
        int failed = check_render(device, 5, 0, 256, 128, 0, 1, 1);
        failed |= check_render(device, 5, 1, 128, 128, 0, 1, 1);
        failed |= check_render(device, 1, 0, 256, 256, 1, 1, 1);
        failed |= check_render(device, 1, 0, 256, 256, 1, 0.75f, 1);
        failed |= check_render(device, 1, 0, 256, 256, 1, 0.77f, 1);
        failed |= check_render(device, 1, 0, 256, 256, 1, 1, 0.5f);
        failed |= check_render(device, 1, 0, 256, 256, 1, 1.25f, 1.25f);
        failed |= check_weights(device);
        failed |= check_activation(device);
        if (!failed) puts("Metal scaling checks passed (including all eight kernels)");
        return failed;
    }
}
