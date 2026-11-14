/* SPDX-License-Identifier: GPL-2.0-or-later
 * GC0328 640x480 BT.601 / onboard ISP / 8-bit I2C register protocol.
 * KY50 wiring follows sheet 2/7, never the GC0308 register protocol.
 */
#include "gc0328.h"
#include "gc0328_regs.h"
#include "media.h"
#include "cvi_mipi.h"
#include "cvi_vi.h"
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static int bus=-1,gpio=-1,clock_up,dev_up,pipe_up,started,channel_up;
static volatile uint32_t *mux;
static const unsigned offsets[]={0xb8,0xbc,0xf0,0x138,0x13c,0x140,0x144,0x148,
    0x14c,0x150,0x154,0x158,0x15c,0x160,0x164};
static uint32_t saved[sizeof(offsets)/sizeof(offsets[0])];
#define CHECK(x) do { int r=(x);if(r){fprintf(stderr,"GC0328 %s failed %#x\n",#x,r);return -1;} }while(0)
static int reg_write(unsigned char addr,unsigned char value)
{
    unsigned char data[]={addr,value};return write(bus,data,2)==2?0:-1;
}
static int reg_read(unsigned char addr)
{
    unsigned char value=0;
    struct i2c_msg messages[]={{.addr=0x21,.len=1,.buf=&addr},
        {.addr=0x21,.flags=I2C_M_RD,.len=1,.buf=&value}};
    struct i2c_rdwr_ioctl_data rd={.msgs=messages,.nmsgs=2};
    return ioctl(bus,I2C_RDWR,&rd)==2?value:-1;
}
static int power_down(int value)
{
    struct gpiohandle_data data={.values={value}};
    return ioctl(gpio,GPIOHANDLE_SET_LINE_VALUES_IOCTL,&data);
}
static int board_prepare(void)
{
    for(int i=0;i<16;i++) {
        char path[40];struct gpiochip_info info={0};
        snprintf(path,sizeof(path),"/dev/gpiochip%d",i);
        int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0) continue;
        if(!ioctl(fd,GPIO_GET_CHIPINFO_IOCTL,&info)&&!strcmp(info.label,"3021000.gpio")) {
            struct gpiohandle_request req={.lineoffsets={1},.lines=1,
                .flags=GPIOHANDLE_REQUEST_OUTPUT,.default_values={1}};
            strcpy(req.consumer_label,"ky50-gc0328");
            int rc=ioctl(fd,GPIO_GET_LINEHANDLE_IOCTL,&req);close(fd);
            if(rc) return -1;
            gpio=req.fd;break;
        }
        close(fd);
    }
    if(gpio<0) return -1;
    int fd=open("/dev/mem",O_RDWR|O_SYNC|O_CLOEXEC);if(fd<0) return -1;
    void *p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0x03001000);close(fd);
    if(p==MAP_FAILED) return -1;
    mux=p;
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) saved[i]=mux[offsets[i]/4];
    mux[0xb8/4]&=~7U;mux[0xbc/4]&=~7U;
    mux[0xf0/4]=(mux[0xf0/4]&~7U)|3;
    return 0;
}

#include "cvi_isp.h"
#include <pthread.h>
static int isp_up,thread_up;
static pthread_t isp_thread;
static void *isp_worker(void *p) { (void)p;CVI_ISP_Run(0);return NULL; }
static void sns_noop(VI_PIPE pipe) { (void)pipe; }
static CVI_S32 sns_size(VI_PIPE pipe, ISP_CMOS_SENSOR_IMAGE_MODE_S *m) {
    (void)pipe;return m && m->u16Width==640 && m->u16Height==480 ? 0 : -1;
}
static CVI_S32 sns_sync(VI_PIPE pipe,ISP_SNS_SYNC_INFO_S *s) {
    (void)pipe;memset(s,0,sizeof(*s));s->ispCfg.frm_num=1;
    s->ispCfg.img_size[0]=(ISP_WDR_SIZE_S){.stWndRect={0,0,640,480},.stSnsSize={640,480},.stMaxSize={640,480}};
    return 0;
}
static int sns_cb_start(void) {
    ISP_SNS_ATTR_INFO_S info={.eSensorId=328};
    ISP_SENSOR_REGISTER_S reg={.stSnsExp={.pfn_cmos_sensor_init=sns_noop,
        .pfn_cmos_sensor_exit=sns_noop,.pfn_cmos_sensor_global_init=sns_noop,
        .pfn_cmos_set_image_mode=sns_size,.pfn_cmos_get_sns_reg_info=sns_sync}};
    return CVI_ISP_SensorRegCallBack(0,&info,&reg);
}

int gc0328_start(void)
{
    const char *wiring=getenv("KY50_DVP_CLOCK_WIRING");
    int swapped=wiring && !strcmp(wiring,"swapped");
    CHECK(sns_cb_start());CHECK(board_prepare());
    struct combo_dev_attr_s attr={.input_mode=INPUT_MODE_BT601,.mac_clk=RX_MAC_CLK_400M,
        .mclk={.cam=0,.freq=CAMPLL_FREQ_24M},.devno=0,
        /* VI copies active_w/h into its crop, independently of width/height. */
        .img_size={.width=640,.height=480,.max_width=640,.max_height=480,
            .active_w=640,.active_h=480},
        .ttl_attr={.vi=TTL_VI_SRC_VI1,.ttl_fmt=TTL_VSDE_11B,.raw_data_type=RAW_DATA_8BIT,
        .func={6,-1,-1,5,3,8,9,7,1,2,0,4,-1,-1,-1,-1,-1,-1,-1,-1}}};
    CHECK(CVI_MIPI_SetMipiReset(0,1));
    CHECK(CVI_MIPI_SetMipiAttr(0,&attr));
    /* KY50 J7.13 FP-CLK -> module pin 12 IM_MCK; J7.17 FP-PCLK <-
     * module pin 8 IM_PCLK (24-position cable reverses numbering).
     * Schematic places MCLK on VIVO_CLK and PCLK on VIVO_D10; this differs
     * from SDK's fixed VI1_CLK pad. Preserve sensor clock while verifying
     * actual board reception; a missing frame must fail and roll back. */
    mux[0x164/4]=(mux[0x164/4]&~7U)|(swapped?1:5);
    mux[0x138/4]=(mux[0x138/4]&~7U)|(swapped?5:1);
    CHECK(CVI_MIPI_SetSensorClock(0,1));clock_up=1;
    usleep(10000);CHECK(power_down(0));usleep(20000);
    bus=open("/dev/i2c-2",O_RDWR|O_CLOEXEC);if(bus<0) return -1;
    CHECK(ioctl(bus,I2C_SLAVE,0x21));
    int id=reg_read(0xf0);printf("GC0328 I2C2/0x21 chip_id=%#x expected=0x9d\n",id);
    if(id!=0x9d) return -1;
    for(unsigned i=0;i<sizeof(gc0328_regs)/sizeof(gc0328_regs[0]);i++) {
        CHECK(reg_write(gc0328_regs[i][0],(gc0328_regs[i][0]==0xf1 || gc0328_regs[i][0]==0xf2)?0:gc0328_regs[i][1]));
        if(gc0328_regs[i][0]==0xfe && gc0328_regs[i][1]==0x80) usleep(5000);
    }
    /* Y Cb Y Cr, positive VS/HREF, continuous PCLK; exact 640x480 crop. */
    static const unsigned char format[][2]={{0xfe,0},{0x44,0x22},{0x46,0x03},
        {0x50,1},{0x51,0},{0x52,0},{0x53,0},{0x54,0},{0x55,1},{0x56,0xe0},
        {0x57,2},{0x58,0x80},{0xf1,0},{0xf2,0}};
    for(unsigned i=0;i<sizeof(format)/sizeof(format[0]);i++) CHECK(reg_write(format[i][0],format[i][1]));
    if(!swapped) {
        fprintf(stderr,"GC0328 initialized, but schematic PCLK=VIVO_D10 cannot feed VI1_CLK. "
            "Rewire clocks and set verified KY50_DVP_CLOCK_WIRING=swapped before capture.\n");
        return -2;
    }
    VI_DEV_ATTR_S dev={.snrFps=15,.stSize={640,480},.enIntfMode=VI_MODE_BT601,
        .enInputDataType=VI_DATA_TYPE_YUV,.enDataSeq=VI_DATA_SEQ_YUYV,
        .enWorkMode=VI_WORK_MODE_1Multiplex,.enYuvSceneMode=VI_ISP_YUV_SCENE_BYPASS};
    VI_DEV_BIND_PIPE_S bind={.u32Num=1,.MipiDev=0};
    for(int i=0;i<VI_MAX_PIPE_NUM;i++) bind.PipeId[i]=i?-1:0;
    CHECK(CVI_VI_SetDevAttr(0,&dev));CHECK(CVI_VI_SetDevBindAttr(0,&bind));
    CHECK(CVI_VI_EnableDev(0));dev_up=1;
    VI_PIPE_ATTR_S pipe={.u32MaxW=640,.u32MaxH=480,.enPixFmt=PIXEL_FORMAT_RGB_BAYER_12BPP,
        .enBitWidth=DATA_BITWIDTH_12,.stFrameRate={-1,-1},.bNrEn=CVI_TRUE,
        .bYuvBypassPath=CVI_TRUE,.enCompressMode=COMPRESS_MODE_NONE};
    CHECK(CVI_VI_CreatePipe(0,&pipe));pipe_up=1;
    CHECK(CVI_VI_StartPipe(0));started=1;
    CHECK(CVI_ISP_MemInit(0));isp_up=1;
    ISP_PUB_ATTR_S pub={.stWndRect={0,0,640,480},.stSnsSize={640,480},.f32FrameRate=20,.enBayer=BAYER_RGGB,.enWDRMode=WDR_MODE_NONE,.u8LaneNum=1};
    CHECK(CVI_ISP_SetPubAttr(0,&pub));CHECK(CVI_ISP_Init(0));
    CHECK(pthread_create(&isp_thread,NULL,isp_worker,NULL));thread_up=1;
    VI_CHN_ATTR_S channel={.stSize={640,480},.enPixelFormat=PIXEL_FORMAT_YUYV,
        .enDynamicRange=DYNAMIC_RANGE_SDR8,.enVideoFormat=VIDEO_FORMAT_LINEAR,
        .enCompressMode=COMPRESS_MODE_NONE,.u32Depth=2,.u32BindVbPool=(unsigned)-1};
    CHECK(CVI_VI_SetChnAttr(0,0,&channel));CHECK(CVI_VI_EnableChn(0,0));channel_up=1;
    usleep(200000);CHECK(reg_write(0xf1,7));CHECK(reg_write(0xf2,1));
    return 0;
}
void gc0328_stop(void)
{
    if(channel_up) CVI_VI_DisableChn(0,0);
    if(bus>=0) { reg_write(0xfe,0);reg_write(0xf2,0);close(bus);bus=-1; }
    if(isp_up) { CVI_ISP_Exit(0);isp_up=0; }
    if(thread_up) { pthread_join(isp_thread,NULL);thread_up=0; }
    if(started) CVI_VI_StopPipe(0);
    if(pipe_up) CVI_VI_DestroyPipe(0);
    CVI_ISP_SensorUnRegCallBack(0,328);
    if(dev_up) CVI_VI_DisableDev(0);
    if(gpio>=0) power_down(1);
    if(clock_up) CVI_MIPI_SetSensorClock(0,0);
    if(mux) {
        for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) mux[offsets[i]/4]=saved[i];
        munmap((void *)mux,4096);mux=NULL;
    }
    if(gpio>=0) { close(gpio);gpio=-1; }
    clock_up=dev_up=pipe_up=started=channel_up=0;
}
