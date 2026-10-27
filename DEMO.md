# ZeroMesh 2-machine demo (`demo/m5-all`)

## On other machines (donors)

One script — branch, Blender check, build, `zmd`, monitor:

```bash
./scripts/setup_donor.sh
# with team phrase from the requester:
./scripts/setup_donor.sh --join 'ZM-XXXX-…-XXC'
```

Order inside the script:
1. `git checkout demo/m5-all` + pull  
2. Blender on PATH + `demo_env.sh`  
3. Build `zm` / `zmd` if missing (`ninja -j2`)  
4. Start `zmd`  
5. `zm monitor` (leave running)

## On the requester laptop

```bash
source scripts/demo_env.sh
zmd &                        # or already running
zm team create               # share the printed phrase with donors
zm-render apps/zm-render/tests/data/spin_cube.blend \
  --frames 1-48 --chunk 8 -o /tmp/renders --remote-only
zm monitor
```

Needs: build once, `pip install -e ./sdk/python -e ./apps/zm-render`, Blender on donors.
