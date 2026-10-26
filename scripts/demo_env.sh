# Source before building/running the demo worktree.
# Usage: source scripts/demo_env.sh
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS="${ZM_DEPS:-$ROOT/../zeromesh/.deps}"
if [ ! -d "$DEPS/usr" ]; then
  DEPS="$ROOT/.deps"
fi
export PKG_CONFIG_PATH="$DEPS/usr/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LIBRARY_PATH="$DEPS/usr/lib/x86_64-linux-gnu${LIBRARY_PATH:+:$LIBRARY_PATH}"
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:$DEPS/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="/usr/bin:/bin:$DEPS/usr/bin:$ROOT/build:$PATH"
export ZM_BUILD_DIR="$ROOT/build"
if [ -f "$ROOT/.venv/bin/activate" ]; then
  # shellcheck disable=SC1091
  source "$ROOT/.venv/bin/activate"
fi
echo "demo env: ROOT=$ROOT  ZM_BUILD_DIR=$ZM_BUILD_DIR"