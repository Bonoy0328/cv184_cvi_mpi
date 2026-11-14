#!/usr/bin/env python3
"""Compare separately captured face-input modes; serial stage sums are per job."""
import argparse,json,re,statistics
from pathlib import Path
from analyze_pipeline_timing import analyze
from analyze_face_live_timing import summary

def result(path):
    data=analyze(path)
    modes=re.findall(r'FACE_PREPROCESS mode=(\w+)',path.read_text(errors='replace'))
    jobs=[j for j in data['jobs'] if j['kind']=='face' and 'display_ms' in j]
    rows=[]
    for j in jobs:
        prep=[v for n in ('nv12','color_backend','align','align_expanded') for v in j['stages'].get(n,[])]
        rows.append(dict(line=j['line'],prepare_wall_ms=sum(v[0] for v in prep),prepare_cpu_ms=sum(v[1] for v in prep),
            task_ms=j['job_ms'],rgb_candidate_ms=j['ir']['rgb_us']/1000 if j['ir'] else 0,
            ir_ms=j['ir']['elapsed_us']/1000 if j['ir'] else 0,handoff_ms=j.get('handoff_ms',0),
            attendance_ms=j.get('attendance_ms',0),task_to_display_ms=j['job_to_display_ms'],
            first_attempt_to_display_ms=j['first_attempt_to_display_ms'],attempts=j['attempts']))
    return dict(mode=modes[-1] if modes else 'unknown',source=str(path),samples=rows,
        summary={k:summary([r[k] for r in rows]) for k in rows[0] if k!='line'} if rows else {},
        gates=data['gates'],perf=data['perf'],groups=data['groups'])
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);p.add_argument('logs',nargs='+',type=Path)
    a=p.parse_args();r=[result(f) for f in a.logs];a.output.write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps([{k:v for k,v in x.items() if k in ('mode','summary')} for x in r],indent=2))
