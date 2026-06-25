#!/usr/bin/env python3
"""
Live FEC demo backend.

Serves the web UI and runs simulations through the PROJECT'S REAL GF(256) codec
(build/libfecsim.so, which wraps src/fec.c + src/fec_codec.c). Python decides
the loss pattern and the transfer-timing model; every parity and recovered byte
comes from the C library.

Run from the repo root:
    bash tools/fec-demo/build_demo.sh     # builds build/libfecsim.so
    python3 tools/fec-demo/server.py      # then open http://localhost:8088

Endpoints:
    GET  /                      -> web/index.html
    GET  /<asset>               -> web/<asset>  (app.js, style.css)
    POST /api/simulate?...      -> single run; raw file bytes in the body
    POST /api/sweep?...         -> loss sweep for the chart; raw bytes in body
"""
import ctypes
import json
import math
import os
import random
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

ROOT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(ROOT, "..", ".."))
WEB = os.path.join(ROOT, "web")
SO = os.path.join(REPO, "build", "libfecsim.so")

PORT = 8088
MAX_FILE = 4 * 1024 * 1024          # cap upload to keep the UI responsive

# --- transfer-timing model constants (match results/README.md + fec-sim.c) ---
RATE_BPS = 10_000_000               # 10 Mbit/s link
RTT_MS = 50.0
RTO_MIN = 1000.0                    # this stack's initial/min RTO
RTO_CAP = 60000.0

# ------------------------------------------------------------------ codec glue
if not os.path.exists(SO):
    raise SystemExit(f"missing {SO} — run: bash tools/fec-demo/build_demo.sh")
_lib = ctypes.CDLL(SO)
_lib.fecsim_encode.restype = ctypes.c_int
_lib.fecsim_recover.restype = ctypes.c_int
_lib.fecsim_max_k.restype = ctypes.c_int
_lib.fecsim_max_r.restype = ctypes.c_int
MAX_K = _lib.fecsim_max_k()
MAX_R = _lib.fecsim_max_r()


def encode_block(src_bytes, k, r, sym):
    src = (ctypes.c_ubyte * len(src_bytes)).from_buffer_copy(src_bytes)
    parity = (ctypes.c_ubyte * (r * sym))()
    rc = _lib.fecsim_encode(src, k, r, sym, parity)
    if rc != 0:
        raise RuntimeError(f"fecsim_encode rc={rc}")
    return bytes(parity)


def recover_block(symbols_bytes, present, k, r, sym):
    sb = (ctypes.c_ubyte * len(symbols_bytes)).from_buffer_copy(symbols_bytes)
    pm = (ctypes.c_ubyte * len(present))(*present)
    out = (ctypes.c_ubyte * (k * sym))()
    rc = _lib.fecsim_recover(sb, pm, k, r, sym, out)
    return bytes(out), rc


# ------------------------------------------------------------------ timing model
def rto_stall(rnd, p):
    stall, cur = 0.0, RTO_MIN
    while True:
        stall += cur
        if rnd.random() >= p:
            break
        cur = min(cur * 2.0, RTO_CAP)
    return stall


def window_segments(sym):
    """TCP window size in segments ~ the bandwidth-delay product. Using the full
    BDP is the TCP-favouring (optimistic) choice, so it does not flatter FEC."""
    return max(1, int(RATE_BPS * (RTT_MS / 1000.0) / 8.0 / sym))


def sack_stall(rnd, lost_idx, S, p, W):
    """Idealised TCP with SACK fast-recovery: all the holes in one RTT-window are
    retransmitted together in ~1 RTT (not one RTT per loss). A re-lost hole costs
    another recovery round; the final few segments can't trigger three duplicate
    ACKs, so a tail loss waits for a full RTO."""
    if not lost_idx:
        return 0.0
    by_win = {}
    for i in lost_idx:
        by_win.setdefault(i // W, []).append(i)
    stall = 0.0
    for idxs in by_win.values():
        stall += RTT_MS                              # one fast-recovery round
        remaining = len(idxs)
        while remaining > 0:                         # retransmitted holes re-lost
            relost = sum(1 for _ in range(remaining) if rnd.random() < p)
            if relost:
                stall += RTT_MS
            remaining = relost
    if any(i >= S - 3 for i in lost_idx):            # tail can't be fast-retx'd
        stall += RTO_MIN - RTT_MS
    return stall


# ------------------------------------------------------------------ simulation
def simulate(data, k, r, sym, loss_pct, seed, want_blocks=True):
    """Run one transfer of `data` through FEC and the two TCP baselines."""
    p = loss_pct / 100.0
    rnd = random.Random(seed)

    if len(data) == 0:
        data = b"\x00"
    S = (len(data) + sym - 1) // sym                # source segments
    nblocks = (S + k - 1) // k
    data = data + b"\x00" * (S * sym - len(data))   # zero-pad final segment

    fec_stall = tcp_stall = 0.0
    lost_pkts = recovered_blocks = busted_blocks = recovered_syms = lost_syms = 0
    recover_failures = 0
    blocks = []
    lost_src_global = []                # global indices of lost source segments
    W = window_segments(sym)

    seg = 0
    for b in range(nblocks):
        blk_k = min(k, S - b * k)
        base = b * k * sym
        src_bytes = data[base: base + blk_k * sym]
        parity = encode_block(src_bytes, blk_k, r, sym)

        present = [1] * (blk_k + r)
        lost_src, lost_red = [], []
        for i in range(blk_k):
            if rnd.random() < p:
                present[i] = 0
                lost_src.append(i)
        for j in range(r):
            if rnd.random() < p:
                present[blk_k + j] = 0
                lost_red.append(j)

        lost_pkts += len(lost_src) + len(lost_red)
        lost_syms += len(lost_src)

        recovered = True
        if lost_src:
            symbols = src_bytes + parity
            out, rc = recover_block(symbols, present, blk_k, r, sym)
            if rc == 0:
                if out != src_bytes:                # real byte verification
                    recover_failures += 1
                recovered_blocks += 1
                recovered_syms += len(lost_src)
            else:
                recovered = False
                busted_blocks += 1
                for i in lost_src:                  # FEC fell back to RTO
                    fec_stall += rto_stall(rnd, p)

        # TCP loses the SAME source segments (fair: identical channel)
        for i in lost_src:
            tcp_stall += rto_stall(rnd, p)
            lost_src_global.append(b * k + i)

        if want_blocks:
            blocks.append({
                "k": blk_k, "r": r,
                "lost_src": lost_src, "lost_red": lost_red,
                "recovered": recovered,
            })
        seg += blk_k

    # idealised TCP recovers all of this transfer's source losses with SACK
    tcp_fast_stall = sack_stall(rnd, lost_src_global, S, p, W)

    serialize_tcp = S * sym / RATE_BPS * 8000.0
    serialize_fec = (S + r * nblocks) * sym / RATE_BPS * 8000.0
    overhead = (serialize_fec / serialize_tcp - 1.0) * 100.0 if serialize_tcp else 0.0

    return {
        "params": {"k": k, "r": r, "n": k + r, "sym": sym,
                   "loss_pct": loss_pct, "seed": seed,
                   "file_bytes": int(len(data)), "segments": S, "blocks": nblocks,
                   "window_segments": W},
        "totals": {
            "wire_packets_fec": S + r * nblocks,
            "wire_packets_tcp": S,
            "lost_packets": lost_pkts,
            "lost_source_syms": lost_syms,
            "recovered_syms": recovered_syms,
            "recovered_blocks": recovered_blocks,
            "busted_blocks": busted_blocks,
            "recover_failures": recover_failures,
            "overhead_pct": round(overhead, 2),
        },
        "timing_ms": {
            "fec": round(serialize_fec + RTT_MS + fec_stall, 1),
            "tcp_rto": round(serialize_tcp + RTT_MS + tcp_stall, 1),
            "tcp_fast": round(serialize_tcp + RTT_MS + tcp_fast_stall, 1),
        },
        "blocks": blocks,
    }


def sweep(data, k, r, sym, seed, losses, trials):
    """Each loss point is the average of `trials` independent runs, so the curve
    is smooth and monotonic instead of one-shot noise on a small file."""
    rows = []
    for i, lp in enumerate(losses):
        afec = arto = afast = abust = 0.0
        for t in range(trials):
            res = simulate(data, k, r, sym, lp,
                           seed + i * 7919 + t * 104729, want_blocks=False)
            afec += res["timing_ms"]["fec"]
            arto += res["timing_ms"]["tcp_rto"]
            afast += res["timing_ms"]["tcp_fast"]
            abust += res["totals"]["busted_blocks"]
        rows.append({"loss_pct": lp,
                     "fec": round(afec / trials, 1),
                     "tcp_rto": round(arto / trials, 1),
                     "tcp_fast": round(afast / trials, 1),
                     "busted_blocks": round(abust / trials, 2)})
    return {"params": {"k": k, "r": r, "sym": sym, "trials": trials}, "rows": rows}


# ------------------------------------------------------------------ http layer
def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def parse_params(q):
    g = lambda name, d: q.get(name, [d])[0]
    k = clamp(int(g("k", 8)), 1, MAX_K)
    r = clamp(int(g("r", 2)), 1, MAX_R)
    sym = clamp(int(g("sym", 512)), 1, 8192)
    seed = int(g("seed", 12345))
    return k, r, sym, seed


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):           # quiet console
        pass

    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self):
        n = int(self.headers.get("Content-Length", 0))
        n = min(n, MAX_FILE)
        return self.rfile.read(n) if n else b""

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/":
            path = "/index.html"
        fp = os.path.normpath(os.path.join(WEB, path.lstrip("/")))
        if not fp.startswith(WEB) or not os.path.isfile(fp):
            return self._send(404, "not found", "text/plain")
        ctype = {".html": "text/html", ".js": "application/javascript",
                 ".css": "text/css"}.get(os.path.splitext(fp)[1], "text/plain")
        with open(fp, "rb") as f:
            self._send(200, f.read(), ctype)

    def do_POST(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        data = self._read_body()
        if not data:                      # no file -> deterministic random data
            size = clamp(int(q.get("size", ["262144"])[0]), 1, MAX_FILE)
            rng = random.Random(int(q.get("seed", ["12345"])[0]))
            data = bytes(rng.getrandbits(8) for _ in range(size))
        k, r, sym, seed = parse_params(q)
        try:
            if u.path == "/api/simulate":
                loss = clamp(float(q.get("loss", ["2.0"])[0]), 0.0, 100.0)
                out = simulate(data, k, r, sym, loss, seed)
            elif u.path == "/api/sweep":
                losses = [0, 0.25, 0.5, 1, 1.5, 2, 3, 4, 5, 6, 8, 10]
                trials = clamp(int(q.get("trials", ["200"])[0]), 1, 500)
                out = sweep(data, k, r, sym, seed, losses, trials)
            else:
                return self._send(404, "not found", "text/plain")
        except Exception as e:            # surface codec/param errors to the UI
            return self._send(400, json.dumps({"error": str(e)}))
        self._send(200, json.dumps(out))


if __name__ == "__main__":
    print(f"FEC live demo on http://localhost:{PORT}  (real codec: {SO})")
    print(f"geometry limits: k<= {MAX_K}, r<= {MAX_R}   Ctrl-C to stop")
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
