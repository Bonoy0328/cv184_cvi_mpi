from pathlib import Path
import subprocess,shlex
s=Path('/tmp/ky50-gc0328-stages2.c').read_text()
s=s.replace('i<sizeof(gc0328_regs)/sizeof(gc0328_regs[0]);i++','i<sizeof(gc0328_regs)/sizeof(gc0328_regs[0]) && i<(unsigned)atoi(getenv("REG_LIMIT")?getenv("REG_LIMIT"):"999");i++')
p='/tmp/ky50-gc0328-reglimit.c';Path(p).write_text(s)
cmd=shlex.split(subprocess.check_output(['make','-B','-n','dvp_probe'],text=True).strip())
subprocess.run([p if x=='gc0328.c' else '/tmp/dvp_probe_reglimit' if x=='dvp_probe' else x for x in cmd],check=True)
