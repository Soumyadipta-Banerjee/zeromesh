# Short OpenFOAM cavity sweep

This directory contains two independent 2-D lid-driven cavity cases:
`reynolds-10` and `reynolds-100`. Both use a 60 x 60 x 1 mesh and run `icoFoam`
for 2,000 time steps. They are sized for a visible two-node speed-up demo: a
typical laptop should take roughly two minutes to run both sequentially, while
two donors can process one case each. Exact wall time depends on CPU speed and
the installed OpenFOAM build.

The case files come from OpenFOAM Foundation's OpenFOAM-9 `icoFoam/cavity`
tutorial at commit `d87800e1bde07696061acb231ec68df126a937cc`. The only
changes are the mesh size, end time, output interval, and the viscosity in
`reynolds-100`.
They are distributed under GPL-3.0-or-later; see `COPYING`.

Each case needs meshing before solving. `zm-cfd` runs that sequence on every
donor with:

```bash
zm-cfd tests/data/cfd-cavity-sweep \
  --solver bash \
  --solver-arg=-lc \
  --solver-arg='blockMesh && icoFoam' \
  --bashrc /path/to/OpenFOAM/etc/bashrc \
  --remote-only \
  -o /tmp/zm-cfd-results
```

Use the actual OpenFOAM setup script installed on every donor. The output
directory contains each case's generated mesh, time directories, and logs.

To measure the speed-up, run this first on one machine with the same build:

```bash
time zm-cfd tests/data/cfd-cavity-sweep \
  --solver bash --solver-arg=-lc --solver-arg='blockMesh && icoFoam' \
  --bashrc /path/to/OpenFOAM/etc/bashrc --local-only -o /tmp/zm-cfd-local
```

Then run the `--remote-only` command above with two admitted donors and compare
the wall-clock time. Delete the old output directory or use a new one for each run.
