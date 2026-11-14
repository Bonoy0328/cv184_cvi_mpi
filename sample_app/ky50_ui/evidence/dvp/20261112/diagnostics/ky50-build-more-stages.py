from pathlib import Path
import shlex,subprocess
s=Path('/tmp/ky50-gc0328-stages.c').read_text()
s=s.replace('    usleep(10000);','    if(stage==6){usleep(300000);goto vi;}\n    usleep(10000);')
s=s.replace('usleep(20000);','usleep(20000);\n    if(stage==7)goto vi;')
s=s.replace('    if(id!=0x9d) return -1;','    if(id!=0x9d) return -1;\n    if(stage==8)goto vi;')
s=s.replace('    /* Y Cb Y Cr','    if(stage==9)goto vi;\n    /* Y Cb Y Cr')
p='/tmp/ky50-gc0328-stages2.c';Path(p).write_text(s)
cmd=shlex.split(subprocess.check_output(['make','-B','-n','dvp_probe'],text=True).strip())
subprocess.run([p if x=='gc0328.c' else '/tmp/dvp_probe_stages2' if x=='dvp_probe' else x for x in cmd],check=True)
