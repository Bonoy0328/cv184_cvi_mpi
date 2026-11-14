#include "face_ir_calibration.h"
#include "biometric_worker.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int detect_calls,region_calls;
static int live_detector(uint8_t *data,uint16_t w,uint16_t h){(void)data;(void)w;(void)h;++detect_calls;return 1;}
static int live_region(const uint8_t *data,uint16_t w,uint16_t h,const PvaFaceGeometry *g){(void)data;(void)w;(void)h;assert(g->score>0);++region_calls;return 1;}

static FacePoseRes_t pose(int i)
{
    FacePoseRes_t p={.score=.98f,.status=STATUS_OK};
    int cx=240+i%9*3,cy=390+i%7*4;
    p.rect=(RECT_t){cx,cy,160,200};
    int pts[]={-40,-38,40,-38,0,0,-28,45,28,45};
    for(int j=0;j<5;++j){p.keypoints[2*j]=cx+pts[2*j];p.keypoints[2*j+1]=cy+pts[2*j+1];}
    return p;
}
static void mapped_point(float x,float y,float *a,float *b){*a=1.02f*x+.03f*y+8;*b=.01f*x+.99f*y-12;}
static PvaFaceGeometry ir_face(const FacePoseRes_t *p)
{
    PvaFaceGeometry g={.score=.98f};
    mapped_point(p->rect.centerX-80,p->rect.centerY-100,&g.box[0],&g.box[1]);
    mapped_point(p->rect.centerX+80,p->rect.centerY+100,&g.box[2],&g.box[3]);
    for(int j=0;j<5;++j)mapped_point(p->keypoints[2*j],p->keypoints[2*j+1],&g.keypoints[2*j],&g.keypoints[2*j+1]);
    return g;
}
int main(int argc,char **argv)
{
    if(argc==2) {
        int rc=face_ir_calibration_init("/nonexistent-ky50-profile-check",argv[1]);
        if(rc)return 1;
        FaceIrCalibrationStatus verified;face_ir_calibration_status(0,&verified);
        printf("PASS shared application profile rms_px=%.3f\n",verified.rms_px);return 0;
    }
    assert(argc==1);
    char dir[]="/tmp/face-ir-cal-XXXXXX",empty[]="/tmp/face-ir-default-XXXXXX";
    assert(mkdtemp(dir)&&mkdtemp(empty));
    assert(face_ir_calibration_init(dir,NULL)==1);
    FacePoseRes_t p=pose(0);PvaFaceGeometry ir=ir_face(&p),out;
    FaceIrPair pair={1000000,1010000};FaceIrCalibrationStatus state;
    FaceBackend backend={.live_ir=live_detector,.live_ir_region=live_region};uint8_t input=0;
    assert(face_ir_calibration_map(&p,540,960,&pair,&out)==0);
    assert(bio_face_live_ir(&backend,&input,540,960,&p,&pair)==1&&detect_calls==1&&region_calls==0);
    assert(!face_ir_calibration_begin(100));
    face_ir_calibration_feed(NULL,&ir,540,960,&pair,101);
    FaceIrPair bad_pair={1000000,1100000};
    face_ir_calibration_feed(&p,&ir,540,960,&bad_pair,102);
    face_ir_calibration_status(102,&state);assert(state.count==0);
    for(int i=0;i<FACE_IR_CALIBRATION_FRAMES;++i) {
        p=pose(i);ir=ir_face(&p);pair=(FaceIrPair){1000000+i*33333,1010000+i*33333};
        face_ir_calibration_feed(&p,&ir,540,960,&pair,110+i);
        face_ir_calibration_feed(&p,&ir,540,960,&pair,110+i); /* duplicate cannot count */
        face_ir_calibration_status(110+i,&state);
        assert(state.count==i+1);
        if(i<FACE_IR_CALIBRATION_FRAMES-1)assert(state.state==FACE_IR_COLLECTING);
    }
    assert(state.state==FACE_IR_DONE&&state.calibrated&&state.rms_px<.001);
    assert(face_ir_calibration_map(&p,540,960,&pair,&out)==1);
    assert(bio_face_live_ir(&backend,&input,540,960,&p,&pair)==1&&detect_calls==1&&region_calls==1);
    for(int j=0;j<10;++j)assert(fabs(out.keypoints[j]-ir.keypoints[j])<.01);
    assert(face_ir_calibration_map(&p,540,960,&bad_pair,&out)==-1);
    assert(face_ir_calibration_map(&p,960,540,&pair,&out)==-1);
    FacePoseRes_t edge=p;int dx=420-p.rect.centerX;edge.rect.centerX+=dx;
    for(int j=0;j<5;++j)edge.keypoints[2*j]+=dx;
    assert(face_ir_calibration_map(&edge,540,960,&pair,&out)==1); /* face inside; context may pad */
    edge.rect.centerX+=30;for(int j=0;j<5;++j)edge.keypoints[2*j]+=30;
    assert(face_ir_calibration_map(&edge,540,960,&pair,&out)==-1); /* mapped face itself overflows */
    assert(bio_face_live_ir(&backend,&input,540,960,&edge,&pair)==0&&detect_calls==1&&region_calls==1);
    edge.rect.centerX=600;assert(face_ir_calibration_map(&edge,540,960,&pair,&out)==-1);
    assert(!face_ir_calibration_begin(1000));face_ir_calibration_cancel();
    assert(face_ir_calibration_map(&p,540,960,&pair,&out)==1);
    assert(face_ir_calibration_init(dir,NULL)==0); /* restart persistence */
    char path[256];snprintf(path,sizeof(path),"%s/face_ir_calibration.conf",dir);
    assert(face_ir_calibration_init(empty,path)==0); /* common bundled profile */
    assert(face_ir_calibration_map(&p,540,960,&pair,&out)==1);
    assert(!face_ir_calibration_begin(2000));
    face_ir_calibration_status(182001,&state);assert(state.state==FACE_IR_ERROR);
    assert(face_ir_calibration_map(&p,540,960,&pair,&out)==1);
    /* Gross timing/motion outliers are removed and replaced: never save a
     * profile fitted to fewer than the requested 120 observations. */
    assert(!face_ir_calibration_begin(200000));
    for(int i=0;i<FACE_IR_CALIBRATION_FRAMES;++i) {
        p=pose(i);ir=ir_face(&p);pair=(FaceIrPair){8000000+i*33333,8010000+i*33333};
        if(i<5) {
            ir.box[0]+=30;ir.box[2]+=30;
            for(int j=0;j<5;++j)ir.keypoints[2*j]+=30;
        }
        face_ir_calibration_feed(&p,&ir,540,960,&pair,200100+i);
    }
    face_ir_calibration_status(200300,&state);assert(state.state==FACE_IR_COLLECTING&&state.count==115);
    for(int i=120;i<125;++i) {
        p=pose(i);ir=ir_face(&p);pair=(FaceIrPair){8000000+i*33333,8010000+i*33333};
        face_ir_calibration_feed(&p,&ir,540,960,&pair,200300+i);
    }
    face_ir_calibration_status(201000,&state);assert(state.state==FACE_IR_DONE&&state.count==120);
    char override_path[256];snprintf(override_path,sizeof(override_path),"%s/face_ir_calibration.conf",empty);
    unlink(override_path);
    FILE *f=fopen(path,"a");assert(f);fputs("garbage\n",f);fclose(f);
    assert(face_ir_calibration_init(dir,NULL)==-1);
    assert(face_ir_calibration_map(&p,540,960,&pair,&out)==-1); /* never fail open */
    unlink(path);rmdir(dir);rmdir(empty);
    puts("PASS face IR calibration: 120 unique paired frames, affine fit, persistence, shared defaults, bounds, timing, cancellation, malformed file");
    return 0;
}
