#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""Turn bench_web_android.sh's per-config logs into a table, beside the native rows.

    python3 scratchpad/bench_web_android_parse.py scratchpad/web_bench

Also the single implementation of the `[stats]` extraction, so the runner and
the table can never disagree about what a line means:

    python3 scratchpad/bench_web_android_parse.py --extract-stats <file>

reads a logcat excerpt OR a uiautomator XML dump and prints the compact stats
JSON on stdout (exit 1 if there is none).

──────────────────────────────── the caveat ─────────────────────────────────
The web and native numbers are NOT the same quantity, and the table says so on
every run rather than trusting the reader to remember:

* the web `ms_median` is a **wall-clock frame time** — everything the page does,
  including compositing and present;
* the native `GS_TS[adreno] TOTAL` is **GPU stage time for ONE view**, emitted
  once per 120 `renderEye` calls, and excludes the runtime's weave entirely.

So the honest cross-reads are web `gpu_ms_median` (a WebGL timer query, when
`timerQuery` is true) against native `2 x TOTAL/view`, and `fps` against the
native renderer-only ceiling `1000 / (2 x TOTAL/view)` — a CEILING, never a
measured frame rate. The columns are labelled that way; do not quote the native
ceiling as an fps this device was observed to hit.

A row whose `mode` is `mono` is flagged **MONO** and must not be quoted as a
2-view result: a hands-off `am start` that never engages the 3D session renders
one view, which is a different amount of work.
"""

import argparse
import html
import importlib.util
import json
import os
import re
import statistics
import sys

SAMPLE_RE = re.compile(r"^#SAMPLE\s+(?P<body>.*)$")
META_RE = re.compile(r"^#META\s+(?P<body>.*)$")
STATS_RE = re.compile(r"^#STATS\s+(?P<body>.*)$")
WEDGE_RE = re.compile(r"^#WEDGE\s")
PHASE_RE = re.compile(r"^#PHASE\s+(?P<body>.*)$")
KV_RE = re.compile(r'(\w+)=("([^"]*)"|\S+)')

# The Mac reference the coordinator supplied, for scale. It is a DIFFERENT
# machine, a different panel and a mono 1280x720 canvas — it bounds nothing on
# the pad, it only says which order of magnitude to expect.
MAC_REF = [
    ("playcanvas", "mono 1280x720 rs=1", "60 fps (vsync-capped)", "—"),
    ("spark", "mono 1280x720 rs=1", "47 fps", "p95 50 ms"),
]

# Order the table in the order the run executes them (engines paired), not
# alphabetically — a reader comparing two engines should find them adjacent.
DEFAULT_ORDER = ["pc_ports_rs100", "sp_ports_rs100", "pc_ports_rs060", "sp_ports_rs060",
                 "pc_tahoe_rs100", "sp_tahoe_rs100", "pc_tahoe_rs060", "sp_tahoe_rs060"]


def kv(body):
    out = {}
    for m in KV_RE.finditer(body):
        out[m.group(1)] = m.group(3) if m.group(3) is not None else m.group(2)
    return out


# ───────────────────────── the `[stats]` extraction ──────────────────────────
def _balanced_object(text, start):
    """Return the JSON object beginning at text[start] == '{', or None.

    String-aware, so it is not fooled by a brace inside a value — and, more to
    the point here, not fooled by Chromium wrapping the whole console message in
    its OWN quotes without escaping the JSON's:

        chromium: [INFO:CONSOLE(1)] "[stats] {"engine":"spark",...}", source: …

    Scanning from the '{' with brace balance terminates exactly at the matching
    '}', leaving the trailing `", source: …` outside.
    """
    depth = 0
    in_str = False
    esc = False
    for i in range(start, len(text)):
        c = text[i]
        if in_str:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
            continue
        if c == '"':
            in_str = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    return None


def extract_stats(text):
    """Pull the stats object out of a logcat excerpt or a uiautomator dump."""
    # A uiautomator dump is XML, so the div's text arrives entity-escaped
    # (&quot;engine&quot;). Unescape before looking for JSON.
    if "<?xml" in text[:200] or "&quot;" in text:
        text = html.unescape(text)

    cands = []
    for m in re.finditer(r"\[stats\]", text):
        b = text.find("{", m.end())
        if b != -1:
            cands.append(b)
    if not cands:
        # uiautomator path: the div holds the same JSON with no `[stats]` tag.
        cands = [m.start() for m in re.finditer(r'\{\s*"', text)]

    best = None
    for b in cands:
        raw = _balanced_object(text, b)
        if not raw:
            continue
        try:
            obj = json.loads(raw)
        except ValueError:
            continue
        if not isinstance(obj, dict):
            continue
        if not ({"engine", "phase", "ms_median", "fps"} & set(obj)):
            continue
        # The page logs `[stats]` once, at done — but if a run somehow produced
        # more than one, the LAST complete one wins, and a `done` beats any
        # other phase.
        if best is None or obj.get("phase") == "done":
            best = obj
    return best


# ───────────────────────────── log -> row ────────────────────────────────────
def parse_log(path):
    r = {"config": os.path.splitext(os.path.basename(path))[0],
         "engine": "?", "assetkey": "?", "rs": "?", "url": "",
         "rotation": "?", "browser": "?", "reader": "none",
         "warm_s": None, "win_s": None, "done_t": None,
         "cdp_ready": "?", "stats": None, "samples": [], "phases": [],
         "wedge": 0, "foreground": "", "foreground_end": "", "readerr": []}
    with open(path, errors="replace") as fh:
        for line in fh:
            m = STATS_RE.match(line)
            if m:
                r["stats"] = extract_stats(m.group("body")) or r["stats"]
                continue
            m = META_RE.match(line)
            if m:
                f = kv(m.group("body"))
                for k in ("config", "engine", "assetkey", "rs", "url", "rotation",
                          "browser", "reader", "cdp_ready", "foreground",
                          "foreground_end"):
                    if k in f:
                        r[k] = f[k]
                for k in ("warm_s", "win_s", "done_t"):
                    if k in f:
                        try:
                            r[k] = float(f[k])
                        except ValueError:
                            pass
                continue
            m = SAMPLE_RE.match(line)
            if m:
                f = kv(m.group("body"))
                s = {"t": float(f.get("t", 0) or 0)}
                clk = f.get("gpuclk", "NA")
                s["clk"] = int(clk) if clk.isdigit() else None
                th = f.get("thermal", "")
                n = re.search(r"(?:mStatus|[Ss]tatus)\D*(\d+)", th)
                s["thermal"] = int(n.group(1)) if n else None
                tp = f.get("gpu_temp", "NA")
                s["temp"] = int(tp) if tp.lstrip("-").isdigit() else None
                s["wake"] = f.get("wake", "")
                s["busy0"] = f.get("gpubusy", "").strip() in ("0 0", "0  0", "")
                r["samples"].append(s)
                continue
            m = PHASE_RE.match(line)
            if m:
                r["phases"].append(kv(m.group("body")))
                continue
            if WEDGE_RE.match(line):
                r["wedge"] += 1
            if line.startswith("#READERR"):
                r["readerr"].append(line[len("#READERR"):].strip())
    return r


def window_samples(r):
    """The samples inside the page's own measurement window.

    The page measures the LAST `win_s` seconds before `done`, so the GPU clock
    that belongs beside its numbers is the clock over that slice — not over the
    whole config, most of which is loading and warm-up at a different clock.
    """
    if r["done_t"] is None or r["win_s"] is None:
        return r["samples"]
    lo, hi = r["done_t"] - r["win_s"], r["done_t"]
    sel = [s for s in r["samples"] if lo <= s["t"] <= hi]
    return sel or r["samples"]


def fmt(v, nd=2):
    if v is None:
        return "—"
    if isinstance(v, str):
        return v
    return ("%%.%df" % nd) % v


def mhz(hz):
    return "—" if hz is None else str(int(round(hz / 1e6)))


# ─────────────────────────── the native comparison ───────────────────────────
def load_native(path, here):
    """Native rows: a pre-baked .md table, or bench_android.sh's raw logs.

    Returns (kind, payload) where kind is 'md' (verbatim text) or 'rows'.
    """
    if path and os.path.isfile(path) and path.endswith(".md"):
        with open(path, errors="replace") as fh:
            return "md", fh.read()
    if not path or not os.path.isdir(path):
        return None, None
    mod_path = os.path.join(here, "bench_android_parse.py")
    if not os.path.isfile(mod_path):
        return None, None
    spec = importlib.util.spec_from_file_location("bench_android_parse", mod_path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    rows = []
    for f in sorted(os.listdir(path)):
        if not f.endswith(".log") or f.endswith(".load.log"):
            continue
        rr = mod.parse_log(os.path.join(path, f))
        rr["sum"] = mod.summarise(rr)
        rows.append(rr)
    return "rows", rows


def print_native(kind, payload):
    print("\n## Native Vulkan splat app on the same panel (for scale)\n")
    if kind == "md":
        print(payload.rstrip())
        return
    if kind != "rows" or not payload:
        print("_No native rows found. Run `scratchpad/bench_android.sh`, then pass its"
              " output directory with `--native`._")
        return
    print("From `bench_android.sh` — the app's `GS_TS[adreno]` line, **GPU stage time")
    print("for ONE view**, excluding the runtime's weave. `2 x TOTAL/view` is the")
    print("two-view renderer cost; the ceiling column is `1000 / (2 x TOTAL/view)` and")
    print("is an upper bound on frame rate, **not a measured fps**.\n")
    print("| config | asset | keep | N | render | TOTAL/view ms | 2 x TOTAL ms | "
          "renderer-only ceiling fps | GPU clock min/med/max MHz |")
    print("|" + "---|" * 9)
    for r in payload:
        s = r["sum"]
        if not s:
            print("| %s | %s | %s | — | — | **no GS_TS lines** | — | — | — |"
                  % (r["config"], r["asset"], r["keep"]))
            continue
        two = 2 * s["total"]
        clk = r["clk"]
        clks = ("%d/%d/%d" % (min(clk) // 10**6,
                              int(statistics.median(clk)) // 10**6,
                              max(clk) // 10**6)) if clk else "—"
        print("| %s | %s | %s | %s | %s | %s | %s | %s | %s |"
              % (r["config"], r["asset"], r["keep"], "{:,}".format(s["N"]),
                 s["render"], fmt(s["total"]), fmt(two), fmt(1000.0 / two, 1), clks))


# ─────────────────────────────────── main ────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", nargs="?", help="directory of <config>.log files")
    ap.add_argument("--extract-stats", metavar="FILE",
                    help="print the stats JSON found in FILE and exit")
    ap.add_argument("--native", default=None,
                    help="native rows: a .md table, or a bench_android.sh output dir. "
                         "Default: ../results/android_np02j_table.md, else ../android_bench")
    ap.add_argument("--order", default="", help="comma-separated config order")
    a = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))

    if a.extract_stats:
        try:
            with open(a.extract_stats, errors="replace") as fh:
                text = fh.read()
        except OSError:
            return 1
        st = extract_stats(text)
        if st is None:
            return 1
        print(json.dumps(st, separators=(",", ":")))
        return 0

    if not a.dir:
        ap.error("a directory of <config>.log files is required")
    logs = sorted(f for f in os.listdir(a.dir) if f.endswith(".log"))
    if not logs:
        sys.exit("no <config>.log files in %s" % a.dir)
    order = [x.strip() for x in a.order.split(",") if x.strip()] or DEFAULT_ORDER
    logs.sort(key=lambda f: order.index(os.path.splitext(f)[0])
              if os.path.splitext(f)[0] in order else len(order))

    rows = [parse_log(os.path.join(a.dir, f)) for f in logs]

    print("## Web splat viewer in the Android DisplayXR Browser — NP02J "
          "(SD 8 Gen 2 / Adreno 740, 1600x2560)\n")
    b = next((r["browser"] for r in rows if r["browser"] != "?"), "?")
    print("`dxr-splat-ab.vercel.app` with `&stats=1`; browser `org.chromium.chrome` "
          "%s. Each config is a\ncold `am start`, then the PAGE runs its own protocol "
          "(warm-up, then a measurement window)\nand freezes `window.__stats` at "
          "`phase=done`, which is read exactly once.\n" % b)
    print("`ms_median` / `fps` are **wall-clock frame** figures. `gpu ms` is the page's "
          "WebGL timer\nquery and is `—` wherever `timerQuery` was false. GPU clock is "
          "sliced to the page's own\nmeasurement window, not averaged over the load.\n")

    hdr = ("| config | engine | asset | rs | mode | buffer | dpr | rScale | splats | "
           "frames | ms med | ms p95 | **fps** | gpu ms med | gpu ms p95 | "
           "GPU clk min/med/max MHz | thermal | reader |")
    print(hdr)
    print("|" + "---|" * 18)

    mono_rows, missing_rows, wedge_rows = [], [], []
    for r in rows:
        st = r["stats"]
        if r["wedge"]:
            wedge_rows.append(r["config"])
        if st is None:
            missing_rows.append(r["config"])
            print("| %s | %s | %s | %s | " % (r["config"], r["engine"], r["assetkey"], r["rs"])
                  + "— | " * 13 + "**no stats** |")
            continue
        mode = st.get("mode", "?")
        if mode == "mono":
            mono_rows.append(r["config"])
        buf = st.get("buffer")
        bufs = "%sx%s" % (buf[0], buf[1]) if isinstance(buf, (list, tuple)) and len(buf) >= 2 else "—"
        sel = window_samples(r)
        clks = [s["clk"] for s in sel if s["clk"]]
        clkstr = ("%s/%s/%s" % (mhz(min(clks)), mhz(statistics.median(clks)), mhz(max(clks)))
                  if clks else "—")
        ths = [s["thermal"] for s in sel if s["thermal"] is not None]
        thstr = "max %d" % max(ths) if ths else "—"
        gm = st.get("gpu_ms_median") if st.get("timerQuery") else None
        gp = st.get("gpu_ms_p95") if st.get("timerQuery") else None
        splats = st.get("splats")
        print("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | **%s** | "
              "%s | %s | %s | %s | %s |"
              % (r["config"], st.get("engine", r["engine"]), r["assetkey"], r["rs"],
                 ("**MONO**" if mode == "mono" else mode),
                 bufs, fmt(st.get("dpr"), 2), fmt(st.get("renderScale"), 2),
                 "{:,}".format(splats) if isinstance(splats, int) else "—",
                 st.get("frames", "—"),
                 fmt(st.get("ms_median")), fmt(st.get("ms_p95")), fmt(st.get("fps"), 1),
                 fmt(gm), fmt(gp), clkstr, thstr, r["reader"]))

    # ── the things a reader must be told before quoting any of the above ──
    print("\n### Read this before quoting a row\n")
    if mono_rows:
        print("* **%d row(s) came back `mode=mono`** — %s. A hands-off `am start` that "
              "never engaged the 3D session rendered ONE view, so these are not 2-view "
              "results and must not be compared with the native 2-view numbers. The "
              "`mode` field is recorded verbatim, not corrected."
              % (len(mono_rows), ", ".join("`%s`" % c for c in mono_rows)))
    else:
        print("* No row reported `mode=mono`: every config engaged a multi-view session "
              "hands-off.")
    if missing_rows:
        print("* **No stats at all for %s.** The per-config `.png` screencap is the only "
              "evidence for those; read the numbers off it or re-run just those configs "
              "with `--only=`." % ", ".join("`%s`" % c for c in missing_rows))
    readers = sorted({r["reader"] for r in rows})
    print("* Readers used: %s. **CDP is the primary path** — it is the only reader "
          "that has ever returned a number on this device. `logcat` is a demoted "
          "fallback: on this official/release build the page's `[stats]` console "
          "line is not mirrored into logcat at all." % ", ".join("`%s`" % x for x in readers))

    # ── DEVICE-STATE FORENSICS ──
    # A config that measured nothing because the DISPLAY WENT TO SLEEP looks
    # identical, in the stats columns, to one that measured nothing because the
    # reader broke. The sampler can tell them apart, so it must.
    asleep, idle, adbfail, wrongfg = [], [], [], []
    for r in rows:
        sel = r["samples"]
        if not sel:
            continue
        na = sum(1 for s_ in sel if s_["clk"] is None)
        if na > len(sel) // 2:
            adbfail.append("`%s` (%d/%d reads unanswered)" % (r["config"], na, len(sel)))
        live = [s_ for s_ in sel if s_["clk"]]
        # A MAJORITY rule, not `all`: the first sample of a config often carries
        # the previous config's stale gpubusy counters, and one such sample must
        # not veto the diagnosis of a config that rendered nothing.
        if live:
            parked = sum(1 for s_ in live if s_["clk"] <= 300 * 10**6 and s_["busy0"])
            if parked >= 0.9 * len(live):
                idle.append("`%s` (%d/%d samples parked)" % (r["config"], parked, len(live)))
        if any(s_.get("wake") and "Awake" not in s_["wake"] for s_ in sel):
            asleep.append("`%s`" % r["config"])
        fg = r["foreground_end"] or r["foreground"]
        if fg and "org.chromium.chrome" not in fg:
            wrongfg.append("`%s` -> %s" % (r["config"], fg))
    if idle:
        print("* **The GPU never left its idle floor** for %s: clock pinned at the "
              "minimum with `gpubusy` at zero for the whole config. Nothing was "
              "rendered — this is a DEVICE problem (display asleep / app not "
              "foreground), not a slow renderer and not a reader bug." % ", ".join(idle))
    if asleep:
        print("* **The display was not `Awake`** during %s. A hands-off run injects no "
              "input, so the pad's display timeout is the single most dangerous "
              "variable in it; hold it with `svc power stayon true`." % ", ".join(asleep))
    if wrongfg:
        print("* **Chrome was not the foreground window** at the end of %s. `am start` "
              "reporting `Starting: Intent …` says nothing about visibility."
              % ", ".join(wrongfg))
    if adbfail:
        print("* **adb stopped answering** during %s — the sampler's reads came back "
              "empty. Every adb call in the run loop must be bounded (the run of "
              "2026-09-22 had none and blocked for 2 h 55 m)." % ", ".join(adbfail))
    errs = sorted({e for r in rows for e in r["readerr"]})
    if errs:
        print("* Reader errors recorded: %s" % "; ".join("`%s`" % e[:120] for e in errs[:4]))

    # ── vsync quantisation ──
    # Two different renderers reporting the SAME median to the decimal is not a
    # tie, it is a cap: the measurement is pacing-bound and does not compare the
    # engines at all.
    meds = {}
    for r in rows:
        if r["stats"] and r["stats"].get("ms_median"):
            meds.setdefault(round(float(r["stats"]["ms_median"]), 2), []).append(r["config"])
    for v, cs in meds.items():
        engs = {c.split("_")[0] for c in cs}
        if len(cs) > 1 and len(engs) > 1:
            n60 = v / 16.67
            print("* **%s report an identical `ms_median` of %.2f ms** — %.1fx a 60 Hz "
                  "vblank. Two different renderers do not agree to the decimal by "
                  "chance: these rows are pinned to a presentation cadence and do "
                  "**not** discriminate the engines. Compare them only at a setting "
                  "that gets off the cap."
                  % (", ".join("`%s`" % c for c in cs), v, n60))
    bad_rot = [r["config"] for r in rows if r["rotation"] not in ("rotation=1", "?")]
    if bad_rot:
        print("* **Wrong orientation** for %s — landscape (`rotation=1`) is the measured "
              "configuration; portrait silently changes the canvas and invalidates the "
              "comparison." % ", ".join("`%s`" % c for c in bad_rot))
    if wedge_rows:
        print("* **`leiadisp@1.0-service select() timeout` seen** during %s — that is the "
              "lens-MCU wedge signature. Treat those rows as suspect and power-cycle the "
              "panel before believing a repeat." % ", ".join("`%s`" % c for c in wedge_rows))
    else:
        print("* No `leiadisp@1.0-service … select() timeout` lines: the lens MCU did not "
              "wedge during the run.")
    notq = [r["config"] for r in rows if r["stats"] and not r["stats"].get("timerQuery")]
    if notq:
        print("* `timerQuery` was false for %s, so their `gpu ms` columns are empty by "
              "contract, not by failure." % ", ".join("`%s`" % c for c in notq))

    print("\n### Mac reference (different machine, mono 1280x720 — order of magnitude only)\n")
    print("| engine | config | fps | note |")
    print("|---|---|---|---|")
    for e, c, f, n in MAC_REF:
        print("| %s | %s | %s | %s |" % (e, c, f, n))

    # ── native ──
    nat = a.native
    if nat is None:
        cand_md = os.path.join(os.path.dirname(os.path.abspath(a.dir)),
                               "results", "android_np02j_table.md")
        cand_dir = os.path.join(os.path.dirname(os.path.abspath(a.dir)), "android_bench")
        nat = cand_md if os.path.isfile(cand_md) else (
            cand_dir if os.path.isdir(cand_dir) else None)
    kind, payload = load_native(nat, here)
    print_native(kind, payload)
    if nat:
        print("\n_Native source: `%s`_" % nat)

    print("\n### Per-config provenance\n")
    for r in rows:
        st = r["stats"] or {}
        print("* **%s** — `%s`" % (r["config"], r["url"] or "?"))
        print("  * browser %s, %s, reader `%s`, stats at t=%s s, cdp_ready=%s"
              % (r["browser"], r["rotation"], r["reader"],
                 fmt(r["done_t"], 0), r["cdp_ready"]))
        if st:
            print("  * page: engine=%s mode=%s supported=%s renderer=%s rig=%s "
                  "focusSource=%s warm=%ss win=%ss frames=%s"
                  % (st.get("engine"), st.get("mode"), st.get("supported"),
                     st.get("renderer"), st.get("rig"), st.get("focusSource"),
                     st.get("warm_s"), st.get("win_s"), st.get("frames")))
            if st.get("cssBox"):
                print("  * cssBox=%s buffer=%s dpr=%s renderScale=%s"
                      % (st.get("cssBox"), st.get("buffer"), st.get("dpr"),
                         st.get("renderScale")))
            if st.get("asset"):
                print("  * asset: `%s`" % st["asset"])
        if r["phases"]:
            print("  * phases: %s" % ", ".join("%s@%ss" % (p.get("phase"), p.get("t"))
                                               for p in r["phases"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
