# zm-cc

Compile C, C++ and Fortran across a ZeroMesh team. Each translation unit (TU) is one item: a donor
compiles it in full against an uploaded copy of the source tree, and the object lands where the
build expects it. Nothing here is specific to CMake, Meson or one project; input comes from a
compilation database or a command line.

```bash
# Start zmd, then:
zm-cc --compdb build/compile_commands.json -o zm-objs/
zm-cc --cc g++ --flags "-O2 -std=c++20 -Iinclude" src/a.cpp src/b.cpp
zm-cc -- gcc -O2 -c foo.c -o foo.o
```

Install with the SDK: `pip install ./sdk/python ./apps/zm-cc`.

## How a build runs

1. **Units.** Every source in every command becomes a unit. A plain compile (`-c`) runs on the
   mesh; anything else (a link, `-S`, `-E`, PCH, modules, profiling; see the table below) runs
   unchanged on this machine after the mesh work, and is listed as `here`.
2. **Workspace.** The common directory of every unit's directory, its source and the relative paths
   its `-I`/`-include`/`--sysroot` options name is snapshotted (`--root DIR` overrides it). Build
   products (`*.o *.a *.so *.d *.gch ...`) never travel; `.zmignore` and the secrets deny list apply,
   and only changed files are read again on the next build.
3. **Compile.** On a donor each unit runs its own command in its own directory inside the upload,
   with paths under the workspace rewritten relative to it, system paths (`/usr/include`) left to the
   donor, the command's `-o`/`-MD`/`-MF` replaced by `-o <stem>.<hash>.o -MD -MF <stem>.<hash>.d`.
4. **Objects** are written to `-o DIR` (default `zm-out/`) as `<stem>.<hash8>.o`, or, for the
   `-- CMD -o X` form without `-o`, to `X`. `report.json` beside them records each unit's node,
   timings, cache status and, on failure, its kind and compiler output.
5. **Link** (optional) here, once every unit compiled: `--link-output PATH [--link-flags F]` links
   the objects with the C++ driver when any unit is C++; `--link-ninja BUILD_DIR --link-targets T...`
   runs a meson/ninja build's link steps against zm-cc's objects without recompiling.

**Caching.** A unit's cache key is its command, the toolchain and the files its depfile lists, so
after a one-header edit only the units that include that header compile again, on any machine of
the team. `--no-cache` compiles everything.

## The compiler on donors

- **Default: the donors' own compiler**, which must be the exact build this machine has (driver,
  `cc1`/`cc1plus`, assembler and their libraries), so an object is the same wherever it compiles. A
  donor with a different build is excluded, and the report says so.
- **`--env capture`**: the compiler is captured here and shipped by digest, so donors without it (or
  with another version) compile too. For each compiler and language one real unit compiles under a
  trace (driver, `cc1*`, `as` and their libraries), and the compiler's header search list
  (`cc -E -v`) is taken whole, because other units include headers that one does not. The first
  capture reads every header once (a minute or more for a large `/usr/include`); later ones reuse
  it. Donors read only the headers a compile opens.

## Flags

Each option on a compile command is classified by the table in `zm_cc/flags.py`:

| Kind | Examples | On the donor |
| --- | --- | --- |
| codegen | `-O2 -g -std= -W -m -f`, `-march=`, `-fPIC` | kept |
| preprocessor | `-I -D -U -include -isystem --sysroot` | kept; paths under the workspace rewritten |
| driver/dependency | `-MD -MF -MT -Wp,` `-c -C -P` | replaced by zm-cc's own `-c` and depfile |
| output | `-o` | replaced by zm-cc's object name |
| language | `-x` | kept |
| passthrough | `-S -E -M -fsyntax-only -emit-pch -include-pch -fmodules* -fprofile-* --coverage -save-temps`, stdin | the whole command runs here, unchanged |

## Options

| Option | Meaning |
| --- | --- |
| `--compdb FILE` / `SOURCES --cc CC --flags F` / `-- CMD` | the input; exactly one form per run |
| `--root DIR` | the workspace to upload (default: inferred, see above) |
| `--link-output PATH`, `--link-flags F` | link here after a clean build |
| `--link-ninja BUILD_DIR --link-targets T...` | with `--compdb`: meson/ninja link steps from these objects |

Every SDK flag applies: `-o/--out`, `--cores` (default 1 per compile; raise it for `-flto=auto`),
`--mem`, `--env host|capture|sha256:TREE`, `--remote-only`, `--local-only`, `--keep-going`,
`--no-cache`, `--batch N`, `--dry-run` (the plan, no daemon needed), `--show-upload`, `--resume RUN`,
`--socket`, `-v`. See `zm-cc --help`.

## Worked example: this repo's `core/`

```bash
jq --arg root "$PWD/core/" '[.[] | select(.file | startswith($root))]' \
   build/compile_commands.json > core-cc.json

zm-cc --dry-run --compdb core-cc.json        # read the plan first
zm-cc --compdb core-cc.json -o zm-objs/core
zm-cc --compdb core-cc.json -o zm-objs/core --link-ninja build --link-targets zmd
```

## Troubleshooting

- **`zmd is not running`**: start `zmd`, or pass `--socket`.
- **Every donor fails with `bwrap: loopback: Failed RTM_NEWADDR`**: Ubuntu's AppArmor userns
  restriction; `zm doctor` names the fix on each node.
- **Donors excluded with a toolchain mismatch**: their compiler is not this machine's build. Use
  `--env capture`.
- **`unit cwd ... is outside --root`**: the database spans trees with no common directory you want to
  upload; pass `--root` to a common parent.
- **A header is missing on the donor**: it lives outside the workspace and outside the compiler's
  search list (an absolute `-I/opt/...`). Pass `--root` to include it, or use `--env capture`, which
  takes such paths when the probe compile reads them.
- **A command that writes to stdout** (`-E` without `-o`) runs here, and that output is discarded;
  give it `-o`.

## Limits

- Debug info records the donor's working directory (`DW_AT_comp_dir`); source line markers keep the
  paths as written.
- The end-to-end tests build with gcc/g++, clang and gfortran (a preprocessed `.F90`) against a real
  `zmd`; other compilers that accept gcc's options should work but are untested.
