# ZeroMesh 2-machine demo (`demo/m5-all`)

## On other machines (donors)

One script — Blender through `zm monitor`:

```bash
bash ./scripts/donor_step.sh
# more worker slots on this machine:
bash ./scripts/donor_step.sh --slots 4
# with team phrase from the requester:
bash ./scripts/donor_step.sh --join 'ZM-XXXX-…-XXC'
```

Use `bash` even if your shell is fish.

**Donor limits** (owner-controlled; enforced on each donor regardless of requester policy):

| Setting | Flag | Env | `donor.toml` key | Default |
|--------|------|-----|------------------|---------|
| Concurrent worker slots | `--slots N` | `ZM_SLOTS` | `slots` | `1` |
| Cores per slot | `--cores-per-slot N` | `ZM_CORES_PER_SLOT` | `cores_per_slot` | machine cores ÷ slots |
| Extra queued tasks | `--queue-depth N` | `ZM_QUEUE_DEPTH` | `queue_depth` | `0` |
| Cap tasks from one requester | `--max-per-requester N` | `ZM_MAX_PER_REQUESTER` | `max_per_requester` | `0` (unlimited) |

Precedence: **CLI flag → env → `~/.config/zeromesh/donor.toml` → default.**

## On the requester laptop

```bash
cd ~/Desktop/HackMna/zeromesh-demo
source scripts/demo_env.sh
./build-verify/zmd &
./build-verify/zm team create        # share the printed phrase
./build-verify/zm status             # wait until donors show ADMITTED

zm-render apps/zm-render/tests/data/spin_cube.blend \
  --frames 1-500 --chunk 16 -o /tmp/renders --blender blender --policy pull

./build-verify/zm monitor
```

Needs: build once, `pip install -e ./sdk/python -e ./apps/zm-render`.

**Requester dispatch** (`zm-render --policy …`, `JobSpec.dispatch`):

| Policy | When to use |
|--------|-------------|
| `pull` (default) | One task per free slot; next task when a slot finishes. Good default for mixed donors. |
| `batch` / `batch:N` | Also keep up to `N` tasks queued on each donor (`prefetch`, default `1`, capped by its `queue_depth`). Hides transfer time between small tasks. |
| `fast` | Hand out tasks up to each donor’s `slots + queue_depth` immediately. |
| `local-first` | Fill this node’s slots before using peers. |
| `spread` | Same as `pull` (most free slots first). |
| *plugin name* | Custom `IDispatchPolicy` loaded by `zmd` (adapters can ship plugins). |

Shared param (any policy): `--max-in-flight N` → `max_in_flight`. Donor limits always win: a policy can only use slots donors offer.

Task `resources.cores`: default `0` = full slot share; a positive `--cores` only lowers it. `--remote-only` still forces peers only (`may_run_local=false`), independent of policy.
