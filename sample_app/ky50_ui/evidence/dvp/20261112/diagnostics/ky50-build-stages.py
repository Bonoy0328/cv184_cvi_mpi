from pathlib import Path
import subprocess,shlex
s=Path('gc0328.c').read_text()
s=s.replace('    CHECK(board_prepare());','    int stage=atoi(getenv("PROBE_STAGE")?getenv("PROBE_STAGE"):"5");\n    printf("PROBE_STAGE=%d\\n",stage);\n    if(stage==0)goto vi;\n    CHECK(board_prepare());\n    if(stage==1)goto vi;')
s=s.replace('CHECK(CVI_MIPI_SetMipiReset(0,1));','CHECK(CVI_MIPI_SetMipiReset(0,1));\n    if(stage==2)goto vi;')
s=s.replace('CHECK(CVI_MIPI_SetMipiAttr(0,&attr));','CHECK(CVI_MIPI_SetMipiAttr(0,&attr));\n    if(stage==3)goto vi;')
s=s.replace('clock_up=1;','clock_up=1;\n    if(stage==4)goto vi;')
s=s.replace('    VI_DEV_ATTR_S dev=','vi:;\n    VI_DEV_ATTR_S dev=')
s=s.replace('VI_MODE_BT601','VI_MODE_MIPI').replace('VI_DATA_TYPE_YUV','VI_DATA_TYPE_RGB').replace('PIXEL_FORMAT_YUYV','PIXEL_FORMAT_NV21').replace('.bYuvBypassPath=CVI_TRUE','.bYuvBypassPath=CVI_FALSE')
s=s.replace('CHECK(CVI_VI_SetDevAttr(0,&dev));','CHECK(CVI_VI_EnablePatgen(0));CHECK(CVI_VI_SetDevAttr(0,&dev));')
p='/tmp/ky50-gc0328-stages.c';Path(p).write_text(s)
cmd=shlex.split(subprocess.check_output(['make','-B','-n','dvp_probe'],text=True).strip())
subprocess.run([p if x=='gc0328.c' else '/tmp/dvp_probe_stages' if x=='dvp_probe' else x for x in cmd],check=True)
