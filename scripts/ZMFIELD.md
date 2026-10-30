# zmfield.sh: ZeroMesh vs HTCondor on real laptops

One bash script (`scripts/zmfield.sh`) that sets up, runs and reports a ZeroMesh vs HTCondor comparison
across two or more Linux laptops on one LAN. Linux only (bubblewrap, cgroup v2, FUSE). Run it with
`bash scripts/zmfield.sh ...` even if your login shell is fish. `--dry-run` prints what a command would do.

## Two-laptop recipe

A = requester (also the HTCondor central manager + submit node), B = donor (HTCondor execute node).
Both on the same LAN, both plugged in, awake, lid open.

**1. On both laptops** (clone the repo, check out `v2-integration`):

```bash
bash scripts/zmfield.sh doctor                 # read-only checks
bash scripts/zmfield.sh setup --open-firewall  # deps, bubblewrap>=0.12 if needed, build, HTCondor
bash scripts/zmfield.sh selftest               # meson test; first answer to "does v2-integration compile and pass?"
```

`v2-integration` at `2341adc` builds and passes all 10 `meson test` suites on Arch (one host). If `selftest` fails
on a laptop, that is new information: stop and look before running the bench.

**2. Requester (A), then donor (B):**

```bash
# A
bash scripts/zmfield.sh requester --peer <B-ip>
#   prints:  bash scripts/zmfield.sh donor --cm <A-ip> --ip <B-ip> --join 'ZM-...'
# B  (paste that line)
bash scripts/zmfield.sh donor --cm <A-ip> --ip <B-ip> --join 'ZM-...'
```

`requester` waits up to 60 s for B to show up as ADMITTED in `zm status`. Without `--peer` the HTCondor
allow-list is A's whole /24 with CLAIMTOBE authentication: anyone on that subnet could submit jobs. Always pass
`--peer` on a network you do not control.

**3. Bench, on A:**

```bash
bash scripts/zmfield.sh bench                                   # w1,w2,w3; zm-local, zm, condor-default, condor-tuned
bash scripts/zmfield.sh bench --workloads w1 --cases 24 --secs 10
bash scripts/zmfield.sh bench --ssh me@<B-ip>                   # lets w3 kill/restore B automatically
```

Results go to `~/.zmfield/results/<UTC-timestamp>/` (`results.csv`, `report.md`, `hostinfo.txt`, `logs/`).
`report [DIR]` regenerates the markdown. `down` stops everything on a machine.

## What the bench runs

Same payload for every system: `sim.sh`, a pure-shell counted loop calibrated on the requester to about `--secs`
seconds. Default is remote-only: A only submits, B does the work (`zm run --remote-only`; HTCondor
`requirements` excludes A). `--all-nodes` lets A execute too. Each run gets a fresh copy of the input directory and a
unique nonce argument, so ZeroMesh's action cache cannot turn a later system's run into a no-op.

| workload | what it measures |
|---|---|
| w1 | CPU fan-out: `--cases` (24) tasks. Wall time vs the `zm-local` single-machine run. |
| w2 | Data moved once: 12 tasks over a `--mb` (128) MB random file. Requester NIC bytes. Then one small case file is changed and everything is re-run: does ZeroMesh reuse the unchanged tasks (per-task input roots are merged in v2-integration), and what does HTCondor re-send. |
| w3 | Churn: 40 tasks, donor killed at about 30% of the no-kill pool run (`kill`; add `--with-term` for SIGTERM / `systemctl stop`). Completion time, all outputs correct, `retry N/3` count from `zm run`. Forces all-nodes so the requester can finish the work. |

Bytes are rx+tx on the requester NIC towards the donor, so other traffic on that interface is included.
HTCondor "tuned" applies `scripts/baselines/condor_tuned.conf` on the requester only (plus `job_lease_duration = 60`);
the donor keeps its default settings.

Not measured: owner comfort, freeze-first departure on a real lid close / suspend / unplug (w3 only kills or stops
processes), the isolated effect of tail hedging, cold vs warm caches across repeated bench runs.

## HTCondor install tiers

`setup` tries, in order: the distro package (`htcondor` on apt, `condor` on dnf/zypper), HTCondor's official
installer (`get.htcondor.org --minicondor`, apt/dnf only), then a `podman`/`docker` container
(`docker.io/htcondor/mini:lts`, `--network host --uts host`). Arch has no official package, so Arch uses the container
tier. `--no-condor` skips all of it. It then writes `/etc/condor/config.d/99-zmfield.conf` (or mounts it into the
container) and restarts the service. `down` removes those drop-ins again.

## Verified vs not

Verified on the author's Arch laptop:
- `v2-integration` at `2341adc` builds (`ninja -j6`) and passes all 10 `meson test` suites with the README venv.
  That is one host only: no second machine, no HTCondor, no network was involved.
- `bash -n` is clean; `--help`, `doctor` and `--dry-run` for `setup`, `requester`, `donor`, `bench`, `selftest`,
  `down` and `chaos` print the expected plans on an Arch box. shellcheck was not installed, so it was not run.
- The pure-logic helpers were run against scratch directories: workdir staging, `sim.sh` output and calibration,
  HTCondor submit-file generation, rendered HTCondor config for both roles, and report generation from a fake CSV.

**Not verified:**
- That the build and tests pass on other distros or compilers (Ubuntu ships bubblewrap < 0.12.0).
- Package names and steps on anything but Arch (apt/dnf/zypper lists are from the README or best-effort); the
  bubblewrap-from-source path (`ZMF_BWRAP_TAG`, default `v0.12.0`, may not exist; meson option names are from memory).
- Every HTCondor config knob, the CLAIMTOBE auth setup, config.d ordering on each distro's package, and the
  `get.htcondor.org` flags. `requester` prints the effective `CONDOR_HOST`/`DAEMON_LIST` so a mismatch is visible;
  if the pool does not form, read `/var/log/condor/*`.
- The container tier, especially two hosts in `--network host` containers on the unverified `htcondor/mini` image.
- Anything that needs two machines: team join, discovery, the actual bench numbers, `--ssh` chaos (condor kills need
  passwordless sudo on the donor), manual chaos mode (the script asks you to run a command and press ENTER).
- Firewall opening beyond ufw/firewalld/iptables basics; nftables-only setups are not handled.
