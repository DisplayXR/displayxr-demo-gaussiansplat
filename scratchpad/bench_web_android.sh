#!/usr/bin/env bash
# Hands-off benchmark of the WEB splat viewer (https://dxr-splat-ab.vercel.app)
# inside the Android DisplayXR Browser on the NP02J (Nubia Pad 3D II,
# SD 8 Gen 2 / Adreno 740, panel 1600x2560).
#
# Eight configs — {playcanvas, spark} x {Portsmouth, Tahoe} x {rs=1, rs=0.6} —
# each a cold browser start on a `?stats=1` URL. The PAGE owns the protocol:
# it runs `warm` seconds of warm-up then a `win` second measurement window
# (defaults 30 / 10, deliberately NOT overridden here), walks
# `window.__stats.phase` loading -> warmup -> measuring -> done, and at `done`
# freezes the object. This script only waits for `done` and reads it ONCE.
#
# Companion to scratchpad/bench_android.sh, which measures the NATIVE Vulkan
# splat app on the same panel. bench_web_android_parse.py prints both tables
# side by side; the native numbers are the point of comparison.
#
# ─────────────────────────── THE PAD PROTOCOL ────────────────────────────────
# This device is shared with a human and with other agent sessions. The rules
# below are copied from scratchpad/bench_android.sh because each one was paid
# for by a real collision (feedback_pad_lock_protocol):
#
#  * THE LOCK GATES THE WHOLE RUN, not just the lock write. Two sessions in
#    three days pinned rotation and launched apps under another session's live
#    measurement because they gated only their own `echo > pad.lock`. EVERY
#    device write below sits after the gate. An EMPTY lock file means
#    held-by-unknown and is also a refusal.
#  * THE SERIAL IS PINNED. Two pads have shared one USB cable, and
#    `ro.product.model` cannot discriminate them; this checks `ro.serialno`.
#  * THE LOCK IS WRITTEN BY pad-lock.sh, NEVER BY HAND — it is the only atomic
#    claim (`set -C`) and the only writer that reads back to prove it is ours.
#  * ROTATION IS PINNED FOR THE RUN AND FREED AT THE END. The OEM rewrites
#    user_rotation at every activity launch, so the pin is RE-ASSERTED after
#    each `am start` and then GATED on the measured rotation. The resting state
#    David asked for is auto-rotate ON (accelerometer_rotation 1,
#    user_rotation 1, fixed-to-user-rotation DISABLED) and the EXIT trap
#    restores exactly that, whatever went wrong.
#  * THE RUNTIME SERVICE IS NEVER FORCE-STOPPED (reference_android_live_testing).
#    Only org.chromium.chrome is stopped, and only between configs.
#  * usagestats IS SNAPSHOT AT BOTH ENDS — logcat rolls within minutes on this
#    unit, so it is the only artefact that can later prove no foreign app
#    launch landed inside a measurement window.
#
# ──────────────────────────── READING window.__stats ─────────────────────────
# Three readers, tried in this order, and the one that produced each row is
# recorded as `#META reader=` so a number is never unattributable:
#
#   1. LOGCAT (PRIMARY) — the page console.log()s `[stats] {...}` exactly once,
#      and Chromium mirrors console messages into logcat under the `chromium`
#      tag. No forwarding, no sockets, nothing to enable; works on a release
#      APK. This is the reader the run is designed around.
#   2. CDP (fallback, NEVER EXERCISED AGAINST THIS DEVICE) — `adb forward
#      tcp:$CDP_PORT localabstract:<socket>` then Runtime.evaluate
#      `JSON.stringify(window.__stats)` via scratchpad/cdp_client.py (stdlib
#      only; no pip, no npm). The client itself is proven end-to-end against
#      desktop Chrome; what is unproven is the NP02J half of it.
#
#      Source reading of the fork says it should just work, with no switch and
#      no debuggable build: `ProcessInitializationHandler` constructs a
#      `DevToolsServer("chrome")` and calls `setRemoteDebuggingEnabled(true,
#      ALLOW_DEBUG_PERMISSION)` unconditionally in deferred startup, giving the
#      abstract socket `chrome_devtools_remote`, and `devtools_auth.cc`
#      explicitly authorises a peer uid of `shell` — which is exactly the
#      adb-forward case. Note the socket appears a second or two AFTER the
#      first activity, so the probe polls rather than asking once.
#
#      There is deliberately no "enable CDP" lever here: the only related
#      command-line switch, `--remote-debugging-socket-name`, RENAMES the
#      socket and cannot turn the server on, so editing
#      /data/local/tmp/chrome-command-line would be a device write that buys
#      nothing. If the probe comes back empty, the answer is to find out why,
#      not to poke that file.
#   3. UIAUTOMATOR (last resort, also never exercised here) — `uiautomator
#      dump` + parse the on-screen `<div id="dxr-stats">`. Weakest of the
#      three: Chromium only exposes page text to the accessibility tree when an
#      a11y service is active, so this can legitimately come back empty on a
#      perfectly healthy run.
#
#   Plus, unconditionally, a `screencap` per config: the stats div is on screen
#   at `done`, so a human can read every number off the PNG even if all three
#   machine readers fail. That is evidence, not a reader.
#
# ────────────────────────────────── USAGE ────────────────────────────────────
#   ./scratchpad/bench_web_android.sh --dry-run   # plan + preflight, touches nothing
#   ./scratchpad/bench_web_android.sh             # the full run (~15-20 min)
#   ./scratchpad/bench_web_android.sh --only=pc_ports_rs100,sp_ports_rs100
#
# Env overrides: PAD_SERIAL, HANDLE, PURPOSE, OUT, CDP_PORT, STATS_DEADLINE_S,
# SAMPLE_S, COOLDOWN_S, BASE_URL, TAHOE_ASSET, TAHOE_FOCUS, ONLY
# (comma-separated config labels).
#
# Output: scratchpad/web_bench/<config>.log (+ .png, + stats.json), parsed by
#         scratchpad/bench_web_android_parse.py scratchpad/web_bench

set -u

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# ── configuration ────────────────────────────────────────────────────────────
PAD_SERIAL="${PAD_SERIAL:-327343950099}"
# The handle must match whoever ALREADY holds the lock if the pad was reserved
# ahead of the run — the gate below is re-entrant for our own handle and a
# refusal for anyone else's, so getting this wrong reads as "HELD" rather than
# quietly stealing the pad. `bench_android.sh` reserved the pad under
# mac/help-amazon for this same benchmark, so that is the default.
HANDLE="${HANDLE:-mac/help-amazon}"
PURPOSE="${PURPOSE:-web splat A/B benchmark in the DisplayXR Browser, 8 configs, ~20 min}"
PKG=org.chromium.chrome
# Re-resolved read-only at preflight; this is only the fallback if resolution
# fails. Verified 2026-09-22 on 327343950099 (browser 154.0.8037.17).
ACT_DEFAULT="$PKG/com.google.android.apps.chrome.IntentDispatcher"

OUT="${OUT:-$HERE/web_bench}"
BASE_URL="${BASE_URL:-https://dxr-splat-ab.vercel.app/}"
TAHOE_ASSET="${TAHOE_ASSET:-https://kypxikvxlrwsvwslpbmd.supabase.co/storage/v1/object/public/show-media/visitor/1edbeea3-246e-4a9c-af86-7ae06f7872f6/scene.sog}"
TAHOE_FOCUS="${TAHOE_FOCUS:-2.4}"

# The page's own defaults. NOT passed on the URL — they are recorded here only
# so the deadline below is a budget and not a guess, and so the parser can slice
# the GPU samples to the measurement window.
WARM_S="${WARM_S:-30}"
WIN_S="${WIN_S:-10}"
# load + warm + win + grace. Tahoe is a network .sog from Supabase, so `load`
# is the term with the variance; 45 s of grace is the coordinator's figure.
STATS_DEADLINE_S="${STATS_DEADLINE_S:-180}"
POLL_S="${POLL_S:-3}"
SAMPLE_S="${SAMPLE_S:-5}"
COOLDOWN_S="${COOLDOWN_S:-8}"

CDP_PORT="${CDP_PORT:-9222}"
CMDLINE_FILE=/data/local/tmp/chrome-command-line
# The runtime APK must not be in Android's "stopped" state or `bindService`
# from the GPU process fails silently and inline-3D stays dormant — every row
# would then read mode=mono for a reason that has nothing to do with the page.
RUNTIME_PKG=org.freedesktop.monado.openxr_runtime.out_of_process

PAD_LOCK_SH="${PAD_LOCK_SH:-$HOME/Documents/GitHub/displayxr-installer/android-bundle/scripts/pad-lock.sh}"
LOCK=/data/local/tmp/pad.lock
CDP="$HERE/cdp_client.py"
PARSE="$HERE/bench_web_android_parse.py"

# config list: "<label>|<engine>|<asset key>|<rs>"
#
# ORDER IS DELIBERATE: the two engines for a given (asset, rs) run BACK TO BACK.
# A 20-minute run on a passively cooled tablet drifts thermally from start to
# finish, and the comparison this benchmark exists to make is playcanvas vs
# spark — so the drift is pushed OUT of that comparison and into the
# less-interesting asset/rs axis, instead of being spread evenly over both.
CFGS=(
    "pc_ports_rs100|playcanvas|portsmouth|1"
    "sp_ports_rs100|spark|portsmouth|1"
    "pc_ports_rs060|playcanvas|portsmouth|0.6"
    "sp_ports_rs060|spark|portsmouth|0.6"
    "pc_tahoe_rs100|playcanvas|tahoe|1"
    "sp_tahoe_rs100|spark|tahoe|1"
    "pc_tahoe_rs060|playcanvas|tahoe|0.6"
    "sp_tahoe_rs060|spark|tahoe|0.6"
)

DRY=0
ONLY="${ONLY:-}"
for a in "$@"; do
    case "$a" in
        --dry-run) DRY=1 ;;
        --only=*) ONLY="${a#--only=}" ;;
        -h|--help) sed -n '2,110p' "$0"; exit 0 ;;
        *) echo "unknown argument: $a" >&2; exit 2 ;;
    esac
done

if [ -n "$ONLY" ]; then
    keep=()
    for c in "${CFGS[@]}"; do
        case ",$ONLY," in *",${c%%|*},"*) keep+=("$c") ;; esac
    done
    [ "${#keep[@]}" -gt 0 ] || { echo "--only matched no config" >&2; exit 2; }
    CFGS=("${keep[@]}")
fi

export ANDROID_SERIAL="$PAD_SERIAL"
say() { printf '%s  %s\n' "$(date -u +%H:%M:%SZ)" "$*"; }
die() { printf 'FATAL: %s\n' "$*" >&2; exit 1; }

url_for() {
    # $1 engine, $2 asset key, $3 rs
    u="${BASE_URL}?engine=$1&orbit=0&rs=$3&stats=1"
    [ "$2" = "tahoe" ] && u="$u&asset=$TAHOE_ASSET&focus=$TAHOE_FOCUS"
    printf '%s' "$u"
}

# ── preflight: READ-ONLY, runs even under --dry-run ──────────────────────────
say "adb devices -l:"
adb devices -l | sed 's/^/    /'
att=$(adb devices | awk 'NR>1 && $2=="device" {print $1}')
printf '%s\n' "$att" | grep -qx "$PAD_SERIAL" \
    || die "PAD_SERIAL=$PAD_SERIAL is not attached. Attached: ${att:-<none>}"
serial=$(adb shell getprop ro.serialno | tr -d '\r')
pname=$(adb shell getprop ro.product.name | tr -d '\r')
[ "$serial" = "$PAD_SERIAL" ] \
    || die "attached serial '$serial' != PAD_SERIAL '$PAD_SERIAL' (the cable has been swapped before)"
say "device $serial ($pname) — model=$(adb shell getprop ro.product.model | tr -d '\r')"

[ -x "$CDP" ] || die "cdp_client.py not found/executable at $CDP"
[ -f "$PARSE" ] || say "WARNING: $PARSE missing — the run still records raw logs"

# Launcher activity, resolved live rather than hardcoded.
ACT=$(adb shell cmd package resolve-activity --brief -a android.intent.action.VIEW \
        -d https://example.com "$PKG" 2>/dev/null | tail -1 | tr -d '\r')
case "$ACT" in
    "$PKG"/*) : ;;
    *) say "WARNING: could not resolve a VIEW activity for $PKG (got '${ACT:-<empty>}') — using $ACT_DEFAULT"
       ACT="$ACT_DEFAULT" ;;
esac
say "launcher activity: $ACT"

browser_ver=$(adb shell dumpsys package "$PKG" 2>/dev/null | sed -n 's/.*versionName=\(.*\)/\1/p' | head -1 | tr -d '\r')
[ -n "$browser_ver" ] || die "$PKG is not installed on $serial"
dbg=$(adb shell dumpsys package "$PKG" 2>/dev/null | grep -m1 'flags=\[' | tr -d '\r')
say "browser: $PKG versionName=$browser_ver"
say "  package flags: $dbg"
case "$dbg" in
    *DEBUGGABLE*) say "  -> DEBUGGABLE build: the devtools socket should open with no switch" ;;
    *) say "  -> NOT debuggable: whether chrome_devtools_remote opens is unknown until it RUNS" ;;
esac

cmdline=$(adb shell "cat $CMDLINE_FILE 2>/dev/null" | tr -d '\r')
say "$CMDLINE_FILE: ${cmdline:-<absent>}"
say "  (recorded, NOT edited — inline-3D is on by default on Android since the"
say "   fork's 0103 patch, and no switch can turn the devtools server on anyway)"

# The woven session is hands-free — `inline-3d` collapses to kInline in Blink,
# so it needs no transient user activation, no tap and no fullscreen. What it
# DOES need is the runtime APK out of Android's stopped state: a never-launched
# (or force-stopped) runtime package makes the GPU process's bindService fail
# silently, and the page then reports mode=mono for a reason that has nothing to
# do with the renderer. Read-only check; waking it is a device write this script
# will not make behind your back.
rt=$(adb shell dumpsys package "$RUNTIME_PKG" 2>/dev/null | grep -m1 'stopped=' | tr -d '\r' | tr -s ' ')
rtpid=$(adb shell "pidof $RUNTIME_PKG" 2>/dev/null | tr -d '\r')
say "runtime pkg: ${rt:-<not installed>}"
say "runtime pid: ${rtpid:-<not running>}"
case "$rt" in
    *stopped=true*|"")
        say "WARNING: the runtime package is stopped or absent. inline-3D will NOT bind and"
        say "         EVERY config will come back mode=mono. Wake it first (one device write,"
        say "         and NOT a force-stop of the service):"
        say "             adb -s $PAD_SERIAL shell monkey -p $RUNTIME_PKG 1"
        ;;
    *) say "  -> not stopped: the inline-3D bindService precondition is satisfied" ;;
esac

# Network. A config that cannot fetch the page or the .sog is not a slow config,
# it is a void one — but ICMP to a CDN is routinely dropped, so this warns.
if adb shell "ping -c1 -W3 dxr-splat-ab.vercel.app >/dev/null 2>&1; echo \$?" | tr -d '\r' | grep -qx 0; then
    say "network: the pad can reach dxr-splat-ab.vercel.app"
else
    say "WARNING: the pad did not answer a ping for dxr-splat-ab.vercel.app — if it is"
    say "         genuinely offline every config will report a loading phase and time out"
fi

th=$(adb shell dumpsys thermalservice 2>/dev/null | grep -m1 -i 'thermal status' | tr -d '\r' | tr -s ' ')
clk=$(adb shell cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null | tr -d '\r')
say "thermal at rest: ${th:-?}   gpuclk: ${clk:-?}"
[ -n "$clk" ] || say "WARNING: /sys/class/kgsl/kgsl-3d0/gpuclk unreadable — GPU columns will be empty"

say "configs (${#CFGS[@]}), engines paired back-to-back so thermal drift stays out of the A/B:"
for c in "${CFGS[@]}"; do
    lbl="${c%%|*}"; eng=$(printf '%s' "$c" | cut -d'|' -f2)
    ast=$(printf '%s' "$c" | cut -d'|' -f3); rs="${c##*|}"
    printf '    %-15s %s\n' "$lbl" "$(url_for "$eng" "$ast" "$rs")"
done
say "per config: cold am start -> page warm ${WARM_S}s -> page measures ${WIN_S}s -> phase=done"
say "            deadline ${STATS_DEADLINE_S}s, then force-stop $PKG (NEVER the runtime service)"

# ── THE GATE. Every device WRITE below this point depends on it. ─────────────
L=$(adb shell "cat $LOCK 2>/dev/null" | tr -d '\r')
if adb shell "test -e $LOCK" 2>/dev/null && [ -z "$L" ]; then
    echo "HELD: empty lock file = held by unknown. A human must resolve this."
    exit 1
fi
OURS=0
if [ -n "$L" ] && [ "${L%% *}" = "$HANDLE" ]; then
    OURS=1
    say "lock is already OURS: $L"
elif [ -n "$L" ]; then
    echo "HELD: $L"
    echo "Do nothing. Message the holder; never clear a foreign lock outside the"
    echo "four-step liveness rule (displayxr-installer#56)."
    exit 1
else
    say "lock is FREE"
fi

if [ "$DRY" = 1 ]; then
    say "── what the run WOULD change on the device ──"
    say "  * pad.lock taken as '$HANDLE' (released on exit)"
    say "  * rotation PINNED landscape for the run, restored to auto-rotate ON at exit"
    say "  * $PKG force-stopped between configs and launched ${#CFGS[@]}x via am start"
    say "  * logcat buffer cleared once per config (adb logcat -c)"
    say "  * /sdcard/dxr_stats_dump.xml written by uiautomator (reader 3 only, if"
    say "    readers 1 and 2 both come back empty)"
    say "  * $CMDLINE_FILE is NOT touched"
    say "  * NOTHING is installed, uninstalled, or pm-cleared; no setprop; no settings"
    say "    put beyond the rotation pair; the runtime service is NEVER stopped"
    say "  * side effect: up to ${#CFGS[@]} extra browser tabs are left open (am start"
    say "    opens a tab per config); removing them would need a device write, so it"
    say "    is reported rather than done"
    say "--dry-run: stopping before the first device write. Nothing was changed."
    exit 0
fi

# ── take the lock (atomic, proves it is ours), then arm the restore trap ─────
[ -x "$PAD_LOCK_SH" ] || die "pad-lock.sh not found at $PAD_LOCK_SH — it is the ONLY sanctioned writer of $LOCK; do not hand-write one"
if [ "$OURS" = 1 ]; then
    HELD_BY_US=1
else
    PAD_SERIAL="$PAD_SERIAL" "$PAD_LOCK_SH" take "$HANDLE" "$PURPOSE" || die "could not take the pad lock"
    HELD_BY_US=1
fi

mkdir -p "$OUT"
CDP_READY=0
CDP_SOCKET=""

cleanup() {
    rc=$?
    say "── cleanup ──"
    adb shell am force-stop "$PKG" >/dev/null 2>&1
    adb forward --remove "tcp:$CDP_PORT" >/dev/null 2>&1
    # Resting state (David, 2026-09-07/08): auto-rotate ON, user_rotation 1,
    # fixed-to-user-rotation DISABLED. Never hand the pad back pinned, never in
    # portrait.
    adb shell wm fixed-to-user-rotation disabled >/dev/null 2>&1
    adb shell wm user-rotation free >/dev/null 2>&1
    adb shell settings put system accelerometer_rotation 1 >/dev/null 2>&1
    adb shell settings put system user_rotation 1 >/dev/null 2>&1
    ar=$(adb shell settings get system accelerometer_rotation 2>/dev/null | tr -d '\r')
    ur=$(adb shell settings get system user_rotation 2>/dev/null | tr -d '\r')
    fx=$(adb shell wm fixed-to-user-rotation 2>/dev/null | tr -d '\r')
    # mCurrentOrientation is NOT a settable target — with auto-rotate ON the
    # display follows how the device is lying, so gate on these three instead.
    say "rotation restored: accelerometer_rotation=$ar user_rotation=$ur fixed-to-user-rotation='$fx' (want 1 / 1 / disabled)"
    adb shell dumpsys usagestats > "$OUT/usagestats_end.txt" 2>/dev/null
    if [ "${HELD_BY_US:-0}" = 1 ]; then
        PAD_SERIAL="$PAD_SERIAL" "$PAD_LOCK_SH" release "$HANDLE" || \
            say "WARNING: could not release the lock — check it by hand: adb shell cat $LOCK"
    fi
    say "logs: $OUT/<config>.log   parse with: python3 $PARSE $OUT"
    exit $rc
}
trap cleanup EXIT INT TERM

adb shell dumpsys usagestats > "$OUT/usagestats_start.txt" 2>/dev/null
{
    printf 'device=%s (%s)\n' "$serial" "$pname"
    printf 'browser=%s %s\n' "$PKG" "$browser_ver"
    printf 'activity=%s\n' "$ACT"
    printf 'cmdline=%s\n' "${cmdline:-<absent>}"
    printf 'started=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$OUT/run_meta.txt"

# ── rotation pin ─────────────────────────────────────────────────────────────
pin_landscape() {
    adb shell settings put system accelerometer_rotation 0 >/dev/null 2>&1
    adb shell wm user-rotation lock 1 >/dev/null 2>&1
    adb shell wm fixed-to-user-rotation enabled >/dev/null 2>&1
}
gate_landscape() {
    r=$(adb shell dumpsys window displays 2>/dev/null | grep -o 'rotation=[0-9]' | head -1 | tr -d '\r')
    # The warning goes to STDERR: this function's stdout is captured by callers,
    # and a warning mixed into it would be recorded AS the rotation.
    [ "$r" = "rotation=1" ] || say "WARNING: ${r:-<no rotation read>} (wanted rotation=1) — this config is measured in the wrong orientation" >&2
    printf '%s' "$r"
}
pin_landscape
say "landscape pinned: $(gate_landscape)"

# ── devtools probe: only answerable while the browser RUNS ───────────────────
probe_cdp() {
    # /proc/net/unix lists abstract sockets with a leading '@'. The name is not
    # assumed: the bind falls back to chrome_devtools_remote_<pid> if the plain
    # name is taken, and a fork could open webview_devtools_remote_<pid>
    # instead — so we forward to whatever is actually there.
    #
    # The socket is opened in DEFERRED startup, a second or two after the first
    # activity, so this polls instead of asking once.
    CDP_SOCKET=""
    pw=0
    while [ "$pw" -lt "${CDP_PROBE_S:-20}" ]; do
        CDP_SOCKET=$(adb shell "cat /proc/net/unix 2>/dev/null" | tr -d '\r' \
            | awk '{print $NF}' | grep '^@.*devtools_remote' | head -1 | sed 's/^@//')
        [ -n "$CDP_SOCKET" ] && break
        sleep 2
        pw=$((pw + 2))
    done
    if [ -z "$CDP_SOCKET" ]; then
        say "CDP: no *devtools_remote socket while $PKG is running -> reader 2 UNAVAILABLE"
        say "     (the source says it should be there; the socket is opened in DEFERRED"
        say "      startup, so if this fires on a page that never finished loading, look"
        say "      at the page before concluding the fork has no devtools server)"
        CDP_READY=0
        return 1
    fi
    say "CDP: socket @$CDP_SOCKET is open"
    adb forward "tcp:$CDP_PORT" "localabstract:$CDP_SOCKET" >/dev/null 2>&1 \
        || { say "CDP: adb forward failed -> reader 2 UNAVAILABLE"; CDP_READY=0; return 1; }
    if "$CDP" --port "$CDP_PORT" --version >"$OUT/cdp_version.json" 2>"$OUT/cdp_version.err"; then
        say "CDP: endpoint live — $(sed -n 's/.*"Browser": "\([^"]*\)".*/\1/p' "$OUT/cdp_version.json" | head -1)"
        CDP_READY=1
        return 0
    fi
    say "CDP: socket exists but /json/version did not answer -> reader 2 UNAVAILABLE"
    say "     $(head -2 "$OUT/cdp_version.err" | tr '\n' ' ')"
    CDP_READY=0
    return 1
}

# ── sampler: gpu clock / busy / thermal, every SAMPLE_S ──────────────────────
sampler() {
    log="$1"
    t=0
    while :; do
        clk=$(adb shell cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null | tr -d '\r')
        mclk=$(adb shell cat /sys/class/kgsl/kgsl-3d0/max_gpuclk 2>/dev/null | tr -d '\r')
        busy=$(adb shell cat /sys/class/kgsl/kgsl-3d0/gpubusy 2>/dev/null | tr -d '\r' | tr -s ' ')
        pct=$(adb shell cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage 2>/dev/null | tr -d '\r')
        gtemp=$(adb shell cat /sys/class/kgsl/kgsl-3d0/temp 2>/dev/null | tr -d '\r')
        th=$(adb shell dumpsys thermalservice 2>/dev/null | grep -m1 -iE 'thermal status|mStatus' | tr -d '\r' | tr -s ' ')
        printf '#SAMPLE t=%s gpuclk=%s max_gpuclk=%s gpubusy="%s" gpu_busy_pct=%s gpu_temp=%s thermal="%s"\n' \
            "$t" "${clk:-NA}" "${mclk:-NA}" "${busy:-NA}" "${pct:-NA}" "${gtemp:-NA}" "${th:-NA}" >> "$log"
        sleep "$SAMPLE_S"
        t=$((t + SAMPLE_S))
    done
}

# ── readers ──────────────────────────────────────────────────────────────────
# Each prints the stats JSON on stdout and returns 0, or returns non-zero.
# The brace-balanced extraction lives in ONE place (the parser) because the
# logcat line wraps the JSON in Chromium's own quotes:
#   chromium: [INFO:CONSOLE(1)] "[stats] {"engine":"spark",...}", source: https://...
read_logcat() {
    adb logcat -d 2>/dev/null | grep -F '[stats]' > "$1.logcat" || true
    [ -s "$1.logcat" ] || return 1
    python3 "$PARSE" --extract-stats "$1.logcat" 2>/dev/null
}
read_cdp() {
    [ "$CDP_READY" = 1 ] || return 1
    "$CDP" --port "$CDP_PORT" --match dxr-splat-ab \
           --eval 'JSON.stringify(window.__stats)' 2>/dev/null
}
read_uiautomator() {
    adb shell uiautomator dump /sdcard/dxr_stats_dump.xml >/dev/null 2>&1 || return 1
    adb shell "cat /sdcard/dxr_stats_dump.xml 2>/dev/null" > "$1.uixml" 2>/dev/null
    [ -s "$1.uixml" ] || return 1
    python3 "$PARSE" --extract-stats "$1.uixml" 2>/dev/null
}
# The phase, for progress only — never for the headline number.
peek_phase() {
    if [ "$CDP_READY" = 1 ]; then
        "$CDP" --port "$CDP_PORT" --match dxr-splat-ab \
               --eval '(window.__stats&&window.__stats.phase)||"?"' 2>/dev/null
    fi
}

# ── the run ──────────────────────────────────────────────────────────────────
first=1
for cfg in "${CFGS[@]}"; do
    label="${cfg%%|*}"
    engine=$(printf '%s' "$cfg" | cut -d'|' -f2)
    assetk=$(printf '%s' "$cfg" | cut -d'|' -f3)
    rs="${cfg##*|}"
    url=$(url_for "$engine" "$assetk" "$rs")
    LOG="$OUT/$label.log"
    : > "$LOG"

    say "════ $label  engine=$engine asset=$assetk rs=$rs ════"
    say "  $url"
    adb shell am force-stop "$PKG" >/dev/null 2>&1
    sleep 2
    adb logcat -c >/dev/null 2>&1

    {
        printf '#META config=%s engine=%s assetkey=%s rs=%s\n' "$label" "$engine" "$assetk" "$rs"
        printf '#META url=%s\n' "$url"
        printf '#META browser=%s device=%s (%s)\n' "$browser_ver" "$serial" "$pname"
        printf '#META warm_s=%s win_s=%s\n' "$WARM_S" "$WIN_S"
        printf '#META started=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } >> "$LOG"

    t0=$(date +%s)
    # The URL carries '&', so it must survive BOTH this shell and the device's
    # shell: adb shell re-parses its argument. Single quotes inside the
    # double-quoted command string are what protect it.
    adb shell "am start -a android.intent.action.VIEW -d '$url' -n $ACT" \
        > "$OUT/$label.amstart" 2>&1 || say "WARNING: am start returned non-zero"
    sed 's/^/#AMSTART /' "$OUT/$label.amstart" >> "$LOG"
    sleep 3
    # The OEM rewrites user_rotation=0 at every activity launch, so the pin only
    # holds if re-asserted AFTER the launch — then gated, never trusted.
    pin_landscape
    printf '#META rotation=%s\n' "$(gate_landscape)" >> "$LOG"

    # The devtools question is only answerable while the browser RUNS, so the
    # probe is the first thing the gated run does. Retried each config while it
    # keeps failing: a single early miss should not disable reader 2 for the
    # whole run.
    if [ "$CDP_READY" = 0 ]; then
        [ "$first" = 1 ] && say "  probing for a devtools socket (only answerable while the browser runs)"
        first=0
        probe_cdp || true
    fi
    printf '#META cdp_ready=%s cdp_socket=%s\n' "$CDP_READY" "${CDP_SOCKET:-none}" >> "$LOG"

    sampler "$LOG" &
    SAMPLER_PID=$!

    say "  waiting for phase=done (page does ${WARM_S}s warm + ${WIN_S}s window; deadline ${STATS_DEADLINE_S}s)"
    stats=""
    reader=none
    waited=0
    lastphase=""
    while [ "$waited" -lt "$STATS_DEADLINE_S" ]; do
        sleep "$POLL_S"
        waited=$((waited + POLL_S))
        # Reader 1: the page console.log()s `[stats] {...}` exactly ONCE, at
        # done — so its mere presence IS the done signal.
        stats=$(read_logcat "$OUT/$label")
        if [ -n "$stats" ]; then reader=logcat; break; fi
        # Reader 2: same signal, structured.
        if [ "$CDP_READY" = 1 ]; then
            p=$(peek_phase)
            if [ -n "$p" ] && [ "$p" != "$lastphase" ]; then
                lastphase="$p"
                printf '#PHASE t=%s phase=%s\n' "$waited" "$p" >> "$LOG"
                say "    t=${waited}s phase=$p"
            fi
            if [ "$p" = "done" ]; then
                stats=$(read_cdp)
                if [ -n "$stats" ]; then reader=cdp; break; fi
            fi
        fi
    done
    done_t="$waited"

    # Reader 3, and only if 1 and 2 both came back empty.
    if [ -z "$stats" ]; then
        say "  no stats from logcat/CDP after ${waited}s — trying uiautomator"
        stats=$(read_uiautomator "$OUT/$label")
        [ -n "$stats" ] && reader=uiautomator
    fi

    pkill -P "$SAMPLER_PID" 2>/dev/null
    kill "$SAMPLER_PID" 2>/dev/null; wait "$SAMPLER_PID" 2>/dev/null

    # Evidence a human can read even if every machine reader failed: the stats
    # div is on screen at `done`.
    adb exec-out screencap -p > "$OUT/$label.png" 2>/dev/null
    [ -s "$OUT/$label.png" ] || rm -f "$OUT/$label.png"

    # The lens-MCU wedge signature. Captured per config because `logcat -c` at
    # the top of the next config would otherwise erase it.
    adb logcat -d 2>/dev/null | grep -i 'leiadisp@1.0-service.*select() timeout' \
        | tail -20 | sed 's/^/#WEDGE /' >> "$LOG"

    {
        printf '#META reader=%s done_t=%s\n' "$reader" "$done_t"
        printf '#META ended=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } >> "$LOG"
    if [ -n "$stats" ]; then
        printf '#STATS %s\n' "$stats" >> "$LOG"
        printf '%s\n' "$stats" > "$OUT/$label.stats.json"
        mode=$(printf '%s' "$stats" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("mode","?"))' 2>/dev/null)
        fps=$(printf '%s' "$stats" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("fps","?"))' 2>/dev/null)
        med=$(printf '%s' "$stats" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("ms_median","?"))' 2>/dev/null)
        say "  [$reader] mode=$mode fps=$fps ms_median=$med  (t=${done_t}s)"
        [ "$mode" = "mono" ] && say "  NOTE: mode=mono — the 3D/woven session did not engage on a hands-off am start."
    else
        printf '#META stats=MISSING\n' >> "$LOG"
        say "  WARNING: no window.__stats after ${waited}s from any reader."
        say "           $OUT/$label.png is the only evidence for this config."
    fi

    adb shell am force-stop "$PKG" >/dev/null 2>&1
    sleep "$COOLDOWN_S"
done

say "all ${#CFGS[@]} configs done"
# cleanup() runs from the EXIT trap: forward removed, cmdline restored, rotation
# freed, lock released.
