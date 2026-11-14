from pathlib import Path
import shlex,subprocess
base=Path('gc0328.c').read_text()
start=base.index('    VI_DEV_ATTR_S dev=');end=base.index('\n    return 0;',start)
init=base[start:end].replace('VI_MODE_BT601','VI_MODE_MIPI').replace('VI_DATA_TYPE_YUV','VI_DATA_TYPE_RGB').replace('.enPixelFormat=PIXEL_FORMAT_YUYV','.enPixelFormat=PIXEL_FORMAT_NV21').replace('.bYuvBypassPath=CVI_TRUE','.bYuvBypassPath=CVI_FALSE').replace('COMPRESS_MODE_NONE','COMPRESS_MODE_TILE')
init=init.replace('CHECK(CVI_VI_SetDevAttr(0,&dev));','CHECK(CVI_VI_EnablePatgen(0));CHECK(CVI_VI_SetDevAttr(0,&dev));')
prefix='#include "gc0328.h"\n#include "cvi_vi.h"\n#include <stdio.h>\nstatic int dev_up,pipe_up,started,channel_up;\n#define CHECK(x) do { int r=(x);if(r){fprintf(stderr,"%s: %#x\\n",#x,r);return -1;} }while(0)\n'
text=prefix+'int gc0328_start(void) {\n'+init+'\nreturn 0; }\nvoid gc0328_stop(void) {if(channel_up)CVI_VI_DisableChn(0,0);if(started)CVI_VI_StopPipe(0);if(pipe_up)CVI_VI_DestroyPipe(0);if(dev_up)CVI_VI_DisableDev(0);}\n'
cmd=shlex.split(subprocess.check_output(['make','-B','-n','dvp_probe'],text=True).strip())
for name,source in [('cleanpat640',text),('cleanpatnone',text.replace('COMPRESS_MODE_TILE','COMPRESS_MODE_NONE'))]:
 f='/tmp/ky50-gc0328-'+name+'.c';out='/tmp/dvp_probe_'+name;Path(f).write_text(source)
 args=[f if x=='gc0328.c' else out if x=='dvp_probe' else x for x in cmd]
 subprocess.run(args,check=True);print(out)
