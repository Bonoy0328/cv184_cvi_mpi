#!/usr/bin/env python3
"""Decode three live RTSP streams concurrently without saving camera images."""
import argparse
import pathlib
import re
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('host')
p.add_argument('--seconds', type=int, default=30)
p.add_argument('--min-fps', type=float, default=12)
p.add_argument('--output', type=pathlib.Path, default=pathlib.Path('build/rtsp/client'))
a = p.parse_args()
if a.seconds < 1 or a.min_fps <= 0:
    p.error('seconds and min-fps must be positive')
a.output.mkdir(parents=True, exist_ok=True)
processes = []
ok = True
try:
    for name in ('rgb', 'ir', 'dvp'):
        log = (a.output / (name + '.log')).open('w')
        progress = a.output / (name + '.progress')
        progress.unlink(missing_ok=True)
        cmd = ['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'info', '-xerror',
               '-rtsp_transport', 'tcp', '-i', 'rtsp://%s:8554/%s' % (a.host, name),
               '-an', '-t', str(a.seconds), '-progress', str(progress), '-f', 'null', '-']
        processes.append(dict(name=name, proc=subprocess.Popen(cmd, stdout=log, stderr=log),
                              log=log, progress=progress, start=time.monotonic(), end=None))
    deadline = time.monotonic() + a.seconds * 1.2 + 10
    while time.monotonic() < deadline:
        for item in processes:
            if item['end'] is None and item['proc'].poll() is not None:
                item['end'] = time.monotonic()
        if all(item['end'] is not None for item in processes):
            break
        time.sleep(.1)
    for item in processes:
        name, proc = item['name'], item['proc']
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait()
        item['log'].close()
        elapsed = (item['end'] or time.monotonic()) - item['start']
        progress = item['progress']
        frames = [int(line.split('=', 1)[1]) for line in progress.read_text().splitlines()
                  if line.startswith('frame=')] if progress.exists() else []
        count = frames[-1] if frames else 0
        errors = re.findall(r'non monoton|error while|corrupt|Invalid NAL|concealing',
                            (a.output / (name + '.log')).read_text(), re.I)
        # A stream with a nominal timestamp rate above its real rate can decode
        # enough frames but take too long. Check wall time as well as frame count.
        passed = (proc.returncode == 0 and count >= a.seconds * a.min_fps and
                  elapsed <= a.seconds * 1.2 + 5 and not errors)
        print('%s rc=%d decoded=%d wall_seconds=%.2f decode_errors=%d pass=%s' %
              (name, proc.returncode, count, elapsed, len(errors), passed))
        ok &= passed
finally:
    for item in processes:
        proc = item['proc']
        if proc.poll() is None:
            proc.kill(); proc.wait()
        item['log'].close()
raise SystemExit(0 if ok else 1)
