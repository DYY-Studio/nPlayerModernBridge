/* Rewrite the captured host shader with one scaling kernel, for the MSL
 * compile test. Not part of the shipped dylib. */
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
static void *probe_caller; static void *probe_base;
static int probe_dladdr(const void *a, Dl_info *i){(void)a;i->dli_fbase=probe_base;return 1;}
#define dladdr probe_dladdr
#define __builtin_return_address(l) probe_caller
#include "../bridge/npa_renderer_pp_hdr_bridge.c"
#undef dladdr
#undef __builtin_return_address
static char *slurp(const char *p){FILE*f=fopen(p,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char*b=malloc(n+1);fread(b,1,n,f);b[n]=0;fclose(f);return b;}
int main(int argc,char**argv){
  char *s=slurp(argv[1]); if(!s){fprintf(stderr,"read %s\n",argv[1]);return 2;}
  /* argv[5] = downscale kernel, argv[6] = upscale kernel (defaults to 5),
   * argv[7] = downscaling anti-ringing, argv[8] = upscaling anti-ringing,
   * argv[9] = anti-aliasing setting (0 skip, 1 balance, 2 quality). */
  int down=atoi(argv[5]);
  int up=argc>6?atoi(argv[6]):down;
  int ar_down=argc>7?atoi(argv[7]):0;
  int ar_up=argc>8?atoi(argv[8]):ar_down;
  int aa=argc>9?atoi(argv[9]):0;
  char *r=npa_rewrite_shader(s,atoi(argv[3]),atoi(argv[4]),1,down,up,ar_down,ar_up,aa,argc>10?atoi(argv[10]):0);
  if(!r){fprintf(stderr,"REWRITE FAILED\n");return 3;}
  FILE*f=fopen(argv[2],"wb");fputs(r,f);fclose(f);
  free(r);free(s);return 0;
}
