# zm-task — General-Purpose Distributed Task Runner

Distribute **any** folder + `run.sh` across your ZeroMesh team. Each line in
`run.sh` becomes an independent task that runs in parallel on different nodes.

## Quick Start

```bash
# Install
cd apps/zm-task && pip install -e .

# Create a task folder
mkdir my_task && cd my_task

# Add your payload files
cp ~/data/*.csv .

# Create run.sh — each line runs on a different node
cat > run.sh << 'EOF'
python process.py --shard 0 --input data.csv --output out/result_0.json
python process.py --shard 1 --input data.csv --output out/result_1.json
python process.py --shard 2 --input data.csv --output out/result_2.json
python process.py --shard 3 --input data.csv --output out/result_3.json
EOF

# Submit to the mesh
zm-task . -o /tmp/results --remote-only
```

## How It Works

1. **Parse**: `zm-task` reads `run.sh` and extracts each executable line as an
   independent command.
2. **Upload**: The entire task folder (run.sh + all other files) is uploaded to
   ZeroMesh CAS as a Merkle tree.
3. **Distribute**: Each command (or group of commands with `--group-size`) is
   submitted as a task to the mesh. The scheduler places them on available donors.
4. **Execute**: Each donor runs its assigned command(s) via `/bin/sh -c "..."`.
   Commands should write outputs to `out/` (or custom `--outputs` path).
5. **Collect**: Outputs from all tasks are fetched back and merged into
   `--output` directory.

## run.sh Format

```bash
#!/bin/sh
# Lines starting with # are ignored (except @label directives)
# Blank lines are ignored
# Shebang line is ignored

# @label train_shard_0
python train.py --shard 0

# @label train_shard_1
python train.py --shard 1

# Line continuations work
ffmpeg -i input.mp4 \
  -vf "scale=1920:1080" \
  -o out/output.mp4
```

### Features

- **Comments**: `#` lines are skipped
- **Labels**: `# @label <name>` sets a human-readable label for the next command
- **Continuations**: Lines ending with `\` are joined
- **Any language**: Commands can be anything — Python, bash, ffmpeg, gcc, etc.

## CLI Reference

```
zm-task <task_dir> [options]

Positional:
  task_dir              Path to folder containing run.sh + payload files

Options:
  -o, --output DIR      Output directory (default: output/)
  --outputs PATHS       Comma-separated output paths to collect (default: out/)
  --group-size N        Commands per group (default: 1 = max parallelism)
  --env {host,mirror}   Environment provider (default: host)
  --mirror PATH         Host path to export for --env mirror (repeatable)
  --cores N             Cores per task group (default: 1)
  --remote-only         Only use peer donors
  --local-only          Only run locally
  --shell PATH          Shell on donors (default: /bin/sh)
  --socket PATH         Override zmd socket path
  --dry-run             Parse and print tasks without submitting
```

## Examples

### Distribute a build across nodes

```bash
# task folder structure:
# my_build/
#   run.sh
#   src/
#   Makefile

cat > my_build/run.sh << 'EOF'
make -j4 module_a   && cp build/module_a out/
make -j4 module_b   && cp build/module_b out/
make -j4 module_c   && cp build/module_c out/
EOF

zm-task my_build/ -o /tmp/build_results --remote-only --cores 4
```

### Batch image processing

```bash
# task folder:
# images/
#   run.sh
#   convert.sh
#   *.jpg

cat > images/run.sh << 'EOF'
./convert.sh img001.jpg out/img001.webp
./convert.sh img002.jpg out/img002.webp
./convert.sh img003.jpg out/img003.webp
EOF

zm-task images/ -o /tmp/converted
```

### Dry run to inspect task splitting

```bash
zm-task my_task/ --dry-run --group-size 3
```
