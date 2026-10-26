# M5 acceptance: zm-render on four machines

**Goal.** `zm-render shot.blend --frames 1-240` on a 4-machine team produces the
same frames as a local Blender render. With `--env mirror`, a donor without
Blender still renders frames.

## Prerequisites

- M5a Python SDK installed (`pip install ./sdk/python`)
- `zm-render` installed (`pip install ./apps/zm-render`)
- Four machines on one ZeroMesh team; Blender installed on the requester and
  (for host mode) on donors
- A packed `.blend` with relative paths only (or use `--env mirror`)

## Host-mode run

On A (requester):

```bash
# Local reference (same Blender version, Cycles CPU, fixed seed)
blender -b shot.blend -o //ref/frame_#### -F PNG -s 1 -e 240 -a

# Distributed
zm-render shot.blend --frames 1-240 --chunk 10 -o dist/
```

Compare decoded pixels (not file bytes), e.g. with Pillow. All 240 frames must
match. Record wall times below.

## Mirror-mode run (needs M5c)

On a donor **without** Blender installed:

```bash
zm-render shot.blend --frames 1-24 --env mirror -o mirror_out/
```

Confirm `executed_by` is that donor for at least one chunk, and frames match
the local reference for 1–24.

## Results

| Date | Requester | Donors | Mode | Local time | Team time | Match? | Notes |
|---|---|---|---|---|---|---|---|
| | | | host | | | | |
| | | | mirror | | | | |
