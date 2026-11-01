# ZeroMesh Python SDK

Write an **adapter**: a small program that describes a tool's work as items. The platform snapshots
the workspace, ships the software, places items on the team's machines, retries what infrastructure
broke, caches what did not change, writes each item's outputs back as it finishes and leaves a
standard report.

## Installation

```bash
pip install -e sdk/python      # needs grpcio and protobuf; a running zmd
```

## An adapter

```python
from zeromesh.adapter import Adapter, AdapterError, Item, Out, Env, Res, Policy, main

class Sweep(Adapter):
    name = "zm-sweep"                       # the command; `zm sweep` runs `zm-sweep`
    kind = "solver"                         # costs and memory are learned per kind
    env = Env.host("solver", version=">=2") # or Env.capture([...]) / Env.ref("sha256:...")
    res = Res(cores=2, mem="1G")            # mem set = a hard limit; unset = learned
    policy = Policy(keep_going=True)

    def add_args(self, p):                  # domain flags only
        p.add_argument("cases", nargs="+")

    def check(self, a):                     # fail early, before anything is uploaded
        if not a.cases:
            raise AdapterError("no cases")

    def items(self, a):
        for case in a.cases:
            yield Item(id=case, argv=["solver", case], outputs=[Out(f"{case}.out")])

    def finish(self, a, run):               # here, after the run, with every result;
        print(len(run.succeeded), "solved; outputs under", run.out_dir)  # may return an exit status

if __name__ == "__main__":
    main(Sweep)
```

`main` exits 0 when every item succeeded, 1 when some did not, 2 for a usage error or a missing zmd,
and 130 on Ctrl-C (finished items are kept; `--resume RUN` continues). A non-zero status returned by
`finish()` (a local merge or link failed) wins when the run itself succeeded. With no items, nothing
is submitted and `finish()` still runs. `workspace()` may return
`Workspace(dir, exclude=["*.o", "build/"])` to keep build products out of the snapshot.

### Items

| Field | Meaning |
|---|---|
| `id` | stable across runs: names the item in reports, the cache and `--resume` |
| `argv`, `cwd`, `env` | the command, its directory relative to the workspace root, extra variables |
| `outputs` | `Out(path, dest=None, required=True)`: written to `dest` under `--out` (default: the same path); `Out.changed(dest)`: every file the item creates or modifies |
| `inputs` | workspace paths it reads (default: all); the cache keys on them |
| `footprint` | `Depfile("x.d", search_dirs=[...])`: the item writes a Make-style depfile and the cache keys on what it lists |
| `stage`, `seq`, `after`, `cost`, `priority` | stage name, position for `combine`, items that must succeed first, a size hint, order |

### Environments

- `Env.host("blender", version=">=4.0")`: donors use their own install. Without `version`, only donors
  with exactly this machine's build qualify; with it, the version is probed at offer time.
- `Env.capture(probes=[["blender", "--version"]], paths=["/opt/blender"])`: captured here (what the probes
  read and run, plus the paths) and shipped by digest, so donors without the software run the items.
- `Env.ref("sha256:...")`: a tree captured earlier by `zm env capture`.

### Stages and combine

The class attributes describe the default stage. `stages(a)` returns more `Stage(name=..., env=...,
res=..., kind=..., where="anywhere|local|remote")`; an item picks one with `stage=`. `combine(a)` returns
one command for consecutive items of the default stage, with `{first}` and `{last}` replaced by their
`seq`: tools with a slow start (a renderer loading a scene) pay it once per batch, and each item's
outputs are still committed and retried on their own.

### Shared flags

Every adapter takes `--env host|capture|sha256:TREE`, `--cores N`, `--mem SIZE`, `--gpu N`,
`--remote-only`, `--local-only`, `--keep-going`, `--no-cache`, `--batch N`, `--deadline DURATION`,
`--budget DURATION`, `--dry-run`, `--show-upload`, `--force-large`, `--resume RUN`, `--socket PATH`,
`-v` (stream item output) and `-o/--out DIR` (default `zm-out`; `report.json` and
`report.trace.json` go there).

## Sessions

For work that grows from results:

```python
from zeromesh import Session, Item, Out

with Session("zm-explore", workspace=".") as s:
    s.add(Item(id="seed", argv=["solver", "seed"], outputs=[Out("seed.out")]))
    for result in s.as_completed():
        if result.id == "seed":
            s.add(Item(id="refine", argv=["solver", "refine"], outputs=[Out("refine.out")]))
            s.close()                      # no more items; the loop ends when they are done
```

Only the session adds items, so zmd keeps a session's run open only while a client watches it, as
a `Session` does from the start. If its process dies before closing, the run is cancelled a minute
later (`ended_by` "disconnect"), unless a restarted process takes it over first with
`Session.attach(run_id)`.

## Reports

```python
from zeromesh import report
rep = report.load("zm-out/report.json")   # zeromesh.v1.run_pb2.Report
for r in report.failed(rep):
    print(report.describe(r))             # "case7: out of memory: ..."
```

## The low-level client

`zeromesh.connect()` returns a `Client` with one method per ClientApi call (`snapshot`,
`capture_env`, `submit`, `watch`, `get_report`, `cancel`, `resume`, `peers`, `doctor`, `team_*`, ...).
The socket is `$ZM_SOCKET` (set by `zm` for the adapters it starts), else the one zmd uses.

## Examples and tests

`examples/hello_world.py` and `examples/parameter_sweep.py` are complete adapters.
`meson test -C build sdk_python` runs the SDK's tests against real zmd processes.
