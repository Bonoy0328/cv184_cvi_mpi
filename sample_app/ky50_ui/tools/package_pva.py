"""Stage the matching SDK configuration and models for one KY50 build."""
import json
from pathlib import Path
import shutil
import sys

dest = Path(sys.argv[1])
palm, face, finger = (value == "1" for value in sys.argv[2:])
config = json.loads(Path("pva/pvaConf.json").read_text())
config["facepose"]["enable"] = face and config["facepose"]["enable"]
config["fingerprint"]["enable"] = finger and config["fingerprint"]["enable"]
(dest / "pvaConf.json").write_text(json.dumps(config, indent=4) + "\n")
models = Path("pva/models")
for enabled, patterns in (
    (palm, ("pva.model", "manifest.json")),
    (face, ("pva_face.model", "face*")),
    (finger, ("pva_fingerprint.model", "pva_fingerprint.model.sha256", "fingerprint*")),
):
    if enabled:
        for pattern in patterns:
            matches = list(models.glob(pattern))
            if not matches:
                sys.exit(f"Missing PVA package input: {pattern}")
            for src in matches:
                shutil.copy2(src, dest)
