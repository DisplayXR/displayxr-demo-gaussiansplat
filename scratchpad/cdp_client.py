#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
"""A dependency-free Chrome DevTools Protocol reader (HTTP + RFC 6455 WebSocket).

Why hand-rolled: the box that drives the pad must not need `pip install
websocket-client` or `npm i ws` — a benchmark that cannot run because a wheel is
missing is a benchmark that does not get run. Everything here is stdlib
(`socket`, `ssl`-free, `hashlib`, `struct`, `base64`, `http.client`), Python 3.6+.

Used by `bench_web_android.sh` to read `window.__stats` out of the DisplayXR
Browser on the NP02J over `adb forward tcp:<port> localabstract:<socket>`. The
same code talks to desktop Chrome, which is what `--selftest` exercises.

    # list every debuggable target behind a forwarded port
    ./cdp_client.py --port 9222 --list

    # evaluate in the first page whose URL contains a substring
    ./cdp_client.py --port 9222 --match dxr-splat-ab --eval 'JSON.stringify(window.__stats)'

    # prove the whole path against desktop Chrome (launches ONE headless Chrome,
    # kills it again) — no device, no adb
    ./cdp_client.py --selftest

Exit codes: 0 ok, 3 no DevTools HTTP endpoint, 4 no matching page target,
5 Runtime.evaluate failed / threw, 2 usage.

`--eval` prints the evaluated value on stdout and nothing else, so a shell can
capture it directly. Diagnostics go to stderr.
"""

import argparse
import base64
import hashlib
import http.client
import json
import os
import random
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def log(*a):
    print(*a, file=sys.stderr)


# ───────────────────────────── HTTP side (/json) ─────────────────────────────
# Chrome's DevTools HTTP endpoint rejects requests whose Host header is neither
# "localhost" nor a bare IP (DNS-rebinding protection), so the Host is pinned
# explicitly rather than left to http.client's default.
def http_json(port, path, host="127.0.0.1", timeout=5.0):
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        conn.request("GET", path, headers={"Host": "localhost:%d" % port,
                                           "Connection": "close"})
        resp = conn.getresponse()
        body = resp.read().decode("utf-8", "replace")
        if resp.status != 200:
            raise RuntimeError("HTTP %d %s from %s" % (resp.status, resp.reason, path))
        return json.loads(body)
    finally:
        conn.close()


def wait_endpoint(port, host="127.0.0.1", timeout=20.0):
    """Block until /json/version answers. Returns the version dict."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            return http_json(port, "/json/version", host=host, timeout=2.0)
        except Exception as e:      # connection refused while Chrome boots
            last = e
            time.sleep(0.25)
    raise RuntimeError("no DevTools endpoint on %s:%d after %.0fs (%s)"
                       % (host, port, timeout, last))


def list_targets(port, host="127.0.0.1"):
    return http_json(port, "/json", host=host)


def pick_page(targets, match=None):
    """First `page` target whose url/title contains `match` (or any page)."""
    pages = [t for t in targets if t.get("type") == "page"
             and t.get("webSocketDebuggerUrl")]
    if match:
        hit = [t for t in pages
               if match in t.get("url", "") or match in t.get("title", "")]
        pages = hit
    return pages[0] if pages else None


# ────────────────────────── WebSocket side (RFC 6455) ────────────────────────
class WebSocket(object):
    """The minimum client that CDP needs: text frames, client masking,
    continuation frames, ping/pong, 64-bit lengths. No extensions, no TLS."""

    def __init__(self, url, timeout=30.0):
        # ws://127.0.0.1:9222/devtools/page/<id>
        if not url.startswith("ws://"):
            raise ValueError("only ws:// is supported here, got %r" % url)
        rest = url[len("ws://"):]
        netloc, _, path = rest.partition("/")
        path = "/" + path
        host, _, port = netloc.partition(":")
        port = int(port or 80)
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self._buf = b""
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n" % (path, host, port, key)
        )
        self.sock.sendall(req.encode())
        head = self._read_until(b"\r\n\r\n")
        status = head.split(b"\r\n", 1)[0].decode("latin-1")
        if "101" not in status:
            raise RuntimeError("websocket upgrade refused: %s" % status)
        want = base64.b64encode(
            hashlib.sha1((key + GUID).encode()).digest()).decode()
        accept = ""
        for line in head.decode("latin-1").split("\r\n"):
            if line.lower().startswith("sec-websocket-accept:"):
                accept = line.split(":", 1)[1].strip()
        if accept != want:
            raise RuntimeError("bad Sec-WebSocket-Accept (%r != %r)" % (accept, want))

    # -- raw buffered reads -------------------------------------------------
    def _read_until(self, marker):
        while marker not in self._buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("socket closed during handshake")
            self._buf += chunk
        head, _, self._buf = self._buf.partition(marker)
        return head + marker

    def _read_exact(self, n):
        while len(self._buf) < n:
            chunk = self.sock.recv(max(65536, n - len(self._buf)))
            if not chunk:
                raise RuntimeError("socket closed mid-frame")
            self._buf += chunk
        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    # -- frames -------------------------------------------------------------
    def _send_frame(self, opcode, payload):
        # RFC 6455 §5.1: every client frame MUST be masked.
        mask = struct.pack("!I", random.getrandbits(32))
        n = len(payload)
        hdr = struct.pack("!B", 0x80 | opcode)
        if n < 126:
            hdr += struct.pack("!B", 0x80 | n)
        elif n < (1 << 16):
            hdr += struct.pack("!BH", 0x80 | 126, n)
        else:
            hdr += struct.pack("!BQ", 0x80 | 127, n)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(hdr + mask + masked)

    def send_text(self, s):
        self._send_frame(0x1, s.encode("utf-8"))

    def recv_text(self):
        """Return the next complete text message, answering pings inline."""
        data = b""
        op = None
        while True:
            b0, b1 = struct.unpack("!BB", self._read_exact(2))
            fin = b0 & 0x80
            opcode = b0 & 0x0F
            masked = b1 & 0x80
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack("!H", self._read_exact(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", self._read_exact(8))[0]
            mask = self._read_exact(4) if masked else None
            payload = self._read_exact(n) if n else b""
            if mask:
                payload = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
            if opcode == 0x8:                       # close
                raise RuntimeError("peer closed the websocket")
            if opcode == 0x9:                       # ping -> pong
                self._send_frame(0xA, payload)
                continue
            if opcode == 0xA:                       # pong
                continue
            if opcode in (0x1, 0x2):
                op, data = opcode, payload
            elif opcode == 0x0:                     # continuation
                data += payload
            if fin:
                if op == 0x1:
                    return data.decode("utf-8", "replace")
                # a binary frame is not something CDP sends; skip it
                data, op = b"", None

    def close(self):
        try:
            self._send_frame(0x8, b"")
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass


# ───────────────────────────────── CDP ───────────────────────────────────────
class CDP(object):
    def __init__(self, ws_url, timeout=30.0):
        self.ws = WebSocket(ws_url, timeout=timeout)
        self._id = 0

    def call(self, method, params=None):
        self._id += 1
        mine = self._id
        self.ws.send_text(json.dumps({"id": mine, "method": method,
                                      "params": params or {}}))
        # CDP interleaves unsolicited events with replies; keep reading until
        # the reply carrying OUR id shows up.
        while True:
            msg = json.loads(self.ws.recv_text())
            if msg.get("id") == mine:
                if "error" in msg:
                    raise RuntimeError("%s: %s" % (method, msg["error"]))
                return msg.get("result", {})

    def evaluate(self, expression, await_promise=False):
        r = self.call("Runtime.evaluate", {
            "expression": expression,
            "returnByValue": True,
            "awaitPromise": bool(await_promise),
        })
        if r.get("exceptionDetails"):
            d = r["exceptionDetails"]
            desc = (d.get("exception") or {}).get("description") or d.get("text")
            raise RuntimeError("evaluate threw: %s" % desc)
        return (r.get("result") or {}).get("value")

    def close(self):
        self.ws.close()


def evaluate_on_port(port, expression, match=None, host="127.0.0.1",
                     timeout=20.0, await_promise=False):
    """One-shot: endpoint -> target -> evaluate -> value. Raises on failure."""
    wait_endpoint(port, host=host, timeout=timeout)
    tgt = pick_page(list_targets(port, host=host), match)
    if tgt is None:
        raise LookupError("no page target matching %r" % (match,))
    c = CDP(tgt["webSocketDebuggerUrl"])
    try:
        return c.evaluate(expression, await_promise=await_promise)
    finally:
        c.close()


# ─────────────────────────────── self-test ───────────────────────────────────
CHROME_CANDIDATES = [
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
]


def selftest(port=9223):
    """Prove the HTTP + websocket + Runtime.evaluate path end-to-end.

    Browser hygiene (feedback_browser_process_hygiene): ONE Chrome, its own
    throwaway profile dir so it can never touch the user's session, and it is
    killed in a `finally` — including on Ctrl-C.
    """
    chrome = next((p for p in CHROME_CANDIDATES if os.path.exists(p)), None)
    if not chrome:
        log("SELFTEST SKIP: no Chrome/Chromium found in", CHROME_CANDIDATES)
        return 1
    profile = tempfile.mkdtemp(prefix="dxr-cdp-selftest-")
    proc = subprocess.Popen(
        [chrome, "--headless=new", "--remote-debugging-port=%d" % port,
         "--user-data-dir=" + profile, "--no-first-run",
         "--no-default-browser-check", "--disable-gpu", "about:blank"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        ver = wait_endpoint(port, timeout=30.0)
        log("  endpoint: %s (%s)" % (ver.get("Browser"), ver.get("V8-Version")))
        tgts = list_targets(port)
        log("  targets: %d — %s" % (len(tgts),
                                    ", ".join("%s:%s" % (t.get("type"), t.get("url"))
                                              for t in tgts[:4])))
        tgt = pick_page(tgts)
        if tgt is None:
            log("SELFTEST FAIL: no page target")
            return 1
        c = CDP(tgt["webSocketDebuggerUrl"])
        try:
            # 1. trivial round-trip
            got = c.evaluate("1+1")
            assert got == 2, got
            log("  Runtime.evaluate 1+1 -> %r" % (got,))
            # 2. the shape this benchmark actually reads: install a
            #    window.__stats exactly like the page's contract, read it back
            #    as a JSON string, and parse it the way the runner will.
            c.evaluate(
                "window.__stats={medianMs:8.31,p95Ms:12.07,fps:118.4,"
                "gpuMs:5.2,bufferW:2560,bufferH:1440,views:2,mode:'woven'};1")
            raw = c.evaluate("JSON.stringify(window.__stats)")
            st = json.loads(raw)
            assert st["mode"] == "woven" and abs(st["medianMs"] - 8.31) < 1e-9, st
            log("  window.__stats round-trip -> %s" % raw)
            # 3. absent object must come back as the JS string "undefined"
            #    (json.dumps of undefined is undefined), not blow up
            missing = c.evaluate("JSON.stringify(window.__nope)")
            log("  missing object -> %r (expected None/undefined)" % (missing,))
            # 4. a long payload, to exercise the 16-bit/64-bit length paths
            big = c.evaluate("'x'.repeat(200000).length")
            assert big == 200000, big
            log("  200 kB payload round-trip OK")
        finally:
            c.close()
        log("SELFTEST PASS")
        return 0
    except Exception as e:
        log("SELFTEST FAIL:", e)
        return 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.kill()
        shutil.rmtree(profile, ignore_errors=True)
        log("  (headless Chrome pid %d terminated, profile removed)" % proc.pid)


# ─────────────────────────────────── CLI ─────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=9222)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--list", action="store_true", help="dump /json targets")
    ap.add_argument("--version", action="store_true", help="dump /json/version")
    ap.add_argument("--match", default=None,
                    help="substring of the target page's URL or title")
    ap.add_argument("--eval", dest="expr", default=None,
                    help="JS expression; its value is printed on stdout")
    ap.add_argument("--await-promise", action="store_true")
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("--selftest", action="store_true",
                    help="launch ONE headless Chrome and prove the path")
    a = ap.parse_args()

    if a.selftest:
        return selftest()

    try:
        if a.version:
            print(json.dumps(http_json(a.port, "/json/version", host=a.host), indent=2))
            return 0
        if a.list:
            tg = list_targets(a.port, host=a.host)
            print(json.dumps([{k: t.get(k) for k in
                               ("type", "title", "url", "id", "webSocketDebuggerUrl")}
                              for t in tg], indent=2))
            return 0
    except Exception as e:
        log("no DevTools endpoint on %s:%d — %s" % (a.host, a.port, e))
        return 3

    if not a.expr:
        ap.error("one of --list / --version / --eval / --selftest is required")
    try:
        val = evaluate_on_port(a.port, a.expr, match=a.match, host=a.host,
                               timeout=a.timeout, await_promise=a.await_promise)
    except LookupError as e:
        log(str(e))
        return 4
    except RuntimeError as e:
        if "no DevTools endpoint" in str(e):
            log(str(e))
            return 3
        log("evaluate failed: %s" % e)
        return 5
    except Exception as e:
        log("evaluate failed: %s" % e)
        return 5
    print(val if isinstance(val, str) else json.dumps(val))
    return 0


if __name__ == "__main__":
    sys.exit(main())
