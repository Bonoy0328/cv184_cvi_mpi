from pathlib import Path
import shlex,subprocess
base=Path('gc0328.c').read_text()
cmd=shlex.split(subprocess.check_output(['make','-B','-n','dvp_probe'],text=True).strip())
variants={
'isp_path':base.replace('VI_ISP_YUV_SCENE_BYPASS','VI_ISP_YUV_SCENE_ISP').replace('.enPixelFormat=PIXEL_FORMAT_YUYV','.enPixelFormat=PIXEL_FORMAT_NV21'),
'pat_rgb':base.replace('CHECK(CVI_VI_SetDevAttr(0,&dev));','CHECK(CVI_VI_EnablePatgen(0));CHECK(CVI_VI_SetDevAttr(0,&dev));').replace('VI_DATA_TYPE_YUV','VI_DATA_TYPE_RGB').replace('.bYuvBypassPath=CVI_TRUE','.bYuvBypassPath=CVI_FALSE').replace('.enPixelFormat=PIXEL_FORMAT_YUYV','.enPixelFormat=PIXEL_FORMAT_NV21'),
}
callbacks='''
#include "cvi_isp.h"
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
'''
variants['callback']=base.replace('int gc0328_start(void)',callbacks+'\nint gc0328_start(void)').replace('CHECK(board_prepare());','CHECK(sns_cb_start());CHECK(board_prepare());').replace('if(pipe_up) CVI_VI_DestroyPipe(0);','if(pipe_up) CVI_VI_DestroyPipe(0);\n    CVI_ISP_SensorUnRegCallBack(0,328);')
for name,src in variants.items():
    f='/tmp/ky50-gc0328-'+name+'.c';out='/tmp/dvp_probe_'+name
    Path(f).write_text(src)
    args=[f if x=='gc0328.c' else out if x=='dvp_probe' else x for x in cmd]
    if name=='callback':
        i=args.index('-Wl,--end-group');args[i:i]=['-lisp','-lcvi_bin','-lisp_algo','-lae','-lawb','-laf']
    subprocess.run(args,check=True)
    print(out)
