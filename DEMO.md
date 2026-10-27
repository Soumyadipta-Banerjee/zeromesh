# ZeroMesh 2-machine demo (`demo/m5-all`)

## On other machines (donors)

One script — Blender through `zm monitor`:

```bash
bash ./scripts/donor_step.sh
# with team phrase from the requester:
bash ./scripts/donor_step.sh --join 'ZM-XXXX-…-XXC'
```

Use `bash` even if your shell is fish.

## On the requester laptop

```bash
cd ~/Desktop/HackMna/zeromesh-demo
source scripts/demo_env.sh
./build-verify/zmd &
./build-verify/zm team create        # share the printed phrase
./build-verify/zm status             # wait until donors show ADMITTED

# same blender path donors print (from command -v / realpath), or /usr/local/bin/blender
BLENDER="$(command -v blender)"; BLENDER="$(realpath "$BLENDER")"
zm-render apps/zm-render/tests/data/spin_cube.blend \
  --frames 1-500 --chunk 16 -o /tmp/renders --remote-only --blender "$BLENDER"

./build-verify/zm monitor
```

Needs: build once, `pip install -e ./sdk/python -e ./apps/zm-render`.
