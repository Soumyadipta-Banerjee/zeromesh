# ZeroMesh 2-machine demo (`demo/m5-all`)

## One-time on each machine

```bash
source scripts/demo_env.sh
# build: meson setup build-verify --native-file /tmp/zm-native.ini && ninja -C build-verify -j2
# requester only: pip install -e ./sdk/python -e ./apps/zm-render
```

Both machines need `blender` on `PATH`. Firewall: `8981/tcp`, `8988/udp`.

## Run

**This laptop (offload):**

```bash
./scripts/demo_quick.sh
```

**Other machines (donors)** — when the join phrase prints:

```bash
./scripts/setup_donor.sh --join 'ZM-XXXX-…-XXC'
```

Then press Enter on the laptop. Monitor with `q` / Ctrl-C.
