#!/usr/bin/env bash
# Gaussian-splat GPU benchmark on the NP02J (Nubia Pad 3D II, SD 8 Gen 2 /
# Adreno 740, panel 1600x2560), driven entirely from this box over adb.
#
# It measures the Adreno graphics-path splat renderer per stage, via the app's
# own throttled `GS_TS[adreno]` line (one line per 120 renderEye calls, and
# TOTAL is PER EYE), across a keep-fraction ladder on two photo-lifted clouds.
#
#   asset               keep   gaussians    rig
#   ports_hint_only.sog 1.00   1,179,648    camera (hint-only block -> estimated
#                       0.50     589,824    intrinsics + median-disparity focus)
#                       0.42     495,452
#                       0.25     294,912
#   tahoe_mono.sog      same ladder, 1,179,648 at 1.00, full v2 camera block
#                       (fx=fy=1471.7, cx=1024, cy=768, 2048x1536)
#
# ONE file per asset plus `debug.dxr.gs.keep`, not four pre-decimated files: the
# Android arm decimates at LOAD, hash-selected so the thinning is spatially
# uniform, so the ladder is a property of the run and not of the asset.
#
# ─────────────────────────── THE PAD PROTOCOL ────────────────────────────────
# This device is shared. Read ~/.claude/.../feedback_pad_lock_protocol.md and
# feedback_tablet_launch_landscape.md before changing anything here. The rules
# this script implements, and why each one is not optional:
#
#  * THE LOCK GATES THE WHOLE RUN, not just the lock write. Two sessions in
#    three days installed APKs, set props and pinned rotation under another
#    session's live measurement because they gated only their own `echo >
#    pad.lock`. Every device WRITE below sits after the gate, and the gate
#    exits 1 on a non-empty lock. An EMPTY lock file means held-by-unknown and
#    is also a refusal.
#  * THE SERIAL IS PINNED. Two pads have shared one USB cable; a lock
#    "appearing from nowhere" is first evidence of a different device, not of a
#    rogue writer. `ro.product.model` cannot discriminate these units, so this
#    checks `ro.serialno` and prints `ro.product.name`.
#  * THE LOCK IS WRITTEN BY pad-lock.sh, NEVER BY HAND. It is the only ATOMIC
#    claim (`set -C`) and the only writer that reads back and proves the lock is
#    ours. A plain `echo >` destroyed a claim made 17 s earlier, for real.
#  * ROTATION IS PINNED FOR THE RUN AND FREED AT THE END. Landscape is the
#    measured orientation; portrait silently invalidates the numbers. The OEM
#    rewrites user_rotation at every activity launch, so the pin is RE-ASSERTED
#    after each `am start` and then GATED on the measured rotation. The resting
#    state David asked for is auto-rotate ON — accelerometer_rotation 1,
#    user_rotation 1, fixed-to-user-rotation DISABLED — and the EXIT trap
#    restores exactly that, whatever went wrong.
#  * usagestats IS SNAPSHOT AT BOTH ENDS. logcat rolls within minutes on this
#    unit, so it is the only artefact that can later prove no foreign app
#    launch landed inside the measurement window.
#
# ────────────────────────────────── USAGE ────────────────────────────────────
#   ./scratchpad/bench_android.sh                 # the full run (~25-30 min)
#   ./scratchpad/bench_android.sh --dry-run       # print the plan, touch nothing
#   ./scratchpad/bench_android.sh --restore       # put the released APK back
#
# Env overrides: APK, ASSET_SRC, HANDLE, PAD_SERIAL, WARMUP_S, MIN_TS_LINES,
# CAPTURE_MAX_S, SAMPLE_S, BENCH_INCLUDE_PORTS50=1.
#
# Output: scratchpad/android_bench/<config>.log — one file per config, holding
# the raw logcat plus `#SAMPLE` / `#MEM` / `#META` lines. Feed the directory to
# scratchpad/bench_android_parse.py.

set -u

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$HERE/.." && pwd)

# ── configuration ────────────────────────────────────────────────────────────
PAD_SERIAL="${PAD_SERIAL:-327343950099}"
HANDLE="${HANDLE:-mac/help-amazon}"
PURPOSE="${PURPOSE:-splat benchmark ~30 min}"
PKG=com.displayxr.gausssplat_vk_android
ACT="$PKG/.MainActivity"
APK="${APK:-$REPO/android/build/outputs/apk/release/android-release.apk}"
ASSET_SRC="${ASSET_SRC:-$HERE/assets}"
OUT="${OUT:-$HERE/android_bench}"

# getExternalFilesDir(): readable by the app with NO runtime permission, and
# writable by `adb push`. /sdcard/Download is NOT usable — this APK declares no
# storage permission, and under scoped storage it could not read it anyway; and
# /data/local/tmp is unreadable from an untrusted_app SELinux domain. See the
# search-order comment in android/src/main/cpp/main.cpp::resolve_scene_override.
DEV_ASSETS="/sdcard/Android/data/$PKG/files"

WARMUP_S="${WARMUP_S:-30}"        # discarded: thermal + shader-cache settling
MIN_TS_LINES="${MIN_TS_LINES:-14}"
CAPTURE_MAX_S="${CAPTURE_MAX_S:-180}"
SAMPLE_S="${SAMPLE_S:-5}"
LOAD_TIMEOUT_S="${LOAD_TIMEOUT_S:-120}"

PAD_LOCK_SH="${PAD_LOCK_SH:-$HOME/Documents/GitHub/displayxr-installer/android-bundle/scripts/pad-lock.sh}"
LOCK=/data/local/tmp/pad.lock

# config list: "<label>|<asset file>|<keep fraction>"
CFGS=(
    "ports_k100|ports_hint_only.sog|1.0"
    "ports_k050|ports_hint_only.sog|0.5"
    "ports_k042|ports_hint_only.sog|0.42"
    "ports_k025|ports_hint_only.sog|0.25"
    "tahoe_k100|tahoe_mono.sog|1.0"
    "tahoe_k050|tahoe_mono.sog|0.5"
    "tahoe_k042|tahoe_mono.sog|0.42"
    "tahoe_k025|tahoe_mono.sog|0.25"
)
# Cross-check that load-time decimation and an offline 50 % decimation land in
# the same place. Off by default — it costs another ~2 min and answers a
# different question from the ladder.
if [ "${BENCH_INCLUDE_PORTS50:-0}" = "1" ]; then
    CFGS+=("ports50_k100|ports_50.sog|1.0")
fi

ASSETS=$(printf '%s\n' "${CFGS[@]}" | cut -d'|' -f2 | sort -u)

DRY=0
MODE=run
for a in "$@"; do
    case "$a" in
        --dry-run) DRY=1 ;;
        --restore) MODE=restore ;;
        -h|--help) sed -n '2,70p' "$0"; exit 0 ;;
        *) echo "unknown argument: $a" >&2; exit 2 ;;
    esac
done

export ANDROID_SERIAL="$PAD_SERIAL"
say() { printf '%s  %s\n' "$(date -u +%H:%M:%SZ)" "$*"; }
die() { printf 'FATAL: %s\n' "$*" >&2; exit 1; }

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
say "device $serial ($pname) — ro.product.model=$(adb shell getprop ro.product.model | tr -d '\r')"

if [ "$MODE" = restore ]; then
    # Only ever run when the pad is free or already ours; same gate as the run.
    L=$(adb shell "cat $LOCK 2>/dev/null" | tr -d '\r')
    if [ -n "$L" ] && [ "${L%% *}" != "$HANDLE" ]; then
        echo "HELD: $L"; exit 1
    fi
    # Restore the EXACT build that was here, not merely "the latest release":
    # the run records it, because the pad is shared and the next session's test
    # may key on that version. RESTORE_TAG overrides; no record and no override
    # falls back to the latest release, and says so.
    tag="${RESTORE_TAG:-}"
    if [ -z "$tag" ] && [ -f "$OUT/installed_before.txt" ]; then
        tag=$(sed -n 's/^versionName=//p' "$OUT/installed_before.txt" | head -1)
    fi
    tmp=$(mktemp -d)
    if [ -n "$tag" ]; then
        say "restoring $tag (recorded before the run)"
        gh release download "$tag" -R DisplayXR/displayxr-demo-gaussiansplat -p '*.apk' -D "$tmp" \
            || die "could not download the APK for $tag (gh release view $tag -R DisplayXR/displayxr-demo-gaussiansplat)"
    else
        say "no recorded pre-run version and no RESTORE_TAG — falling back to the LATEST release"
        gh release download -R DisplayXR/displayxr-demo-gaussiansplat -p '*.apk' -D "$tmp" \
            || die "could not download the released APK (try: gh release list -R DisplayXR/displayxr-demo-gaussiansplat)"
    fi
    apk=$(find "$tmp" -name '*.apk' | head -1)
    adb uninstall "$PKG" >/dev/null 2>&1
    adb install "$apk" || die "install of $apk failed"
    adb shell dumpsys package "$PKG" | grep -m1 versionName
    exit 0
fi

[ -f "$APK" ] || die "APK not found: $APK  (build it: ./gradlew :android:assembleRelease)"
for f in $ASSETS; do
    [ -f "$ASSET_SRC/$f" ] || die "asset not found: $ASSET_SRC/$f"
done
aapt2=$(ls -d "${ANDROID_HOME:-$HOME/Library/Android/sdk}"/build-tools/*/aapt2 2>/dev/null | sort -V | tail -1)
apk_ver="unknown"
[ -n "$aapt2" ] && apk_ver=$("$aapt2" dump badging "$APK" | sed -n "s/.*versionName='\([^']*\)'.*/\1/p" | head -1)

installed=$(adb shell dumpsys package "$PKG" 2>/dev/null | sed -n 's/.*versionName=\(.*\)/\1/p' | head -1 | tr -d '\r')
installed_code=$(adb shell dumpsys package "$PKG" 2>/dev/null | sed -n 's/.*versionCode=\([0-9]*\).*/\1/p' | head -1 | tr -d '\r')
say "installed $PKG: versionName=${installed:-<absent>} versionCode=${installed_code:-<absent>}"
say "bench APK:    $APK (versionName=$apk_ver)"
say "NOTE: the bench APK is signed with a LOCAL dev key, so it cannot replace the"
say "      released APK in place — the run UNINSTALLS ${installed:-the released build} first."
say "      Put it back afterwards with:  $0 --restore"

say "configs (${#CFGS[@]}):"
for c in "${CFGS[@]}"; do
    printf '    %-12s asset=%-20s keep=%s\n' "${c%%|*}" "$(echo "$c" | cut -d'|' -f2)" "${c##*|}"
done

# ── THE GATE. Every device WRITE below this point depends on it. ─────────────
L=$(adb shell "cat $LOCK 2>/dev/null" | tr -d '\r')
if adb shell "test -e $LOCK" 2>/dev/null && [ -z "$L" ]; then
    echo "HELD: empty lock file = held by unknown. A human must resolve this."
    exit 1
fi
if [ -n "$L" ]; then
    echo "HELD: $L"
    echo "Do nothing. Message the holder; never clear a foreign lock outside the"
    echo "four-step liveness rule (displayxr-installer#56)."
    exit 1
fi
say "lock is FREE"

if [ "$DRY" = 1 ]; then
    say "--dry-run: stopping before the first device write. Nothing was changed."
    exit 0
fi

# ── take the lock (atomic, proves it is ours), then arm the restore trap ─────
[ -x "$PAD_LOCK_SH" ] || die "pad-lock.sh not found at $PAD_LOCK_SH — it is the ONLY sanctioned writer of $LOCK; do not hand-write one"
PAD_SERIAL="$PAD_SERIAL" "$PAD_LOCK_SH" take "$HANDLE" "$PURPOSE" || die "could not take the pad lock"
HELD_BY_US=1

mkdir -p "$OUT"

cleanup() {
    rc=$?
    say "── cleanup ──"
    adb shell am force-stop "$PKG" >/dev/null 2>&1
    # Clear our props. An empty value reads back as unset, so the next launch
    # is the bundled butterfly at keep=1.0 again.
    adb shell setprop debug.dxr.gs.scene '""' >/dev/null 2>&1
    adb shell setprop debug.dxr.gs.keep '""' >/dev/null 2>&1
    # Resting state (David, 2026-09-07/08): auto-rotate ON, user_rotation 1
    # (landscape fallback), fixed-to-user-rotation DISABLED. Never hand the pad
    # back pinned, and never hand it back in portrait.
    adb shell wm fixed-to-user-rotation disabled >/dev/null 2>&1
    adb shell wm user-rotation free >/dev/null 2>&1
    adb shell settings put system accelerometer_rotation 1 >/dev/null 2>&1
    adb shell settings put system user_rotation 1 >/dev/null 2>&1
    ar=$(adb shell settings get system accelerometer_rotation 2>/dev/null | tr -d '\r')
    ur=$(adb shell settings get system user_rotation 2>/dev/null | tr -d '\r')
    fx=$(adb shell wm fixed-to-user-rotation 2>/dev/null | tr -d '\r')
    # mCurrentOrientation is NOT a settable target: with auto-rotate ON the
    # display follows how the device is lying, so 0 here is correct. Gate on
    # accelerometer_rotation=1 + fixed-to-user-rotation disabled instead.
    say "rotation restored: accelerometer_rotation=$ar user_rotation=$ur fixed-to-user-rotation='$fx' (want 1 / 1 / disabled)"
    adb shell dumpsys usagestats > "$OUT/usagestats_end.txt" 2>/dev/null
    if [ "${HELD_BY_US:-0}" = 1 ]; then
        PAD_SERIAL="$PAD_SERIAL" "$PAD_LOCK_SH" release "$HANDLE" || \
            say "WARNING: could not release the lock — check it by hand: adb shell cat $LOCK"
    fi
    say "logs: $OUT/<config>.log   parse with: python3 $HERE/bench_android_parse.py $OUT"
    say "restore the released APK:  $0 --restore   (it was ${installed:-<absent>})"
    exit $rc
}
trap cleanup EXIT INT TERM

adb shell dumpsys usagestats > "$OUT/usagestats_start.txt" 2>/dev/null
printf 'versionName=%s\nversionCode=%s\n' "${installed:-}" "${installed_code:-}" > "$OUT/installed_before.txt"

# ── install ──────────────────────────────────────────────────────────────────
say "uninstalling $PKG (was ${installed:-<absent>}) — signature differs, so -r cannot work"
adb uninstall "$PKG" >/dev/null 2>&1
say "installing $APK"
adb install "$APK" || die "adb install failed"
adb shell dumpsys package "$PKG" | sed -n 's/.*versionName=\(.*\)/    installed now: versionName=\1/p' | head -1

# ── push assets ──────────────────────────────────────────────────────────────
adb shell mkdir -p "$DEV_ASSETS" >/dev/null 2>&1
for f in $ASSETS; do
    say "push $f -> $DEV_ASSETS/"
    adb push "$ASSET_SRC/$f" "$DEV_ASSETS/$f" >/dev/null || die "push of $f failed"
done
adb shell ls -l "$DEV_ASSETS" | sed 's/^/    /'

# ── pin landscape for the run ────────────────────────────────────────────────
pin_landscape() {
    adb shell settings put system accelerometer_rotation 0 >/dev/null 2>&1
    adb shell wm user-rotation lock 1 >/dev/null 2>&1
    adb shell wm fixed-to-user-rotation enabled >/dev/null 2>&1
}
gate_landscape() {
    r=$(adb shell dumpsys window displays 2>/dev/null | grep -o 'rotation=[0-9]' | head -1 | tr -d '\r')
    # The warning goes to STDERR: this function's stdout is captured by its
    # callers, and a warning mixed into it would be recorded as the rotation.
    [ "$r" = "rotation=1" ] || say "WARNING: ${r:-<no rotation read>} (wanted rotation=1) — this config is measured in the wrong orientation" >&2
    printf '%s' "$r"
}
pin_landscape
say "landscape pinned: $(gate_landscape)"

# ── sampler: gpu clock / busy / thermal / meminfo, every SAMPLE_S ────────────
sampler() {
    log="$1"
    t=0
    while :; do
        clk=$(adb shell cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null | tr -d '\r')
        mclk=$(adb shell cat /sys/class/kgsl/kgsl-3d0/max_gpuclk 2>/dev/null | tr -d '\r')
        busy=$(adb shell cat /sys/class/kgsl/kgsl-3d0/gpubusy 2>/dev/null | tr -d '\r' | tr -s ' ')
        pct=$(adb shell cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage 2>/dev/null | tr -d '\r')
        th=$(adb shell dumpsys thermalservice 2>/dev/null | grep -m1 -iE 'thermal status|mStatus' | tr -d '\r' | tr -s ' ')
        printf '#SAMPLE t=%s gpuclk=%s max_gpuclk=%s gpubusy="%s" gpu_busy_pct=%s thermal="%s"\n' \
            "$t" "${clk:-NA}" "${mclk:-NA}" "${busy:-NA}" "${pct:-NA}" "${th:-NA}" >> "$log"
        mem=$(adb shell dumpsys meminfo "$PKG" 2>/dev/null | grep -iE 'graphics|gl mtrack|^ *TOTAL' | tr -d '\r' | tr -s ' ' | tr '\n' ';')
        printf '#MEM t=%s %s\n' "$t" "${mem:-NA}" >> "$log"
        sleep "$SAMPLE_S"
        t=$((t + SAMPLE_S))
    done
}

# ── the run ──────────────────────────────────────────────────────────────────
for cfg in "${CFGS[@]}"; do
    label="${cfg%%|*}"
    asset=$(printf '%s' "$cfg" | cut -d'|' -f2)
    keep="${cfg##*|}"
    LOG="$OUT/$label.log"
    : > "$LOG"

    say "════ $label  asset=$asset keep=$keep ════"
    adb shell am force-stop "$PKG" >/dev/null 2>&1

    # The prop takes a BARE FILENAME on purpose: PROP_VALUE_MAX is 92 bytes and
    # "$DEV_ASSETS/" alone is 63 of them, so a full path would be silently
    # truncated and the miss would look like "file not found".
    adb shell setprop debug.dxr.gs.scene "$asset"
    adb shell setprop debug.dxr.gs.keep "$keep"
    {
        printf '#META config=%s asset=%s keep=%s\n' "$label" "$asset" "$keep"
        printf '#META apk=%s device=%s (%s)\n' "$apk_ver" "$serial" "$pname"
        printf '#META started=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf '#META prop.scene=%s prop.keep=%s\n' \
            "$(adb shell getprop debug.dxr.gs.scene | tr -d '\r')" \
            "$(adb shell getprop debug.dxr.gs.keep | tr -d '\r')"
    } >> "$LOG"

    adb logcat -c >/dev/null 2>&1
    adb shell am start -n "$ACT" >/dev/null 2>&1 || say "WARNING: am start returned non-zero"
    sleep 2
    # The OEM rewrites user_rotation=0 at every activity launch, so the pin only
    # holds if it is re-asserted AFTER the launch — then gated, never trusted.
    pin_landscape
    printf '#META rotation=%s\n' "$(gate_landscape)" >> "$LOG"

    # Wait for the scene to actually load. `Loaded <path>: N gaussians` is the
    # app's own line; anything else means the override did not take and the
    # bundled butterfly is on screen, which would silently measure nothing.
    say "waiting for the scene load line (<= ${LOAD_TIMEOUT_S}s)"
    loaded=""
    waited=0
    while [ "$waited" -lt "$LOAD_TIMEOUT_S" ]; do
        loaded=$(adb logcat -d 2>/dev/null | grep -m1 -E 'Loaded .*: [0-9]+ gaussians')
        [ -n "$loaded" ] && break
        sleep 3
        waited=$((waited + 3))
    done
    if [ -z "$loaded" ]; then
        say "WARNING: no scene-load line after ${LOAD_TIMEOUT_S}s — recording what there is and moving on"
    else
        say "  ${loaded##*gausssplat_vk_android: }"
    fi
    adb logcat -d > "$OUT/$label.load.log" 2>/dev/null
    grep -E 'Auto-loading scene|Loaded .*gaussians|Photo-lift signature|Camera rig|GsAdreno: knobs|View [0-9]+ swapchain|Atlas blit|Startup rendering mode' \
        "$OUT/$label.load.log" 2>/dev/null | sed 's/^/#LOAD /' >> "$LOG"
    case "$loaded" in
        *"$asset"*) : ;;
        "") : ;;
        *) say "WARNING: the loaded path does not mention $asset — the override did NOT take" ;;
    esac

    say "warm-up ${WARMUP_S}s (discarded)"
    sleep "$WARMUP_S"

    # ── measurement window ──
    adb logcat -c >/dev/null 2>&1
    printf '#META capture_started=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$LOG"
    adb logcat -v time >> "$LOG" 2>&1 &
    LOGCAT_PID=$!
    sampler "$LOG" &
    SAMPLER_PID=$!

    n=0
    waited=0
    while [ "$waited" -lt "$CAPTURE_MAX_S" ]; do
        sleep 5
        waited=$((waited + 5))
        # grep -c prints 0 AND exits 1 on no match; `|| true` keeps the count
        # (a `|| echo 0` here would produce the two-line "0\n0" that breaks -ge).
        n=$(grep -c 'GS_TS\[adreno\]' "$LOG" 2>/dev/null || true)
        [ "$n" -ge "$MIN_TS_LINES" ] && break
    done
    # pkill -P first: the sampler's own `sleep` / `adb` children outlive a kill
    # of the loop alone and would keep appending to a log we have closed off.
    pkill -P "$SAMPLER_PID" 2>/dev/null
    kill "$SAMPLER_PID" 2>/dev/null; wait "$SAMPLER_PID" 2>/dev/null
    kill "$LOGCAT_PID"  2>/dev/null; wait "$LOGCAT_PID"  2>/dev/null
    printf '#META capture_ended=%s capture_s=%s ts_lines=%s\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$waited" "$n" >> "$LOG"

    if [ "$n" -lt "$MIN_TS_LINES" ]; then
        say "WARNING: only $n GS_TS lines in ${waited}s (wanted >= $MIN_TS_LINES) — under-sampled"
    else
        say "$n GS_TS lines in ${waited}s"
    fi

    adb shell am force-stop "$PKG" >/dev/null 2>&1
    sleep 2
done

say "all ${#CFGS[@]} configs done"
# cleanup() runs from the EXIT trap: props cleared, rotation freed, lock released.
