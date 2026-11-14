#include "hand_overlay.h"
#include <string.h>
#include <math.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
/* Constant lane indices keep the entire transpose in NEON registers. The
 * old loop/array version spilled every tile to the stack at -O2. */
static inline __attribute__((always_inline)) void transpose8(uint8x8_t r[8])
{
    uint8x8x2_t a0=vtrn_u8(r[0],r[1]),a1=vtrn_u8(r[2],r[3]);
    uint8x8x2_t a2=vtrn_u8(r[4],r[5]),a3=vtrn_u8(r[6],r[7]);
    uint16x4x2_t b0=vtrn_u16(vreinterpret_u16_u8(a0.val[0]),vreinterpret_u16_u8(a1.val[0]));
    uint16x4x2_t b1=vtrn_u16(vreinterpret_u16_u8(a0.val[1]),vreinterpret_u16_u8(a1.val[1]));
    uint16x4x2_t b2=vtrn_u16(vreinterpret_u16_u8(a2.val[0]),vreinterpret_u16_u8(a3.val[0]));
    uint16x4x2_t b3=vtrn_u16(vreinterpret_u16_u8(a2.val[1]),vreinterpret_u16_u8(a3.val[1]));
#define PAIR(k,a,b,lane) do { \
    uint32x2x2_t c=vtrn_u32(vreinterpret_u32_u16(a.val[lane]),vreinterpret_u32_u16(b.val[lane])); \
    r[k]=vreinterpret_u8_u32(c.val[0]);r[k+4]=vreinterpret_u8_u32(c.val[1]); \
} while(0)
    PAIR(0,b0,b2,0);PAIR(1,b1,b3,0);PAIR(2,b0,b2,1);PAIR(3,b1,b3,1);
#undef PAIR
}
#endif

int hand_copy_y(uint8_t *dst,unsigned dw,unsigned dh,const uint8_t *src,
                unsigned sw,unsigned sh,unsigned stride,unsigned x,unsigned y)
{
    if(!dst||!src||!dw||!dh||stride<sw||x>sw||y>sh||dw>sw-x||dh>sh-y) return -1;
    for(unsigned row=0;row<dh;++row) memcpy(dst+(size_t)row*dw,src+(size_t)(y+row)*stride+x,dw);
    return 0;
}
int hand_rotate_ccw90(uint8_t *dst,const uint8_t *src,unsigned w,
                     unsigned h,unsigned stride,unsigned unit)
{
    if(!dst||!src||!w||!h||w>65535||h>65535||(unit!=1&&unit!=2)||stride<w*unit) return -1;
#ifdef __ARM_NEON
    /* Preserve VU pairing while transposing both channels independently. */
    unsigned full_w=w&~7U,full_h=h&~7U;
    for(unsigned by=0;by<full_h;by+=8) for(unsigned bx=0;bx<full_w;bx+=8) {
        uint8x8_t v[8],u[8];
#define LOAD(k) do { \
        const uint8_t *p=src+(size_t)(by+k)*stride+bx*unit; \
        if(unit==1)v[k]=vld1_u8(p); \
        else {uint8x8x2_t vu=vld2_u8(p);v[k]=vu.val[0];u[k]=vu.val[1];} \
} while(0)
        LOAD(0);LOAD(1);LOAD(2);LOAD(3);LOAD(4);LOAD(5);LOAD(6);LOAD(7);
#undef LOAD
        transpose8(v);if(unit==2)transpose8(u);
#define STORE(k) do { \
        uint8_t *p=dst+((size_t)(w-1-bx-k)*h+by)*unit; \
        if(unit==1)vst1_u8(p,v[k]); \
        else {uint8x8x2_t vu={{v[k],u[k]}};vst2_u8(p,vu);} \
} while(0)
        STORE(0);STORE(1);STORE(2);STORE(3);STORE(4);STORE(5);STORE(6);STORE(7);
#undef STORE
    }
    for(unsigned y=0;y<h;++y) for(unsigned x=y<full_h?full_w:0;x<w;++x) {
        const uint8_t *s=src+(size_t)y*stride+x*unit;
        uint8_t *d=dst+((size_t)(w-1-x)*h+y)*unit;
        d[0]=s[0];if(unit==2) d[1]=s[1];
    }
#else
    /* Tiles bound both source and destination working sets. */
    for(unsigned by=0;by<h;by+=16) for(unsigned bx=0;bx<w;bx+=16) {
        unsigned ymax=by+16<h?by+16:h,xmax=bx+16<w?bx+16:w;
        for(unsigned x=bx;x<xmax;++x) for(unsigned y=by;y<ymax;++y) {
            const uint8_t *s=src+(size_t)y*stride+x*unit;
            uint8_t *d=dst+((size_t)(w-1-x)*h+y)*unit;
            d[0]=s[0];if(unit==2) d[1]=s[1];
        }
    }
#endif
    return 0;
}
int hand_map(const HandPoseRes_t *copy,const HandViewport *view,HandOverlay *drawing)
{
    if(!copy||!view||!drawing||!view->input_width||!view->input_height||!view->scaled_width||!view->scaled_height) return -1;
    double sx=(double)view->scaled_width/view->input_width;
    double sy=(double)view->scaled_height/view->input_height;
    drawing->status=copy->status;
    drawing->left=lround((copy->rect.centerX-copy->rect.centerWidth/2.0)*sx-view->crop_x);
    drawing->right=lround((copy->rect.centerX+copy->rect.centerWidth/2.0)*sx-view->crop_x);
    drawing->top=lround((copy->rect.centerY-copy->rect.centerHeight/2.0)*sy-view->crop_y);
    drawing->bottom=lround((copy->rect.centerY+copy->rect.centerHeight/2.0)*sy-view->crop_y);
    for(int k=0;k<7;++k) {
        drawing->points[2*k]=lround(copy->keypoints[2*k]*sx-view->crop_x);
        drawing->points[2*k+1]=lround(copy->keypoints[2*k+1]*sy-view->crop_y);
    }
    return 0;
}
static void dot(uint8_t *rgb,int w,int h,int x,int y,uint32_t color)
{
    if(x<0||y<0||x>=w||y>=h) return;
    uint8_t *p=rgb+((size_t)y*w+x)*3;
    p[0]=color>>16;p[1]=color>>8;p[2]=color;
}
static int clip(int x,int max) { return x<0?0:x>=max?max-1:x; }
static void overlay_draw(uint8_t *rgb,unsigned width,unsigned height,const HandOverlay *pose,int points,uint32_t color)
{
    if(!rgb||!pose||!width||!height||width>65535||height>65535) return;
    const int w=width,h=height;
    int l=pose->left,t=pose->top,r=pose->right,b=pose->bottom;
    if(r>l&&b>t&&r>=0&&b>=0&&l<w&&t<h) {
        l=clip(l,w);r=clip(r,w);t=clip(t,h);b=clip(b,h);
        /* Box and all seven landmarks use the same current-frame color. */
        for(int k=-2;k<=2;++k) {
            for(int x=l;x<=r;++x) { dot(rgb,w,h,x,t+k,color);dot(rgb,w,h,x,b+k,color); }
            for(int y=t;y<=b;++y) { dot(rgb,w,h,l+k,y,color);dot(rgb,w,h,r+k,y,color); }
        }
    }
    for(int k=0;k<points;++k) {
        int x=pose->points[k*2],y=pose->points[k*2+1];
        if(x<0||y<0||x>=w||y>=h) continue;
        for(int dy=-6;dy<=6;++dy) for(int dx=-6;dx<=6;++dx) {
            int d=dx*dx+dy*dy;
            if(d<=36) dot(rgb,w,h,x+dx,y+dy,color);
        }
    }
}


void hand_overlay_reset(HandOverlayState *s) { memset(s,0,sizeof(*s)); }
void hand_overlay_detected(HandOverlayState *s)
{
    /* A new true result breaks the miss streak immediately, even when the
     * following feature computation is slow. Keep the last completed drawing
     * until its replacement is ready; do not invent a quality/color. */
    s->missing=0;s->miss_started_ms=0;
}
void hand_overlay_update(HandOverlayState *s,const HandOverlay *drawing,int found,uint64_t now)
{
    if(found>0&&drawing) {
        s->drawing=*drawing;s->updated_ms=now;s->valid=1;hand_overlay_detected(s);
    } else if(found==0&&s->valid) {
        if(!s->missing)s->miss_started_ms=now;
        if(++s->missing>=HAND_DRAW_MISS_FRAMES)s->valid=0;
    } else hand_overlay_reset(s);
}
int hand_overlay_snapshot(const HandOverlayState *s,uint64_t now,HandOverlay *drawing)
{
    if(!s->valid)return 0;
    if(now>=s->updated_ms&&now-s->updated_ms>HAND_DRAW_RESULT_MS)return 0;
    if(s->missing&&now>=s->miss_started_ms&&now-s->miss_started_ms>=HAND_DRAW_MISS_MS)return 0;
    *drawing=s->drawing;return 1;
}

int hand_overlay_display_status(int quality,int confirmed,int enrolling)
{
    if(enrolling)return quality;
    return confirmed?STATUS_OK:quality==STATUS_OK?-1:quality;
}
void hand_draw(uint8_t *rgb,unsigned w,unsigned h,const HandOverlay *pose)
{
    if(pose)overlay_draw(rgb,w,h,pose,7,pose->status<0?0x40bfffu:pose->status==STATUS_OK?HAND_COLOR_OK:HAND_COLOR_INVALID);
}
int face_map(const FacePoseRes_t *copy,const HandViewport *view,HandOverlay *drawing)
{
    if(!copy)return -1;
    HandPoseRes_t geometry={.rect=copy->rect,.status=copy->status};
    memcpy(geometry.keypoints,copy->keypoints,sizeof(copy->keypoints));
    return hand_map(&geometry,view,drawing);
}
void face_draw(uint8_t *rgb,unsigned w,unsigned h,const HandOverlay *pose)
{
    if(pose)overlay_draw(rgb,w,h,pose,5,pose->status<0?0x40bfffu:pose->status==STATUS_OK?HAND_COLOR_OK:HAND_COLOR_INVALID);
}
int color_nv21_ccw90(uint8_t *nv12,const uint8_t *y,const uint8_t *vu,
                     unsigned w,unsigned h,unsigned ys,unsigned uvs)
{
    if(!nv12||!y||!vu||!w||!h||((w|h)&1)||ys<w||uvs<w)return -1;
    if(hand_rotate_ccw90(nv12,y,w,h,ys,1))return -1;
    uint8_t *uv=nv12+(size_t)w*h;
    if(hand_rotate_ccw90(uv,vu,w/2,h/2,uvs,2))return -1;
    for(size_t i=0;i<(size_t)w*h/2;i+=2){uint8_t v=uv[i];uv[i]=uv[i+1];uv[i+1]=v;}
    return 0;
}
int color_nv21_copy(uint8_t *nv12,const uint8_t *y,const uint8_t *vu,
                     unsigned w,unsigned h,unsigned ys,unsigned uvs)
{
    if(!nv12||!y||!vu||!w||!h||((w|h)&1)||ys<w||uvs<w)return -1;
    if(hand_copy_y(nv12,w,h,y,w,h,ys,0,0))return -1;
    uint8_t *uv=nv12+(size_t)w*h;
    for(unsigned row=0;row<h/2;row++) {
        unsigned x=0;const uint8_t *in=vu+(size_t)row*uvs;
        uint8_t *out=uv+(size_t)row*w;
#ifdef __ARM_NEON
        for(;x+16<=w;x+=16)vst1q_u8(out+x,vrev16q_u8(vld1q_u8(in+x)));
#endif
        for(;x<w;x+=2){out[x]=in[x+1];out[x+1]=in[x];}
    }
    return 0;
}
