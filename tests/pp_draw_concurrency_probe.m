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

typedef struct { union { void *align; unsigned char bytes[0x280]; } renderer; id<MTLTexture> y,c,out; id<MTLCommandQueue> queue; id<MTLBuffer> vb,controls; ScaleProbeView *view; } Fixture;
typedef struct { void (**vtable)(void *); pthread_mutex_t mutex; } HostMutex;
static void mutex_noop(void *p){(void)p;}
static void mutex_lock(void *p){pthread_mutex_lock(&((HostMutex *)p)->mutex);}
static void mutex_unlock(void *p){pthread_mutex_unlock(&((HostMutex *)p)->mutex);}
static void (*mutex_vtable[11])(void *)={[0]=mutex_noop,[1]=mutex_noop,[7]=mutex_lock,[10]=mutex_unlock};
static HostMutex host_mutex[2];
static Fixture fixtures[2];static id<MTLRenderPipelineState> final;
static int uploaded[2], skip_draw;
static dispatch_group_t uploads;
/* Mirror the hardware upload's strong slot replacement under its host lock.
 * Run while the draw wrapper is active, once for each renderer. */
static void upload_next(Fixture *f, int index){
 dispatch_group_async(uploads,dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT,0), ^{
  mutex_lock(&host_mutex[index]);
  id<MTLTexture> next=texture(f->y.device,MTLPixelFormatR8Unorm,(int)f->y.width,(int)f->y.height,MTLTextureUsageShaderRead);
  size_t count=next.width*next.height;uint8_t *pixels=malloc(count);memset(pixels,80+index*128,count);
  [next replaceRegion:MTLRegionMake2D(0,0,next.width,next.height) mipmapLevel:0 withBytes:pixels bytesPerRow:next.width];free(pixels);
  id *slot=(id *)(f->renderer.bytes+NPA_RENDERER_TEXTURE_Y);id old=*slot;*slot=[next retain];[old release];
  [f->y release];f->y=next;
  mutex_unlock(&host_mutex[index]);
 });
}
static void host_draw(void *renderer){
 if(skip_draw){skip_draw=0;return;} /* nextDrawable == nil */
 int index=*(id *)((char *)renderer+NPA_RENDERER_QUEUE)==fixtures[0].queue?0:1;
 Fixture *f=&fixtures[index];
 if(!uploaded[index]++){
  upload_next(f,index);
  /* The previous wrapper exposed real slots before the host acquired its lock.
   * Complete the upload in that window to make the old ownership bug reliable. */
  if(renderer==(void *)f->renderer.bytes) dispatch_group_wait(uploads,DISPATCH_TIME_FOREVER);
 }
 npa_host_mutex_call(*(void **)((char *)renderer+0x270),7);
 id<MTLCommandBuffer> cb=[f->queue commandBuffer];MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=f->out;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
 id<MTLRenderCommandEncoder> enc=[cb renderCommandEncoderWithDescriptor:pass];[enc setRenderPipelineState:final];[enc setVertexBuffer:f->vb offset:0 atIndex:0];
 [enc setFragmentBuffer:*(id *)((char *)renderer+NPA_RENDERER_COLOR_CONTROL) offset:0 atIndex:1];
 [enc setFragmentTexture:*(id *)((char *)renderer+NPA_RENDERER_TEXTURE_Y) atIndex:0];[enc setFragmentTexture:*(id *)((char *)renderer+NPA_RENDERER_TEXTURE_CBCR) atIndex:1];[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[enc endEncoding];[cb commit];[cb waitUntilCompleted];assert(!cb.error);npa_host_mutex_call(*(void **)((char *)renderer+0x270),10);
}
int main(void){@autoreleasepool{
 uploads=dispatch_group_create();id<MTLDevice> device=MTLCreateSystemDefaultDevice();char *s=strdup(SOURCE);assert(npa_apply_scale(&s,5,5,0,0,NPA_AA_QUALITY));assert(npa_apply_deband(&s,1));NSError *e=nil;id<MTLLibrary> lib=[device newLibraryWithSource:[NSString stringWithUTF8String:s] options:nil error:&e];free(s);assert(lib);
 MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[lib newFunctionWithName:@"vertexPassthrough"];pd.fragmentFunction=[lib newFunctionWithName:@"fragmentShader"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA32Float;final=[device newRenderPipelineStateWithDescriptor:pd error:&e];assert(final);
 const float vertices[][4]={{-1,-1,0,1},{3,-1,2,1},{-1,3,0,-1}};
 for(int i=0;i<2;i++){
  Fixture *f=&fixtures[i];int w=64+i*32,h=32+i*16;
  f->queue=[device newCommandQueue];f->view=[ScaleProbeView new];f->view->layer=[CAMetalLayer new];f->view->layer.drawableSize=CGSizeMake(w,h);
  f->vb=[device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
  f->y=texture(device,MTLPixelFormatR8Unorm,w,h,MTLTextureUsageShaderRead);f->c=texture(device,MTLPixelFormatRG8Unorm,w/2,h/2,MTLTextureUsageShaderRead);f->out=texture(device,MTLPixelFormatRGBA32Float,w,h,MTLTextureUsageRenderTarget);
  uint8_t *p=malloc(w*h);memset(p,64+i*128,w*h);[f->y replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0 withBytes:p bytesPerRow:w];memset(p,128,w*h);[f->c replaceRegion:MTLRegionMake2D(0,0,w/2,h/2) mipmapLevel:0 withBytes:p bytesPerRow:w];free(p);
  const float controls[3]={0,1,1};f->controls=[device newBufferWithBytes:controls length:sizeof(controls) options:MTLResourceStorageModeShared];
  host_mutex[i].vtable=mutex_vtable;pthread_mutex_init(&host_mutex[i].mutex,NULL);
  [f->y retain]; /* The real host slot owns a reference independent of fixture. */
  unsigned char *r=f->renderer.bytes;*(void **)(r+0x270)=&host_mutex[i];*(id *)(r+NPA_RENDERER_COLOR_CONTROL)=f->controls;*(id *)(r+NPA_RENDERER_DEVICE)=device;*(id *)(r+NPA_RENDERER_QUEUE)=f->queue;*(id *)(r+NPA_RENDERER_LIBRARY)=lib;*(id *)(r+NPA_RENDERER_VIEW)=f->view;*(id *)(r+NPA_RENDERER_VERTEX_BUFFER)=f->vb;*(id *)(r+NPA_RENDERER_TEXTURE_Y)=f->y;*(id *)(r+NPA_RENDERER_TEXTURE_CBCR)=f->c;
 }
 probe_base=(void *)((uintptr_t)host_draw-renderer_bindings[1].original);probe_caller=(void *)((uintptr_t)probe_base+renderer_bindings[1].caller);
 dispatch_apply(2,dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT,0),^(size_t index){
  for(int frame=0;frame<80;frame++){@autoreleasepool{
   Fixture *f=&fixtures[index];npa_renderer_pp_hdr_draw(f->renderer.bytes);
   dispatch_group_wait(uploads,DISPATCH_TIME_FOREVER);
   assert(*(id *)(f->renderer.bytes+NPA_RENDERER_COLOR_CONTROL)==f->controls);
   assert(*(id *)(f->renderer.bytes+NPA_RENDERER_TEXTURE_Y)==f->y);
   float result[4];[f->out getBytes:result bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
   assert(fabsf(result[0]-((frame?80:64)+index*128)/255.0f)<1e-4f);
  }}
 });
 skip_draw=1;npa_renderer_pp_hdr_draw(fixtures[0].renderer.bytes);
 uint16_t intermediate=0;
 [(id<MTLTexture>)npa_prepass.plane[0] getBytes:&intermediate bytesPerRow:2 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
 assert(abs((int)intermediate-80*257)<=2);
 puts("Concurrent draw, upload, snapshot isolation, and skipped drawable passed");
}}
