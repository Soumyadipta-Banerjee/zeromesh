# zm-render

Distribute a Blender frame range across a ZeroMesh team and collect the frames
into one directory.

```bash
# Start zmd, then:
zm-render shot.blend --frames 1-240 -o renders/
zm-render shot.blend --frames 1-240 --chunk 10 --env mirror -o renders/
zm-render shot.blend --frames 1-48 --encode out.mp4
```

Requires the `zeromesh` Python SDK (`sdk/python`) and a running `zmd`. Use
`--env host` when donors have Blender installed, or `--env mirror` once the
env-mirror plugin is available.
