from pathlib import Path
import shlex,subprocess
base=Path('gc0328.c').read_text()
s=base.replace('CHECK(reg_write(gc0328_regs[i][0],gc0328_regs[i][1]));','CHECK(reg_write(gc0328_regs[i][0],gc0328_regs[i][0]==0xf2?0:gc0328_regs[i][1]));')
s=s.replace('{0xf2,1}', '{0xf2,0}')
s=s.replace('channel_up=1;', 'channel_up=1;\n    CHECK(reg_write(0xf2,1));')
cmd=shlex.split(subprocess.check_output(['make','-B','-n','dvp_probe'],text=True).strip())
for name,src in [('latestart',s),('latepat',s.replace('VI_MODE_BT601','VI_MODE_MIPI').replace('VI_DATA_TYPE_YUV','VI_DATA_TYPE_RGB').replace('PIXEL_FORMAT_YUYV','PIXEL_FORMAT_NV21').replace('.bYuvBypassPath=CVI_TRUE','.bYuvBypassPath=CVI_FALSE').replace('CHECK(CVI_VI_SetDevAttr(0,&dev));','CHECK(CVI_VI_EnablePatgen(0));CHECK(CVI_VI_SetDevAttr(0,&dev));').replace('CHECK(reg_write(0xf2,1));','/* sensor remains silent */'))]:
 p='/tmp/ky50-gc0328-'+name+'.c';Path(p).write_text(src)
 subprocess.run([p if x=='gc0328.c' else '/tmp/dvp_probe_'+name if x=='dvp_probe' else x for x in cmd],check=True)
