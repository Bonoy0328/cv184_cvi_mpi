// Board-only synthetic pixels. Does not open cameras or user/template databases.
extern "C" {
#include "media.h"
#include "cvi_sys.h"
#include "cvi_vb.h"
#include "cvi_buffer.h"
}
#include "image.h"
#include "faceRec.h"
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <ctime>
using namespace pva::face;
static double ms(clockid_t c){timespec t;clock_gettime(c,&t);return t.tv_sec*1000.0+t.tv_nsec/1e6;}
template<class Fn>static void bench(const char* name,Fn f){volatile unsigned sum=0;for(int i=0;i<2;++i)sum+=f();
 double w=ms(CLOCK_MONOTONIC),c=ms(CLOCK_THREAD_CPUTIME_ID);for(int i=0;i<30;++i)sum+=f();
 printf("INPUT_BENCH %s wall_ms=%.3f cpu_ms=%.3f checksum=%u\n",name,(ms(CLOCK_MONOTONIC)-w)/30,(ms(CLOCK_THREAD_CPUTIME_ID)-c)/30,sum);}
int main(){
 int lock=open("/tmp/ky50-sensor.lock",O_CREAT|O_RDWR|O_CLOEXEC,0600);
 if(lock<0||flock(lock,LOCK_EX|LOCK_NB)){perror("stop media application first");return 1;}
 const int w=540,h=960;std::vector<uint8_t> nv(size_t(w)*h*3/2);unsigned random=1987;
 for(auto& v:nv){random=random*1664525u+1013904223u;v=random>>24;}
 if(CVI_SYS_Init())return 2;
 VI_VPSS_MODE_S mode={};for(int i=0;i<VI_MAX_PIPE_NUM;++i)mode.aenMode[i]=VI_OFFLINE_VPSS_OFFLINE;
 if(CVI_SYS_SetVIVPSSMode(&mode))return 3;
 VB_CONFIG_S vb={};vb.u32MaxPoolCnt=1;
 vb.astCommPool[0].u32BlkSize=COMMON_GetPicBufferSize(w,h,PIXEL_FORMAT_NV12,DATA_BITWIDTH_8,COMPRESS_MODE_NONE,DEFAULT_ALIGN);
 vb.astCommPool[0].u32BlkCnt=1;vb.astCommPool[0].enRemapMode=VB_REMAP_MODE_CACHED;
 if(CVI_VB_SetConfig(&vb)||CVI_VB_Init())return 4;
 Image hw(w,h),full=imageFromNv12(nv.data(),w,h);
 int rc=media_face_color_nv12(nv.data(),w,h,hw.data.data(),w*3);
 if(rc){media_face_color_stop();CVI_VB_Exit();CVI_SYS_Exit();return 5;}
 double sum=0;int maximum=0;unsigned above2=0;
 for(size_t i=0;i<full.data.size();++i){int e=std::abs(int(full.data[i])-int(hw.data[i]));sum+=e;maximum=std::max(maximum,e);above2+=e>2;}
 printf("INPUT_COLOR pixels=%zu mae=%.6f max=%d above2=%u\n",full.data.size(),sum/full.data.size(),maximum,above2);
 if(maximum>2)rc=6;
 Detection face{{120,200,390,500},{180,280,315,279,247,355,191,430,305,428},.99f};FaceRecognizer rec;
 bench("reference_full_plus_align",[&](){Image f=imageFromNv12(nv.data(),w,h),a,b;rec.align(f,face,a);rec.align(f,face,b,1.2f);return a.data[900]+b.data[900];});
 bench("direct_two_align",[&](){Image a,b;rec.alignNv12(nv.data(),w,h,face,a);rec.alignNv12(nv.data(),w,h,face,b,1.2f);return a.data[900]+b.data[900];});
 bench("vpss_copy_csc_align",[&](){Image f(w,h),a,b;if(media_face_color_nv12(nv.data(),w,h,f.data.data(),w*3))std::exit(7);rec.align(f,face,a);rec.align(f,face,b,1.2f);return a.data[900]+b.data[900];});
 media_face_color_stop();CVI_VB_Exit();CVI_SYS_Exit();close(lock);
 if(!rc)puts("PASS NV12 hardware BGR channels, strides and bounded color difference");return rc;
}
