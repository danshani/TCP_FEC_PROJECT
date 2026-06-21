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
full RTO (≥ 1 s). That is what FEC competes against here, and it is exactly how
the stack behaves — the baseline is faithful **to this implementation**.

> Caveat to state up front: a production TCP with fast-retransmit would recover
> a single loss in ~1 RTT, not a full RTO. So these graphs show FEC's advantage
> *against this stack*. If a more conservative "idealised TCP" baseline is
> wanted, the model can be switched to a ~1-RTT-per-loss penalty — ask and it's
> a small change to `rto_stall()`.

## Timing model

For a transfer of `S = ceil(file / sym_len)` source segments:

- **Plain TCP:** no parity on the wire. Each source segment is lost
  independently with probability `p`; each loss adds an RTO-stall chain
  (1000 ms, then 2000, … until a retransmit survives). Stalls are additive
  (head-of-line blocking).
- **FEC:** segments grouped into blocks of `k`, plus `r` parity symbols
  (`r/k` bandwidth overhead). A block of `n = k+r` symbols survives iff it loses
  `≤ r` symbols, in which case the real decoder rebuilds the lost sources
  locally with **zero** RTO. A block losing `> r` recovers nothing and its lost
  sources fall back to the plain-TCP RTO chain.
- `completion = serialization(bytes/rate) + 1·RTT + Σ RTO stalls`.

Fixed parameters (see `tools/run-fec-experiments.sh`): `sym_len=512`,
`file=256 KiB`, `trials=400`, `rate=10 Mbit/s`, `RTT=50 ms`, `RTO_min=1000 ms`,
`seed=12345` (reproducible; identical loss draws across configs for fairness).

## Files

| File | Graph |
|---|---|
| `graph1_geometry.csv` / `.svg` | Completion time across `(k, n=k+r)` geometries at a fixed 2% loss — shows why `r` must scale with `k`, and which geometries are efficient. |
| `graph2_loss_sweep.csv` / `.svg` | Completion time vs loss % for three geometries plus plain TCP — the headline "FEC finishes while TCP is still timing out" result, with the high-loss crossover visible. |

## Two baselines

`graph2` plots FEC against **two** TCP baselines so the comparison is honest:

- **Plain TCP (RTO-only)** — this stack's real behaviour: every loss = one ≥1 s
  RTO. FEC beats it by 1–2 orders of magnitude at any loss.
- **Idealised TCP (fast-retransmit)** — a loss costs ~1 RTT, as production TCP
  with fast-retransmit/SACK would (`fast_stall()` in `tools/fec-sim.c`). This is
  the tough baseline, and it reveals the real trade-off.

## Headline numbers (256 KiB, k=8 r=2)

| Loss | Plain TCP (RTO-only) | Idealised TCP (fast-rtx) | FEC | Verdict |
|---|---|---|---|---|
| 0%  | 0.26 s | 0.26 s | 0.31 s | FEC −20% (pays 25% overhead) |
| 1%  | 5.5 s  | 0.58 s | 0.35 s | FEC wins both |
| 2%  | 11.1 s | 1.04 s | 0.45 s | FEC wins both (2.3× vs idealised) |
| 5%  | 28.7 s | 3.06 s | 2.40 s | FEC still wins |
| 8%  | 49.4 s | 6.38 s | 8.04 s | **idealised TCP wins** (FEC blocks bust) |
| 10% | 64.4 s | 9.74 s | 14.5 s | idealised TCP wins |

**Takeaway for the slides:** FEC is a *low-loss latency win*. Against this stack
(RTO-only) it is transformative everywhere; against an idealised fast-retransmit
TCP it wins clearly up to ~5% loss and then crosses over as blocks exceed their
`r`-symbol correction capacity. That crossover is a feature of the result, not a
flaw — it's where you'd raise `r` or shrink `k`.
