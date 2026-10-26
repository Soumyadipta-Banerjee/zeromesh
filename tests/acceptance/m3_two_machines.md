# M3 acceptance: two real Linux machines

Automated coverage lives in `tests/integration/test_remote_exec.py` (three `zmd` on one
host). This checklist is what a person runs on **two separate machines** before ticking
M3 in `PLAN.md`.

## Prerequisites

- Two Linux machines on the same LAN (A = requester, B = donor).
- Build from `restructure/core-plugin` (or `main` once M3 has merged): `make build`.
- Open the firewall on both: **8981/tcp** (NodeApi) and **8988/udp** (discovery multicast).
- On Ubuntu, if `bwrap` fails with user-namespace errors:
  `sudo sysctl kernel.apparmor_restrict_unprivileged_userns=0`

## Steps

1. On **both** machines, start the daemon in separate terminals:

       zmd

2. On **A**, create a team and copy the printed phrase (keep the single quotes when
   joining; the check symbol can be `* ~ $ =`):

       zm team create
       # → 'ZM-XXXX-XXXX-XXXX-XXXX-XXXX-XXXX-XXC'

3. On **B**, join with A's phrase:

       zm team join 'ZM-XXXX-XXXX-XXXX-XXXX-XXXX-XXXX-XXC'

4. Within ~15 s, on A:

       zm status

   must list B (first 8 chars of B's node id) with `ADMITTED` = `yes`. A third machine
   that created its own team must **not** appear.

5. On A, run the remote-only job (from an empty working directory):

       zm run --remote-only -o out.txt -- sh -c 'hostname > out.txt'
       cat out.txt

   `out.txt` must contain **B's** hostname, not A's.

6. Optional negative checks (same as the automated suite):
   - `zm run --remote-only -- definitely-not-a-binary-xyz` → stderr contains
     `no node can run this task`
   - `zm team join ZM-0000` → stderr contains `invalid team phrase`

## Pass criteria

- Step 5 produces B's hostname.
- A non-team machine never shows up in `zm status` on A or B.
- Only then mark **M3** `[x]` in `PLAN.md`.
