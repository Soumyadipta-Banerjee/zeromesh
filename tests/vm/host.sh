#!/usr/bin/env bash
# This machine as a node of the VM test team, with its own state (never the user's real zmd).
#
#   tests/vm/host.sh start|stop|wipe|log      zmd from BUILD_DIR (default build), state in BUILD_DIR/vm-host
#   tests/vm/host.sh zm ARGS...               zm against it
#   tests/vm/host.sh env                      prints exports for using it from a shell (eval it)
set -euo pipefail
here=$(cd "$(dirname "$0")/../.." && pwd)
build=$(cd "${BUILD_DIR:-$here/build}" && pwd)
dir=$build/vm-host
export ZM_STATE_DIR=$dir/state ZM_RUNTIME_DIR=$dir/run ZM_CONFIG_DIR=$dir/config ZM_SOCKET=$dir/run/zmd.sock
cmd=${1:-}; shift || true
case $cmd in
start)
  mkdir -p "$ZM_CONFIG_DIR"
  [[ -f $ZM_CONFIG_DIR/donor.toml ]] || echo 'max_step = 0' > "$ZM_CONFIG_DIR/donor.toml"
  setsid nohup "$build/zmd" > "$dir/zmd.log" 2>&1 < /dev/null &
  for _ in $(seq 50); do [[ -S $ZM_SOCKET ]] && exit 0; sleep 0.1; done
  tail -5 "$dir/zmd.log"; exit 1 ;;
stop) pkill -f "^$build/zmd$" || true ;;
wipe) pkill -f "^$build/zmd$" || true; rm -rf "$dir/state" "$dir/run" "$dir/zmd.log" ;;
log)  cat "$dir/zmd.log" ;;
zm)   exec "$build/zm" "$@" ;;
env)  echo "export ZM_STATE_DIR=$ZM_STATE_DIR ZM_RUNTIME_DIR=$ZM_RUNTIME_DIR ZM_CONFIG_DIR=$ZM_CONFIG_DIR ZM_SOCKET=$ZM_SOCKET" ;;
*)    echo "host.sh: start|stop|wipe|log|zm|env" >&2; exit 2 ;;
esac
