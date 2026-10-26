#!/usr/bin/env python3
"""Minimal Blender stand-in for CI: writes one tiny PNG per frame.

Understands the subset of flags zm-render passes:
  -b SCENE  -o //out/frame_####  -F PNG  -s A  -e B  -a

If the scene basename contains 'skip3', frame 3 is omitted (for missing-frame tests).
"""

from __future__ import annotations

import os
import sys

# 1x1 red PNG
PNG = (
    b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01"
    b"\x08\x02\x00\x00\x00\x90wS\xde\x00\x00\x00\x0cIDATx\x9cc\xf8\xcf"
    b"\xc0\x00\x00\x00\x03\x00\x01\x00\x05\xfe\xd4\xef\x00\x00\x00\x00IEND\xaeB`\x82"
)


def main(argv: list[str]) -> int:
    out_pat = None
    start = end = None
    scene = ""
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "-b" and i + 1 < len(argv):
            scene = argv[i + 1]
            i += 2
            continue
        if a == "-o" and i + 1 < len(argv):
            out_pat = argv[i + 1]
            i += 2
            continue
        if a == "-s" and i + 1 < len(argv):
            start = int(argv[i + 1])
            i += 2
            continue
        if a == "-e" and i + 1 < len(argv):
            end = int(argv[i + 1])
            i += 2
            continue
        i += 1

    if out_pat is None or start is None or end is None:
        print("fake_blender: missing -o/-s/-e", file=sys.stderr)
        return 2

    # Blender //path is relative to the .blend directory (= cwd in our sandbox).
    if out_pat.startswith("//"):
        out_pat = out_pat[2:]
    # Replace #### with a python format later per frame.
    if "####" not in out_pat:
        print("fake_blender: expected #### in -o", file=sys.stderr)
        return 2

    skip3 = "skip3" in os.path.basename(scene)
    for frame in range(start, end + 1):
        if skip3 and frame == 3:
            continue
        path = out_pat.replace("####", f"{frame:04d}")
        if not path.lower().endswith(".png"):
            path = path + ".png"
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "wb") as f:
            f.write(PNG)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
