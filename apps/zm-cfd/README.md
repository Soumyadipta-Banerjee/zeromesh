# zm-cfd

`zm-cfd` is an OpenFOAM adapter for independent case sweeps. It snapshots a case
directory, runs one item per case through the ZeroMesh Python SDK, and writes the
files each case changed back as soon as that case finishes. This is useful for parameter sweeps where each
case can run independently. It does not split one coupled OpenFOAM solve across
nodes or provide MPI coordination.

## Install

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -e ./sdk/python -e ./apps/zm-cfd
```

Start `zmd` on each team member. On every worker, make the OpenFOAM environment
available to its daemon. For installations that require an OpenFOAM setup script,
pass that script with `--bashrc` so each case sources it before starting the solver.

## Run a sweep

The input can be one case containing `system/controlDict`, or a parent directory
whose immediate child directories each contain their own `system/controlDict`.
For example:

```text
cases/
  lowRe/
    system/controlDict
    constant/...
    0/...
  highRe/
    system/controlDict
    constant/...
    0/...
```

```bash
zm-cfd cases/ --solver simpleFoam \
  --bashrc /usr/lib/openfoam/openfoam2406/etc/bashrc \
  --remote-only -o cfd-results/
```

A failed case does not stop the others. Every adapter flag of the SDK applies
(`--cores`, `--mem`, `--local-only`, `--resume RUN`, `--dry-run`, ...); see
`zm-cfd --help`.

Replace the example bashrc path with the exact path for the OpenFOAM version installed on
the workers.

If a case needs preprocessing, use `bash -lc` as the solver command. The included
quick demo uses the upstream OpenFOAM cavity case and creates its mesh before solving:

```bash
zm-cfd tests/data/cfd-cavity-sweep --solver bash \
  --solver-arg=-lc --solver-arg='blockMesh && icoFoam' \
  --bashrc /path/to/OpenFOAM/etc/bashrc --remote-only -o /tmp/zm-cfd-results
```

Each case runs in its own working directory. Everything it creates or modifies,
including OpenFOAM time directories, lands in `<out>/<case>/` (`-o`, default
`zm-out`), next to `report.json`, which records per case where it ran, how long it
took and why it failed. Unchanged cases are cache hits on the next run.

## Test without OpenFOAM

The automated tests use a small fake solver with real `zmd` processes. They cover
case discovery, the solver command, output placement, and remote placement. This
checks the adapter plumbing without pretending to validate CFD numerics or an
OpenFOAM installation.
