# Source before building/running the demo worktree.
# Usage: source scripts/demo_env.sh
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS="${ZM_DEPS:-$ROOT/../zeromesh/.deps}"
if [ ! -d "$DEPS/usr" ]; then
  DEPS="$ROOT/.deps"
fi

MULTIARCH=""
if command -v dpkg-architecture >/dev/null 2>&1; then
  MULTIARCH="$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || true)"
fi
if [ -z "$MULTIARCH" ] && command -v gcc >/dev/null 2>&1; then
  MULTIARCH="$(gcc -dumpmachine 2>/dev/null || true)"
fi
if [ -z "$MULTIARCH" ]; then
  MULTIARCH="x86_64-linux-gnu"
fi

_deps_lib="$DEPS/usr/lib/$MULTIARCH"
_sys_lib="/usr/lib/$MULTIARCH"

if [ -d "$_deps_lib/pkgconfig" ]; then
  export PKG_CONFIG_PATH="$_deps_lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi
if [ -d "$_deps_lib" ]; then
  export LIBRARY_PATH="$_deps_lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
fi
_ld_parts=()
if [ -d "$_sys_lib" ]; then
  _ld_parts+=("$_sys_lib")
fi
if [ -d "$_deps_lib" ]; then
  _ld_parts+=("$_deps_lib")
fi
if [ "${#_ld_parts[@]}" -gt 0 ]; then
  IFS=:
  _ld_join="${_ld_parts[*]}"
  unset IFS
  export LD_LIBRARY_PATH="${_ld_join}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

_path_extra=""
if [ -d "$DEPS/usr/bin" ]; then
  _path_extra="$DEPS/usr/bin:"
fi
export PATH="/usr/bin:/bin:${_path_extra}$ROOT/build:$PATH"
export ZM_BUILD_DIR="$ROOT/build"
if [ -f "$ROOT/.venv/bin/activate" ]; then
  # shellcheck disable=SC1091
  source "$ROOT/.venv/bin/activate"
fi
echo "demo env: ROOT=$ROOT  ZM_BUILD_DIR=$ZM_BUILD_DIR"
