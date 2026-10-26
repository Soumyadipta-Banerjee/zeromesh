# ZeroMesh 2-machine demo (`demo/m5-all`)

This tree combines **M3 core** + **M5a SDK** + **M5b zm-render** + **M5c env-mirror**
for a live demo. Branch: `demo/m5-all`. Worktree: `../zeromesh-demo`.

## One-time on each machine

```bash
git clone <repo> zeromesh && cd zeromesh
git checkout demo/m5-all          # or copy this worktree
# deps: see README (grpc, protobuf, libsodium, openssl, bubblewrap, fuse3)
make build

python3 -m venv .venv && source .venv/bin/activate
pip install -e ./sdk/python -e ./apps/zm-render pytest grpcio

# firewall
# 8981/tcp  NodeApi
# 8988/udp  discovery
```

Ubuntu sandbox tip: `sudo sysctl kernel.apparmor_restrict_userns=0`  
(or `kernel.apparmor_restrict_unprivileged_userns=0`).

## Demo script

### 1. Start daemons (both machines)

```bash
./build/zmd
```

### 2. Form a team

On **A** (you):

```bash
./build/zm team create
# → 'ZM-XXXX-…-XXC'
```

On **B** (friend):

```bash
./build/zm team join 'ZM-XXXX-…-XXC'
```

### 3. Prove remote execution (M3)

On A:

```bash
./build/zm status          # B shows ADMITTED=yes within ~15s
./build/zm run --remote-only -o out.txt -- sh -c 'hostname > out.txt'
cat out.txt                # must be B's hostname
```

### 4. Render with Blender on donors (M5b, host env)

Both need `blender` on `PATH`. On A:

```bash
source .venv/bin/activate
zm-render apps/zm-render/tests/data/cube.blend --frames 1-8 --chunk 2 -o /tmp/renders
ls /tmp/renders/frame_*.png
```

### 5. Optional wow: donor without Blender (M5c mirror)

On B, hide Blender from PATH (or use a machine without it). On A:

```bash
zm-render shot.blend --frames 1-8 --env mirror -o /tmp/mirror_out
```

Requires FUSE on the donor and the env-mirror plugin loaded by `zmd`.

## Local smoke (one machine, no friend)

```bash
source .venv/bin/activate
export ZM_BUILD_DIR=$PWD/build
pytest apps/zm-render/tests -v          # fake blender + real zmd
meson test -C build integration_remote --print-errorlogs
```
