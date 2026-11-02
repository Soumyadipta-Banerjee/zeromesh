#!/bin/sh
# Runs a test alone in a fresh cgroup. A CI container has no systemd to ask for a delegated scope,
# so its job sets up the cgroup tree (.github/workflows/ci.yml, arch-linux) and each test starts
# here, where zmd's containment finds the memory, pids and cpu controllers and no other process.
cg=/sys/fs/cgroup/zm-test-$$
mkdir "$cg" && echo $$ > "$cg/cgroup.procs" || exit 1
exec "$@"
