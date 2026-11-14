#include "pixels.h"
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif
void pixels_gray(uint8_t *rgb,const uint8_t *y,unsigned count)
{
    unsigned x=0;
#ifdef __ARM_NEON
    for(;x+8<=count;x+=8) {
        int16x8_t l=vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(y+x))),vdupq_n_s16(16));
        uint16x8_t scaled=vcombine_u16(vqrshrun_n_s32(vmull_n_s16(vget_low_s16(l),298),8),
                                      vqrshrun_n_s32(vmull_n_s16(vget_high_s16(l),298),8));
        uint8x8_t gray=vqmovn_u16(scaled);
        uint8x8x3_t out={{gray,gray,gray}};vst3_u8(rgb+3*x,out);
    }
#endif
    for(;x<count;x++) {
        int v=(298*((int)y[x]-16)+128)>>8;
        uint8_t gray=v<0?0:v>255?255:v;
        rgb[x*3]=rgb[x*3+1]=rgb[x*3+2]=gray;
    }
}
#ifdef __ARM_NEON
#include <arm_neon.h>
static uint8x8_t color(int16x8_t y,int16x8_t u,int16x8_t v,int ku,int kv)
{
    int32x4_t lo=vmull_n_s16(vget_low_s16(y),298);
    int32x4_t hi=vmull_n_s16(vget_high_s16(y),298);
    lo=vmlal_n_s16(lo,vget_low_s16(u),ku);hi=vmlal_n_s16(hi,vget_high_s16(u),ku);
    lo=vmlal_n_s16(lo,vget_low_s16(v),kv);hi=vmlal_n_s16(hi,vget_high_s16(v),kv);
    return vqmovn_u16(vcombine_u16(vqrshrun_n_s32(lo,8),vqrshrun_n_s32(hi,8)));
}
void pixels_nv21(uint8_t *rgb,const uint8_t *y,const uint8_t *vu,unsigned count)
{
    const uint8x8_t vi={0,0,2,2,4,4,6,6},ui={1,1,3,3,5,5,7,7};
    for(unsigned x=0;x<count;x+=8) {
        uint8x8_t uv=vld1_u8(vu+x);
        int16x8_t l=vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(y+x))),vdupq_n_s16(16));
        int16x8_t u=vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vtbl1_u8(uv,ui))),vdupq_n_s16(128));
        int16x8_t v=vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vtbl1_u8(uv,vi))),vdupq_n_s16(128));
        uint8x8x3_t out={{color(l,u,v,0,409),color(l,u,v,-100,-208),color(l,u,v,516,0)}};
        vst3_u8(rgb+3*x,out);
    }
}
void pixels_blend(uint8_t *dst,const uint8_t *rgb,const uint32_t *argb,unsigned count)
{
    for(unsigned x=0;x<count;x+=8) {
        uint8x8x4_t fg=vld4_u8((const uint8_t *)(argb+x)); /* B,G,R,A on ARM LE */
        uint8x8x3_t bg=vld3_u8(rgb+3*x),out;
        const uint8x8_t inverse=vmvn_u8(fg.val[3]);
#define BLEND(c) do { \
            uint16x8_t n=vmull_u8(fg.val[2-c],fg.val[3]); \
            n=vmlal_u8(n,bg.val[c],inverse); \
            n=vaddq_u16(n,vdupq_n_u16(128)); \
            out.val[c]=vshrn_n_u16(vaddq_u16(n,vshrq_n_u16(n,8)),8); \
} while(0)
        BLEND(0);BLEND(1);BLEND(2);
#undef BLEND
        vst3_u8(dst+3*x,out);
    }
}
#else
static uint8_t clamp(int x) { return x<0?0:x>255?255:x; }
void pixels_nv21(uint8_t *rgb,const uint8_t *y,const uint8_t *vu,unsigned count)
{
    for(unsigned x=0;x<count;x++) {
        int l=298*((int)y[x]-16),v=(int)vu[x&~1U]-128,u=(int)vu[(x&~1U)+1]-128;
        rgb[3*x]=clamp((l+409*v+128)>>8);
        rgb[3*x+1]=clamp((l-100*u-208*v+128)>>8);
        rgb[3*x+2]=clamp((l+516*u+128)>>8);
    }
}
void pixels_blend(uint8_t *dst,const uint8_t *rgb,const uint32_t *argb,unsigned count)
{
    for(unsigned x=0;x<count;x++) {
        unsigned p=argb[x],a=p>>24;
        for(unsigned c=0;c<3;c++) dst[3*x+c]=(((p>>(16-8*c))&255)*a+rgb[3*x+c]*(255-a)+127)/255;
    }
}
#endif
