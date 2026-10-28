#!/usr/bin/env python3
"""Licence gate for third-party data assets (run in CI).

Rules (docs/anatomy-roadmap.md, "Licensing rules"):
  - every data file under an `assets/` directory must be covered by a
    MANIFEST.json in its own directory or a parent directory below `assets/`;
  - a manifest's licence must be on the allow-list (no NC, no SA, no unknown);
  - a manifest's attribution string must appear verbatim in the root NOTICE;
  - a manifest directory must also carry a LICENSE file.

Usage:  python3 scripts/check_assets.py [repo_root]
Exit code 0 = OK, 1 = violations (listed on stderr).
"""

import json
import os
import subprocess
import sys

ALLOWED = {"CC0-1.0", "CC-BY-4.0", "public-domain", "MIT", "BSD-2-Clause", "BSD-3-Clause", "Apache-2.0", "Zlib"}
DATA_EXT = {".pack", ".glb", ".gltf", ".obj", ".stl", ".ply", ".fbx", ".nii", ".gz", ".nrrd", ".dcm", ".vtk", ".bin"}
REQUIRED_KEYS = ("name", "source_url", "license", "license_url", "attribution", "retrieved")


def tracked_files(root):
    try:
        out = subprocess.run(["git", "ls-files"], cwd=root, capture_output=True, text=True, check=True).stdout
        return [p for p in out.splitlines() if p]
    except (OSError, subprocess.CalledProcessError):
        files = []
        for d, _, fs in os.walk(root):
            if "/.git" in d or "node_modules" in d:
                continue
            files += [os.path.relpath(os.path.join(d, f), root) for f in fs]
        return files


def main():
    root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), ".."))
    files = tracked_files(root)
    notice_path = os.path.join(root, "NOTICE")
    notice = open(notice_path, encoding="utf-8").read() if os.path.exists(notice_path) else ""
    errors = []
    if not notice:
        errors.append("NOTICE is missing at the repository root")

    manifests = {}
    for f in files:
        if os.path.basename(f) == "MANIFEST.json" and "/assets/" in "/" + f:
            d = os.path.dirname(f)
            try:
                m = json.load(open(os.path.join(root, f), encoding="utf-8"))
            except (OSError, ValueError) as e:
                errors.append(f"{f}: unreadable ({e})")
                continue
            manifests[d] = m
            for k in REQUIRED_KEYS:
                if not m.get(k):
                    errors.append(f"{f}: missing '{k}'")
            lic = m.get("license", "")
            if lic not in ALLOWED:
                errors.append(f"{f}: licence '{lic}' is not on the allow-list {sorted(ALLOWED)}")
            if m.get("attribution") and m["attribution"] not in notice:
                errors.append(f"{f}: attribution not found verbatim in NOTICE")
            if os.path.join(d, "LICENSE") not in files:
                errors.append(f"{d}/LICENSE is missing")

    for f in files:
        parts = f.split("/")
        if "assets" not in parts or "node_modules" in parts:
            continue
        if os.path.splitext(f)[1].lower() not in DATA_EXT:
            continue
        d = os.path.dirname(f)
        covered = False
        while d and os.path.basename(d) != "":
            if d in manifests:
                covered = True
                break
            if os.path.basename(d) == "assets":
                break
            d = os.path.dirname(d)
        if not covered:
            errors.append(f"{f}: data file not covered by a MANIFEST.json")

    for e in errors:
        print("check_assets: " + e, file=sys.stderr)
    print(f"check_assets: {len(manifests)} manifest(s), {len(errors)} problem(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
