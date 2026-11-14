#include "rtsp_stream.h"
#include "cvi_venc.h"
#include "cvi_vpss.h"
#include "cvi_vb.h"
#include "cvi_buffer.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <rtsp.h>

#define RTSP_PORT 8554
#define STREAM_GROUP 4
#define STREAM_FPS 15
#define PACK_LIMIT 64
static const char *names[]={"rgb","ir","dvp"};
static void *sessions[CAMERA_SOURCE_COUNT];
static int created[3],receiving[3],worker_started;
static struct converter { int created,enabled,started,attached; VB_POOL pool; } converters[3] = {
    {.pool=VB_INVALID_POOLID},{.pool=VB_INVALID_POOLID},{.pool=VB_INVALID_POOLID}};
static pthread_t worker;
static atomic_int stopping,idr[3],pending[3],clients[3];
static atomic_uint_fast64_t encoded[3],last_encoded[3];
static uint64_t started_ms,last_sent[3],sent[3],dropped[3],network_dropped[3];
static uint64_t now_ms(void)
{
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
#define CHECK(call) do { int check_result=(call);if(check_result) { \
    fprintf(stderr,"RTSP %s failed rc=%#x\n",#call,check_result);goto fail; } } while(0)
static void on_event(RTSP_EVENT_S *event)
{
    if(!event||event->rtsp_id<0||event->rtsp_id>=3)return;
    int ch=event->rtsp_id;
    if(event->e==RTSP_EVENT_CLI_CONNECT) {
        atomic_fetch_add(&clients[ch],1);atomic_store(&idr[ch],1);
    } else if(event->e==RTSP_EVENT_CLI_DISCONNECT)atomic_fetch_sub(&clients[ch],1);
    printf("RTSP client %s event=%d active=%d\n",names[ch],event->e,atomic_load(&clients[ch]));
}
static int encoder_start(int ch)
{
    unsigned w=ch==CAMERA_DVP?640:960,h=ch==CAMERA_DVP?480:540;
    VENC_CHN_ATTR_S a={0};
    a.stVencAttr.enType=PT_H264;
    a.stVencAttr.u32PicWidth=a.stVencAttr.u32MaxPicWidth=w;
    a.stVencAttr.u32PicHeight=a.stVencAttr.u32MaxPicHeight=h;
    a.stVencAttr.u32BufSize=ch==CAMERA_DVP?256*1024:512*1024;
    a.stVencAttr.u32Profile=0;a.stVencAttr.bByFrame=CVI_TRUE;
    a.stVencAttr.bEsBufQueueEn=CVI_TRUE;a.stVencAttr.u8EsBufQueueDepth=2;
    a.stVencAttr.bIsoSendFrmEn=CVI_TRUE;
    a.stGopAttr.enGopMode=VENC_GOPMODE_NORMALP;
    a.stGopAttr.stNormalP.s32IPQpDelta=2;
    a.stRcAttr.enRcMode=VENC_RC_MODE_H264CBR;
    a.stRcAttr.stH264Cbr=(VENC_H264_CBR_S){.u32Gop=STREAM_FPS,.u32StatTime=1,
        .u32SrcFrameRate=STREAM_FPS,.fr32DstFrameRate=STREAM_FPS,.u32BitRate=ch==CAMERA_DVP?768:2000};
    CHECK(CVI_VENC_CreateChn(ch,&a));created[ch]=1;
    VENC_RC_PARAM_S rc={0};
    CHECK(CVI_VENC_GetRcParam(ch,&rc));
    /* This BSP needs explicit RC parameters; CreateChn alone produced
     * oversized frames despite the CBR attribute. */
    rc.s32FirstFrameStartQp=30;
    rc.stParamH264Cbr.u32MinQp=20;rc.stParamH264Cbr.u32MaxQp=48;
    rc.stParamH264Cbr.u32MinIQp=20;rc.stParamH264Cbr.u32MaxIQp=48;
    rc.stParamH264Cbr.u32MinIprop=1;rc.stParamH264Cbr.u32MaxIprop=10;
    CHECK(CVI_VENC_SetRcParam(ch,&rc));
    VENC_RECV_PIC_PARAM_S recv={.s32RecvPicNum=-1};
    CHECK(CVI_VENC_StartRecvFrame(ch,&recv));receiving[ch]=1;
    return 0;
fail:
    return -1;
}
static int converter_start(int source)
{
    int id=STREAM_GROUP+source;struct converter *c=&converters[source];
    unsigned w=source==CAMERA_DVP?640:960,h=source==CAMERA_DVP?480:540;
    VPSS_GRP_ATTR_S g={.u32MaxW=source==CAMERA_DVP?640:1920,.u32MaxH=source==CAMERA_DVP?480:1080,
        .enPixelFormat=source==CAMERA_DVP?PIXEL_FORMAT_YUYV:PIXEL_FORMAT_NV21,.stFrameRate={-1,-1}};
    VPSS_CHN_ATTR_S ch={.u32Width=w,.u32Height=h,.enPixelFormat=PIXEL_FORMAT_NV12,
        .enVideoFormat=VIDEO_FORMAT_LINEAR,.stFrameRate={-1,-1},.u32Depth=1,.stAspectRatio={.enMode=ASPECT_RATIO_NONE}};
    CHECK(CVI_VPSS_CreateGrp(id,&g));c->created=1;
    CHECK(CVI_VPSS_SetChnAttr(id,0,&ch));
    VB_POOL_CONFIG_S p={.u32BlkCnt=2,.enRemapMode=VB_REMAP_MODE_CACHED};
    p.u32BlkSize=COMMON_GetPicBufferSize(w,h,PIXEL_FORMAT_NV12,DATA_BITWIDTH_8,COMPRESS_MODE_NONE,DEFAULT_ALIGN);
    c->pool=CVI_VB_CreatePool(&p);
    if(c->pool==VB_INVALID_POOLID)goto fail;
    CHECK(CVI_VPSS_AttachVbPool(id,0,c->pool));c->attached=1;
    CHECK(CVI_VPSS_EnableChn(id,0));c->enabled=1;
    CHECK(CVI_VPSS_StartGrp(id));c->started=1;
    return 0;
fail:
    return -1;
}
static void *stream_worker(void *arg)
{
    (void)arg;
    VENC_PACK_S packs[PACK_LIMIT];
    int primed[3]={0};
    uint8_t *access_unit=malloc(1024*1024);
    if(!access_unit){atomic_store(&stopping,1);return NULL;}
    while(!atomic_load(&stopping)) {
        int got=0;
        for(int ch=0;ch<3;ch++) {
            if(!atomic_load(&pending[ch]))continue;
            VENC_STREAM_S stream={.pstPack=packs,.u32PackCount=PACK_LIMIT};
            if(CVI_VENC_GetStream(ch,&stream,0))continue;
            atomic_fetch_sub(&pending[ch],1);got=1;
            int valid=stream.u32PackCount<=PACK_LIMIT;
            for(unsigned i=0;valid&&i<stream.u32PackCount;i++)
                if(!packs[i].pu8Addr||packs[i].u32Offset>packs[i].u32Len)valid=0;
            size_t size=0;int key=0;
            for(unsigned i=0;valid&&i<stream.u32PackCount;i++) {
                VENC_PACK_S *p=&packs[i];size_t len=p->u32Len-p->u32Offset;
                if(len>1024*1024-size){valid=0;break;}
                memcpy(access_unit+size,p->pu8Addr+p->u32Offset,len);size+=len;
                if(p->DataType.enH264EType==H264E_NALU_ISLICE||p->DataType.enH264EType==H264E_NALU_SPS)key=1;
            }
            /* Prime SDP with an IDR, then avoid filling the vendor ring while
             * no client is consuming it. PLAY requests a fresh IDR. */
            if(valid&&size&&(!primed[ch]||atomic_load(&clients[ch])>0)) {
                RTSP_FRAME_S data={.type=FRAME_TYPE_VIDEO};
                data.data[0]=access_unit;data.len[0]=size;data.iskey[0]=key;
                data.vi_pts[0]=packs[0].u64PTS;
                if(RTSP_SendFrame(sessions[ch],&data))++network_dropped[ch];
                else primed[ch]=1;
            }
            CVI_VENC_ReleaseStream(ch,&stream);
            if(valid&&size) {
                atomic_store(&last_encoded[ch],now_ms());
                if(atomic_fetch_add(&encoded[ch],1)==0)
                    printf("RTSP first encoded frame %s\n",names[ch]);
            }
        }
        if(!got)usleep(2000);
    }
    free(access_unit);return NULL;
}
int rtsp_stream_start(void)
{
    /* A peer closing TCP must be handled by the server, not terminate the UI. */
    struct sigaction ignore={.sa_handler=SIG_IGN};sigemptyset(&ignore.sa_mask);
    if(sigaction(SIGPIPE,&ignore,NULL))return -1;
    atomic_store(&stopping,0);
    memset(network_dropped,0,sizeof(network_dropped));
    memset(last_sent,0,sizeof(last_sent));memset(sent,0,sizeof(sent));memset(dropped,0,sizeof(dropped));
    for(int i=0;i<3;i++){atomic_store(&encoded[i],0);atomic_store(&last_encoded[i],0);atomic_store(&idr[i],0);atomic_store(&pending[i],0);atomic_store(&clients[i],0);}
    for(int i=0;i<3;i++) {
        CHECK(encoder_start(i));
        RTSP_INFO_S info={.port=RTSP_PORT,.max_conn=3,.timeout=10};
        RTSP_MEDIA_INFO_S media={.id=i,.video_codec=RTSP_VIDEO_H264,.audio_codec=RTSP_AUDIO_BUTT,
            .framerate=STREAM_FPS,.bitrate_kbps=i==CAMERA_DVP?768:2000,
            .rtp_port=10000+i*4,.video_port=20000+i*4,.audio_port=21000+i*4,.cb=on_event};
        snprintf(media.rtsp_name,sizeof(media.rtsp_name),"%s",names[i]);
        CHECK(RTSP_Create(&sessions[i],&info,&media));
        if(!sessions[i])goto fail;
    }
    for(int i=0;i<3;i++) { CHECK(converter_start(i)); }
    CHECK(pthread_create(&worker,NULL,stream_worker,NULL));worker_started=1;
    started_ms=now_ms();
    printf("RTSP started port=%d paths=rgb,ir,dvp H264 native orientation, no UI; three concurrent clients\n",RTSP_PORT);
    return 0;
fail:
    rtsp_stream_stop();return -1;
}
void rtsp_stream_send(enum camera_source source,const VIDEO_FRAME_INFO_S *frame)
{
    if(!worker_started||source<0||source>=CAMERA_SOURCE_COUNT||atomic_load(&stopping))return;
    uint64_t now=now_ms()*STREAM_FPS;
    if(now<last_sent[source]||atomic_load(&pending[source])>=2)return;
    if(atomic_exchange(&idr[source],0))CVI_VENC_RequestIDR(source,CVI_TRUE);
    VIDEO_FRAME_INFO_S converted={0};int group=STREAM_GROUP+source;
    if(CVI_VPSS_SendFrame(group,frame,0)) {++dropped[source];return;}
    if(CVI_VPSS_GetChnFrame(group,0,&converted,40)) {++dropped[source];return;}
    int rc=CVI_VENC_SendFrame(source,&converted,0);
    if(!rc)atomic_fetch_add(&pending[source],1);
    CVI_VPSS_ReleaseChnFrame(group,0,&converted);
    if(!rc) {last_sent[source]=last_sent[source]&&now-last_sent[source]<2000?last_sent[source]+1000:now+1000;++sent[source];}
    else if(++dropped[source]==1||dropped[source]%100==0)
        fprintf(stderr,"RTSP %s send failed rc=%#x dropped=%llu\n",names[source],rc,(unsigned long long)dropped[source]);
}
int rtsp_stream_health(void)
{
    uint64_t now=now_ms();int ready=1;
    if(!worker_started||atomic_load(&stopping))return -1;
    for(int i=0;i<3;i++) {
        uint64_t last=atomic_load(&last_encoded[i]);
        if(!last)ready=0;
        uint64_t since=last?last:started_ms;
        /* Worker may publish a timestamp just after this thread samples now. */
        if(now>since&&now-since>5000)return -1;
    }
    return ready;
}
void rtsp_stream_stop(void)
{
    atomic_store(&stopping,1);
    /* The component copies frames into its own bounded network buffer. */
    if(worker_started){pthread_join(worker,NULL);worker_started=0;}
    for(int i=0;i<3;i++) {
        if(sessions[i]){RTSP_Destroy(sessions[i]);sessions[i]=NULL;}
        if(receiving[i])CVI_VENC_StopRecvFrame(i);
        if(created[i]) {
            CVI_VENC_DestroyChn(i);
            printf("RTSP stopped %s sent=%llu encoded=%llu dropped=%llu network_dropped=%llu\n",names[i],
                (unsigned long long)sent[i],(unsigned long long)atomic_load(&encoded[i]),(unsigned long long)dropped[i],(unsigned long long)network_dropped[i]);
        }
        receiving[i]=created[i]=0;
    }
    for(int i=0;i<3;i++) {
        int group=STREAM_GROUP+i;struct converter *c=&converters[i];
        if(c->started)CVI_VPSS_StopGrp(group);
        if(c->enabled)CVI_VPSS_DisableChn(group,0);
        if(c->attached)CVI_VPSS_DetachVbPool(group,0);
        if(c->created)CVI_VPSS_DestroyGrp(group);
        if(c->pool!=VB_INVALID_POOLID)CVI_VB_DestroyPool(c->pool);
        *c=(struct converter){.pool=VB_INVALID_POOLID};
    }
}
