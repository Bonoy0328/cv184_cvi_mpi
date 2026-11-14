#!/usr/bin/env python3
"""Summarize KY50_TRACE_STALL text; never reads image or user database data."""
import argparse
import collections
import json
import re
import statistics
from pathlib import Path


def fields(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))


def summary(values):
    if not values:
        return None
    return dict(n=len(values), median=round(statistics.median(values), 3),
                min=round(min(values), 3), max=round(max(values), 3))


def analyze(path):
    jobs, stages, ir, candidate, attempts = [], {}, None, None, []
    rgb, strong = 1, 1
    for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        f = fields(line)
        if "ACCESS face parameters " in line and "liveness" in f:
            rgb = int(f["liveness"].split("/")[0])
            attempts = []
        if "ACCESS application saved" in line or "ACCESS departure " in line:
            strong = int(f.get("strong_liveness", strong))
            attempts = []
        m = re.search(r"FACE_STAGE (\w+) wall_us=(\d+) cpu_us=(\d+)", line)
        if m and not m[1].startswith("detect_"):
            stages.setdefault(m[1], []).append([int(m[2])/1000, int(m[3])/1000])
        if line.startswith("BIO_IR_LIVE "):
            ir = {k: int(v) for k, v in f.items()}
        if line.startswith("BIO_STAGE "):
            if f.get("modality") == "face" and f.get("stage") == "feature_auth":
                job = dict(line=number, rgb=rgb, ir_enabled=strong,
                           job_ms=int(f["elapsed_us"])/1000,
                           end_ms=int(f["uptime_ms"]), result=int(f["result"]),
                           stages=stages, ir=ir)
                job["begin_ms"] = job["end_ms"] - job["job_ms"]
                jobs.append(job)
                attempts.append(job)
                candidate = job if job["result"] == 0 else None
            stages, ir = {}, None
        if line.startswith("FACE_CONFIRM ") and candidate:
            candidate["confirm_ms"] = int(f["uptime_ms"])
            candidate["handoff_ms"] = candidate["confirm_ms"] - candidate["end_ms"]
        if line.startswith("AUTH_STAGE modality=face stage=attendance ") and candidate:
            candidate["attendance_ms"] = int(f["elapsed_us"])/1000
        if line.startswith("ACCESS_PRESENT ") and f.get("modality") == "1" and candidate and "confirm_ms" in candidate:
            candidate["display_ms"] = candidate["confirm_ms"] + int(f["age_ms"])
            candidate["job_to_display_ms"] = candidate["display_ms"] - candidate["begin_ms"]
            candidate["first_attempt_to_display_ms"] = candidate["display_ms"] - attempts[0]["begin_ms"]
            candidate["attempts"] = len(attempts)
            attempts, candidate = [], None
    groups = {}
    for rgb, strong in sorted({(j["rgb"], j["ir_enabled"]) for j in jobs}):
        all_jobs = [j for j in jobs if (j["rgb"], j["ir_enabled"]) == (rgb, strong)]
        success = [j for j in all_jobs if "display_ms" in j]
        result = {key: summary([j[key] for j in success if key in j]) for key in
                  ("job_ms", "handoff_ms", "attendance_ms", "job_to_display_ms", "first_attempt_to_display_ms", "attempts")}
        result["results"] = dict(collections.Counter(j["result"] for j in all_jobs))
        result["rgb_candidate_ms"] = summary([j["ir"]["rgb_us"]/1000 for j in success if j["ir"]])
        result["ir_call_ms"] = summary([j["ir"]["elapsed_us"]/1000 for j in success if j["ir"]])
        result["stages_ms"] = {}
        for name in sorted({name for j in success for name in j["stages"]}):
            vals = [v for j in success for v in j["stages"].get(name, [])]
            result["stages_ms"][name] = {"wall": summary([v[0] for v in vals]), "cpu": summary([v[1] for v in vals])}
        groups[f"rgb{rgb}_ir{strong}"] = result
    return dict(source=str(path), groups=groups, jobs=jobs)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = analyze(args.log)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2)+"\n")
    print(json.dumps(result["groups"], ensure_ascii=False, indent=2))
