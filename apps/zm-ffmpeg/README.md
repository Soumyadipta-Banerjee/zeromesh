# zm-ffmpeg

`zm-ffmpeg` distributes independent FFmpeg conversions across a ZeroMesh team. One input file
becomes one cacheable item, so a large media directory can use every available donor without a
custom scheduler or output collector. Independent failures do not stop the other conversions.

This adapter does not split one FFmpeg process across machines, coordinate hardware encoders, or
verify that two different FFmpeg builds create byte-identical media. By default it requires the
exact FFmpeg build found on the requester; set an explicit version constraint only when donors are
known to be compatible.

## Install

```bash
python3 -m venv .venv
.venv/bin/pip install './sdk/python[dev]' './apps/zm-ffmpeg[dev]'
```

Start `zmd` on the requester and donors. The normal installation needs `ffmpeg` on PATH for every
node that can execute a conversion. `ffprobe` is used only by the integration test.

## Convert files

Inputs are relative to the current directory (the snapshotted workspace). The output suffix
selects each output path, and any FFmpeg conversion arguments follow the literal `--` separator:

```bash
zm-ffmpeg --remote-only --suffix .mp4 -o converted/ \
  clips/a.mov clips/nested/b.mkv -- -c:v libx264 -crf 20 -c:a aac
```

This runs `ffmpeg -nostdin -y -i INPUT … OUTPUT` per input and produces:

```text
converted/clips/a.mp4
converted/clips/nested/b.mp4
converted/report.json
converted/report.trace.json
```

Parent directories are preserved, so equal basenames in different directories do not collide.
Plans that produce the same output path, repeat an input, escape the workspace, or overwrite the
input itself are rejected before connecting to the daemon. Output paths refer to files inside the
sandbox and are placed under the shared `-o` directory only after each conversion succeeds.

Useful controls include:

```bash
zm-ffmpeg clips/a.mov --ffmpeg-version '>=6'       # allow a compatible donor FFmpeg
zm-ffmpeg clips/a.mov --env capture               # capture an FFmpeg for donors without one
zm-ffmpeg clips/a.mov --ffmpeg tools/ffmpeg       # use a workspace-relative executable
zm-ffmpeg clips/a.mov --dry-run                   # show the plan without a daemon
zm-ffmpeg clips/a.mov --show-upload --dry-run     # inspect the upload as well
zm-ffmpeg --resume RUN clips/a.mov                # reuse completed work from an earlier run
```

All SDK shared flags apply, including `--cores`, `--mem`, `--batch`, `--no-cache`, `--local-only`,
`--remote-only`, `--keep-going`, `--dry-run`, and `--resume`; see `zm-ffmpeg --help`. Unchanged
conversions are cache hits. A changed input, output suffix, FFmpeg option, resource request, or
execution environment selects a different cache identity. `--resume` uses the saved specification,
so repeat the same inputs, suffix, and options when restoring its outputs and report.

## Validate

```bash
.venv/bin/python -m pytest -v apps/zm-ffmpeg/tests/test_plan.py
ZM_BUILD_DIR=build PYTHONPATH=tests/integration \
  .venv/bin/python -m pytest -v apps/zm-ffmpeg/tests/test_app_e2e.py
meson test -C build app_zm_ffmpeg --print-errorlogs
```

Unit tests need no daemon or FFmpeg installation. The end-to-end test skips itself when `ffmpeg`
or `ffprobe` is unavailable. When present, it generates a tiny real video, converts it through a
real `zmd`, uses `ffprobe` to verify its streams, and checks that a repeat run replays the original
cached result rather than converting it again.
