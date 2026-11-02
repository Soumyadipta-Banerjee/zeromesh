# zm-render

Render a Blender frame range across a ZeroMesh team. Each frame is an item; the frames land in
`-o` (default `zm-out/`) as they finish, next to `report.json`.

```bash
# Start zmd, then:
zm-render shot.blend --frames 1-240
zm-render shot.blend --frames 1-240 --blender-version ">=4.2,<5"   # only donors with such a Blender
zm-render shot.blend --frames 1-240 --env capture                   # ship this machine's Blender
zm-render shot.blend --frames 1-48 --encode out.mp4                 # ffmpeg here, afterwards
```

- **One Blender per batch.** Consecutive frames on a donor run in one Blender process, so the
  scene loads once; each frame is still committed the moment its file is written. A crash or a
  donor leaving loses only the frame in flight, and a failed frame does not stop the others:
  `--resume RUN` renders just what is missing.
- **Software.** By default any donor whose `blender --version` prints a version takes frames;
  donors without Blender are excluded, and the report says why. `--blender-version` narrows that.
  `--env capture` renders one frame here under a trace and ships what Blender read (plus its
  install directory when it lives outside `/usr`), so donors without Blender render too.
  A Blender inside the scene directory (`--blender ./blender`) travels with the scene.
- **Scene files.** The scene's directory is the workspace. Absolute paths the `.blend` references
  outside it are reported at start: donors do not see them (make paths relative, or pack them).

Every SDK flag applies (`--cores`, default 4 per frame; `--remote-only`, `--batch N`, `--dry-run`,
...); see `zm-render --help`. Requires the `zeromesh` Python SDK (`sdk/python`) and a running `zmd`.
