#!/usr/bin/env bash
# Builds a self-contained bundle of this checkout for test VMs running the same distribution
# (Ubuntu 24.04): zmd, zm, a bubblewrap new enough to run tasks, the shared libraries zmd links
# beyond the base system, the Python SDK and the apps.
#
#   tests/vm/stage.sh [BUILD_DIR] [OUT_DIR]      default: build, build/vm-stage
#   BWRAP=/path/to/bwrap                         default: the first bwrap >= 0.12 found
set -euo pipefail
here=$(cd "$(dirname "$0")/../.." && pwd)
build=$(cd "${1:-$here/build}" && pwd)
out=${2:-$build/vm-stage}

rm -rf "$out"
mkdir -p "$out/bin" "$out/lib" "$out/apps"
cp "$build/zmd" "$build/zm" "$out/bin/"

bwrap=${BWRAP:-}
if [[ -z $bwrap ]]; then
  for c in "$(command -v bwrap || true)"; do
    if [[ -x $c ]] && "$c" --version | awk '{split($2,v,"."); exit !(v[1]>0 || v[2]>=12)}'; then bwrap=$c; break; fi
  done
fi
[[ -n $bwrap ]] || { echo "stage: no bubblewrap >= 0.12 (set BWRAP=)" >&2; exit 1; }
cp "$bwrap" "$out/bin/bwrap"

# Everything zmd and zm link except the C runtime every system has.
base='^(linux-vdso|libc\.so|libm\.so|libstdc\+\+|libgcc_s|ld-linux|libpthread|libdl\.so|librt\.so)'
for bin in "$build/zmd" "$build/zm"; do
  ldd "$bin" | awk '/=> \//{print $1, $3}' | while read -r name path; do
    [[ $name =~ $base ]] || cp -L --update=none "$path" "$out/lib/$name"
  done
done

cp -r "$here/sdk/python" "$out/apps/sdk-python"
for app in zm-render zm-cc zm-cfd; do cp -r "$here/apps/$app" "$out/apps/$app"; done
find "$out/apps" -name __pycache__ -o -name '*.egg-info' -o -name .pytest_cache | xargs rm -rf

echo "staged $(ls "$out/lib" | wc -l) libraries and bwrap $("$out/bin/bwrap" --version | awk '{print $2}') in $out"
