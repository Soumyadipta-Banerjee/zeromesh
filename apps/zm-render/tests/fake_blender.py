#!/usr/bin/env python3
"""Minimal Blender stand-in for CI: writes one tiny PNG per frame.

Understands the subset of flags zm-render passes:
  -b SCENE  [-noaudio]  [-E ENGINE]  -o //out/frame_####  -F PNG  -s A  -e B  -a

If the scene basename contains 'skip3', frame 3 is omitted (for missing-frame tests).
If the scene basename contains 'partialfail' and -s start >= 3, exits with code 1
(for partial-chunk tests).
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

_FLAGS_WITH_ARG = frozenset({"-b", "-o", "-s", "-e", "-E", "-F"})
_FLAGS_BOOL = frozenset({"-a", "-noaudio"})


def main(argv: list[str]) -> int:
    out_pat = None
    start = end = None
    scene = ""
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in _FLAGS_WITH_ARG:
            if i + 1 >= len(argv):
                print(f"fake_blender: {a} requires an argument", file=sys.stderr)
                return 2
            if a == "-b":
                scene = argv[i + 1]
            elif a == "-o":
                out_pat = argv[i + 1]
            elif a == "-s":
                start = int(argv[i + 1])
            elif a == "-e":
                end = int(argv[i + 1])
            i += 2
            continue
        if a in _FLAGS_BOOL:
            i += 1
            continue
        if a.startswith("-") and a not in _FLAGS_WITH_ARG and a not in _FLAGS_BOOL:
            # Unknown option (e.g. future Blender flags): ignore.
            i += 1
            continue
        i += 1

    if out_pat is None or start is None or end is None:
        print("fake_blender: missing -o/-s/-e", file=sys.stderr)
        return 2

    base = os.path.basename(scene)
    if "partialfail" in base and start is not None and start >= 3:
        print("fake_blender: simulating chunk task failure", file=sys.stderr)
        return 1

    # Blender //path is relative to the .blend directory (= cwd in our sandbox).
    if out_pat.startswith("//"):
        out_pat = out_pat[2:]
    # Replace #### with a python format later per frame.
    if "####" not in out_pat:
        print("fake_blender: expected #### in -o", file=sys.stderr)
        return 2

    skip3 = "skip3" in base
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
