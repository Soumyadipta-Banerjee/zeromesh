# Handoff (updated 2026-11-03)

For the next person or agent continuing this work. Read this first, then the
[adapter platform design](design/adapter-platform.md) (what is being built) and the commit messages on
`New_Arch` (why each piece is the way it is).

**Goal.** ZeroMesh is a backend, like LLVM's: the core (zmd, the run engine, placement, the sandboxed
batch runner, the store) is built once, robust and fast, and every workload is a thin adapter
(`zm-cc`, `zm-render`, `zm-cfd`, ...). The core must compete head to head on speed: on compiles,
with distcc; on every workload, with the old architecture it replaced.

## Where things are

- **Branch `New_Arch`** (local, not pushed): the adapter platform built in full on top of
  `origin/zm_perf_cc` (15 commits ahead, 0 behind). `zm_perf_cc` is the old architecture's last state
  (jobs and tasks, `zm-cc` tree/distcc modes); `main` is older still.
- **Build** (meson needs the venv's python for `grpc_tools`; on this laptop libsodium-dev comes from a
  local prefix):

  ```bash
  export PATH=/home/yash/Desktop/zm-deps/venv/bin:$PATH
  meson setup build -Dpkg_config_path=/home/yash/Desktop/zm-deps/usr/lib/x86_64-linux-gnu/pkgconfig
  ninja -C build -j4
  ```
- **Test what you change**, not everything: `ZM_TEST_PLUGIN_DIR=$PWD/build ZM_TEST_FILTER=<Name>
  ./build/unit_tests` (329 tests), and the meson suites the change touches (`meson test -C build
  integration_local integration_remote integration_fanout sdk_python app_zm_cc ...`, 12 suites;
  `app_zm_cc_capture` is slow: its first capture hashes `/usr/include`).
- **Machines:** this laptop plus two Ubuntu 24.04 VMs (`zm-a` 192.168.122.254, `zm-b` .174, 2 vCPU,
  1.5 GB). [tests/vm/README.md](../tests/vm/README.md) has the harness (stage, deploy, form a team,
  shape the link) and what it found. Start them only when memory allows: they take 3.2 GB of the
  laptop's 15 GB.
- **Commits:** `git commit --no-gpg-sign`, no co-author line (for now).

## What New_Arch is, against zm_perf_cc

| | zm_perf_cc | New_Arch |
|---|---|---|
| Public API | jobs and tasks | runs and items (RunApi); batches only between daemons |
| Unit of work | a task of ~25 compiles in one shell | an item: its own sandbox (bwrap over a shared overlay of the inputs) and cgroup |
| Results and cache | per task (25 units) | per item, team-wide, keyed by the item's depfile footprint |
| Memory | no budget; outside its systemd unit, work ran unbounded (the code New_Arch inherited this from froze a 1.5 GB VM for 37 s) | all work in a delegated cgroup with a budget; OOM kills retried with a node to themselves; a pressure watcher stops the newest item when work stalls |
| A failing unit | fails its task; `zm-cc` then compiled all 98 Redis units locally | fails only itself, retried per policy |
| Placement | static: about four tasks | learned costs, longest first, batches formed only for free slots; donors announce `work_cores` and take work only through their executor's hold |
| Environments | host, or a FUSE mirror of chosen paths | host (exact toolchain digest) or captured tree (`zm env capture`, fetched lazily) |
| SDK | Job/Task client | adapters, sessions (held open while a client watches), reports, resume |
| Partial work | `commit_unit` partial results | checkpoints restored on the next attempt; SIGTERM now reaches the item to save one |
| Tests | 315 unit, 10 suites | 329 unit, 12 suites |

Fixes made on 2026-11-03, each with its evidence in the commit message: containment and memory
budget (`e3cddfd`), run done only after its report (`4f86e17`), item exit by pidfd and signals reset
in the item (`e221f17`), sessions need a live watcher (`d334359`), batches only for free slots plus
`work_cores` and executor holds (`4c53193`), TLS server name = node id (`f26d1df`), outputs fetched
beside the event stream and bandwidth measured over the fetch (`7b6afbd`).

## Measured: New_Arch vs zm_perf_cc (2026-11-03)

Redis 7.2.7, `src/*.o` (98 C units), `zm-a` requesting, `zm-a` + `zm-b` working (4 slots), debug
builds of both branches, distcc with the same 4 slots as the control in every pair. Script:
`/tmp/bench.sh new|old N` on `zm-a` (gone after a VM reboot; the commands are in this file's history
and the session notes). `zm_perf_cc`'s `zm-cc` needs `--root ~/oss/redis-7.2.7`: without it the
uploaded root misses `../deps`, its remote tasks fail and it silently compiles every unit locally.

| | zm-cc median | median, quiet host (distcc <= 4.8 s) | best | zm-cc / distcc, quiet | runs >= 6.5 s | first run after a fresh deploy |
|---|---|---|---|---|---|---|
| New_Arch (16 runs) | 5.40 s | 5.30 s | 5.19 s | 1.20 | 3 | 9.7-12.8 s |
| zm_perf_cc (12 runs) | 5.27 s | 5.11 s | 4.66 s | 1.12 | 4 | 7.5-9.0 s |
| distcc | 4.6-4.8 s | | 4.16 s | 1.00 | 0 | |

Both lose to distcc; New_Arch is about 0.2 s behind the old code in steady state and further behind on
a cold start. Both have occasional 8-10 s runs when the laptop under the VMs is busy.

Where New_Arch's time goes (strace on both VMs, so the absolute numbers are inflated):
- **Per item on a donor: ~25 ms between one item's exit and the next item's start.** About 6 ms of it
  is fsync (every new blob is fsynced when the store commits it: `.o`, `.d`, logs); the rest is the
  per-item sandbox, cgroup and directory work in `BatchRunner::RunGroup`. Redis units compile in about
  70 ms, so this is a third of the work.
- **Per remote batch: 0.2-0.4 s idle on the slot between batches.** For every batch the placer walks
  the whole input tree twice (`InputTreeBytes` and the `needed` list in `Placer::Place`, both
  `CollectTree`), and the donor checks every input digest with `HasBlob` at Offer and again at
  RunBatch: about 1,500 files for Redis, the same each time.
- **Start-up: ~0.85 s to the first local item, ~1.35 s to the first remote one.**
- **Cold start:** nothing learned, so the first run is sized blind (8 batches, then 6 once costs are
  known).

## Open items, in order

1. **Performance, head to head.** Remove the overheads above at their root and re-run the Redis A/B
   until New_Arch beats both distcc and `zm_perf_cc`; then real projects (this repo's `core/`, a
   larger C++ tree) and the other adapters.
2. **Owed: a full VM run of `4c53193`** (27 of `core/`'s heavy units, `--remote-only` onto the
   VMs). The last one stopped at 13/27 when the laptop ran short of memory for other reasons.
3. **Step 7, docs:** rewrite the design doc's status, PLAN.md (M7-M11), the READMEs and
   `docs/team/TASKS.md` (still mentions `integration_mirror`) to match the code.
4. **CI:** the workflow changes of `e3cddfd` (systemd user manager on Ubuntu, a cgroup per unit test
   in the Arch container) have never run on GitHub; `New_Arch` is not pushed.
5. **From spec 002** (an old-architecture spec): capabilities and version negotiation, an API lock
   with golden keys, a core neutrality guard, a pure C plugin ABI, REAPI as a supervised process.
   None started.
6. Small: `net::PeerChannel` in `core/net/tls.cpp` is used only by tests. Carried over from the
   2026-11-02 handoff and not re-checked on New_Arch: the REAPI front-end's action cache lives in
   memory and `WaitExecution` is a stub; plugins are not installed by `meson install`; no Landlock.

## Where the record lives

- **Commit messages** on `New_Arch`: each says what was wrong, how it was found and what was
  measured.
- **[docs/design/adapter-platform.md](design/adapter-platform.md):** the design being built; its
  status sections are due in step 7.
- **[tests/vm/README.md](../tests/vm/README.md):** what the laptop + VMs setup found.
