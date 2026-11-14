#!/usr/bin/env python3
"""Separate concurrent detection, worker jobs and business/display text timings."""
import argparse
import collections
import json
import math
import re
from pathlib import Path
from analyze_face_live_timing import fields, summary


def analyze(path):
    jobs, detections, perf = [], collections.defaultdict(list), collections.defaultdict(list)
    gates, storage = collections.defaultdict(list), collections.defaultdict(list)
    stages = {m: {} for m in ("face", "palm")}
    attempts = {m: [] for m in stages}
    candidates = {m: None for m in stages}
    rgb, strong, ir = 1, 1, None
    for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        f = fields(line)
        if line.startswith("FACE_IR_CAL "):
            # Factory IR detections are not stages of the next authentication.
            stages["face"] = {}; attempts["face"] = []; candidates["face"] = None; ir = None
        if "ACCESS face parameters " in line and "liveness" in f:
            rgb = int(f["liveness"].split("/")[0]); attempts = {m: [] for m in stages}
        if "ACCESS application saved" in line or "ACCESS departure " in line:
            strong = int(f.get("strong_liveness", strong)); attempts = {m: [] for m in stages}
        if line.startswith("FACE_GATE "):
            gates[f"{f['gate']}_rgb{rgb}_ir{strong}"].append(dict(
                reason=f["reason"], passed=int(f["pass"]), score=float(f["score"]), threshold=float(f["threshold"])))
        if line.startswith("STORE_STAGE ") and f.get("result") == "0":
            for name, value in f.items():
                if name.endswith("_us"):
                    storage[f["modality"]+"."+name[:-3]].append(int(value)/1000)
        m = re.search(r"(FACE|PALM)_STAGE (\w+) wall_us=(\d+) cpu_us=(\d+)", line)
        if m:
            kind, name, wall, cpu = m[1].lower(), m[2], int(m[3])/1000, int(m[4])/1000
            if name.startswith("detect_") or name == "pose_total":
                detections[kind+"."+name].append([wall, cpu])
            else:
                stages[kind].setdefault(name, []).append([wall, cpu])
        if line.startswith("BIO_IR_LIVE "):
            ir = {k: int(v) for k, v in f.items()}
        if line.startswith("BIO_STAGE "):
            kind = f["modality"]
            if f.get("stage") != "feature_auth":
                # After a held palm success, feature-only quality/exposure jobs
                # continue. Their timings do not belong to the next auth job.
                stages[kind] = {}; ir = None
                continue
            job = dict(line=number, kind=kind, rgb=rgb, strong=strong,
                       job_ms=int(f["elapsed_us"])/1000, end_ms=int(f["uptime_ms"]),
                       result=int(f["result"]), stages=stages[kind], ir=ir if kind=="face" else None)
            job["begin_ms"] = job["end_ms"]-job["job_ms"]
            jobs.append(job); attempts[kind].append(job)
            candidates[kind] = job if job["result"] == 0 else None
            stages[kind] = {}; ir = None
        if line.startswith("BIO_RESULT ") and f.get("valid") == "1":
            candidate = candidates.get(f.get("modality"))
            if candidate:
                candidate["ready_to_consume_ms"] = int(f["wait_us"])/1000
        if line.startswith("ACCESS_PUBLISH "):
            candidate = candidates.get({"1": "face", "2": "palm"}.get(f.get("modality")))
            if candidate:
                candidate["publish_ms"] = int(f["uptime_us"])/1000
        if line.startswith("PALM_SESSION left "):
            attempts["palm"] = []
        for kind in ("face", "palm"):
            candidate = candidates[kind]
            if line.startswith(kind.upper()+"_CONFIRM ") and candidate:
                candidate["confirm_ms"] = int(f["uptime_ms"])
                candidate["handoff_ms"] = candidate["confirm_ms"]-candidate["end_ms"]
            if line.startswith("AUTH_STAGE modality="+kind+" stage=attendance ") and candidate:
                candidate["attendance_ms"] = int(f["elapsed_us"])/1000
        if line.startswith("ACCESS_PRESENT "):
            kind = {"1": "face", "2": "palm"}.get(f.get("modality"))
            candidate = candidates.get(kind)
            if candidate and "confirm_ms" in candidate:
                candidate["display_ms"] = candidate["confirm_ms"]+int(f["age_ms"])
                candidate["job_to_display_ms"] = candidate["display_ms"]-candidate["begin_ms"]
                if "attendance_ms" in candidate:
                    candidate["after_attendance_to_display_ms"] = candidate["display_ms"]-candidate["confirm_ms"]-candidate["attendance_ms"]
                if "publish_ms" in candidate:
                    candidate["publish_to_display_ms"] = candidate["display_ms"]-candidate["publish_ms"]
                candidate["first_attempt_to_display_ms"] = candidate["display_ms"]-attempts[kind][0]["begin_ms"]
                candidate["attempts"] = len(attempts[kind]); attempts[kind] = []; candidates[kind] = None
        m = re.match(r"PERF (\S+) n=(\d+) avg_ms=([\d.]+) max_ms=([\d.]+)", line)
        if m: perf[m[1]].append(dict(n=int(m[2]), avg=float(m[3]), max=float(m[4])))
    groups = {}
    for kind, rgb, strong in sorted({(j["kind"],j["rgb"],j["strong"]) for j in jobs}):
        subset = [j for j in jobs if (j["kind"],j["rgb"],j["strong"]) == (kind,rgb,strong)]
        success = [j for j in subset if "display_ms" in j]
        result = {key: summary([j[key] for j in success if key in j]) for key in
                  ("job_ms", "handoff_ms", "ready_to_consume_ms", "attendance_ms", "after_attendance_to_display_ms", "publish_to_display_ms", "job_to_display_ms", "first_attempt_to_display_ms", "attempts")}
        result["results"] = dict(collections.Counter(j["result"] for j in subset))
        result["ir_call_ms"] = summary([j["ir"]["elapsed_us"]/1000 for j in success if j["ir"]])
        result["rgb_candidate_ms"] = summary([j["ir"]["rgb_us"]/1000 for j in success if j["ir"]])
        result["stages_ms"] = {}
        for name in sorted({n for j in success for n in j["stages"]}):
            values = [v for j in success for v in j["stages"].get(name, [])]
            result["stages_ms"][name] = dict(wall=summary([v[0] for v in values]),cpu=summary([v[1] for v in values]))
        groups[f"{kind}_rgb{rgb}_ir{strong}"] = result
    gate_summary = {}
    for key, values in gates.items():
        scored = [v for v in values if v["reason"] == "score" and math.isfinite(v["score"])]
        failed = [v["score"] for v in scored if not v["passed"]]
        trials = (.3, .25, .2) if key.startswith("quality_") else (.5, .45, .4, .3)
        gate_summary[key] = dict(n=len(values), reasons=dict(collections.Counter(v["reason"] for v in values)),
            failed_reasons=dict(collections.Counter(v["reason"] for v in values if not v["passed"])),
            thresholds=dict(collections.Counter(str(v["threshold"]) for v in scored)),
            passed_scores=summary([v["score"] for v in scored if v["passed"]]), failed_scores=summary(failed),
            # Only this gate's recorded score failures; not an end-to-end success prediction.
            score_failures_passing_lower_gate={str(t): sum(s >= t for s in failed) for t in trials})
    return dict(source=str(path),groups=groups,jobs=jobs, gates=gate_summary,
                storage_ms={key: summary(values) for key,values in storage.items()},
                detection_ms={k: dict(wall=summary([v[0] for v in values]),cpu=summary([v[1] for v in values])) for k,values in detections.items()},
                perf={k:dict(n=sum(v["n"] for v in values),weighted_mean=round(sum(v["avg"]*v["n"] for v in values)/sum(v["n"] for v in values),3),max=max(v["max"] for v in values)) for k,values in perf.items()})


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log",type=Path); parser.add_argument("output",type=Path)
    args=parser.parse_args(); result=analyze(args.log)
    args.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+"\n")
    print(json.dumps({k:v for k,v in result.items() if k!="jobs"},ensure_ascii=False,indent=2))
