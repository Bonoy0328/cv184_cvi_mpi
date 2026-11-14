#include "face_ir_calibration.h"
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define PROFILE_NAME "face_ir_calibration.conf"
#define CALIBRATION_TIMEOUT_MS 180000
typedef struct {
    int valid,width,height;
    double a[6],rms,min_width,max_width;
} Profile;
typedef struct { PvaFaceGeometry rgb,ir; } Observation;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static Profile profile;
static Observation observations[FACE_IR_CALIBRATION_FRAMES];
static FaceIrCalibrationStatus status;
static FaceIrPair previous;
static uint64_t started,last_wait_log;
static unsigned trim_rounds;
static char data_directory[384];

int face_ir_pair_valid(const FaceIrPair *p)
{
    if(!p||!p->rgb_pts_us||!p->ir_pts_us)return 0;
    uint64_t gap=p->rgb_pts_us>p->ir_pts_us?p->rgb_pts_us-p->ir_pts_us:p->ir_pts_us-p->rgb_pts_us;
    return gap<=FACE_IR_PAIR_MAX_US;
}
static int geometry_valid(const PvaFaceGeometry *g,int w,int h)
{
    if(!g||!isfinite(g->score)||g->score<=0||g->score>1)return 0;
    for(int i=0;i<4;++i)if(!isfinite(g->box[i]))return 0;
    if(g->box[0]<0||g->box[1]<0||g->box[2]>w||g->box[3]>h||
       g->box[2]-g->box[0]<20||g->box[3]-g->box[1]<20)return 0;
    for(int i=0;i<5;++i) {
        double x=g->keypoints[2*i],y=g->keypoints[2*i+1];
        if(!isfinite(x)||!isfinite(y)||x<g->box[0]||x>g->box[2]||x>=w||
           y<g->box[1]||y>g->box[3]||y>=h)return 0;
        for(int j=0;j<i;++j)if(x==g->keypoints[2*j]&&y==g->keypoints[2*j+1])return 0;
    }
    double dx=g->keypoints[2]-g->keypoints[0],dy=g->keypoints[3]-g->keypoints[1];
    double area=0;
    for(int i=2;i<5;++i)area=fmax(area,fabs(dx*(g->keypoints[2*i+1]-g->keypoints[1])-dy*(g->keypoints[2*i]-g->keypoints[0])));
    return area>1;
}
static int from_pose(const FacePoseRes_t *p,int w,int h,PvaFaceGeometry *g)
{
    if(!p)return 0;
    *g=(PvaFaceGeometry){0};g->score=p->score;
    g->box[0]=p->rect.centerX-p->rect.centerWidth/2.0f;
    g->box[1]=p->rect.centerY-p->rect.centerHeight/2.0f;
    g->box[2]=g->box[0]+p->rect.centerWidth;g->box[3]=g->box[1]+p->rect.centerHeight;
    for(int i=0;i<10;++i)g->keypoints[i]=p->keypoints[i];
    return geometry_valid(g,w,h);
}
static void point(const Profile *p,double x,double y,float *ox,float *oy)
{
    x/=p->width;y/=p->height;
    *ox=(float)((p->a[0]*x+p->a[1]*y+p->a[2])*p->width);
    *oy=(float)((p->a[3]*x+p->a[4]*y+p->a[5])*p->height);
}
static void transform(const Profile *p,const PvaFaceGeometry *src,PvaFaceGeometry *dst)
{
    *dst=(PvaFaceGeometry){.score=src->score};
    dst->box[0]=dst->box[1]=INFINITY;dst->box[2]=dst->box[3]=-INFINITY;
    for(int i=0;i<4;++i) {
        float x,y;point(p,src->box[(i&1)?2:0],src->box[(i&2)?3:1],&x,&y);
        dst->box[0]=fminf(dst->box[0],x);dst->box[1]=fminf(dst->box[1],y);
        dst->box[2]=fmaxf(dst->box[2],x);dst->box[3]=fmaxf(dst->box[3],y);
    }
    for(int i=0;i<5;++i)point(p,src->keypoints[2*i],src->keypoints[2*i+1],&dst->keypoints[2*i],&dst->keypoints[2*i+1]);
}
static int profile_valid(const Profile *p)
{
    if(p->width!=540||p->height!=960||!isfinite(p->rms)||p->rms<0||p->rms>8||
       !isfinite(p->min_width)||!isfinite(p->max_width)||p->min_width<20||
       p->max_width<p->min_width||p->max_width>p->width)return 0;
    for(int i=0;i<6;++i)if(!isfinite(p->a[i]))return 0;
    double det=p->a[0]*p->a[4]-p->a[1]*p->a[3];
    return p->a[0]>=0.6&&p->a[0]<=1.5&&p->a[4]>=0.6&&p->a[4]<=1.5&&
        fabs(p->a[1])<=0.4&&fabs(p->a[3])<=0.4&&fabs(p->a[2])<=0.5&&fabs(p->a[5])<=0.5&&det>=0.4&&det<=2.25;
}
static unsigned checksum(const char *s,size_t n)
{
    unsigned c=2166136261u;for(size_t i=0;i<n;++i)c=(c^(unsigned char)s[i])*16777619u;return c;
}
/* 1 missing, 0 valid, -1 damaged/incompatible. No raw camera data is persisted. */
static int load_profile(const char *path,Profile *p)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return errno==ENOENT?1:-1;
    char text[1024]={0},extra;struct stat st;
    int ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_size>0&&st.st_size<(off_t)sizeof(text);
    ssize_t n=ok?read(fd,text,sizeof(text)-1):-1;close(fd);
    if(n<=0||n!=st.st_size||memchr(text,0,(size_t)n))return -1;
    char *tail=strstr(text,"checksum=");unsigned c=0;
    if(!tail||sscanf(tail,"checksum=%x %c",&c,&extra)!=1||c!=checksum(text,(size_t)(tail-text)))return -1;
    *tail=0;int frames=0;
    int fields=sscanf(text,"version=1\ncoordinates=rgb1_ir0_ccw90_nv12\nwidth=%d\nheight=%d\nsamples=%d\naffine=%lf %lf %lf %lf %lf %lf\nrms_px=%lf\nface_width=%lf %lf %c",
        &p->width,&p->height,&frames,&p->a[0],&p->a[1],&p->a[2],&p->a[3],&p->a[4],&p->a[5],&p->rms,&p->min_width,&p->max_width,&extra);
    if(fields!=12||frames!=FACE_IR_CALIBRATION_FRAMES||!profile_valid(p))return -1;
    p->valid=1;return 0;
}
static int save_profile(const Profile *p)
{
    int d=open(data_directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(d<0)return -1;
    char text[1024];int n=snprintf(text,sizeof(text),
        "version=1\ncoordinates=rgb1_ir0_ccw90_nv12\nwidth=%d\nheight=%d\nsamples=%d\naffine=%.12g %.12g %.12g %.12g %.12g %.12g\nrms_px=%.9g\nface_width=%.9g %.9g\n",
        p->width,p->height,FACE_IR_CALIBRATION_FRAMES,p->a[0],p->a[1],p->a[2],p->a[3],p->a[4],p->a[5],p->rms,p->min_width,p->max_width);
    n+=snprintf(text+n,sizeof(text)-(size_t)n,"checksum=%08x\n",checksum(text,(size_t)n));
    int fd=openat(d,PROFILE_NAME ".tmp",O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600),rc=-1;
    if(fd>=0) {
        int ok=write(fd,text,(size_t)n)==n&&!fsync(fd);if(close(fd))ok=0;
        if(ok&&!renameat(d,PROFILE_NAME ".tmp",d,PROFILE_NAME)&&!fsync(d))rc=0;
    }
    if(rc)unlinkat(d,PROFILE_NAME ".tmp",0);
    close(d);return rc;
}
int face_ir_calibration_init(const char *directory,const char *application_profile)
{
    pthread_mutex_lock(&mutex);profile=(Profile){0};status=(FaceIrCalibrationStatus){0};
    data_directory[0]=0;
    if(!directory||strlen(directory)+32>=sizeof(data_directory)) {
        profile.valid=-1;pthread_mutex_unlock(&mutex);return -1;
    }
    snprintf(data_directory,sizeof(data_directory),"%s",directory);
    char path[448];snprintf(path,sizeof(path),"%s/%s",directory,PROFILE_NAME);
    int rc=load_profile(path,&profile);const char *source="device";
    if(rc==1&&application_profile){rc=load_profile(application_profile,&profile);source="application";}
    if(rc<0)profile.valid=-1;
    status.calibrated=profile.valid==1;status.rms_px=(float)profile.rms;
    snprintf(status.message,sizeof(status.message),"%s",rc<0?"校准文件无效，请重新校准":rc==1?"尚未校准，使用原红外检测":"已加载人脸双摄校准参数");
    printf("FACE_IR_CAL load=%s result=%d mapped=%d rms=%.3f samples=%d\n",source,rc,profile.valid,profile.rms,FACE_IR_CALIBRATION_FRAMES);
    pthread_mutex_unlock(&mutex);return rc;
}
int face_ir_calibration_begin(uint64_t now)
{
    pthread_mutex_lock(&mutex);
    if(!data_directory[0]){pthread_mutex_unlock(&mutex);return -1;}
    status.state=FACE_IR_COLLECTING;status.count=0;started=now;last_wait_log=0;previous=(FaceIrPair){0};trim_rounds=0;
    memset(observations,0,sizeof(observations));
    snprintf(status.message,sizeof(status.message),"请一人正对摄像头，保持常用距离");
    printf("FACE_IR_CAL begin target=%d\n",FACE_IR_CALIBRATION_FRAMES);
    pthread_mutex_unlock(&mutex);return 0;
}
void face_ir_calibration_cancel(void)
{
    pthread_mutex_lock(&mutex);
    if(status.state==FACE_IR_COLLECTING){status.state=FACE_IR_IDLE;memset(observations,0,sizeof(observations));printf("FACE_IR_CAL cancelled count=%d\n",status.count);}
    pthread_mutex_unlock(&mutex);
}
void face_ir_calibration_status(uint64_t now,FaceIrCalibrationStatus *out)
{
    pthread_mutex_lock(&mutex);
    if(status.state==FACE_IR_COLLECTING&&now>=started&&now-started>CALIBRATION_TIMEOUT_MS) {
        status.state=FACE_IR_ERROR;snprintf(status.message,sizeof(status.message),"采集超时，请检查补光和站位后重试");
        memset(observations,0,sizeof(observations));
    }
    if(out)*out=status;
    pthread_mutex_unlock(&mutex);
}
int face_ir_calibration_active(void)
{
    pthread_mutex_lock(&mutex);int active=status.state==FACE_IR_COLLECTING;pthread_mutex_unlock(&mutex);return active;
}
static int solve(double matrix[3][3],double target[3],double result[3])
{
    double m[3][4];
    for(int i=0;i<3;++i){for(int j=0;j<3;++j)m[i][j]=matrix[i][j];m[i][3]=target[i];}
    for(int k=0;k<3;++k) {
        int pivot=k;for(int i=k+1;i<3;++i)if(fabs(m[i][k])>fabs(m[pivot][k]))pivot=i;
        if(fabs(m[pivot][k])<1e-8)return 0;
        for(int j=k;j<4;++j){double t=m[k][j];m[k][j]=m[pivot][j];m[pivot][j]=t;}
        double div=m[k][k];for(int j=k;j<4;++j)m[k][j]/=div;
        for(int i=0;i<3;++i)if(i!=k){double v=m[i][k];for(int j=k;j<4;++j)m[i][j]-=v*m[k][j];}
    }
    for(int i=0;i<3;++i)result[i]=m[i][3];
    return 1;
}
static double box_iou(const PvaFaceGeometry *a,const PvaFaceGeometry *b)
{
    double iw=fmax(0,fmin(a->box[2],b->box[2])-fmax(a->box[0],b->box[0]));
    double ih=fmax(0,fmin(a->box[3],b->box[3])-fmax(a->box[1],b->box[1]));
    double area=(a->box[2]-a->box[0])*(a->box[3]-a->box[1])+(b->box[2]-b->box[0])*(b->box[3]-b->box[1]);
    return area>iw*ih?iw*ih/(area-iw*ih):0;
}
static int compare_double(const void *a,const void *b){double x=*(const double*)a,y=*(const double*)b;return (x>y)-(x<y);}
/* Fit all five landmark correspondences, validate against independently
 * detected boxes, discard grossly inconsistent pairs and collect replacements. */
static void finish(void)
{
    Profile p={.valid=1,.width=540,.height=960,.min_width=540};
    double m[3][3]={{0}},x[3]={0},y[3]={0};
    for(int i=0;i<status.count;++i)for(int j=0;j<5;++j) {
        const Observation *o=&observations[i];
        double v[3]={o->rgb.keypoints[2*j]/p.width,o->rgb.keypoints[2*j+1]/p.height,1};
        for(int a=0;a<3;++a) {
            x[a]+=v[a]*o->ir.keypoints[2*j]/p.width;y[a]+=v[a]*o->ir.keypoints[2*j+1]/p.height;
            for(int b=0;b<3;++b)m[a][b]+=v[a]*v[b];
        }
    }
    const char *error="校准数据不稳定，请保持正脸后重试";
    if(!solve(m,x,p.a)||!solve(m,y,p.a+3))goto failed;
    double errors[FACE_IR_CALIBRATION_FRAMES],sorted[FACE_IR_CALIBRATION_FRAMES],ious[FACE_IR_CALIBRATION_FRAMES];
    for(int i=0;i<status.count;++i) {
        PvaFaceGeometry mapped;transform(&p,&observations[i].rgb,&mapped);
        double e=0;for(int j=0;j<10;++j){double d=mapped.keypoints[j]-observations[i].ir.keypoints[j];e+=d*d;}
        errors[i]=sorted[i]=sqrt(e/5);ious[i]=box_iou(&mapped,&observations[i].ir);
    }
    qsort(sorted,(size_t)status.count,sizeof(double),compare_double);
    double limit=fmin(15,fmax(4,sorted[status.count/2]*2.5));int kept=0;
    for(int i=0;i<status.count;++i)if(errors[i]<=limit&&ious[i]>=0.65)observations[kept++]=observations[i];
    if(kept<status.count) {
        printf("FACE_IR_CAL trim old=%d kept=%d limit_px=%.3f\n",status.count,kept,limit);
        if(kept<status.count*4/5||++trim_rounds>5)goto failed;
        status.count=kept;snprintf(status.message,sizeof(status.message),"已剔除偏差较大的配对，正在补充采集");return;
    }
    for(int i=0;i<status.count;++i) {
        p.rms+=errors[i]*errors[i];double width=observations[i].rgb.box[2]-observations[i].rgb.box[0];
        p.min_width=fmin(p.min_width,width);p.max_width=fmax(p.max_width,width);
    }
    p.rms=sqrt(p.rms/status.count);
    if(!profile_valid(&p)||sorted[(status.count*95)/100]>12)goto failed;
    if(save_profile(&p)){error="校准参数保存失败，原参数仍保留";goto failed;}
    profile=p;status.calibrated=1;status.rms_px=(float)p.rms;status.state=FACE_IR_DONE;
    snprintf(status.message,sizeof(status.message),"校准成功，已保存并立即生效\n关键点平均误差 %.1f 像素",p.rms);
    printf("FACE_IR_CAL saved samples=%d rms_px=%.3f p95_px=%.3f affine=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
        status.count,p.rms,sorted[(status.count*95)/100],p.a[0],p.a[1],p.a[2],p.a[3],p.a[4],p.a[5]);
    memset(observations,0,sizeof(observations));return;
failed:
    status.state=FACE_IR_ERROR;snprintf(status.message,sizeof(status.message),"%s",error);
    printf("FACE_IR_CAL rejected count=%d reason=%s\n",status.count,error);memset(observations,0,sizeof(observations));
}
void face_ir_calibration_feed(const FacePoseRes_t *rgb,const PvaFaceGeometry *ir,uint16_t w,uint16_t h,
    const FaceIrPair *pair,uint64_t now)
{
    pthread_mutex_lock(&mutex);
    if(status.state!=FACE_IR_COLLECTING){pthread_mutex_unlock(&mutex);return;}
    const char *reason=NULL;PvaFaceGeometry g;
    if(now<started||now-started>CALIBRATION_TIMEOUT_MS)reason="采集超时，请返回后重试";
    else if(w!=540||h!=960||!face_ir_pair_valid(pair))reason="等待时间接近的双摄图像";
    else if(pair->rgb_pts_us<=previous.rgb_pts_us||pair->ir_pts_us<=previous.ir_pts_us)reason="等待新的双摄图像";
    else if(!rgb||rgb->score<0.5f||rgb->status!=STATUS_OK||abs(rgb->yaw)>25||abs(rgb->pitch)>25||abs(rgb->roll)>20||
            !from_pose(rgb,w,h,&g))reason="请一人正对摄像头，保持人脸完整";
    else if(!geometry_valid(ir,w,h)||ir->score<0.5f)reason="等待红外人脸，请开启补光并保持正脸";
    if(reason) {
        snprintf(status.message,sizeof(status.message),"%s",reason);
        if(now>=last_wait_log&&now-last_wait_log>=1000) {
            last_wait_log=now;printf("FACE_IR_CAL waiting count=%d rgb_pts=%llu ir_pts=%llu reason=%s\n",status.count,
                (unsigned long long)(pair?pair->rgb_pts_us:0),(unsigned long long)(pair?pair->ir_pts_us:0),reason);
        }
        pthread_mutex_unlock(&mutex);return;
    }
    previous=*pair;observations[status.count++]=(Observation){g,*ir};
    snprintf(status.message,sizeof(status.message),"正在采集，请保持常用距离并缓慢小幅移动");
    if(status.count%10==0)printf("FACE_IR_CAL progress=%d/%d pair_gap_us=%llu\n",status.count,FACE_IR_CALIBRATION_FRAMES,
        (unsigned long long)(pair->rgb_pts_us>pair->ir_pts_us?pair->rgb_pts_us-pair->ir_pts_us:pair->ir_pts_us-pair->rgb_pts_us));
    if(status.count==FACE_IR_CALIBRATION_FRAMES)finish();
    pthread_mutex_unlock(&mutex);
}
int face_ir_calibration_map(const FacePoseRes_t *rgb,uint16_t w,uint16_t h,const FaceIrPair *pair,PvaFaceGeometry *ir)
{
    pthread_mutex_lock(&mutex);Profile p=profile;pthread_mutex_unlock(&mutex);
    if(ir)*ir=(PvaFaceGeometry){0};
    if(!p.valid)return 0;
    const char *reason=NULL;PvaFaceGeometry g;
    if(p.valid<0||!ir||w!=p.width||h!=p.height)reason="profile_or_dimensions";
    else if(!face_ir_pair_valid(pair))reason="pair_timing";
    else if(!from_pose(rgb,w,h,&g))reason="rgb_geometry";
    else if(rgb->rect.centerWidth<p.min_width*0.7||rgb->rect.centerWidth>p.max_width*1.3)reason="distance_range";
    else {
        transform(&p,&g,ir);
        if(!geometry_valid(ir,w,h))reason="mapped_geometry_outside_or_invalid";
    }
    if(reason&&getenv("KY50_TRACE_STALL"))printf("FACE_IR_MAP reject=%s rgb_width=%u ir_box=%.1f,%.1f,%.1f,%.1f rgb_pts=%llu ir_pts=%llu\n",
        reason,rgb?rgb->rect.centerWidth:0,ir?ir->box[0]:0,ir?ir->box[1]:0,ir?ir->box[2]:0,ir?ir->box[3]:0,
        (unsigned long long)(pair?pair->rgb_pts_us:0),(unsigned long long)(pair?pair->ir_pts_us:0));
    return reason?-1:1;
}
