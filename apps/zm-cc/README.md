# zm-cc

Distribute compilation across a ZeroMesh team. Works with any compiler that can
preprocess and then compile: gcc, g++, clang, clang++, gfortran. Nothing here is
specific to CMake, Meson or one project. Input comes from a compilation database
or from a command line.

```bash
# Start zmd, then:
zm-cc --compdb build/compile_commands.json --out zm-objs/
zm-cc --cc g++ --flags "-O2 -std=c++20 -Iinclude" src/a.cpp src/b.cpp
zm-cc -j8 -- gcc -O2 -c foo.c -o foo.o
```

Install with the SDK: `pip install ./sdk/python ./apps/zm-cc`.

## How a unit is built

distcc's split: the preprocessor runs on the requester, and the donor only compiles.

1. `cc -E` runs locally on each translation unit (TU). The result is self-contained:
   headers are inlined and `-D` is applied, so the donor needs no include paths and no
   source tree.
2. The preprocessed files are staged in one directory and uploaded once. One job is
   submitted, with `--chunk` TUs per task.
3. Each donor compiles with codegen flags only, for example
   `cc -O2 -std=c11 -x cpp-output -c hello.<hash>.i -o out/hello.<hash>.o`.
4. Objects are fetched and placed in `--out`. Each one must exist and be non-empty.

Every flag is classified by the table in `zm_cc/flags.py`:

| Kind | Examples | Where it goes |
| --- | --- | --- |
| codegen | `-O2 -g -std= -W -m -f`, `-march=`, `-fPIC` | local `-E` and remote compile |
| preprocessor-only | `-I -D -U -include -isystem --sysroot` | local `-E` only |
| driver/dependency | `-MD -MF -MT -Wp,` `-c -C -P` | dropped from both |
| output | `-o` | replaced by zm-cc's own object name |
| language | `-x` | local `-E`; remote sets its own |
| passthrough | `-S -E -M -fsyntax-only -emit-pch -include-pch -fmodules* -fprofile-* --coverage -save-temps`, stdin | whole command runs locally, unchanged |

A command that cannot be split runs locally and is reported as `local` with its
reason. A link without `-c` is one such case.

## Input forms

| Form | Example | Notes |
| --- | --- | --- |
| `--compdb FILE` | `zm-cc --compdb build/compile_commands.json` | `command` or `arguments`; relative `directory` is honoured |
| explicit | `zm-cc --cc g++ --flags "-O2 -Iinclude" a.cpp b.cpp` | `--cc` defaults to `cc` |
| `-- CMD` | `zm-cc -j8 -- gcc -O2 -c foo.c -o foo.o` | one command; several sources become several TUs |

Exactly one form per run.

## Options

| Option | Default | Meaning |
| --- | --- | --- |
| `--out DIR` / `-o` | `zm-objs/` | Object directory. With `-- CMD -o X` and no `--out`, the object goes to `X`. |
| `--chunk N` | 1 | TUs per remote task. A chunk runs its compiles in one `/bin/sh -c`, stopping at the first failure. |
| `--cores N` | 2 | Cores reserved per task. See below. |
| `-j N` | CPU count | Concurrency of local preprocessing and passthrough units. |
| `--env host\|mirror` | host | Donor environment. See below. |
| `--mirror PATH` | | Root to mirror (repeatable). Needed with `--env mirror`. |
| `--ld-library-path P` | | `LD_LIBRARY_PATH` for donor tasks, e.g. for libraries under `/opt`. |
| `--remote-only` | | Never run remote-eligible work locally (`may_run_local = false`). |
| `--keep-going` | | Do not cancel after the first failure. Exit 0 even if some units failed; the report lists them. |
| `--submit-timeout S` | 1800 | Upload and submit timeout. A large mirror manifest can exceed the SDK's 10 s default. |
| `--link-output P` `--link-flags F` | | After a clean build, link the objects locally with the same compiler (C++ driver if any unit is C++). |
| `--socket PATH` | | Override the zmd socket. |
| `--dry-run` | | Print the plan (every command that would run) and exit. No daemon needed. |
| `-v` | | Print each local command. |

Output: `zm-objs/zm-cc-report.json` is a compile_commands-style record of every unit
(directory, file, command, output, node, timings, bytes shipped, cache hit) plus an
`objects` map from object to source. Object names are `<stem>.<hash8>.o`, where the
hash comes from the TU path and its output. Two different commands that would write
the same name are an error, not a silent overwrite.

## `--cores`

A compiler is mostly single-threaded, so the default is 2. That leaves room for the
donor's own work and a second compile. A donor with N cores donating N-1 runs about
`floor((N-1)/cores)` tasks at once. Raise `--cores` only for LTO or `-flto=auto`.

## Host versus mirror

- **`--env host`** (default). The donor runs its own compiler. Donors must have a
  compiler that is byte-identical to the requester's, because a donor refuses the task
  otherwise (`toolchain for 'g++' differs from the requester's`). The toolchain digest
  covers the compiler binary and every shared library it links. So host works only
  across identical installs.
- **`--env mirror --mirror ROOT`**. The donor runs a root tree built on the requester.
  Use this for heterogeneous donors. Make the root with `zm-cc mkroot`, then pass it
  as `--mirror`.

The sandbox uses the mirror's `usr/` and `opt/` only when they exist at the top of the
mirrored directory (`plugins/env-mirror/env_mirror.cpp:148-164`). Mirror the root
directory itself, not a subdirectory. Each `--mirror` path is mounted under its
basename. Set `--ld-library-path` for libraries that live outside `/usr/lib`.

## `zm-cc mkroot`

```bash
zm-cc mkroot --cc g++ --out ~/zm-root-gcc
zm-cc --compdb build/compile_commands.json --env mirror --mirror ~/zm-root-gcc
```

It copies, and recreates host symlinks as symlinks:

- the driver and the programs it runs (`-print-prog-name`: cc1, cc1plus, f951, as, ld,
  collect2, lto-wrapper), plus `/bin/sh`
- the GCC internal directory, the include search list (`cc -v`), and the shared
  libraries those binaries link (`ldd`)
- everything a probe compile opens, when `strace` is installed (`--no-strace` skips it)

Each file's content is copied once, at its real path, with `cp -a --reflink=auto`.
Symlinks such as `lib -> usr/lib` stay symlinks. Two copies of libc under different
paths would load twice, so this matters. The tree covers compiling (`-c`). Linking is
local, so the root does not need the libc link files.

## Troubleshooting

- **`zmd is not running`**: start `zmd`, or pass `--socket`.
- **`toolchain for '...' differs from the requester's`**: the donor's compiler is not
  identical. Use `--env mirror` with a root from `zm-cc mkroot`.
- **`owner busy: donated work is held at comfort level N`**: the donor's comfort
  governor is holding work. Set `max_step = 0` in `donor.toml`. For benchmarking only,
  `ZMF_NO_GOVERNOR=1` disables it (see `scripts/zmfield.sh`).
- **`no object produced`**: the task succeeded but wrote no `out/<name>.o`. Compare the
  `command` in `zm-cc-report.json` with the same command run by hand.
- **`preprocess exit N`**: the local `-E` failed. Fix the source or the flags; nothing
  was sent to a donor.
- **Submit or upload timeout on a large mirror**: raise `--submit-timeout`.
- **A missing library at run time**: pass `--ld-library-path` for the directory that
  holds it. `mkroot` prints the value to use.

## Worked example: this repo's `core/`

Build the project once, with any generator that writes a compilation database. Meson
writes `compile_commands.json` into the build directory. `ninja -t compdb` and
`bear -- make` also work. Then keep only the `core/` entries:

```bash
jq --arg root "$PWD/core/" '[.[] | select(.file | startswith($root))]' \
   build/compile_commands.json > core-cc.json

zm-cc --dry-run --compdb core-cc.json        # read the plan first
zm-cc --compdb core-cc.json --out zm-objs/core --chunk 4 --cores 2
```

`zm-cc` prints who compiled what and where the time went. The objects are in
`zm-objs/core/`, and the mapping from object to source is in
`zm-objs/core/zm-cc-report.json`.

## Limits

- Preprocessing happens locally, so the donor sees only the preprocessed text. Anything
  the compiler opens later breaks, such as an assembler `.incbin` in inline asm.
- Dependency files (`.d`) are not written. Use the build system's own dependency
  tracking.
- The debug info records the donor's working directory as `DW_AT_comp_dir`. Source
  line markers keep the original paths.
- Passthrough and link steps always run locally. `--remote-only` does not change them.
- Fortran was smoke-tested with gfortran 16 (`-cpp -P` is used). clang was checked only
  for `-x cpp-output` acceptance, not run end to end.
