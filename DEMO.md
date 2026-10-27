# ZeroMesh 2-machine demo (`demo/m5-all`)

## On other machines (donors)

```bash
bash ./scripts/setup_donor.sh
# with team phrase from the requester:
bash ./scripts/setup_donor.sh --join 'ZM-XXXX-…-XXC'
```

Order: `demo/m5-all` → Blender/env → build → `zmd` → `zm monitor`.

Use `bash` even if your shell is fish (`source scripts/demo_env.sh` is bash).

## On the requester laptop

```bash
cd ~/Desktop/HackMna/zeromesh-demo   # this machine's demo worktree
source scripts/demo_env.sh
./build-verify/zmd &
./build-verify/zm team create        # share the printed phrase
./build-verify/zm status             # wait until donors show ADMITTED
zm-render apps/zm-render/tests/data/spin_cube.blend \
  --frames 1-96 --chunk 8 -o /tmp/renders --remote-only
./build-verify/zm monitor
```

Needs: build once, `pip install -e ./sdk/python -e ./apps/zm-render`. Donors need `blender` on PATH.
