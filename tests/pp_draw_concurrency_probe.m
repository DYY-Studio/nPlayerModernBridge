/* Exercise the exported draw wrapper on two real Metal renderer fixtures.
 * Only frozen host-call resolution is substituted; encoding and readback are real. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
static void *probe_base,*probe_caller;
static int probe_dladdr(const void *address,Dl_info *info){(void)address;info->dli_fbase=probe_base;return 1;}
#define dladdr probe_dladdr
#define __builtin_return_address(level) probe_caller
#define main production_checks_main
#include "pp_scale_render_probe.m"
#undef main
#undef dladdr
#undef __builtin_return_address

typedef struct { union { void *align; unsigned char bytes[0x240]; } renderer; id<MTLTexture> y,c,out; id<MTLCommandQueue> queue; id<MTLBuffer> vb; ScaleProbeView *view; } Fixture;
static Fixture fixtures[2];static id<MTLRenderPipelineState> final;
static void host_draw(void *renderer){
 Fixture *f=renderer==(void *)fixtures[0].renderer.bytes?&fixtures[0]:&fixtures[1];
 id<MTLCommandBuffer> cb=[f->queue commandBuffer];MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=f->out;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
 id<MTLRenderCommandEncoder> enc=[cb renderCommandEncoderWithDescriptor:pass];[enc setRenderPipelineState:final];[enc setVertexBuffer:f->vb offset:0 atIndex:0];
 [enc setFragmentTexture:*(id *)((char *)renderer+NPA_RENDERER_TEXTURE_Y) atIndex:0];[enc setFragmentTexture:*(id *)((char *)renderer+NPA_RENDERER_TEXTURE_CBCR) atIndex:1];[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[enc endEncoding];[cb commit];[cb waitUntilCompleted];assert(!cb.error);
}
int main(void){@autoreleasepool{
 id<MTLDevice> device=MTLCreateSystemDefaultDevice();char *s=strdup(SOURCE);assert(npa_apply_scale(&s,5,5,0,0,NPA_AA_QUALITY));assert(npa_apply_deband(&s,1));NSError *e=nil;id<MTLLibrary> lib=[device newLibraryWithSource:[NSString stringWithUTF8String:s] options:nil error:&e];free(s);assert(lib);
 MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[lib newFunctionWithName:@"vertexPassthrough"];pd.fragmentFunction=[lib newFunctionWithName:@"fragmentShader"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA32Float;final=[device newRenderPipelineStateWithDescriptor:pd error:&e];assert(final);
 const float vertices[][4]={{-1,-1,0,1},{3,-1,2,1},{-1,3,0,-1}};
 for(int i=0;i<2;i++){
  Fixture *f=&fixtures[i];int w=64+i*32,h=32+i*16;
  f->queue=[device newCommandQueue];f->view=[ScaleProbeView new];f->view->layer=[CAMetalLayer new];f->view->layer.drawableSize=CGSizeMake(w,h);
  f->vb=[device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
  f->y=texture(device,MTLPixelFormatR8Unorm,w,h,MTLTextureUsageShaderRead);f->c=texture(device,MTLPixelFormatRG8Unorm,w/2,h/2,MTLTextureUsageShaderRead);f->out=texture(device,MTLPixelFormatRGBA32Float,w,h,MTLTextureUsageRenderTarget);
  uint8_t *p=malloc(w*h);memset(p,64+i*128,w*h);[f->y replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0 withBytes:p bytesPerRow:w];memset(p,128,w*h);[f->c replaceRegion:MTLRegionMake2D(0,0,w/2,h/2) mipmapLevel:0 withBytes:p bytesPerRow:w];free(p);
  unsigned char *r=f->renderer.bytes;*(id *)(r+NPA_RENDERER_DEVICE)=device;*(id *)(r+NPA_RENDERER_QUEUE)=f->queue;*(id *)(r+NPA_RENDERER_LIBRARY)=lib;*(id *)(r+NPA_RENDERER_VIEW)=f->view;*(id *)(r+NPA_RENDERER_VERTEX_BUFFER)=f->vb;*(id *)(r+NPA_RENDERER_TEXTURE_Y)=f->y;*(id *)(r+NPA_RENDERER_TEXTURE_CBCR)=f->c;
 }
 probe_base=(void *)((uintptr_t)host_draw-renderer_bindings[1].original);probe_caller=(void *)((uintptr_t)probe_base+renderer_bindings[1].caller);
 dispatch_apply(2,dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT,0),^(size_t index){
  for(int frame=0;frame<80;frame++){@autoreleasepool{
   Fixture *f=&fixtures[index];npa_renderer_pp_hdr_draw(f->renderer.bytes);
   assert(*(id *)(f->renderer.bytes+NPA_RENDERER_TEXTURE_Y)==f->y);
   float result[4];[f->out getBytes:result bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
   assert(fabsf(result[0]-(64+index*128)/255.0f)<1e-4f);
  }}
 });
 puts("Concurrent draw and restore passed");
}}
