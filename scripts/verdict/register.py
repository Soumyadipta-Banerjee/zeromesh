#!/usr/bin/env python3
"""Pre-registration: hash the files that define the experiment so edits after data collection are detectable."""
import hashlib
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

HASHED = [
    "scripts/verdict/verdict.toml",
    "scripts/baselines/condor_tuned.conf",
    "scripts/zmfield.sh",
    "scripts/verdict/verdict.sh",
    "scripts/verdict/analyze.py",
    "scripts/verdict/stats.py",
    "scripts/verdict/config.py",
    "scripts/verdict/probe.c",
]


def _read(root: Path, rel: str) -> bytes:
    try:
        return (root / rel).read_bytes()
    except FileNotFoundError:
        return b"MISSING"


def compute_hash(root: Path, files: list[str] = HASHED) -> str:
    h = hashlib.sha256()
    for rel in files:
        h.update(rel.encode() + b"\0" + _read(root, rel))
    return h.hexdigest()


def _git_sha(root: Path) -> str:
    try:
        r = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                           capture_output=True, text=True, check=True)
        return r.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "?"


def write_registration(root: Path, out: Path) -> dict:
    reg = {
        "hash": compute_hash(root),
        "git_sha": _git_sha(root),
        "utc": datetime.now(timezone.utc).isoformat(),
        "files": {rel: hashlib.sha256(_read(root, rel)).hexdigest() for rel in HASHED},
    }
    Path(out).write_text(json.dumps(reg, indent=2) + "\n")
    return reg


def check(root: Path, registration: Path) -> bool:
    return json.loads(Path(registration).read_text())["hash"] == compute_hash(root)


def main(argv: list[str]) -> int:
    if len(argv) == 4 and argv[1] == "write":
        write_registration(Path(argv[2]), Path(argv[3]))
        return 0
    if len(argv) == 4 and argv[1] == "check":
        ok = check(Path(argv[2]), Path(argv[3]))
        print("VALID" if ok else "INVALID")
        return 0 if ok else 1
    print("usage: register.py write ROOT OUT | register.py check ROOT REG", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
