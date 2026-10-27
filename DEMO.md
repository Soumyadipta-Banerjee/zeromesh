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

# Use a path that exists on donors (env-host PATH). Do NOT realpath to /opt/...
# unless every donor has the same install layout.
zm-render apps/zm-render/tests/data/spin_cube.blend \
  --frames 1-500 --chunk 16 -o /tmp/renders --remote-only --blender blender

./build-verify/zm monitor
```

Needs: build once, `pip install -e ./sdk/python -e ./apps/zm-render`.

## CFD case-sweep demo

Install the SDK and CFD adapter on the requester:

```bash
pip install -e ./sdk/python -e ./apps/zm-cfd
```

Each donor needs the same OpenFOAM release installed. Run the bundled short
two-case lid-driven-cavity sweep on peer nodes:

```bash
zm-cfd tests/data/cfd-cavity-sweep \
  --solver bash \
  --solver-arg=-lc \
  --solver-arg='blockMesh && icoFoam' \
  --bashrc /path/to/OpenFOAM/etc/bashrc \
  --remote-only \
  -o /tmp/zm-cfd-results
```

Replace `/path/to/OpenFOAM/etc/bashrc` with the installed OpenFOAM setup script.
`zm-cfd` submits one task per independent case, and `zm monitor` shows their
placement and completion while the results are collected under `/tmp/zm-cfd-results`.
