# zm-cfd

`zm-cfd` is an OpenFOAM adapter for independent case sweeps. It uploads a case
directory, submits one solver task per case through the ZeroMesh Python SDK, and
fetches files changed by each run. This is useful for parameter sweeps where each
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
pass that script with `--bashrc` so the task sources it before starting the solver.

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

Replace the example bashrc path with the exact path for the OpenFOAM version installed on
the workers.

If a case needs preprocessing, use `bash -lc` as the solver command. The included
quick demo uses the upstream OpenFOAM cavity case and creates its mesh before solving:

```bash
zm-cfd tests/data/cfd-cavity-sweep --solver bash \
  --solver-arg=-lc --solver-arg='blockMesh && icoFoam' \
  --bashrc /path/to/OpenFOAM/etc/bashrc --remote-only -o /tmp/zm-cfd-results
```

Each case runs in its own working directory. The adapter captures changed files,
including OpenFOAM time directories, and places them under `cfd-results/`. The
progress output includes the node ID that ran each case.

## Test without OpenFOAM

The automated tests use a small fake solver with real `zmd` processes. They cover
case discovery, task construction, output collection, and remote placement. This
checks the adapter plumbing without pretending to validate CFD numerics or an
OpenFOAM installation.
