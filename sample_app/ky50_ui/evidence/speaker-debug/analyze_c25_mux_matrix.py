#!/usr/bin/env python3
"""Compare the middle of each logged C25 stage in a 16 kHz stereo capture."""
import hashlib
import json
from pathlib import Path
import re

import numpy as np

root = Path(__file__).resolve().parent
raw = root / "ky50-c25-mux-matrix.raw"
log = root / "ky50-c25-mux-matrix-stages.log"
text = log.read_text()
capture_start = float(re.search(r"CAPTURE_START\n([0-9.]+)", text)[1])
samples = np.fromfile(raw, dtype="<i2").reshape(-1, 2)
rows = []
for name, uptime in re.findall(r"STAGE=(\S+)\n([0-9.]+)", text):
    # Keep two seconds, away from transitions and unknown subsecond launch delay.
    start = int(float(uptime) - capture_start) + 2
    end = start + 2
    if end * 16000 > len(samples):
        raise ValueError(f"Capture too short for {name}")
    x = samples[start * 16000:end * 16000].astype(float)
    dc = x.mean(axis=0)
    x -= dc
    phase = np.exp(-2j * np.pi * 400 * np.arange(len(x)) / 16000)
    peak = 2 * np.abs(phase @ x) / len(x)
    rows.append({
        "stage": name,
        "stage_uptime_s": float(uptime),
        "capture_window_s": [start, end],
        "dc_counts_lr": dc.tolist(),
        "ac_rms_counts_lr": np.sqrt(np.mean(x * x, axis=0)).tolist(),
        "tone_400hz_peak_counts_lr": peak.tolist(),
    })
result = {
    "sample_rate_hz": 16000,
    "samples_per_channel": len(samples),
    "capture_start_uptime_s": capture_start,
    "sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
               for p in (raw, log)},
    "stages": rows,
    "limits": "Uncalibrated analog feedback counts, not volts. LCD disconnected; "
              "does not prove reset voltage or display/audio coexistence.",
}
output = root / "c25-mux-matrix-analysis.json"
output.write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
