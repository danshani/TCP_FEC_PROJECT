# TCP-FEC live demo

An interactive, browser-based visualiser: drop in **any file**, pick the FEC
geometry (`k` source + `r` parity, so `n = k+r`) and a packet-loss rate, and
watch the **real GF(256) Reed-Solomon codec** reconstruct lost packets in real
time — then compare completion time against plain TCP.

The encode/decode is **not** a JavaScript reimplementation. The browser only
draws; every parity byte and every recovered byte is produced by
`build/libfecsim.so`, a thin shim over the project's actual codec
(`src/fec.c` + `src/fec_codec.c`). The backend byte-verifies every recovery and
reports `recover_failures` (must be 0).

## Run it (under WSL)

```bash
# from the repo root
bash tools/fec-demo/run_demo.sh      # builds the .so and starts the server
```

Then open <http://localhost:8088> in your browser. (WSL2 forwards `localhost`,
so a Windows browser reaches the WSL server directly.)

To stop: Ctrl-C in the terminal.

## What you see

- **Block grid** — one row per FEC block of `k+r` cells: green = source,
  blue = parity, red = lost, green-with-blue-ring = a source FEC rebuilt
  locally. A red-outlined row is an *unrecoverable* block (it lost more than `r`
  symbols), which falls back to TCP retransmission.
- **Live counters** — packets sent / lost, symbols recovered by FEC,
  unrecoverable blocks, bandwidth overhead, progress.
- **Completion time** — FEC vs *Plain TCP (RTO-only, this stack)* vs
  *Idealised TCP (fast-retransmit)*.
- **Loss sweep → graph** — re-runs the current file/geometry across a range of
  loss rates and plots completion time vs loss (the presentation graph, live).

## How the numbers are produced

- **Recovery (real):** `server.py` chunks the file into `sym`-byte segments,
  groups them into blocks of `k`, encodes `r` parity via the C lib, drops
  symbols per a seeded loss process, and recovers via the C lib with byte
  verification.
- **Timing (modelled):** same model as `results/README.md` /
  `tools/fec-sim.c` — this stack's RTO (1000 ms, RFC 6298 backoff, no
  fast-retransmit). Plain TCP pays a full RTO per loss; FEC recovers within a
  block for free and only RTOs on busted blocks; idealised TCP pays ~1 RTT per
  loss.

Fixed model constants live at the top of `server.py`
(`RATE_BPS`, `RTT_MS`, `RTO_MIN`, `RTO_CAP`).

## Files

| File | Role |
|---|---|
| `fecsim_shim.c` | flat C API over the real codec → `build/libfecsim.so` |
| `build_demo.sh` | builds the shared library |
| `server.py` | stdlib HTTP backend; calls the codec via `ctypes` |
| `run_demo.sh` | build + serve in one command |
| `web/` | the browser UI (`index.html`, `app.js`, `style.css`) |
