#include "hand_overlay.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void)
{
    assert(hand_overlay_display_status(STATUS_OK,0,0)==-1);
    assert(hand_overlay_display_status(STATUS_FAILED,0,0)==STATUS_FAILED);
    assert(hand_overlay_display_status(STATUS_FAILED,1,0)==STATUS_OK);
    assert(hand_overlay_display_status(STATUS_OK,0,1)==STATUS_OK);
    /* Color conversion uses actual V/U samples, different padded strides and
     * rectangular input. Reference coordinates are independent of rotation code. */
    uint8_t cy[12*4],cv[14*2],nv[8*4*3/2];
    memset(cy,0xee,sizeof(cy));memset(cv,0xee,sizeof(cv));
    for(int y=0;y<4;y++)for(int x=0;x<8;x++)cy[y*12+x]=y*8+x;
    for(int y=0;y<2;y++)for(int x=0;x<4;x++){cv[y*14+x*2]=180+y*4+x;cv[y*14+x*2+1]=30+y*4+x;}
    assert(!color_nv21_ccw90(nv,cy,cv,8,4,12,14));
    for(int y=0;y<8;y++)for(int x=0;x<4;x++)assert(nv[y*4+x]==x*8+7-y);
    for(int y=0;y<4;y++)for(int x=0;x<2;x++) {
        assert(nv[32+y*4+x*2]==30+x*4+3-y);
        assert(nv[32+y*4+x*2+1]==180+x*4+3-y);
    }
    assert(color_nv21_ccw90(nv,cy,cv,7,4,12,14)==-1);
    assert(color_nv21_ccw90(nv,cy,cv,8,4,12,7)==-1);
    uint8_t copied[8*4*3/2];
    assert(!color_nv21_copy(copied,cy,cv,8,4,12,14));
    for(int y=0;y<4;y++)for(int x=0;x<8;x++)assert(copied[y*8+x]==y*8+x);
    for(int y=0;y<2;y++)for(int x=0;x<4;x++) {
        assert(copied[32+y*8+x*2]==30+y*4+x);
        assert(copied[32+y*8+x*2+1]==180+y*4+x);
    }
    assert(color_nv21_copy(copied,cy,cv,7,4,12,14)<0);
    assert(color_nv21_copy(copied,cy,cv,8,4,12,7)<0);
    FacePoseRes_t face={.rect={100,100,40,40},.status=-1};
    HandPoseRes_t hand={.rect={300,100,40,40},.status=STATUS_OK};
    HandViewport direct={480,854,480,854,0,0};HandOverlay fd,hd;
    uint8_t *both=calloc(480*854,3);assert(both);
    assert(!face_map(&face,&direct,&fd)&&!hand_map(&hand,&direct,&hd));
    face_draw(both,480,854,&fd);hand_draw(both,480,854,&hd);
    assert(both[(80*480+100)*3]==0x40&&both[(80*480+100)*3+2]==0xff);
    assert(both[(80*480+300)*3]==0x36&&both[(80*480+300)*3+1]==0xff);free(both);
    puts("PASS RGB NV21 padded planes to full rotated NV12 with real UV; simultaneous face and hand overlays");
    /* Simulated GDC padded half-size output: 576x960, effective 540x960.
     * Removing storage padding must preserve ALL rows/columns of the image. */
    uint8_t *padded=malloc(576*960),*gray=malloc(540*960);
    assert(padded&&gray);
    memset(padded,0xee,576*960);
    for(int y=0;y<960;++y) for(int x=0;x<540;++x) padded[y*576+x]=(x+3*y)%251;
    assert(hand_copy_y(gray,540,960,padded,576,960,576,0,0)==0);
    for(int y=0;y<960;++y) for(int x=0;x<540;++x) assert(gray[y*540+x]==(x+3*y)%251);
    assert(hand_copy_y(gray,540,960,padded,576,960,576,0,1)==-1);
    assert(hand_copy_y(gray,540,960,padded,576,960,539,0,0)==-1);
    free(gray);free(padded);
    /* Unequal dimensions, padded rows, and partial 16-pixel tiles. Check the
     * inverse coordinate transform independently, including both VU bytes. */
    for(unsigned unit=1;unit<=2;++unit) {
        const unsigned w=960,h=540,stride=w*unit+32;
        uint8_t *src=malloc(stride*h),*dst=malloc(w*h*unit);
        assert(src&&dst);memset(src,0xee,stride*h);
        for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x)
            for(unsigned c=0;c<unit;++c) src[y*stride+x*unit+c]=(x+3*y+71*c)%251;
        assert(hand_rotate_ccw90(dst,src,w,h,stride,unit)==0);
        for(unsigned ry=0;ry<w;++ry) for(unsigned rx=0;rx<h;++rx)
            for(unsigned c=0;c<unit;++c) assert(dst[(ry*h+rx)*unit+c]==(w-1-ry+3*rx+71*c)%251);
        assert(hand_rotate_ccw90(dst,src,w,h,w*unit-1,unit)==-1);
        free(src);free(dst);
    }
    HandPoseRes_t raw={0},original;
    raw.rect=(RECT_t){270,480,200,400};
    uint16_t points[]={0,0,100,200,270,480,539,959,100,100,400,800,65535,65535};
    memcpy(raw.keypoints,points,sizeof(points));original=raw;
    HandPoseRes_t copy=raw;
    HandViewport view={540,960,486,864,2,4};HandOverlay drawing;
    assert(hand_map(&copy,&view,&drawing)==0);
    assert(drawing.left==151&&drawing.right==331&&drawing.top==248&&drawing.bottom==608);
    assert(drawing.points[0]==-2&&drawing.points[1]==-4);
    assert(drawing.points[2]==88&&drawing.points[3]==176);
    assert(drawing.points[4]==241&&drawing.points[5]==428);
    assert(drawing.points[6]==483&&drawing.points[7]==859);
    uint8_t *rgb=calloc(480*854,3);assert(rgb);
    hand_draw(rgb,480,854,&drawing);
    assert(rgb[(176*480+88)*3]==(HAND_COLOR_OK>>16));  /* valid mapped point */
    assert(rgb[0]==0);                 /* offscreen point is not pinned to (0,0) */
    assert(memcmp(&raw,&original,sizeof(raw))==0);
    assert(memcmp(&copy,&original,sizeof(copy))==0);
    for(int status=0;status<=22;status++) {
        copy.status=status;HandPoseRes_t saved=copy;
        assert(!hand_map(&copy,&view,&drawing));memset(rgb,0,480*854*3);
        hand_draw(rgb,480,854,&drawing);
        unsigned color=status==STATUS_OK?HAND_COLOR_OK:HAND_COLOR_INVALID,pixels=0;
        for(unsigned i=0;i<480*854;i++) {
            unsigned c=(rgb[i*3]<<16)|(rgb[i*3+1]<<8)|rgb[i*3+2];
            if(c){assert(c==color);pixels++;}
        }
        assert(pixels>100&&memcmp(&copy,&saved,sizeof(copy))==0);
        assert(rgb[(248*480+200)*3]==color>>16); /* box matches every point */
        assert(rgb[(176*480+88)*3]==color>>16);
    }
    drawing.left=-50;drawing.top=-90;drawing.right=700;drawing.bottom=1000;
    drawing.points[0]=0;drawing.points[1]=0;
    drawing.points[2]=479;drawing.points[3]=853;
    hand_draw(rgb,480,854,&drawing); /* ASan covers clipped edges and corners. */
    free(rgb);
    HandOverlayState state={0};HandOverlay held,good=drawing;good.status=STATUS_OK;
    hand_overlay_update(&state,&good,1,1000);
    assert(hand_overlay_snapshot(&state,1000,&held));
    assert(!memcmp(&held,&good,sizeof(good)));
    hand_overlay_update(&state,NULL,0,1100);
    assert(hand_overlay_snapshot(&state,1249,&held));
    assert(!hand_overlay_snapshot(&state,1250,&held)); /* 150ms, no third frame needed */
    hand_overlay_detected(&state); /* next true, feature calculation in progress */
    assert(hand_overlay_snapshot(&state,1300,&held));
    assert(!hand_overlay_snapshot(&state,1501,&held)); /* watchdog still bounds old result */
    hand_overlay_update(&state,&good,1,2000);
    hand_overlay_update(&state,NULL,0,2010);
    hand_overlay_update(&state,NULL,0,2020);
    assert(hand_overlay_snapshot(&state,2020,&held));
    hand_overlay_update(&state,NULL,0,2030);
    assert(!hand_overlay_snapshot(&state,2030,&held)); /* third miss hides immediately */
    hand_overlay_detected(&state);assert(!hand_overlay_snapshot(&state,2031,&held));
    good.status=STATUS_TOO_BRIGHT;hand_overlay_update(&state,&good,1,2200);
    assert(hand_overlay_snapshot(&state,2200,&held)&&held.status==STATUS_TOO_BRIGHT);
    assert(hand_overlay_snapshot(&state,2199,&held)); /* concurrent publish newer than UI tick */
    hand_overlay_update(&state,NULL,-1,2210);assert(!hand_overlay_snapshot(&state,2210,&held));
    hand_overlay_update(&state,&good,1,2300);hand_overlay_reset(&state);
    assert(!hand_overlay_snapshot(&state,2300,&held)); /* source/capture failure clears immediately */
    puts("PASS display-only debounce: 2 misses retained, 3 misses/150ms hide, true breaks streak before slow features, 500ms completed-result watchdog, copied quality/geometry, source/error reset");
    puts("PASS full half-size Y, CCW90 Y/VU, padded stride, viewport transform, clipped drawing, immutable algorithm result, all STATUS values with uniform red/green box and points");
    return 0;
}
