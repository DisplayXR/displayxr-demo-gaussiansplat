#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""Turn bench_android.sh's per-config logs into the H6-style table.

    python3 scratchpad/bench_android_parse.py scratchpad/android_bench

Reads every ``<config>.log`` in the directory and emits a Markdown table with
one row per config: per-stage MEAN, ``TOTAL/eye`` mean with ``[min, max]``, the
sample count ``n``, plus the GPU clock min/median/max and the worst thermal
status seen inside that config's measurement window.

Three things this deliberately reports rather than assumes:

* **TOTAL is per EYE.** The app's ``GS_TS[adreno]`` line is emitted once per 120
  ``renderEye`` calls and times ONE eye, so a frame costs roughly ``2 x TOTAL``
  plus whatever the runtime's weave adds. The table never silently doubles it.
* **The asset is named on every row.** Two clouds share one keep ladder here, and
  a row that does not say which one it measured is not attributable.
* **``N`` comes out of the log, not out of the keep fraction.** ``keep`` is a
  request; ``N`` is what the loader actually kept after hash selection, and the
  two are not identical.
"""

import argparse
import os
import re
import statistics
import sys

# GS_TS[adreno] N=1179648 drawn=1179648 render=1920x1200(scale=1.00 keep=1.00) |
#   preproc=3.83 keygen=0.56 radix=0.87 draw=10.95 blit=0.02 | TOTAL=16.54 ms
TS_RE = re.compile(
    r"GS_TS\[adreno\]\s+N=(?P<N>\d+)\s+drawn=(?P<drawn>\d+)\s+"
    r"render=(?P<rw>\d+)x(?P<rh>\d+)\(scale=(?P<scale>[\d.]+)\s+keep=(?P<keep>[\d.]+)\)\s*\|\s*"
    r"preproc=(?P<preproc>[\d.]+)\s+keygen=(?P<keygen>[\d.]+)\s+radix=(?P<radix>[\d.]+)\s+"
    r"draw=(?P<draw>[\d.]+)\s+blit=(?P<blit>[\d.]+)\s*\|\s*TOTAL=(?P<total>[\d.]+)\s*ms"
)
SAMPLE_RE = re.compile(r"^#SAMPLE\s+(?P<body>.*)$")
KV_RE = re.compile(r'(\w+)=("([^"]*)"|\S+)')
META_RE = re.compile(r"^#META\s+(?P<body>.*)$")
LOAD_RE = re.compile(r"^#LOAD\s+(?P<body>.*)$")

STAGES = ("preproc", "keygen", "radix", "draw", "blit")


def kv(body):
    out = {}
    for m in KV_RE.finditer(body):
        out[m.group(1)] = m.group(3) if m.group(3) is not None else m.group(2)
    return out


def parse_log(path):
    r = {
        "config": os.path.splitext(os.path.basename(path))[0],
        "asset": "?", "keep": "?", "rotation": "?", "apk": "?",
        "eff_view": "", "swapchain": "", "gaussians": None, "rig": "",
        "rig_detail": "", "scene_path": "", "scene_src": "?",
        "ts": [], "clk": [], "thermal": [], "capture_s": None, "ts_lines": None,
    }
    with open(path, errors="replace") as fh:
        for line in fh:
            m = TS_RE.search(line)
            if m:
                d = m.groupdict()
                r["ts"].append({k: (int(d[k]) if k in ("N", "drawn", "rw", "rh")
                                    else float(d[k])) for k in d})
                continue
            m = META_RE.match(line)
            if m:
                f = kv(m.group("body"))
                for k in ("config", "asset", "keep", "rotation", "apk"):
                    if k in f:
                        r[k] = f[k]
                if "capture_s" in f:
                    r["capture_s"] = f["capture_s"]
                if "ts_lines" in f:
                    r["ts_lines"] = f["ts_lines"]
                continue
            m = SAMPLE_RE.match(line)
            if m:
                f = kv(m.group("body"))
                clk = f.get("gpuclk", "NA")
                if clk.isdigit():
                    r["clk"].append(int(clk))
                th = f.get("thermal", "")
                n = re.search(r"(?:mStatus|[Ss]tatus)\D*(\d+)", th)
                if n:
                    r["thermal"].append(int(n.group(1)))
                continue
            m = LOAD_RE.match(line)
            body = m.group("body") if m else line
            if "Atlas blit:" in body:
                e = re.search(r"eff_view=(\d+x\d+)", body)
                if e and not r["eff_view"]:
                    r["eff_view"] = e.group(1)
            if "swapchain:" in body and not r["swapchain"]:
                e = re.search(r"swapchain:\s*(\d+x\d+)", body)
                if e:
                    r["swapchain"] = e.group(1)
            if "Auto-loading scene:" in body and not r["scene_path"]:
                # The app appends " (debug.dxr.gs.scene)" / " (bundled)" to say
                # WHERE the path came from; keep the path, drop the annotation.
                p = body.split("Auto-loading scene:", 1)[1].strip()
                r["scene_path"] = re.sub(r"\s*\((?:debug\.dxr\.gs\.scene|bundled)\)\s*$", "", p)
                r["scene_src"] = "override" if "debug.dxr.gs.scene" in p else "bundled"
            if "gaussians" in body and r["gaussians"] is None:
                e = re.search(r"Loaded\s+(\S+):\s+(\d+)\s+gaussians", body)
                if e:
                    r["scene_path"] = r["scene_path"] or e.group(1)
                    r["gaussians"] = int(e.group(2))
            if "Camera rig:" in body and not r["rig_detail"]:
                r["rig"] = "camera"
                r["rig_detail"] = body.split("Camera rig:", 1)[1].strip()
            if "framing with the display rig" in body and not r["rig"]:
                r["rig"] = "display (camera rig unresolved)"
            if "Photo-lift signature:" in body and not r["rig"]:
                r["rig"] = "PHOTO LIFT" if "PHOTO LIFT" in body else "object scan"
    return r


def fmt(v, nd=2):
    return "—" if v is None else f"{v:.{nd}f}"


def summarise(r):
    ts = r["ts"]
    if not ts:
        return None
    s = {"n": len(ts)}
    for st in STAGES:
        s[st] = statistics.mean(x[st] for x in ts)
    tot = [x["total"] for x in ts]
    s["total"] = statistics.mean(tot)
    s["total_med"] = statistics.median(tot)
    s["total_min"] = min(tot)
    s["total_max"] = max(tot)
    s["N"] = ts[-1]["N"]
    s["drawn"] = ts[-1]["drawn"]
    s["render"] = f"{ts[-1]['rw']}x{ts[-1]['rh']}"
    s["scale"] = ts[-1]["scale"]
    s["keep_seen"] = ts[-1]["keep"]
    return s


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", help="directory of <config>.log files")
    ap.add_argument("--order", default="", help="comma-separated config order")
    args = ap.parse_args()

    logs = sorted(f for f in os.listdir(args.dir)
                  if f.endswith(".log") and not f.endswith(".load.log"))
    if args.order:
        want = [x.strip() for x in args.order.split(",")]
        logs.sort(key=lambda f: want.index(os.path.splitext(f)[0])
                  if os.path.splitext(f)[0] in want else len(want))
    if not logs:
        sys.exit(f"no <config>.log files in {args.dir}")

    rows = []
    for f in logs:
        r = parse_log(os.path.join(args.dir, f))
        r["sum"] = summarise(r)
        rows.append(r)

    print("## Android splat benchmark — NP02J (SD 8 Gen 2 / Adreno 740)\n")
    print("Per-stage GPU time from the app's throttled `GS_TS[adreno]` line, which")
    print("is emitted once per 120 `renderEye` calls and times **one eye**: a frame")
    print("costs roughly `2 x TOTAL` plus the runtime's weave. Stage columns are")
    print("MEANS; `TOTAL/eye` is the mean with `[min, max]`. `N` is what the loader")
    print("actually kept, not the requested keep fraction.\n")

    hdr = ("| config | asset | keep | eff_view | render | N | drawn | preproc | "
           "keygen | radix | draw | blit | **TOTAL/eye** | median | n | "
           "GPU clock min/med/max (MHz) | thermal |")
    print(hdr)
    print("|" + "---|" * 17)
    for r in rows:
        s = r["sum"]
        if s is None:
            print(f"| {r['config']} | {r['asset']} | {r['keep']} | "
                  + "— | " * 13 + "**no GS_TS lines** |")
            continue
        clk = r["clk"]
        clks = (f"{min(clk)//1000000}/{int(statistics.median(clk))//1000000}/"
                f"{max(clk)//1000000}") if clk else "—"
        th = f"max {max(r['thermal'])}" if r["thermal"] else "—"
        print(f"| {r['config']} | {r['asset']} | {r['keep']} | "
              f"{r['eff_view'] or r['swapchain'] or '—'} | {s['render']} | "
              f"{s['N']:,} | {s['drawn']:,} | "
              f"{fmt(s['preproc'])} | {fmt(s['keygen'])} | {fmt(s['radix'])} | "
              f"{fmt(s['draw'])} | {fmt(s['blit'])} | "
              f"**{fmt(s['total'])}** [{fmt(s['total_min'])}, {fmt(s['total_max'])}] | "
              f"{fmt(s['total_med'])} | {s['n']} | {clks} | {th} |")

    print("\n### Per-config provenance\n")
    for r in rows:
        s = r["sum"]
        print(f"* **{r['config']}** — apk `{r['apk']}`, {r['rotation'] or 'rotation ?'}, "
              f"capture {r['capture_s'] or '?'}s, "
              f"{s['n'] if s else 0} GS_TS line(s)")
        print(f"  * scene: `{r['scene_path'] or '?'}` [{r['scene_src']}]"
              + (f", {r['gaussians']:,} gaussians at load" if r["gaussians"] else ""))
        print(f"  * rig: {r['rig'] or '?'}"
              + (f" — {r['rig_detail']}" if r["rig_detail"] else ""))
        if s and abs(float(r["keep"]) - s["keep_seen"]) > 1e-6:
            print(f"  * **keep MISMATCH**: asked {r['keep']}, renderer reports "
                  f"{s['keep_seen']} — the prop did not take")
        if s and s["n"] < 14:
            print(f"  * **under-sampled**: n={s['n']} < 14")
        # Only judge an orientation that was actually recorded: a missing
        # #META rotation means the config never started, which the "no GS_TS
        # lines" row already says. Warning about it twice, in the wrong words,
        # is how a reader ends up chasing a rotation bug that is not there.
        if r["rotation"].startswith("rotation=") and r["rotation"] != "rotation=1":
            print(f"  * **wrong orientation**: {r['rotation']} — landscape is "
                  "the measured configuration")


if __name__ == "__main__":
    main()
