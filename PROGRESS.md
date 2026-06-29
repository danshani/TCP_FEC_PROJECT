# TCP-FEC Project — Progress Log

_Last updated: 2026-06-21 (added measurement + live demo workstream)_

## NEW: measurement harness + live demo (for the presentation)

Goal shifted to *demonstrating* FEC's value with graphs and a live app. Both use
the **real GF(256) codec** (`src/fec.c` + `src/fec_codec.c`) for recovery; only
the transfer *timing* is modelled, from this stack's real numbers (RTO=1000 ms,
RFC 6298 backoff, **no fast-retransmit** — dup ACKs ignored at `tcp_input.c:591`,
so every loss costs a full RTO).

- **Simulator:** `tools/fec-sim.c` (+ `make fec-sim`). Bernoulli loss across a
  whole file; every block recovered by the actual decoder and byte-verified
  (`recover_fail` must be 0). Two TCP baselines: RTO-only (this stack) and
  idealised **SACK fast-recovery** (`sack_stall()`: holes in one RTT-window
  recovered together in ~1 RTT, window = bandwidth-delay product).
- **Datasets/graphs:** `tools/run-fec-experiments.sh` → `results/*.csv`;
  `tools/plot_fec.py` → `results/*.svg`. `results/README.md` documents the model
  and headline numbers. Graph 1 = time by geometry (k,n) at fixed loss; Graph 2 =
  time vs loss for 3 geometries + both baselines; Graph 3 = "winning band"
  (overhead vs the max loss FEC still beats SACK-TCP, per geometry).
  Key result, two stories: **vs RTO-only** FEC is transformative everywhere;
  **vs realistic SACK TCP** FEC is a targeted low-loss win (k=8,r=2 to ~2%
  loss), and the crossover is engineered by geometry (bigger blocks / more `r`
  push it higher; `r=1` never wins).
- **Live demo:** `tools/fec-demo/` — drop any file, pick k/r/loss, watch the
  real codec rebuild lost packets live, then compare completion time and run a
  loss-sweep graph (averaged over N trials so it's smooth). Backend `server.py`
  (stdlib http + ctypes) calls `build/libfecsim.so` (shim
  `tools/fec-demo/fecsim_shim.c` over the real codec). Same SACK model as the
  offline simulator. Run: `bash tools/fec-demo/run_demo.sh` →
  <http://localhost:8088>. All endpoints tested; 0 recovery failures.

---

## What this is

Adding a **Forward Error Correction layer to Level-IP** (a userspace TCP/IP stack).
The FEC is a **systematic Reed-Solomon erasure code over GF(256)**: the sender groups
every `k` TCP data segments into a block and emits `r` parity ("repair") segments, so a
receiver can **reconstruct a lost segment from parity before TCP's retransmission timer
fires** — trading bandwidth for latency on lossy links. It's negotiated at SYN time and
only engages between two Level-IP peers; against anything else it falls back to plain TCP.

Geometry (negotiated, currently fixed): `k = 8`, `r = 2`, `sym_len = 512`.

---

## Branch / commit map (all pushed to github.com/danshani/TCP_FEC_PROJECT)

```
main
  26953d0  Priority #1: TX encode/flush + SYN-time negotiation

feature/fec-synack-negotiation   (stacked on main)
  cbd3685  Emit TCP options on the SYN-ACK so FEC can negotiate both ways
  2d06fe4  Add a wire-format selftest for the FEC handshake
  777f89d  liblevelip: handle FD flags in fcntl instead of returning EINVAL

feature/fec-receiver             (stacked on feature/fec-synack-negotiation) <-- CURRENT
  019ecc1  Fix skb->payload to skip TCP options, not just the header
  592e3ca  Add the FEC receiver: decode lost segments and splice them in
  800960c  Match thread-entry and init function signatures
```

All commits authored as `danshani <danshani11@gmail.com>`, no co-author trailers.
`feature/fec-receiver` contains the full stack of all the above relative to `main`.

Not yet merged to `main`. Suggested merge order: synack branch first, then receiver.

---

## Architecture / where things live

**FEC core (pre-existing, standalone, fuzz-tested):**
- `include/fec.h`, `src/fec.c` — GF(256) polynomial primitives (tables, mul/div, Horner, interpolation)
- `include/fec_codec.h`, `src/fec_codec.c` — systematic block encoder/decoder

**Wire format (ours):** `include/fec_frame.h`
- `TCP_OPT_FEC_PERM` (opt 30) — SYN negotiation: version/k/r/sym_len
- `TCP_OPT_FEC` (opt 31) — per-segment tag: block_id + symbol index + type (source/repair)
- `fec_repair_hdr` — prefixes a repair segment's payload (base_seq, tail_len, k, ...)

**TX path:** `src/tcp_output.c`
- `tcp_send` intercepts MSS payloads, tags source symbols, zero-copy / ragged staging,
  synchronous full-block flush, out-of-band repair emission (no seq consumed), FIN-time flush
- `tcp_fec_init_shared / enable / release` lifecycle; shared GF tables

**Negotiation:** `src/tcp_output.c` (SYN + SYN-ACK option emit), `src/tcp_input.c` (`tcp_parse_opts`)
- Exact-geometry match required; any mismatch → plain TCP

**RX path:** `src/tcp_input.c`
- `tcp_fec_input` intercepts FEC-tagged segments *before* the window check: repair
  consumed (never enters stream), source registered then delivered normally
- `tcp_fec_rx_recover` reconstructs missing symbols; `tcp_fec_splice` injects them via
  `tcp_data_queue` so `rcv_nxt` advances past the gap
- Per-connection 4-slot block ring (`struct tcp_fec` / `tcp_fec_block_rx` in `include/tcp.h`)

**Supporting fixes:**
- `src/tcp.c` — `skb->payload` now skips TCP options (latent bug; was `th->data`)
- `tools/liblevelip.c` — `fcntl` F_GETFD/F_SETFD delegation (curl set CLOEXEC → EINVAL before)
- `ipc/netdev/timer` — pthread entry / init signature fixes

**Tests + Makefile targets:**
- `make fec-test` / `fec-codec-test` — core + codec
- `make handshake-test` — wire-format oracle (`tests/handshake-wireformat-selftest.c`)
- `make fec-receiver-test` — receiver recovery (`tests/fec-receiver-selftest.c`)

---

## Test status (as of last run)

| Check | Result |
|---|---|
| Whole-stack compile, `-Wall -Werror` | ✅ |
| Core / codec / handshake / receiver selftests | ✅ all pass |
| Sanitized fuzz (ASan+UBSan): codec 900k (3 seeds) + receiver | ✅ pass |
| Live stack: starts, ICMP 0% loss | ✅ |
| Live small HTTP GET through stack (no-regression) | ✅ |
| Live **large** (354 KB) HTTP GET through stack | ❌ **fails — see Known Issue #1** |

The TX + negotiation + RX code is implemented, compiles clean, and the recovery logic is
byte-verified. **What is NOT yet proven: an actual on-the-wire FEC recovery** (needs two
FEC peers — see Remaining Work #1).

---

## Known issues / open questions

### #1 — Large transfers through lvl-ip fail with IPC desync  🚩 (investigate first)
A ~354 KB file fetched via `./level-ip curl` fails on every attempt with
`ERR: IPC read response expected type 4, actual: type <garbage>, pid <garbage>` and an
md5 mismatch. Small transfers work fine.

**Assessment: strongly suspected PRE-EXISTING, not our FEC work.** Reasoning:
- The desync is in the `liblevelip`↔`lvl-ip` **IPC control protocol**, which sits *above*
  the TCP layer we changed.
- Our `skb->payload` fix is a provable **no-op** for the server's segments (they carry no
  TCP options → offset 0 → byte-identical to before).
- The receiver code is **dormant** (FEC isn't negotiated against a real Linux server, so
  `tsk->fec == NULL` and `tcp_fec_input` is never entered).
- We never touched the IPC read/streaming path.
- It only appears under volume (many IPC round-trips) — classic pre-existing streaming bug.

**Could NOT confirm empirically** because the pre-FEC commit `8c1f379` does not build on
the GCC 15.2 toolchain (`-Wincompatible-pointer-types` is a hard error now; baseline has
`tcp_alloc_sock`/thread-entry signature mismatches). Confirming requires patching the
baseline's signatures too.

**Next step:** reproduce on a minimal patched baseline (or instrument `ipc.c`/`liblevelip.c`
read framing) to localize the desync. Look at `src/ipc.c` IPC response sizing and
`tools/liblevelip.c` `transmit_lvlip`/read loop for large/partial reads.

### #2 — No way to test FEC recovery end to end
Requires **two FEC-speaking peers** with loss between them. The stack can't host that:
no `listen`/`accept` (no lvl-ip server), and `tap0` / IP `10.0.0.5` / `/tmp/lvlip.socket`
are all hardcoded, so two instances collide.

---

## Remaining work (prioritized)

1. **Investigate Known Issue #1** (large-transfer IPC desync) — confirm pre-existing and,
   ideally, fix it, since it blocks any meaningful throughput test.
2. **Two-peer test scaffolding** — parameterize tap/IP/IPC-socket (env or CLI), or add a
   minimal loopback/server mode, so we can run two FEC peers + `tc netem` loss and finally
   **watch a dropped segment get reconstructed instead of retransmitted.** This is the one
   thing the whole feature still hasn't been proven to do live.
3. **Priority #2 — kill per-block heap allocations** (`fec_encoder_generate`,
   `fec_decoder_recover`, `fec_poly_interpolate_coefficients`) → per-socket arenas.
4. **Priority #3 — SIMD GF(256) multiply** (PSHUFB/NEON) for the O(k²·L) kernel + Horner.
5. **Docs** — `Documentation/fec-algorithm.md` covers the codec but not the TCP
   integration / wire format / negotiation.
6. **Merge** `feature/fec-synack-negotiation` → `main`, then `feature/fec-receiver`.

---

## Environment / how to build & test

- Built and run under **WSL2** (Ubuntu, GCC 15.2). Repo lives on `/mnt/c/...` (v9fs, which
  *does* store file caps here).
- `lvl-ip` needs `cap_setpcap,cap_net_admin` — **`setcap` does NOT survive a relink**, so
  re-`setcap` after every build (or run via `make debug`, whose recipe does it). A fresh
  binary without caps exits at startup with `Error on network admin capability drop`.
- `/usr/bin/ip` also needs `cap_net_admin` (the `/usr/sbin/ip` symlink can't be `setcap`'d).
- Offline battery (no root): `make fec-test fec-codec-test handshake-test fec-receiver-test`
  plus sanitized fuzz of the codec/receiver selftests.
- Live: `./setup_tap.sh`, then `setcap` the binary, run `./lvl-ip &`, and
  `( cd tools && ./level-ip curl ... )`. The full `make test` suite is flaky here
  (parallel stacks collide on the single tap/socket; `tc netem` duplication unsupported on
  this kernel) — prefer a single-stack manual curl.

---

## Where to resume tomorrow

Current branch `feature/fec-receiver` @ `800960c`, working tree clean (only `.claude/` and
the `.code-workspace` untracked). Everything is committed and pushed.

**Start with Known Issue #1** (large-transfer IPC desync) — decide whether to confirm it's
pre-existing and fix it, or move to the two-peer scaffolding (#2) to prove recovery. Both
are good next moves; #1 unblocks throughput testing, #2 unblocks the headline feature demo.
