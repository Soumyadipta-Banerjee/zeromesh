# SDD ledger — plan: docs/superpowers/plans/2026-10-26-core-plugin-part1-m0-m3.md
Spec: docs/superpowers/specs/2026-10-26-core-plugin-architecture-design.md
Setup: Ruling: work on branch restructure/core-plugin in the main checkout (no separate worktree) — repo is small, user wants progress fast, branch isolates from main — cost if wrong: none beyond a dirty checkout if interrupted.
Pre-flight:
- T4 SandboxEnv → T7 RunSpec.sandbox_env: match.
- T6 store::Digest, MerkleBuilder::Overlay/BuildTree(rules) → T9, T11: match.
- T7 SandboxRunner::Run → T9 LocalExecutor: match.
- T8 Telemetry + Peer fields (arch,node_port,cores_free,mem_available) → T13 AnnounceFields, T16 Candidate: match.
- T9 Executor/TaskCallbacks → T16 Placer: match.
- T3 Policy defaults ↔ T5 Normalize: proto3 cannot tell explicit false from unset. Ruling: declare Policy scalars with proto3 `optional`; Normalize fills only absent fields — honours explicit may_run_local=false / max_retries=0 — cost if wrong: small proto edit.
- T3 v1::Digest ↔ T6 store::Digest: no converter specified. Ruling: add core/node/convert.hpp (ToProto/FromProto) in T9 — cost if wrong: none.
- T14 tests need NodeService (T15). Ruling: T14 creates NodeService with Admit/AdmitConfirm/Ping + pin gate; T15 extends the same class — as plan text implies.
- T11 Team* UNIMPLEMENTED → T17 implements: match.
Task 1: Ruling: frozen tests live in frozen/tests/ instead of beside each module — one place, same effect — cost if wrong: a later git mv.
Task 1: Ruling: CI disables apparmor_restrict_unprivileged_userns — Ubuntu 24.04 runners block bwrap otherwise — cost if wrong: CI step is a no-op.
Task 1: complete (commits f182634..298f196, tests: meson test -C build --print-errorlogs → Full log written to /home/bb/Projects/zeromesh/build/meson-logs/testlog.txt)
Task 2: Ruling: M0 left unticked ("tick once CI passes") — plan says tick after CI, and CI can't run without a push the user hasn't approved — cost if wrong: one checkbox.
Task 2: Ruling: stale README.html moved to frozen/docs/README-old.html — it repeated the false claims — cost if wrong: a git mv back.
Task 2: complete (commits 298f196..6095309, tests: meson test -C build --print-errorlogs → Full log written to /home/bb/Projects/zeromesh/build/meson-logs/testlog.txt)
Task 3: Ruling: all unit tests moved to tests/unit/ now (plan did it piecemeal in T6) — one mechanical move — cost if wrong: none.
Task 3: Ruling: Chunk carries Digest on the first chunk of each object — receiver must verify each object — cost if wrong: one field.
Task 3: Ruling: OutputEntry gains is_symlink/symlink_target, TaskResult gains cancelled — Task 7 needs symlink outputs and cancellation in results — cost if wrong: unused fields.
Task 3: Ruling: Python stubs generated into build/proto/ (not build/py/) — Meson custom_target outputs must live in the defining subdir's build dir — cost if wrong: a PYTHONPATH string.
Task 3: complete (commits 6095309..c33f99d, tests: meson test -C build --print-errorlogs → Full log written to /home/bb/Projects/zeromesh/build/meson-logs/testlog.txt)
Task 4: Ruling: de-flaked test_mdns (fixed 300 ms sleep → 5 s poll with re-announce) — flaked 1/7 runs; Task 13 rewrites it anyway — cost if wrong: none.
Task 4: complete (commits c33f99d..3edb5c2, tests: meson test -C build --print-errorlogs → Full log written to /home/bb/Projects/zeromesh/build/meson-logs/testlog.txt)
Task 5: Ruling: Normalize also defaults env_spec.name to "host" — spec §7.2 makes host the default env; a task with no env would otherwise be unplaceable — cost if wrong: one line.
Task 5: complete (commits 3edb5c2..9606b51, tests: meson test -C build --print-errorlogs → Full log written to /home/bb/Projects/zeromesh/build/meson-logs/testlog.txt)
Interruption: user rebased restructure/core-plugin onto origin/main (a53b02b) and committed the in-progress Task 6 move as e8be2fe; ledger hashes for Tasks 1-5 predate the rebase (new: 8e4aecd, 3321e57, 1d718bc, e57dd54, ba2f7ed). Work continues in worktree ~/Projects/zeromesh-restructure; Task 6 tests restored from stash@{0} (stash kept).
Task 6: Ruling: CasStore validates hashes (64 lowercase hex) on every lookup — hashes arrive from peers in M3 and were used as raw path components — cost if wrong: none.
Task 6: Ruling: BuildTree throws on names containing newline/tab — the tab/newline manifest format can't represent them — cost if wrong: such files can't be uploaded (rare).
Task 6: complete (commits ba2f7ed..4cdba98, tests: meson test -C build --print-errorlogs → Full log written to /home/bb/Projects/zeromesh-restructure/build/meson-logs/testlog.txt)
Task 7: Ruling: user directed 'don't test too much, first finish' — from here, write code in larger batches with a reduced set of key tests per task (integration acceptance tests kept), instead of RED/GREEN per test case — cost if wrong: less unit coverage, caught later by the final review.
Task 7: Ruling: bwrap gets --clearenv (+HOME=/tmp) and --new-session — otherwise the daemon's environment (SSH_AUTH_SOCK, tokens) leaks into tasks and a task could inject keystrokes into a terminal — cost if wrong: tasks needing env vars must declare them.
Task 7: Ruling: bwrap exit 1 with "bwrap: execvp" mapped to 127 — keeps the shell convention the plan's test pins — cost if wrong: one string match.
Task 7: Ruling: capture_changed compares size+mtime_ns only (no digest) — cheap, sufficient for "new or modified" — cost if wrong: a rewrite with identical size+mtime is missed.
Task 7: complete (commits 4cdba98..fa9e39c, tests: meson test -C build --print-errorlogs → Full log written to /home/bb/Projects/zeromesh-restructure/build/meson-logs/testlog.txt)
Task 8: complete via subagent (rc/t8 5c8da42, merged); tests 52/52. Rulings: announce adds arch,node_port,cores_free,mem_available (old 9-field accepted); malformed packets dropped (stoi used to crash listener); NodeState throws on corrupt node_id rather than silently re-identifying.
Task 10: complete via subagent (rc/t10 e233a79, merged). Ruling: absolute argv[0] currently rejected by env-host Check — misleading; to fix: accept absolute paths that are executables under the mounted host dirs — cost if wrong: users must use bare command names.
Task 12: complete via subagent (rc/t10 1c2858f, merged); tests 60/60 after merge of t10 alone. Ruling: Crockford check symbol may be * ~ $ = (shell-special) — format kept per spec; CLI help must say to quote the phrase (Task 17).
Task 9: complete via subagent (rc/t9 71b3533, merged; conflict in meson.build resolved); tests 71/71. Rulings: inputs_from.path = destination dir (outputs land at <path>/<output.path>); inputs_from implies dependency; unknown env fails at LocalExecutor not Submit; overlaid single-file outputs lose exec bit (Task 6 Overlay) — minor.
Task 13: complete via subagent (rc/t13 77b44ab, merged). Rulings: multicast_port→discovery_port; SetTeam clears PeerTable; drain is not sticky (next announce restores active) — Part 2 should add sticky drain; memory_bytes not in wire format (TUI RAM 0 for peers) — minor.
Task 14: complete via subagent (rc/t14 9b10296, merged). Rulings: Identity carries node_id; TLS1.3 via TlsServerCredentials + NoOp verifier (pin gate is the authZ); node ids restricted to [A-Za-z0-9._-]{1,64}; pending admissions capped at 256, one MAC guess per Admit.
Task 11: complete via subagent (rc/t11 9693e1c, merged); unit + integration_local 10/10. src/ removed; M1, M2 ticked. Rulings: FetchOutputs refuses symlinked parents; cache-hit runs replay logs from CAS; Ctrl-C cancels (exit 130).
Tasks 15+16: complete via subagent (rc/t15 5f48a52, cdcf785, merged); tests 93/93. Rulings: Cancel(task_id); blob timeouts 10s+1s/MiB; reservations keyed by (authenticated peer, task); Drain marks authenticated caller only; RunTask log backpressure at 4 MiB; keepalive-timeout path untested (clean-shutdown path tested).
