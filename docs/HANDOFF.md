# Handoff: core + plugin restructure (2026-10-26)

For the next person or agent continuing this work. Read this first, then the
[design](superpowers/specs/2026-10-26-core-plugin-architecture-design.md), the
[Part 1 plan](superpowers/plans/2026-10-26-core-plugin-part1-m0-m3.md) and
[team/TASKS.md](team/TASKS.md).

## Where things are

- **Branch:** `restructure/core-plugin`. All merged work is here. It is not pushed and not merged to `main`.
- **Build:** `make build && make test`, giving 93/93 unit tests and the `integration_local` suite (10 tests). Everything passes and the build has no warnings.
- **Unfinished work:** branch `rc/t17` has one WIP commit (`ae5c64c`, "DO NOT MERGE, unbuilt"), with partial Task 17 edits to `pin_store`, `client_service` and `common.proto`. Read it for ideas or discard it.

## Done (plan Tasks 1–16)

| Milestone | Status | Proof |
|---|---|---|
| M0 Honesty pass | Done locally; tick in PLAN.md once CI passes on GitHub | `frozen/README.md`, PLAN/README rewritten |
| M1 Contracts | ✅ | `proto/zeromesh/v1/*.proto`, `core/plugin/plugin.hpp`, Python 3-task job test |
| M2 Core + local daemon | ✅ | `zmd` + `zm run` end to end (`tests/integration/test_local_daemon.py`) |
| M3 Remote execution | **Building blocks done, daemon wiring not done** | see below |

M3 pieces that exist and are unit-tested, but that `zmd` doesn't use yet:
- `core/net/team.*`: team secret phrase, team id, HMAC (Task 12)
- `core/net/discovery.*`: team-scoped, HMAC-authenticated multicast discovery (Task 13)
- `core/net/{identity,pin_store,tls}.*` and `core/node/{admission,node_service,node_client}.*`: certificates, pinned mTLS, admission handshake (Task 14)
- `core/node/{donor_policy,blob_transfer}.*`, plus NodeApi Offer/Push/Pull/Run/Cancel/Drain (Task 15)
- `core/sched/{rank,placer}.*`: placement, local vs remote (Task 16). Tested in-process with two nodes in `tests/unit/test_placer.cpp`.

## Task 17 status (done locally)

Daemon wiring, Team RPCs, `zm team`, Placer, admission loop, `integration_remote`, and
`tests/acceptance/m3_two_machines.md` are in. Automated tests pass; **do not tick M3 in
PLAN.md** until someone runs the two-machine acceptance. Next: whole-branch review, then
push and open a PR (only when asked).

`core/net/tls.cpp` uses the gRPC 1.51 `StaticDataCertificateProvider` API (Ubuntu noble /
conda-forge). Newer gRPC renamed some of those types — re-check if you bump past 1.51.

## Known gaps and decisions to revisit (from the ledger)

- **Drain isn't sticky.** The next 2 s announcement marks a draining node active again. Part 2 (M4) should add sticky drain and resume.
- **The keepalive-timeout path isn't tested.** A peer that vanishes without a clean shutdown is only covered by configuration. The clean-shutdown path is tested.
- **The RAM column shows 0 for peers.** `mem_total` isn't in the discovery wire format.
- **Overlaid single-file outputs lose the exec bit.** `MerkleBuilder::Overlay` stores files with `is_executable=false`.
- **Retries and re-dispatch don't exist yet.** A lost peer fails the task. That's Part 2 (M4).
- **Contract gaps for later streams** (details in `team/TASKS.md`):
  - `plugin::TaskContext` has no requester id or blob access (needed for M5c env-mirror).
  - `ClientApiHandle` and front-end loading aren't wired (needed for M6).
- **No Landlock.** bwrap's mount layout is the write boundary; see ADR-012 in PLAN.md.

## Where the record lives

- **Decision ledger** (every ruling, with its cost if wrong): `.superpowers/sdd/2026-10-26-core-plugin-part1-m0-m3/progress.md` in this worktree. It's git-ignored scratch, so copy the rulings out if you delete the worktree.
- **Commit history** on `restructure/core-plugin`: one merge per task.
