# M4 acceptance: three real Linux machines

Automated coverage lives in `tests/integration/test_fanout.py` (three `zmd` on one
host, SIGKILL of one donor mid `--each`). This checklist is what a person runs on
**three separate machines** before ticking M4 in `PLAN.md`.

## Prerequisites

- Three Linux machines on the same LAN (A = requester, B and C = donors).
- Build from `stream/m4-fanout` (or `main` once M4 has merged): `make build`.
- Open the firewall on all three: **8981/tcp** (NodeApi) and **8988/udp** (discovery).
- On Ubuntu, if `bwrap` fails with user-namespace errors:
  `sudo sysctl kernel.apparmor_restrict_unprivileged_userns=0`

## Steps

1. On **all three** machines, start the daemon:

       zmd

2. On **A**, create a team and copy the phrase:

       zm team create

3. On **B** and **C**, join:

       zm team join 'ZM-…'

4. Within ~15 s, `zm status` on A lists B and C with `ADMITTED` = `yes`.

5. On A, create 20 CPU-bound cases (~30 s each) and time a single-machine baseline:

       mkdir -p cases
       for i in $(seq -w 0 19); do printf 'case %s\n' "$i" > "cases/$i"; done
       # A simple CPU burner used as ./sim:
       cat > sim <<'EOF'
       #!/bin/sh
       # Busy-loop ~30 s of single-core work, then write a stamp.
       end=$(( $(date +%s) + 30 ))
       while [ "$(date +%s)" -lt "$end" ]; do :; done
       echo ok > "$1.out"
       EOF
       chmod +x sim

       # Single-machine baseline (local only), record wall time:
       /usr/bin/time -f '%e' zm run --local-only --each 'cases/*' -- ./sim {}

6. Fan-out across the team (record wall time and hostnames from `zm status`):

       /usr/bin/time -f '%e' zm run --each 'cases/*' -- ./sim {}

   Wall time must be **under 45%** of the single-machine baseline. All 20 `cases/*.out`
   files must exist and contain `ok`.

7. Kill one donor mid-job and confirm completion:

       # Start the job again (or a fresh 20-case set), then within a few seconds on B:
       kill -9 $(pidof zmd)

       # On A the job must still exit 0 with all 20 outputs correct. stderr should
       # mention at least one `retry 1/3`.

## Results (fill in)

| Field | Value |
|---|---|
| Date | |
| A hostname / node id | |
| B hostname / node id | |
| C hostname / node id | |
| Single-machine time (s) | |
| Three-machine time (s) | |
| Ratio (three / single) | |
| SIGKILL donor + all 20 ok | |
| Saw `retry 1/3` | |

## Pass criteria

- Step 6 finishes in under 45% of the step 5 baseline.
- Step 7 completes all 20 cases correctly after killing one donor's `zmd`.
- Only then mark **M4** `[x]` in `PLAN.md`.
