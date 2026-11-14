#!/usr/bin/env python3
"""Fit timestamped DAC DEM status to a tone; these are not analog voltages."""
import json
from pathlib import Path

import numpy as np

root = Path(__file__).resolve().parent
data = np.loadtxt(root / "ky50-dac-dem-tone.csv", delimiter=",",
                  skiprows=1, usecols=(0, 2, 3))
times = (data[:, 0] - data[0, 0]) / 1e9
result = {
    "samples": len(times),
    "duration_s": float(times[-1]),
    "note": "Read-only DAC ANA3 DEM status, not analog audio voltage; "
            "sine fits to bit population counts with actual monotonic timestamps.",
    "channels": [],
}
for channel in range(2):
    values = data[:, channel + 1]

    def fit(frequency):
        phase = 2 * np.pi * frequency * times
        basis = np.column_stack([np.cos(phase), np.sin(phase),
                                 np.ones(len(times))])
        coefficients = np.linalg.lstsq(basis, values, rcond=None)[0]
        return {
            "frequency_hz": float(frequency),
            "amplitude_popcount": float(np.hypot(*coefficients[:2])),
            "explained_variance": float(
                1 - np.var(values - basis @ coefficients) / np.var(values)),
        }

    search = [fit(f) for f in np.arange(395, 405.01, 0.05)]
    result["channels"].append({
        "channel": channel,
        "mean_popcount": float(values.mean()),
        "std_popcount": float(values.std()),
        "best_near_400hz": max(search, key=lambda item: item["explained_variance"]),
        "comparison": [fit(f) for f in [100, 200, 300, 399, 400, 401, 500, 800]],
    })
(root / "dac-dem-analysis.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
