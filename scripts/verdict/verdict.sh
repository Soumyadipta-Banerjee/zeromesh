#!/usr/bin/env bash
# verdict.sh: orchestration for the ZeroMesh-vs-HTCondor verdict suite. Sourced by scripts/zmfield.sh
# (`zmfield.sh verdict register|run|report`); it only defines functions, so it can also be sourced alone.
#
# Runs pre-registered randomised blocks and writes the results CSV that analyze.py consumes. Workloads are
# plugged in as `vd_workload_<wl>` functions (w1, w2 here); a workload without a function is skipped with a
# log line, so adding w3..w6 means adding functions, not restructuring the loop.
#
# ZMV_FAKE=1 swaps the real backend for a synthetic one (see vd_exec) so the whole flow runs without a pool.
# Everything that talks to a real pool (precheck, calibration, exec_system, ssh) is UNVERIFIED on hardware.

VD_DIR=${VD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
VD_PY=${VD_PY:-python3}
# w5 is last so a donor lost to its kills only endangers its own cell; w4 is part of w3's function.
VD_BLOCK_WORKLOADS="w1 w2 w3 w6 w5"
VD_BLOCK=0; VD_ORDER=0; VD_SSH=""
VD_X_TTFR=""; VD_X_REEXEC=""; VD_X_PROBE=""; VD_X_PROBE_IDLE=""; VD_X_TASKS=""
VD_ITERS_REF=""; VD_SECS_REF=""

vd_fake() { [ "${ZMV_FAKE:-0}" = 1 ]; }

# vd_nonce BLOCK WL SYS -> unique per block, workload and system
vd_nonce() { printf '%s-b%s-%s-%s\n' "$RUNID" "$1" "$2" "$3"; }

# vd_sleep SECS
vd_sleep() { vd_fake || sleep "$1"; }

# vd_csvq FIELD -> FIELD, double-quoted if it holds a comma, quote or newline
vd_csvq() {
    case $1 in
    *[,\"$'\n']*) printf '"%s"' "${1//\"/\"\"}" ;;
    *) printf '%s' "$1" ;;
    esac
}

# vd_row WL STEP SYS VARIANT WALL RX TX OK TOTAL RETRIES NOTE   (installed as VD_ROW_HOOK)
# Column order: block,order,workload,step,system,variant,wall_s,rx_mb,tx_mb,ok,total,retries,
#               ttfr_s,reexec_task_s,probe_p99_ms,probe_idle_p99_ms,tasks_done,notes
vd_row() {
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$VD_BLOCK" "$VD_ORDER" "$1" "$2" "$3" "$4" "$5" "$6" "$7" "$8" "$9" "${10}" \
        "$VD_X_TTFR" "$VD_X_REEXEC" "$VD_X_PROBE" "$VD_X_PROBE_IDLE" "$VD_X_TASKS" "$(vd_csvq "${11:-}")" >>"$CSV"
    VD_X_TTFR=""; VD_X_REEXEC=""; VD_X_PROBE=""; VD_X_PROBE_IDLE=""; VD_X_TASKS=""
}

VD_HEADER='block,order,workload,step,system,variant,wall_s,rx_mb,tx_mb,ok,total,retries,ttfr_s,reexec_task_s,probe_p99_ms,probe_idle_p99_ms,tasks_done,notes'

# cell_done BLOCK WL STEP SYS VARIANT -> status 0 if $CSV already holds that cell
cell_done() {
    grep -q -E "^$1,[^,]*,$2,$3,$4,$5," "$CSV" 2>/dev/null
}

# block_state BLOCK: block_excluded -> 0 if an exclusion row exists; block_has_data -> 0 if any data row exists
block_excluded() { grep -q -E "^$1,[^,]*,-,excluded," "$CSV" 2>/dev/null; }
block_has_data() { grep -q -E "^$1,[^,]*,[^,-][^,]*,[^,]*,[^,]+," "$CSV" 2>/dev/null; }

# vd_exec SYS WL STEP VARIANT STAGEDIR
vd_exec() {
    local sys=$1 wl=$2 stepn=$3 variant=$4 stage=$5
    if ! vd_fake; then
        exec_system "$sys" "$wl" "$stepn" "$variant" "$(vd_nonce "$VD_BLOCK" "$wl" "$sys")" "$stage"
        return
    fi
    local wv=${sys^^} tv=VD_${wl^^}_TASKS tasks
    wv=ZMV_FAKE_WALL_${wv//-/_}
    tasks=${!tv:-1}
    emit_row "$wl" "$stepn" "$sys" "$variant" "${!wv:-10}" 0 "${ZMV_FAKE_TX:-1.0}" "$tasks" "$tasks" 0 "fake=1"
}

# ---------------------------------------------------------------------------------------
# precheck
# ---------------------------------------------------------------------------------------
# Runs on a host (locally or over ssh) via `bash -s`; prints the failing condition and exits 1.
VD_HOST_CHECK='
if ls /sys/class/power_supply/BAT* >/dev/null 2>&1; then
    ok=0
    for f in /sys/class/power_supply/*/online; do
        [ -r "$f" ] && [ "$(cat "$f")" = 1 ] && ok=1
    done
    [ "$ok" = 1 ] || { echo ac_power; exit 1; }
fi
awk "{exit !(\$1 < 1.0)}" /proc/loadavg || { echo load; exit 1; }
'

VD_PRECHECK_REASON=""
vd_precheck_once() {
    local why
    VD_PRECHECK_REASON=""
    if vd_fake; then
        if [ "${ZMV_FAKE_PRECHECK_FAIL:-}" = "$VD_BLOCK" ]; then VD_PRECHECK_REASON=fake_precheck_failure; return 1; fi
        return 0
    fi
    if ! why=$(bash -s <<<"$VD_HOST_CHECK" 2>/dev/null); then VD_PRECHECK_REASON="requester_$why"; return 1; fi
    if ! why=$(ssh -o BatchMode=yes -o ConnectTimeout=10 "$VD_SSH" bash -s <<<"$VD_HOST_CHECK" 2>/dev/null); then
        VD_PRECHECK_REASON="donor_${why:-ssh_unreachable}"; return 1
    fi
    if [ "$(admitted_peers)" -lt 1 ]; then VD_PRECHECK_REASON=no_admitted_zm_peer; return 1; fi
    if [ "$(condor_machines | wc -l)" -lt 2 ]; then VD_PRECHECK_REASON=condor_pool_under_2_machines; return 1; fi
}

# precheck_block -> status 0 if block $VD_BLOCK may run. One retry after 60 s; on a second failure an
# exclusion row (workload "-") is written and the status is 1.
precheck_block() {
    vd_precheck_once && return 0
    warn "block $VD_BLOCK precheck failed ($VD_PRECHECK_REASON); retrying in 60 s"
    vd_sleep 60
    vd_precheck_once && return 0
    if block_has_data "$VD_BLOCK"; then
        warn "block $VD_BLOCK already has data and failed its precheck on resume ($VD_PRECHECK_REASON): skipping its remaining cells without an exclusion"
        return 1
    fi
    warn "block $VD_BLOCK excluded: $VD_PRECHECK_REASON"
    VD_ORDER=0
    emit_row - excluded "" "" "" "" "" "" "" "" "excluded=$VD_PRECHECK_REASON"
    return 1
}

# ---------------------------------------------------------------------------------------
# calibration and workloads
# ---------------------------------------------------------------------------------------
# vd_calibrate WL: one `calibrate` per session; other task_secs scale linearly.
# Sets ITERS for WL's VD_<WL>_TASK_SECS.
vd_calibrate() {
    local wl=$1 tv=VD_${1^^}_TASK_SECS
    if [ -z "$VD_ITERS_REF" ]; then
        if vd_fake; then ITERS=${ZMV_FAKE_ITERS:-1000000}; else calibrate; fi
        VD_ITERS_REF=$ITERS; VD_SECS_REF=$SECS
    fi
    ITERS=$(awk -v r="$VD_ITERS_REF" -v t="${!tv}" -v s="$VD_SECS_REF" 'BEGIN{printf "%d", r*t/s}')
}

# vd_systems_for_block -> the systems to run this block, in randomised order. zm-local only in the warm-up.
vd_systems_for_block() {
    local order
    order=$("$VD_PY" "$VD_DIR/config.py" order "$VD_RUN_SEED" "$VD_BLOCK" zm condor-default condor-tuned)
    if [ "$VD_BLOCK" -lt 0 ]; then echo "zm-local $order"; else echo "$order"; fi
}

# vd_stage_cpu WL -> prints the stage dir; stages WL's cpu cases once per session and sets STAGE_NAMES
vd_stage_cpu() {
    local wl=$1 tv=VD_${1^^}_TASKS stage=$RUNROOT/stage-$1
    STAGE_NAMES=$RUNROOT/names-$wl.txt
    if [ ! -d "$stage/cases" ]; then
        if vd_fake; then mkdir -p "$stage/cases"
        else stage_cpu "$stage" "${!tv}"; fi
        ls "$stage/cases" >"$STAGE_NAMES"
    fi
    echo "$stage"
}

# vd_simple_workload WL: the W1/W2 shape, a CPU fan-out of VD_<WL>_TASKS tasks (step main, variant none)
vd_simple_workload() {
    local wl=$1 sys stage i=0
    PAYLOAD_FORCE=cpu; CUR_ALL=0
    vd_calibrate "$wl"
    stage=$(vd_stage_cpu "$wl")
    for sys in $(vd_systems_for_block); do
        VD_ORDER=$i; i=$((i+1))
        if cell_done "$VD_BLOCK" "$wl" main "$sys" none; then continue; fi
        vd_exec "$sys" "$wl" main none "$stage"
        vd_sleep "$VD_RUN_SETTLE_S"
    done
}

vd_workload_w1() { vd_simple_workload w1; }
vd_workload_w2() { vd_simple_workload w2; }

vd_run_workload() {
    if declare -F "vd_workload_$1" >/dev/null; then "vd_workload_$1"
    else log "workload $1: not implemented in this build, skipped"; fi
}

vd_run_block() { # vd_run_block B
    VD_BLOCK=$1
    step "verdict block $VD_BLOCK"
    if block_excluded "$VD_BLOCK"; then log "block $VD_BLOCK was excluded earlier; skipped"; return 0; fi
    precheck_block || return 0
    local wl
    for wl in $VD_BLOCK_WORKLOADS; do vd_run_workload "$wl"; done
}

# ---------------------------------------------------------------------------------------
# run / register / report
# ---------------------------------------------------------------------------------------
vd_scope_write() { # vd_scope_write RES LAPTOPS DONORS NETWORK CONDOR_VERSION GIT_SHA EXTENSION_USED
    "$VD_PY" - "$@" <<'EOF'
import json, sys
res, laptops, donors, network, condor, sha, ext = sys.argv[1:8]
with open(res + "/scope.json", "w") as f:
    json.dump({"laptops": int(laptops), "donors": int(donors), "network": network, "condor_version": condor,
               "git_sha": sha, "extension_used": ext == "true"}, f, indent=2)
    f.write("\n")
EOF
}

vd_scope_mark_extended() { # vd_scope_mark_extended RES
    "$VD_PY" - "$1/scope.json" <<'EOF'
import json, sys
p = sys.argv[1]
d = json.load(open(p))
d["extension_used"] = True
with open(p, "w") as f:
    json.dump(d, f, indent=2)
    f.write("\n")
EOF
}

# vd_analyze RES ROOT ANALYZER_DIR [--smoke] -> runs ANALYZER_DIR/analyze.py (its siblings import from the same
# dir); its stdout (may hold NEEDS_EXTENSION) is passed through. A fake-backend result dir is stamped afterwards.
vd_analyze() {
    local res=$1 root=$2 adir=$3 rc=0; shift 3
    [ -f "$adir/analyze.py" ] || die "no analyzer at $adir/analyze.py"
    "$VD_PY" "$adir/analyze.py" "$res/results.csv" --config "$res/verdict.toml" \
        --registration "$res/registration.json" --root "$root" --out "$res" "$@" || rc=$?
    vd_stamp_fake "$res"
    return $rc
}

# vd_stamp_fake RES: results from the fake backend must never read as a measurement
vd_stamp_fake() {
    [ -f "$1/fake" ] && [ -f "$1/report.md" ] || return 0
    { echo "FAKE BACKEND — synthetic data, not a measurement"; cat "$1/report.md"; } >"$1/report.md.tmp"
    mv "$1/report.md.tmp" "$1/report.md"
}

# vd_reg_hash FILE -> the registration hash, empty if unreadable
vd_reg_hash() {
    "$VD_PY" -c 'import json,sys; print(json.load(open(sys.argv[1]))["hash"])' "$1" 2>/dev/null || true
}

vd_real_setup() { # NIC, pool and calibration state, as cmd_bench does
    load_env
    [ -x "$BUILD/zm" ] || die "not built: run setup first"
    zmd_alive || die "zmd is not running: run 'requester' first"
    [ "$(admitted_peers)" -ge 1 ] || die "no ADMITTED peer in 'zm status'; get the donor joined first"
    local target=$VD_PEER
    if [ -z "$target" ] && [ -f "$ZMF_HOME/state/peer_ip" ]; then target=$(cat "$ZMF_HOME/state/peer_ip"); fi
    if [ -z "$target" ]; then target=$(ZM status 2>/dev/null | grep -o -E '\(([0-9]+\.){3}[0-9]+:[0-9]+\)' | head -1 | tr -d '()' | cut -d: -f1 || true); fi
    if [ -n "$target" ]; then IFACE=$(iface_for "$target"); fi
    if [ -z "$IFACE" ]; then
        IFACE=$(iface_for 1.1.1.1)
        warn "could not work out the NIC towards the donor (pass --peer IP); using $IFACE"
    fi
    CONDOR_OK=0
    if [ "$(condor_tier)" != none ] && [ "$(condor_machines | wc -l)" -ge 2 ]; then CONDOR_OK=1
    else warn "HTCondor pool has fewer than 2 machines: condor runs will fail their precheck"; fi
    SCOPE=donor; LOCAL_ONLY=0; SSH_TARGET=$VD_SSH
}

cmd_verdict_run() {
    local smoke=0 resume="" network=unknown
    VD_SSH=""; VD_PEER=""
    while [ $# -gt 0 ]; do
        case "$1" in
        --ssh) VD_SSH=${2:?}; shift ;;
        --peer) VD_PEER=${2:?}; shift ;;
        --smoke) smoke=1 ;;
        --resume) resume=${2:?}; shift ;;
        --network) network=${2:?}; shift ;;
        *) echo "zmfield: verdict run: unknown option $1" >&2; exit 2 ;;
        esac
        shift
    done
    if [ -z "$VD_SSH" ]; then echo "zmfield: verdict needs --ssh for kills and the owner probe" >&2; exit 2; fi
    local reg=$ROOT/scripts/verdict/registration.json
    if [ "$smoke" = 0 ] && ! "$VD_PY" "$VD_DIR/register.py" check "$ROOT" "$reg" >/dev/null 2>&1; then
        echo "zmfield: config changed since registration (or never registered): run 'verdict register' and commit it" >&2
        exit 2
    fi
    local cfg=$VD_DIR/verdict.toml
    if [ -n "$resume" ]; then
        [ -f "$resume/results.csv" ] || die "--resume: no results.csv in $resume"
        RES=${resume%/}
        # the dir must be the run this registration covers, and must be the same kind of run
        if [ "$smoke" = 1 ] && [ ! -f "$RES/smoke" ]; then
            echo "zmfield: config changed since registration: $RES is not a smoke run, refusing --smoke --resume" >&2; exit 2
        fi
        if [ "$smoke" = 0 ]; then
            local dh lh
            dh=$(vd_reg_hash "$RES/registration.json"); lh=$(vd_reg_hash "$reg")
            if [ -f "$RES/smoke" ] || [ -z "$dh" ] || [ "$dh" != "$lh" ]; then
                echo "zmfield: config changed since registration: $RES was collected under a different (or smoke) registration" >&2; exit 2
            fi
        fi
        cfg=$RES/verdict.toml
        [ -f "$cfg" ] || die "--resume: no verdict.toml in $RES"
    fi
    eval "$("$VD_PY" "$VD_DIR/config.py" shell "$cfg")"
    if [ "$smoke" = 1 ]; then VD_RUN_BLOCKS=3; VD_RUN_EXTEND_TO=0; fi

    if [ -z "$resume" ]; then
        RES=$ZMF_HOME/results/verdict-$(date -u +%Y%m%dT%H%M%SZ)
    fi
    RUNID=$(basename "$RES"); RUNROOT=$ZMF_HOME/work/$RUNID; CSV=$RES/results.csv
    mkdir -p "$RES/logs" "$RUNROOT"
    log "verdict results -> $RES"

    local donors=1
    if ! vd_fake; then vd_real_setup; donors=$(admitted_peers); fi
    if [ -z "$resume" ]; then
        local f
        if [ -f "$reg" ]; then cp "$reg" "$RES/registration.json"
        else "$VD_PY" "$VD_DIR/register.py" write "$ROOT" "$RES/registration.json"; fi   # smoke only: archive the tree as-is
        cp "$VD_DIR/verdict.toml" "$RES/verdict.toml"
        cp "$ROOT/$VD_CONDOR_TUNED_CONF" "$RES/condor_tuned.conf" 2>/dev/null || warn "condor_tuned.conf not copied"
        while read -r f; do
            [ -f "$ROOT/$f" ] || continue
            mkdir -p "$RES/registered_tree/$(dirname "$f")"; cp "$ROOT/$f" "$RES/registered_tree/$f"
        done < <("$VD_PY" -c 'import sys; sys.path.insert(0, sys.argv[1]); import register; print("\n".join(register.HASHED))' "$VD_DIR"
                echo scripts/verdict/register.py)   # analyze.py imports it; not in register.HASHED, archived so the archive runs standalone
        [ "$smoke" = 1 ] && : >"$RES/smoke"
        if vd_fake && [ "$smoke" = 0 ]; then : >"$RES/fake"; fi
        local cv=fake sha
        vd_fake || cv=$(condor_cmd condor_version 2>/dev/null | head -1 || echo ?)
        sha=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo ?)
        vd_scope_write "$RES" "$((donors+1))" "$donors" "$network" "$cv" "$sha" false
        echo "$VD_HEADER" >"$CSV"
        vd_fake || write_hostinfo
    fi
    VD_ROW_HOOK=vd_row

    local b out args=()
    [ "$smoke" = 1 ] && args=(--smoke)
    for b in -1 $(seq 1 "$VD_RUN_BLOCKS"); do vd_run_block "$b"; done
    out=$(vd_analyze "$RES" "$ROOT" "$VD_DIR" ${args[@]+"${args[@]}"}) || die "analyze.py failed"
    if [ "$smoke" = 0 ] && [[ $out == *NEEDS_EXTENSION* ]] && [ "$VD_RUN_EXTEND_TO" -gt "$VD_RUN_BLOCKS" ]; then
        log "verdict inconclusive at n=$VD_RUN_BLOCKS: extending to n=$VD_RUN_EXTEND_TO"
        vd_scope_mark_extended "$RES"
        for b in $(seq $((VD_RUN_BLOCKS+1)) "$VD_RUN_EXTEND_TO"); do vd_run_block "$b"; done
        out=$(vd_analyze "$RES" "$ROOT" "$VD_DIR") || die "analyze.py failed"
    fi
    vd_fake || [ "${ZMF_KEEP_WORK:-0}" = 1 ] || rm -rf "$RUNROOT"
    log "report: $RES/report.md"
}

cmd_verdict_register() {
    local out=$ROOT/scripts/verdict/registration.json h
    "$VD_PY" "$VD_DIR/register.py" write "$ROOT" "$out" || die "register failed"
    h=$("$VD_PY" -c 'import json,sys; print(json.load(open(sys.argv[1]))["hash"][:8])' "$out")
    echo "registered config hash $h -> $out"
    echo "commit it before collecting data (not run for you):"
    echo "git add scripts/verdict/registration.json && git commit -m \"verdict: register $h\" && git push"
}

cmd_verdict_report() {
    local d=${1:-}
    [ -n "$d" ] && [ -f "$d/results.csv" ] || die "verdict report DIR: no results.csv in '${d}'"
    local args=()
    [ -f "$d/smoke" ] && args=(--smoke)
    # validity and numbers both come from the archived copies, not the live tree
    vd_analyze "${d%/}" "${d%/}/registered_tree" "${d%/}/registered_tree/scripts/verdict" ${args[@]+"${args[@]}"} >/dev/null
    log "report: ${d%/}/report.md"
}

cmd_verdict() {
    local sub=${1:-}
    [ $# -gt 0 ] && shift
    case "$sub" in
    register) cmd_verdict_register "$@" ;;
    run) cmd_verdict_run "$@" ;;
    report) cmd_verdict_report "$@" ;;
    *) echo "usage: zmfield.sh verdict register | run --ssh USER@DONOR [--peer IP] [--smoke] [--resume DIR] [--network LABEL] | report DIR" >&2; exit 2 ;;
    esac
}
