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

## TODO: where to pick up (plan Task 17)

All of this goes in `core/node/daemon.{hpp,cpp}`, `core/node/client_service.*` and `cli/`:

1. **Make long-lived pieces `Daemon` members:** PeerTable, TelemetrySampler, `net::Identity::LoadOrCreate(state_dir, node_id)`, `net::PinStore(state_dir)`, `net::Discovery`, the team key (`std::optional<net::TeamKey>` loaded with `net::TeamFile::Load`, behind a mutex).
2. **Start the NodeApi server** on `0.0.0.0:<node_port>` with `net::ServerCreds(identity)`. **You must call `net::AllowKeepalivePings(builder)`**, or the server rejects clients' keepalive pings. Construct it with the full ctor:
   `NodeService(node_id, pins, team_fn, identity.fingerprint, self_info_fn, cas, registry, local_executor, telemetry_fn, DonorPolicy::Load(config_dir), peer_table)`.
3. **Swap the JobManager executor** from `LocalExecutor` to
   `sched::Placer(local_executor, peer_table, client_factory, cas, merkle, node_id, telemetry_fn)`.
   `client_factory` returns a pinned `std::shared_ptr<node::NodeClient>` for an admitted peer, or nullptr.
4. **Add an admission loop:** a thread that every ~2 s takes active same-team peers that aren't pinned yet (`PinStore::FingerprintFor(id)` empty) and runs `NodeClient::Admit(team)`, with a 30 s backoff on failure. Keep admitted clients in a map, `node_id → shared_ptr<NodeClient>`.
5. **Team RPCs** in `ClientService` (they return UNIMPLEMENTED today):
   - TeamCreate: generate, save, `Discovery::SetTeam`, return the phrase.
   - TeamJoin: `ParsePhrase`, returning INVALID_ARGUMENT "invalid team phrase" on failure.
   - TeamLeave: `SetTeam(nullopt)`, `TeamFile::Remove`, `PinStore::Clear`.
   - TeamInfo.
6. **CLI:** `zm team create|join '<phrase>'|leave|info`. Print the phrase **single-quoted**, because its check symbol can be `* ~ $ =`. `zm status` should show peers and whether they're admitted.
7. **On SIGTERM:** stop discovery, `NodeClient::Drain` to peers with tasks in flight, then shut down both servers.
8. **Integration test** `tests/integration/test_remote_exec.py`, registered as `integration_remote` with `is_parallel: false`. It runs 3 `zmd` on one machine (separate `ZM_STATE_DIR`/`ZM_RUNTIME_DIR`, random `ZM_NODE_PORT`s, one shared random `ZM_DISCOVERY_PORT`). A creates a team, B joins it, C uses its own team. Tests:
   - peers discovered and admitted (B yes, C no)
   - `zm run --remote-only -- sh -c 'printf %s "$ZM_NODE_ID" > out.txt'` gives B's id
   - remote logs stream back
   - a missing command gives "no node can run this task"
   - a C identity calling B gets PERMISSION_DENIED
   - a bad phrase gives "invalid team phrase"
9. **Write** `tests/acceptance/m3_two_machines.md` (two real Linux machines; firewall 8981/tcp and 8988/udp; on Ubuntu set `kernel.apparmor_restrict_unprivileged_userns=0` for bwrap). Tick M3 in PLAN.md **only** after a person runs it on two machines.

After Task 17, do the final whole-branch review, then push and open a PR.

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
