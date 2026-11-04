# ZeroMesh Acceptance Runs

This directory contains human-executed acceptance runbooks for ZeroMesh milestones. While automated unit and integration tests run in CI, milestones are ticked in `PLAN.md` **only** after manual acceptance passes on real physical or virtual machines.

## Role of the Ops Stream

1. **Scheduling & Execution:** Ops coordinates manual runs with stream authors and independent reviewers.
2. **Recording:** Results, hostnames, timestamps, hardware specifications, and outputs are committed directly to the corresponding runbook (`tests/acceptance/m*_*.md`).
3. **Milestone Verification:** Once acceptance passes and evidence is recorded, the milestone is ticked `[x]` in `PLAN.md`.

## Active Runbooks

| Milestone | Runbook | Scope | Status |
|---|---|---|---|
| **M3** | [`m3_two_machines.md`](m3_two_machines.md) | 2 real Linux nodes: mTLS pairing, LAN discovery, remote task run | Pending verification |
| **M4** | `m4_three_machines.md` | 3 nodes: `--each` fan-out, retries upon donor SIGKILL | Designed (with M4) |
| **M5** | `m5_render.md` | 4 nodes: distributed Blender 240-frame render with mirror mode | Designed (with M5) |

## Recording Template

When running a checklist, append a **Results** section to the runbook in the following format:

```markdown
## Acceptance Run: YYYY-MM-DD

- **Tester:** Name (@github-handle)
- **Node A (Requester):** `hostname-a` (Ubuntu 24.04, 8 cores, 16 GB RAM)
- **Node B (Donor):** `hostname-b` (Arch Linux, 16 cores, 32 GB RAM)
- **ZeroMesh Commit:** `<sha>`
- **Steps Verified:**
  - [x] Team phrase generated and joined.
  - [x] Peer discovered and admitted within 15 seconds.
  - [x] Output of `zm run --remote-only`:
    ```
    <paste out.txt content showing Node B hostname>
    ```
  - [x] Negative checks (non-team node rejection, invalid phrase rejection).
- **Result:** PASS / FAIL
```
