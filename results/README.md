# FEC vs TCP — completion-time experiments

Datasets and graphs for the presentation. Everything here is regenerated from
source; nothing is hand-edited.

## How to regenerate

```bash
make fec-sim                       # build the simulator (build/fec-sim)
bash tools/run-fec-experiments.sh  # writes results/graph1_*.csv, graph2_*.csv
python3 tools/plot_fec.py          # writes results/*.svg
```

## What is REAL vs MODELLED (read before defending the numbers)

**Real — the erasure recovery.** Every block the simulator claims FEC recovers
is reconstructed by the project's actual GF(256) decoder (`src/fec.c` +
`src/fec_codec.c`) and the recovered bytes are compared to the originals. The
`recover_fail` column is the count of mismatches and is **0 at every data
point** — i.e. these graphs are produced by the same recovery code that runs in
the stack, not by a combinatorial approximation of it.

**Modelled — the transfer timing.** Completion time is computed from this
stack's *real* loss-recovery behaviour:

| Parameter | Value | Source in the code |
|---|---|---|
| Initial / minimum RTO | 1000 ms | `src/tcp_input.c:228` |
| RTO backoff | ×2 per consecutive loss | `src/tcp_output.c:451` (RFC 6298) |
| Fast-retransmit / SACK recovery | **none** — dup ACKs ignored | `src/tcp_input.c:591` |

Because Level-IP has **no fast retransmit**, its *only* loss-recovery mechanism
is the RTO timer, so every lost segment stalls the transfer for at least one
full RTO (≥ 1 s). That is what FEC competes against on this stack, and it is
exactly how the stack behaves — the baseline is faithful **to this
implementation**.

To avoid over-claiming, we also model a **second, tougher baseline**: production
TCP with **SACK fast-recovery**, where all the holes in one RTT-window are
retransmitted together in ~1 RTT (not one RTT per loss). See `sack_stall()` in
`tools/fec-sim.c`. The window is the bandwidth-delay product (the TCP-favouring
choice). This is the honest "vs real-world TCP" line.

## Timing model

For a transfer of `S = ceil(file / sym_len)` source segments:

- **Plain TCP (RTO-only, this stack):** no parity on the wire. Each source
  segment is lost independently with probability `p`; each loss adds an RTO-stall
  chain (1000 ms, then 2000, … until a retransmit survives). Stalls are additive
  (head-of-line blocking).
- **Idealised TCP (SACK fast-recovery):** same losses, but the holes within one
  RTT-window are recovered together in ~1 RTT. A re-lost hole costs another
  round; the un-fast-retransmittable tail waits for an RTO.
- **FEC:** segments grouped into blocks of `k`, plus `r` parity symbols
  (`r/k` bandwidth overhead). A block of `n = k+r` symbols survives iff it loses
  `≤ r` symbols, in which case the real decoder rebuilds the lost sources
  locally with **zero** RTO. A block losing `> r` recovers nothing and its lost
  sources fall back to the plain-TCP RTO chain.
- `completion = serialization(bytes/rate) + 1·RTT + Σ stalls`.

Fixed parameters (see `tools/run-fec-experiments.sh`): `sym_len=512`,
`file=256 KiB`, `trials=400`, `rate=10 Mbit/s`, `RTT=50 ms`, `RTO_min=1000 ms`,
`seed=12345` (reproducible; identical loss draws across configs for fairness).

## Files

| File | Graph |
|---|---|
| `graph1_geometry.csv` / `.svg` | Completion time across `(k, n=k+r)` geometries at a fixed 2% loss — shows why `r` must scale with `k`, and which geometries are efficient. |
| `graph2_loss_sweep.csv` / `.svg` | Completion time vs loss % for three geometries against **both** TCP baselines — the headline result, with the high-loss crossover visible. |
| `graph3_winband.csv` / `.svg` | Per geometry: the highest loss rate at which FEC still beats idealised SACK-TCP, plotted against bandwidth overhead — the FEC design space. |

## Two baselines

`graph2` plots FEC against **two** TCP baselines so the comparison is honest:

- **Plain TCP (RTO-only)** — this stack's real behaviour: every loss = one ≥1 s
  RTO. FEC beats it by 1–2 orders of magnitude at any loss.
- **Idealised TCP (SACK fast-recovery)** — holes in one RTT-window recovered
  together in ~1 RTT, as production TCP does (`sack_stall()` in
  `tools/fec-sim.c`). This is the tough baseline, and it reveals the real
  trade-off: FEC wins only in a **low-loss band**.

## Headline numbers (256 KiB, k=8 r=2)

| Loss | Plain TCP (RTO-only) | Idealised TCP (SACK) | FEC | Verdict |
|---|---|---|---|---|
| 0%  | 0.26 s | 0.26 s | 0.31 s | FEC −17% (pays 25% overhead) |
| 1%  | 5.5 s  | 0.44 s | 0.36 s | FEC wins both |
| 2%  | 11.1 s | 0.52 s | 0.47 s | FEC wins both (marginally vs SACK) |
| 3%  | 17.0 s | 0.57 s | 0.74 s | **SACK TCP wins** (FEC starts busting) |
| 5%  | 28.6 s | 0.67 s | 2.36 s | SACK TCP wins |
| 10% | 65.1 s | 0.94 s | 14.5 s | SACK TCP wins |

**Takeaway for the slides:** FEC trades bandwidth for latency.

- **vs this stack (RTO-only):** transformative everywhere — 1–2 orders of
  magnitude, because the stack has no fast-retransmit (every loss = a full RTO).
- **vs realistic SACK TCP:** FEC is a **targeted low-loss latency tool**. For
  k=8,r=2 it wins up to ~2% loss, then loses (it pays overhead *and* its blocks
  bust). The crossover loss is **engineered by geometry** (`graph3`): more parity
  or larger blocks push it higher — e.g. k=16,r=4 holds to ~5%, and k=32,r=4
  reaches ~3% at only 12.5% overhead (beating k=8,r=2 at half the cost). `r=1` is
  useless — it never beats SACK TCP.
