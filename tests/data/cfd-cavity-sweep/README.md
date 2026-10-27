# Short OpenFOAM cavity sweep

This directory contains two small, independent 2-D lid-driven cavity cases:
`reynolds-10` and `reynolds-100`. Both use a 20 x 20 x 1 mesh and run `icoFoam`
for only 0.05 simulated seconds (ten time steps). They are intended as a quick
two-node ZeroMesh demo, rather than a numerically meaningful benchmark.

The case files come from OpenFOAM Foundation's OpenFOAM-9 `icoFoam/cavity`
tutorial at commit `d87800e1bde07696061acb231ec68df126a937cc`. The only
changes are the shortened end time and the viscosity in `reynolds-100`.
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
