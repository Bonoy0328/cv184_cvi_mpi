from pathlib import Path
import shlex,subprocess
base=Path('/tmp/ky50-gc0328-callback.c').read_text()
base=base.replace('#include "cvi_isp.h"','#include "cvi_isp.h"\n#include <pthread.h>\nstatic int isp_up,thread_up;\nstatic pthread_t isp_thread;\nstatic void *isp_worker(void *p) { (void)p;CVI_ISP_Run(0);return NULL; }')
base=base.replace('VI_CHN_ATTR_S channel=', '''CHECK(CVI_ISP_MemInit(0));isp_up=1;
    ISP_PUB_ATTR_S pub={.stWndRect={0,0,640,480},.stSnsSize={640,480},.f32FrameRate=20,.enBayer=BAYER_RGGB,.enWDRMode=WDR_MODE_NONE,.u8LaneNum=1};
    CHECK(CVI_ISP_SetPubAttr(0,&pub));CHECK(CVI_ISP_Init(0));
    CHECK(pthread_create(&isp_thread,NULL,isp_worker,NULL));thread_up=1;
    VI_CHN_ATTR_S channel=''')
base=base.replace('if(started) CVI_VI_StopPipe(0);', 'if(isp_up) { CVI_ISP_Exit(0);isp_up=0; }\n    if(thread_up) { pthread_join(isp_thread,NULL);thread_up=0; }\n    if(started) CVI_VI_StopPipe(0);')
cmd=shlex.split(subprocess.check_output(['make','-B','-n','dvp_probe'],text=True).strip())
variants={
'fullisp':base,
'fullpat':base.replace('CHECK(CVI_VI_SetDevAttr(0,&dev));','CHECK(CVI_VI_EnablePatgen(0));CHECK(CVI_VI_SetDevAttr(0,&dev));').replace('VI_DATA_TYPE_YUV','VI_DATA_TYPE_RGB').replace('VI_MODE_BT601','VI_MODE_MIPI').replace('.bYuvBypassPath=CVI_TRUE','.bYuvBypassPath=CVI_FALSE').replace('.enPixelFormat=PIXEL_FORMAT_YUYV','.enPixelFormat=PIXEL_FORMAT_NV21'),
}
for name,src in variants.items():
    f='/tmp/ky50-gc0328-'+name+'.c';out='/tmp/dvp_probe_'+name;Path(f).write_text(src)
    args=[f if x=='gc0328.c' else out if x=='dvp_probe' else x for x in cmd]
    i=args.index('-Wl,--end-group');args[i:i]=['-lisp','-lcvi_bin','-lisp_algo','-lae','-lawb','-laf']
    subprocess.run(args,check=True);print(out)
