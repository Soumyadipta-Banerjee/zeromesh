#!/usr/bin/env bash
# Manages ZeroMesh on one test VM over SSH (user with passwordless sudo, key in $ZM_VM_KEY).
#
#   tests/vm/node.sh HOST setup        install rsync, strace, python venv (once)
#   tests/vm/node.sh HOST deploy [STAGE]  copy the stage (tests/vm/stage.sh) to ~/zeromesh-stage, set up
#                                      the SDK and apps in ~/venv, restart zmd with a fresh state
#   tests/vm/node.sh HOST shape wifi|bad|off  what the VM sends goes through tc netem: "wifi" ~5ms +-3ms,
#                                      0.3% loss, 60 Mbit/s; "bad" 40ms +-20ms, 3% loss, 8 Mbit/s
#   tests/vm/node.sh HOST update [STAGE]  the same, keeping zmd's state (team, store, cache)
#   tests/vm/node.sh HOST start|stop|log|wipe
#   tests/vm/node.sh HOST zm ARGS...   run zm there
#   tests/vm/node.sh HOST sh 'CMD'     run a shell command there, with the stage on PATH
#   ZM_VM_PROFILE=bench-old           separate state/config/runtime when comparing architectures
set -euo pipefail
host=$1; cmd=$2; shift 2
key=${ZM_VM_KEY:-$HOME/Desktop/zm-qemu/id_zm}
user=${ZM_VM_USER:-zm}
profile=${ZM_VM_PROFILE:-}
[[ $profile =~ ^[a-zA-Z0-9_-]*$ ]] || { echo "node.sh: invalid ZM_VM_PROFILE" >&2; exit 2; }
node_home=".zm${profile:+-$profile}"
ssh_opts=(-i "$key" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5)
remote() { ssh "${ssh_opts[@]}" "$user@$host" "$@"; }
env_line='source ~/zeromesh-stage/env.sh'
env_line+="; export ZM_STATE_DIR=\"\$HOME/$node_home/state\" ZM_RUNTIME_DIR=\"\$HOME/$node_home/run\" ZM_CONFIG_DIR=\"\$HOME/$node_home/config\""

case $cmd in
setup)
  remote 'sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq rsync strace python3-venv >/dev/null && python3 -m venv ~/venv && ~/venv/bin/pip install -q setuptools wheel "grpcio>=1.84" "protobuf>=4.21" pytest'
  # zmd started over SSH moves itself into a systemd user scope; lingering keeps that scope alive
  # after the SSH session ends.
  remote 'sudo loginctl enable-linger "$USER"'
  ;;
deploy|update)
  stage=${1:-$(cd "$(dirname "$0")/../.." && pwd)/build/vm-stage}
  remote 'pkill -x zmd || true'
  rsync -a --delete -e "ssh ${ssh_opts[*]}" "$stage/" "$user@$host:zeromesh-stage/"
  remote 'cat > ~/zeromesh-stage/env.sh <<"E"
export PATH="$HOME/zeromesh-stage/bin:$HOME/venv/bin:$PATH"
export LD_LIBRARY_PATH="$HOME/zeromesh-stage/lib"
export ZM_STATE_DIR="$HOME/.zm/state" ZM_RUNTIME_DIR="$HOME/.zm/run" ZM_CONFIG_DIR="$HOME/.zm/config"
E
~/venv/bin/pip install -q --no-deps --no-build-isolation -e ~/zeromesh-stage/apps/sdk-python \
  -e ~/zeromesh-stage/apps/zm-render -e ~/zeromesh-stage/apps/zm-cc -e ~/zeromesh-stage/apps/zm-cfd 2>&1 | grep -v "^$" || true'
  [[ $cmd == deploy ]] && "$0" "$host" wipe
  "$0" "$host" start
  ;;
start)
  remote "$env_line; zm status >/dev/null 2>&1 && exit 0;
          mkdir -p ~/$node_home/config || exit 1;
          [ -f ~/$node_home/config/donor.toml ] || echo 'max_step = 0' > ~/$node_home/config/donor.toml;
          setsid nohup zmd > ~/$node_home/zmd.log 2>&1 < /dev/null &
          for i in \$(seq 50); do [ -S ~/$node_home/run/zmd.sock ] && zm status >/dev/null 2>&1 && exit 0; sleep 0.1; done; tail -5 ~/$node_home/zmd.log; exit 1"
  ;;
stop)  remote 'pkill -x zmd; for i in $(seq 50); do pgrep -x zmd >/dev/null || exit 0; sleep 0.2; done; pkill -9 -x zmd' ;;
shape)
  case ${1:-} in
  wifi) spec='delay 5ms 3ms distribution normal loss 0.3% rate 60mbit' ;;
  bad)  spec='delay 40ms 20ms distribution normal loss 3% rate 8mbit' ;;
  off)  spec='' ;;
  *) echo "node.sh: shape wifi|bad|off" >&2; exit 2 ;;
  esac
  remote "dev=\$(ip -o route show default | awk '{print \$5}'); sudo tc qdisc del dev \$dev root 2>/dev/null;
          [ -z '$spec' ] || sudo tc qdisc add dev \$dev root netem $spec; tc qdisc show dev \$dev | head -1"
  ;;
wipe)  remote "pkill -x zmd || true; rm -rf ~/$node_home/state ~/$node_home/run ~/$node_home/zmd.log" ;;
log)   remote "cat ~/$node_home/zmd.log" ;;
zm)    remote "$env_line; zm $(printf '%q ' "$@")" ;;
sh)    remote "$env_line; $1" ;;
*)     echo "node.sh: unknown command $cmd" >&2; exit 2 ;;
esac
