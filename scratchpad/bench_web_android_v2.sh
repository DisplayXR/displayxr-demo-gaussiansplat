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
# RANKING CORRECTED BY THE 2026-09-22 RUN. The plan was logcat-primary; the
# artefacts say logcat does not work at all on this build.
#
#   1. CDP (PRIMARY, and the only reader that has ever returned a number here)
#      — `adb forward tcp:$CDP_PORT localabstract:<socket>` then
#      Runtime.evaluate `JSON.stringify(window.__stats)` via cdp_client.py
#      (stdlib only; no pip, no npm). Confirmed live on the pad: the socket
#      @chrome_devtools_remote opened on a NOT-debuggable release APK and
#      /json/version answered `Chrome/154.0.8037.17`, exactly as the source
#      reading predicted (DevToolsServer is constructed unconditionally in
#      deferred startup; devtools_auth.cc authorises a peer uid of `shell`).
#
#      TWO v2 fixes, both from real failures:
#      * THE SOCKET NAME IS RE-PROBED EVERY CONFIG. v1 probed once and cached
#        `CDP_READY=1`, so it never noticed the transport dying. From config 3
#        onward every Runtime.evaluate failed while the page was demonstrably
#        still rendering. `devtools_server.cc` falls back to
#        `chrome_devtools_remote_<pid>` when the plain name is still bound —
#        which is exactly what an `am force-stop` + relaunch can produce — and
#        a forward pointing at the old name then connects to nothing.
#      * THE TARGET IS MATCHED STRICTLY. `am force-stop` does NOT close tabs;
#        they restore on the next launch, so /json lists every previous
#        config's page and "first page matching dxr-splat-ab" is a coin flip.
#        A backgrounded tab's rAF is throttled, so it never reaches `done` —
#        and a tab from an EARLIER config still holds a frozen `done` object,
#        which would be read as this config's result. v2 requires `engine=`
#        AND `rs=` AND (Tahoe) `asset=` to match, closes every other page
#        target first, and logs all target URLs as `#TARGETS`.
#   2. LOGCAT (DEMOTED to a fallback — it produced nothing). The page
#      console.log()s `[stats] {...}` once, but all six `.logcat` files from
#      the run are 0 bytes, including the two configs whose `[stats]`-shaped
#      object CDP read successfully. On this official/release build Chromium
#      does not mirror page console messages into logcat. Kept because it costs
#      nothing and would be the best reader if a future build enables it.
#   3. UIAUTOMATOR (last resort). It never yielded stats either, but it earned
#      its place for a different reason: its dumps are what IDENTIFIED the
#      failure. They showed `com.zte.mifavor.launcher` and then
#      `com.android.systemui` keyguard in the foreground instead of Chrome.
#      Keep it, and keep dumping it on failure.
#
#   Plus, unconditionally, a `screencap` per config. Two failing configs
#   produced BYTE-IDENTICAL PNGs 24 minutes apart — the single clearest
#   evidence that the screen was frozen and nothing was rendering.
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

# ── v2: hard timeouts ────────────────────────────────────────────────────────
# macOS ships no coreutils `timeout`. perl's alarm + exec does the job and does
# it BETTER for this case: exec replaces the perl process, so SIGALRM lands on
# the adb client itself and kills it — there is no wrapper left holding a pipe.
# Verified: `perl -e 'alarm 2; exec @ARGV' sleep 10` returns 142 after 2.0 s.
#
# Why this exists: in the 2026-09-22 run every adb round-trip degraded as the
# device went into deep idle, and the sampler's `adb shell cat` returned empty
# on 1,653 of 1,801 reads over 2 h 17 m. Nothing had a timeout, so a 3-second
# poll became a 3-hour block.
T_ADB_FAST="${T_ADB_FAST:-15}"     # getprop/dumpsys/cat/am — must be instant
T_ADB_LOGCAT="${T_ADB_LOGCAT:-30}" # a full logcat -d dump
T_CDP="${T_CDP:-25}"               # cdp_client.py round-trip
T_UIAUTO="${T_UIAUTO:-20}"
T_SCREENCAP="${T_SCREENCAP:-15}"
adbt() { n="$1"; shift; perl -e 'alarm shift; exec @ARGV' "$n" adb "$@"; }

# ── v2: keep the display awake ───────────────────────────────────────────────
# THE defect that ended the 2026-09-22 run. The benchmark is hands-off by
# design, so it injects no input for 20+ minutes and the pad's display timeout
# fires. Configs 5 and 6 were screencapped showing the systemui KEYGUARD (clocks
# 21:40 / 22:04, matching the failure timestamps to the minute) with the GPU
# parked at 220 MHz, gpubusy "0 0" and the die at 27-31 C — nothing rendered at
# all. `svc power stayon true` is the input-free way to hold it awake, and the
# original value is read first and restored by the EXIT trap.
STAYON="${STAYON:-1}"
STAYON_ORIG=""
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
    [ -n "${URL_EXTRA:-}" ] && u="${u}${URL_EXTRA}"
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

# Display power. The hands-off run injects no input, so the pad's display
# timeout is the single most dangerous variable in it.
STAYON_ORIG=$(adbt "$T_ADB_FAST" shell settings get global stay_on_while_plugged_in 2>/dev/null | tr -d '\r')
wake=$(adbt "$T_ADB_FAST" shell dumpsys power 2>/dev/null | grep -m1 'mWakefulness=' | tr -d '\r' | tr -s ' ')
scr_off=$(adbt "$T_ADB_FAST" shell settings get system screen_off_timeout 2>/dev/null | tr -d '\r')
say "display: ${wake:-?}  screen_off_timeout=${scr_off:-?} ms  stay_on_while_plugged_in=${STAYON_ORIG:-?}"
if [ "$STAYON" = 1 ]; then
    say "  -> the run will hold the screen awake (svc power stayon true) and restore '${STAYON_ORIG:-?}' at exit"
else
    say "  WARNING: STAYON=0. The 2026-09-22 run died exactly here: no input for 20 min,"
    say "           display timed out, keyguard came up, and 4 of 6 configs measured nothing."
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
    if [ "$STAYON" = 1 ]; then
        say "  * svc power stayon true for the run; stay_on_while_plugged_in restored to"
        say "    '${STAYON_ORIG:-?}' by the EXIT trap (this is THE fix for the 2026-09-22 failure)"
    fi
    say "  * stale browser tabs closed via CDP Target.closeTarget before each config"
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

CLEANED=0
cleanup() {
    rc=$?
    # The v1 trap was armed on EXIT INT TERM, so a SIGTERM ran it TWICE (the
    # 2026-09-22 log ends with two identical "── cleanup ──" blocks, two
    # rotation restores and two lock releases). The second release is the
    # dangerous one: it would clear a lock a NEXT session had already taken.
    [ "$CLEANED" = 1 ] && exit $rc
    CLEANED=1
    say "── cleanup ──"
    adbt "$T_ADB_FAST" shell am force-stop "$PKG" >/dev/null 2>&1
    adb forward --remove "tcp:$CDP_PORT" >/dev/null 2>&1
    if [ "$STAYON" = 1 ]; then
        # Restore the ORIGINAL value, not a guessed default: on this pad it read
        # back as a bitmask, and "0" is not automatically right.
        if [ -n "$STAYON_ORIG" ] && [ "$STAYON_ORIG" != "null" ]; then
            adbt "$T_ADB_FAST" shell settings put global stay_on_while_plugged_in "$STAYON_ORIG" >/dev/null 2>&1
        else
            adbt "$T_ADB_FAST" shell svc power stayon false >/dev/null 2>&1
        fi
        now=$(adbt "$T_ADB_FAST" shell settings get global stay_on_while_plugged_in 2>/dev/null | tr -d '\r')
        say "stay_on_while_plugged_in restored: ${now:-?} (was ${STAYON_ORIG:-?})"
    fi
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
    adbt 60 shell dumpsys usagestats > "$OUT/usagestats_end.txt" 2>/dev/null
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

# ── v2: hold the display awake for the run ──────────────────────────────────
# `svc power stayon true` sets stay_on_while_plugged_in to the AC|USB|WIRELESS
# bitmask. It needs no input injection and no wakelock permission, and the pad
# is on USB for the whole run by definition (it is how we are talking to it).
if [ "$STAYON" = 1 ]; then
    adbt "$T_ADB_FAST" shell svc power stayon true >/dev/null 2>&1
    v=$(adbt "$T_ADB_FAST" shell settings get global stay_on_while_plugged_in 2>/dev/null | tr -d '\r')
    say "display held awake: stay_on_while_plugged_in=$v (was ${STAYON_ORIG:-?})"
    [ "$v" = "0" ] && say "WARNING: stay-on did NOT take — the display will time out and the run will measure nothing"
fi

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

# ── devtools probe — RE-RUN EVERY CONFIG (v2) ────────────────────────────────
# v1 probed once and cached the result. That is precisely how the 2026-09-22 run
# spent 4.5 hours reading nothing: from config 3 onward every Runtime.evaluate
# failed, but `CDP_READY` was still 1 from config 1 and `#META cdp_socket=` kept
# reporting a name nobody had re-measured. A cached "the transport works" is
# worse than no transport at all.
probe_cdp() {
    # /proc/net/unix lists abstract sockets with a leading '@'. The name is NOT
    # assumed: devtools_server.cc binds `chrome_devtools_remote` but falls back
    # to `chrome_devtools_remote_<pid>` when that name is still held — which a
    # force-stop + relaunch can cause — so we forward to whatever is there NOW.
    CDP_READY=0
    CDP_SOCKET=""
    pw=0
    while [ "$pw" -lt "${CDP_PROBE_S:-20}" ]; do
        CDP_SOCKET=$(adbt "$T_ADB_FAST" shell "cat /proc/net/unix 2>/dev/null" | tr -d '\r' \
            | awk '{print $NF}' | grep '^@.*devtools_remote' | head -1 | sed 's/^@//')
        [ -n "$CDP_SOCKET" ] && break
        sleep 2
        pw=$((pw + 2))
    done
    if [ -z "$CDP_SOCKET" ]; then
        say "CDP: no *devtools_remote socket while $PKG is running -> reader UNAVAILABLE"
        return 1
    fi
    # Always tear the old forward down first: a forward left pointing at a name
    # that has since changed connects to nothing and fails silently.
    adb forward --remove "tcp:$CDP_PORT" >/dev/null 2>&1
    adb forward "tcp:$CDP_PORT" "localabstract:$CDP_SOCKET" >/dev/null 2>&1 \
        || { say "CDP: adb forward failed -> reader UNAVAILABLE"; return 1; }
    if perl -e 'alarm shift; exec @ARGV' "$T_CDP" \
         "$CDP" --port "$CDP_PORT" --version >"$OUT/cdp_version.json" 2>"$OUT/cdp_version.err"; then
        CDP_READY=1
        return 0
    fi
    say "CDP: @$CDP_SOCKET exists but /json/version did not answer -> reader UNAVAILABLE"
    say "     $(head -2 "$OUT/cdp_version.err" | tr '\n' ' ')"
    return 1
}

# ── sampler: gpu clock / busy / thermal, every SAMPLE_S ──────────────────────
# v1 made FIVE separate `adb shell` round-trips per sample plus a dumpsys, every
# 5 s, with no timeout. Against a device in deep idle they degraded until 1,653
# of 1,801 reads returned empty. v2 does ONE `adb shell` per sample, bounded.
# An `NA` now means "the device did not answer in $T_ADB_FAST s", which is
# itself a datum the parser can count.
sampler() {
    log="$1"
    t=0
    while :; do
        line=$(adbt "$T_ADB_FAST" shell '
            K=/sys/class/kgsl/kgsl-3d0
            echo "CLK=$(cat $K/gpuclk 2>/dev/null)"
            echo "MAX=$(cat $K/max_gpuclk 2>/dev/null)"
            echo "BUSY=$(cat $K/gpubusy 2>/dev/null)"
            echo "PCT=$(cat $K/gpu_busy_percentage 2>/dev/null)"
            echo "TEMP=$(cat $K/temp 2>/dev/null)"
            echo "THERM=$(dumpsys thermalservice 2>/dev/null | grep -m1 -i "thermal status")"
            echo "WAKE=$(dumpsys power 2>/dev/null | grep -m1 mWakefulness=)"
        ' 2>/dev/null | tr -d '\r')
        get() { printf '%s' "$line" | sed -n "s/^$1=//p" | head -1; }
        printf '#SAMPLE t=%s gpuclk=%s max_gpuclk=%s gpubusy="%s" gpu_busy_pct=%s gpu_temp=%s thermal="%s" wake="%s"\n' \
            "$t" "$(get CLK | tr -s ' ')" "$(get MAX)" "$(get BUSY | tr -s ' ')" \
            "$(get PCT)" "$(get TEMP)" "$(get THERM | tr -s ' ')" "$(get WAKE | tr -s ' ')" >> "$log"
        sleep "$SAMPLE_S"
        t=$((t + SAMPLE_S))
    done
}

# ── readers ──────────────────────────────────────────────────────────────────
# Each prints the stats JSON on stdout and returns 0, or returns non-zero.
# EVERY ONE IS BOUNDED, and none of them swallow stderr any more: v1 sent all
# reader stderr to /dev/null, which is why a CDP transport that was failing on
# every single call for four hours looked exactly like "the page isn't done
# yet". Errors now land in <config>.reader.err and in the log as #READERR.
READERR=""
note_err() {
    [ -s "$1" ] || return 0
    sed 's/^/#READERR /' "$1" >> "$LOG"
    READERR=$(head -1 "$1")
}

# The brace-balanced extraction lives in ONE place (the parser) because the
# logcat line wraps the JSON in Chromium's own unescaped quotes:
#   chromium: [INFO:CONSOLE(1)] "[stats] {"engine":"spark",...}", source: …
read_logcat() {
    adbt "$T_ADB_LOGCAT" logcat -d > "$1.full.logcat" 2>"$1.reader.err" || true
    note_err "$1.reader.err"
    grep -F '[stats]' "$1.full.logcat" > "$1.logcat" 2>/dev/null || true
    [ -s "$1.logcat" ] || return 1
    python3 "$PARSE" --extract-stats "$1.logcat" 2>/dev/null
}
read_cdp() {
    [ "$CDP_READY" = 1 ] || return 1
    perl -e 'alarm shift; exec @ARGV' "$T_CDP" \
        "$CDP" --port "$CDP_PORT" $CDP_MATCH \
        --eval 'JSON.stringify(window.__stats)' 2>"$1.reader.err"
    rc=$?
    note_err "$1.reader.err"
    return $rc
}
read_uiautomator() {
    adbt "$T_UIAUTO" shell uiautomator dump /sdcard/dxr_stats_dump.xml >/dev/null 2>&1 || return 1
    adbt "$T_ADB_FAST" shell "cat /sdcard/dxr_stats_dump.xml 2>/dev/null" > "$1.uixml" 2>/dev/null
    [ -s "$1.uixml" ] || return 1
    python3 "$PARSE" --extract-stats "$1.uixml" 2>/dev/null
}
# The phase, for progress only — never for the headline number.
peek_phase() {
    [ "$CDP_READY" = 1 ] || return 1
    perl -e 'alarm shift; exec @ARGV' "$T_CDP" \
        "$CDP" --port "$CDP_PORT" $CDP_MATCH \
        --eval '(window.__stats&&window.__stats.phase)||"?"' 2>"$1.reader.err"
    rc=$?
    note_err "$1.reader.err"
    return $rc
}

# Which window is actually in front. This one line would have diagnosed the
# whole 2026-09-22 failure in seconds: the answer was com.zte.mifavor.launcher
# and then com.android.systemui (the keyguard), not Chrome.
foreground() {
    adbt "$T_ADB_FAST" shell dumpsys window 2>/dev/null \
        | grep -m1 -E 'mCurrentFocus|mFocusedApp' | tr -d '\r' | tr -s ' '
}
awake() {
    adbt "$T_ADB_FAST" shell dumpsys power 2>/dev/null \
        | grep -m1 'mWakefulness=' | tr -d '\r' | tr -s ' '
}

# ── the run ──────────────────────────────────────────────────────────────────
for cfg in "${CFGS[@]}"; do
    label="${cfg%%|*}"
    engine=$(printf '%s' "$cfg" | cut -d'|' -f2)
    assetk=$(printf '%s' "$cfg" | cut -d'|' -f3)
    rs="${cfg##*|}"
    url=$(url_for "$engine" "$assetk" "$rs")
    LOG="$OUT/$label.log"
    : > "$LOG"

    # STRICT target selection. `am force-stop` does not close tabs, so by
    # config 3 the browser restores every earlier page and /json lists them all.
    # Matching only "dxr-splat-ab" would pick one at random: a backgrounded tab
    # whose rAF is throttled (never reaches done), or worse, an EARLIER
    # config's tab still holding its frozen done object — which would be
    # recorded as this config's result. Require the discriminating params.
    # `rs=$rs&` not `rs=$rs`: every URL puts `&stats=1` after rs, so the
    # trailing & makes the match exact and keeps rs=1 from matching an rs=1.5.
    CDP_MATCH="--match-all engine=$engine --match-all rs=$rs&"
    [ "$assetk" = "tahoe" ] && CDP_MATCH="$CDP_MATCH --match-all asset="

    say "════ $label  engine=$engine asset=$assetk rs=$rs ════"
    say "  $url"
    adbt "$T_ADB_FAST" shell am force-stop "$PKG" >/dev/null 2>&1
    sleep 2
    adbt "$T_ADB_FAST" logcat -c >/dev/null 2>&1

    {
        printf '#META config=%s engine=%s assetkey=%s rs=%s\n' "$label" "$engine" "$assetk" "$rs"
        printf '#META url=%s\n' "$url"
        printf '#META browser=%s device=%s (%s)\n' "$browser_ver" "$serial" "$pname"
        printf '#META warm_s=%s win_s=%s\n' "$WARM_S" "$WIN_S"
        printf '#META started=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf '#META cdp_match=%s\n' "$CDP_MATCH"
    } >> "$LOG"

    # `Browser.EXTRA_APPLICATION_ID` is upstream Chrome's "reuse the tab this
    # app opened last time" contract, so the run keeps ONE tab instead of
    # accumulating eight. UNVERIFIED on this fork (the patch repo carries no
    # Chromium source), which is why CDP still closes stale targets below —
    # this is the cheap half of a belt-and-braces pair, not the load-bearing one.
    am_extra=""
    [ "${REUSE_TAB:-1}" = 1 ] && am_extra="--es com.android.browser.application_id $PKG"
    # The URL carries '&', so it must survive BOTH this shell and the device's
    # shell: adb shell re-parses its argument. Single quotes inside the
    # double-quoted command string are what protect it.
    adbt "$T_ADB_FAST" shell "am start -a android.intent.action.VIEW -d '$url' -n $ACT $am_extra" \
        > "$OUT/$label.amstart" 2>&1 || say "WARNING: am start returned non-zero"
    sed 's/^/#AMSTART /' "$OUT/$label.amstart" >> "$LOG"
    sleep 3
    # The OEM rewrites user_rotation=0 at every activity launch, so the pin only
    # holds if re-asserted AFTER the launch — then gated, never trusted.
    pin_landscape
    printf '#META rotation=%s\n' "$(gate_landscape)" >> "$LOG"

    # GATE ON WHAT IS ACTUALLY IN FRONT. The 2026-09-22 run launched happily
    # ("Starting: Intent …" on every config) into a locked screen and measured
    # nothing for four hours. `am start` succeeding says nothing about
    # visibility.
    fg=$(foreground); wk=$(awake)
    printf '#META foreground=%s\n#META %s\n' "$fg" "$wk" >> "$LOG"
    say "  foreground: ${fg:-?}"
    case "$wk" in
        *Awake*) : ;;
        *) say "  WARNING: ${wk:-<no wakefulness read>} — the display is not awake. This config"
           say "           will measure NOTHING. Check stay-on; do not trust the numbers." ;;
    esac
    case "$fg" in
        *"$PKG"*) : ;;
        *) say "  WARNING: the foreground window is not $PKG. On 2026-09-22 this was"
           say "           com.zte.mifavor.launcher / com.android.systemui (keyguard)." ;;
    esac

    # Re-probe EVERY config (see probe_cdp). Cheap, and the cached-transport
    # assumption is what cost the last run.
    probe_cdp || true
    printf '#META cdp_ready=%s cdp_socket=%s\n' "$CDP_READY" "${CDP_SOCKET:-none}" >> "$LOG"

    # Log every devtools target, then close the ones that are not ours. A
    # wrong-tab read is invisible in the output otherwise.
    if [ "$CDP_READY" = 1 ]; then
        perl -e 'alarm shift; exec @ARGV' "$T_CDP" "$CDP" --port "$CDP_PORT" --list-brief \
            2>/dev/null | sed 's/^/#TARGETS /' >> "$LOG"
        perl -e 'alarm shift; exec @ARGV' "$T_CDP" "$CDP" --port "$CDP_PORT" \
            $CDP_MATCH --close-others 2>&1 | sed 's/^/#CLOSED /' >> "$LOG"
    fi

    sampler "$LOG" &
    SAMPLER_PID=$!

    say "  waiting for phase=done (page does ${WARM_S}s warm + ${WIN_S}s window; deadline ${STATS_DEADLINE_S}s WALL CLOCK)"
    stats=""
    reader=none
    lastphase=""
    fails=0
    # WALL CLOCK. v1 counted loop iterations x POLL_S and called it seconds; when
    # each iteration silently grew to 18-28 s, its "180 s deadline" expired after
    # 28 MINUTES, and one config never reached the iteration count at all and ran
    # for 2 h 55 m. Elapsed time is now measured, never inferred.
    t_start=$(date +%s)
    elapsed=0
    while [ "$elapsed" -lt "$STATS_DEADLINE_S" ]; do
        sleep "$POLL_S"
        elapsed=$(( $(date +%s) - t_start ))
        if [ "$CDP_READY" = 1 ]; then
            p=$(peek_phase "$OUT/$label")
            if [ -z "$p" ]; then
                fails=$((fails + 1))
                # Consecutive failures mean the TRANSPORT is gone, not that the
                # page is slow. Re-probe once, then give up on this config
                # rather than burning the whole deadline on a dead socket.
                if [ "$fails" = 5 ]; then
                    say "    5 consecutive CDP failures (${READERR:-no stderr}) — re-probing the socket"
                    probe_cdp || true
                    printf '#META cdp_reprobe=%s socket=%s t=%s\n' \
                        "$CDP_READY" "${CDP_SOCKET:-none}" "$elapsed" >> "$LOG"
                fi
                if [ "$fails" -ge 12 ]; then
                    say "    giving up on CDP for this config after $fails failures"
                    break
                fi
            else
                fails=0
                if [ "$p" != "$lastphase" ]; then
                    lastphase="$p"
                    printf '#PHASE t=%s phase=%s\n' "$elapsed" "$p" >> "$LOG"
                    say "    t=${elapsed}s phase=$p"
                fi
                if [ "$p" = "done" ]; then
                    stats=$(read_cdp "$OUT/$label")
                    if [ -n "$stats" ]; then reader=cdp; break; fi
                fi
            fi
        fi
        # Demoted to a fallback, and no longer run every poll: a full `logcat -d`
        # dump every 3 s is minutes of USB traffic per config on its own.
        if [ "$CDP_READY" != 1 ] && [ $((elapsed % 15)) -lt "$POLL_S" ]; then
            stats=$(read_logcat "$OUT/$label")
            if [ -n "$stats" ]; then reader=logcat; break; fi
        fi
    done
    done_t="$elapsed"

    # Last-resort readers, only if the loop produced nothing.
    if [ -z "$stats" ]; then
        say "  no stats from CDP after ${done_t}s — trying logcat, then uiautomator"
        stats=$(read_logcat "$OUT/$label")
        [ -n "$stats" ] && reader=logcat
    fi
    if [ -z "$stats" ]; then
        stats=$(read_uiautomator "$OUT/$label")
        [ -n "$stats" ] && reader=uiautomator
    fi

    pkill -P "$SAMPLER_PID" 2>/dev/null
    kill "$SAMPLER_PID" 2>/dev/null; wait "$SAMPLER_PID" 2>/dev/null

    # State at the moment of the verdict, so a failure is self-diagnosing.
    { printf '#META foreground_end=%s\n#META end_%s\n' "$(foreground)" "$(awake)"; } >> "$LOG"
    # Full logcat ONCE, at the end — not in the poll loop.
    [ -s "$OUT/$label.full.logcat" ] || adbt "$T_ADB_LOGCAT" logcat -d > "$OUT/$label.full.logcat" 2>/dev/null

    # Evidence a human can read even if every machine reader failed.
    perl -e 'alarm shift; exec @ARGV' "$T_SCREENCAP" adb exec-out screencap -p > "$OUT/$label.png" 2>/dev/null
    [ -s "$OUT/$label.png" ] || rm -f "$OUT/$label.png"

    # The lens-MCU wedge signature. Taken from the full dump we already have.
    grep -i 'leiadisp@1.0-service.*select() timeout' "$OUT/$label.full.logcat" 2>/dev/null \
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
        say "  WARNING: no window.__stats after ${done_t}s from any reader."
        say "           last reader error: ${READERR:-<none recorded>}"
        say "           $OUT/$label.png is the only evidence for this config."
    fi

    adbt "$T_ADB_FAST" shell am force-stop "$PKG" >/dev/null 2>&1
    sleep "$COOLDOWN_S"
done

say "all ${#CFGS[@]} configs done"
# cleanup() runs from the EXIT trap: forward removed, cmdline restored, rotation
# freed, lock released.
