#!/usr/bin/env bash
#
# Generate the CSV datasets for the two presentation graphs, using build/fec-sim
# (real GF(256) codec + this stack's RTO-only timing model).
#
#   results/graph1_geometry.csv  — completion time across (k, n=k+r) at a fixed
#                                  loss rate. Motivates which geometries to pick.
#   results/graph2_loss_sweep.csv — completion time vs loss % for three chosen
#                                  geometries plus the plain-TCP baseline.
#
# Run from the repo root:  bash tools/run-fec-experiments.sh
set -euo pipefail

SIM=build/fec-sim
[ -x "$SIM" ] || { echo "build $SIM first: make fec-sim"; exit 1; }

# ---- fixed experiment parameters (documented so the graphs are reproducible) -
SYM=512                # symbol length (bytes) — matches TCP_FEC_DEFAULT_SYM_LEN
FILE=262144            # transfer size: 256 KiB
TRIALS=400             # Monte-Carlo runs per point
RATE=10000000          # link rate: 10 Mbit/s
RTT=50                 # base round-trip time (ms)
RTO_MIN=1000           # stack's initial/min RTO (src/tcp_input.c:228)
RTO_CAP=60000          # RFC 6298 RTO ceiling (src/tcp_output.c)
SEED=12345             # fixed -> reproducible; same loss draws across configs

mkdir -p results
HDR="k,r,sym,file,loss_pct,trials,t_tcp_ms,t_tcp_fast_ms,t_fec_ms,overhead_pct,busted_blocks,recover_fail"

# ---- Graph 1: geometry sweep at a fixed, low loss rate ----------------------
G1_LOSS=2.0
echo "$HDR" > results/graph1_geometry.csv
for K in 4 8 16 32; do
  for R in 1 2 3 4; do
    "$SIM" "$K" "$R" "$SYM" "$FILE" "$G1_LOSS" "$TRIALS" \
           "$RATE" "$RTT" "$RTO_MIN" "$RTO_CAP" "$SEED" \
      >> results/graph1_geometry.csv
  done
done
echo "wrote results/graph1_geometry.csv (loss=${G1_LOSS}%)"

# ---- Graph 2: loss sweep for three chosen geometries ------------------------
# Three operating points spanning the overhead/protection trade-off:
#   k8  r2  -> 25%   overhead, strong protection
#   k16 r3  -> ~19%  overhead, balanced
#   k16 r2  -> ~12%  overhead, lean
CONFIGS=("8 2" "16 3" "16 2")
LOSSES=(0 0.25 0.5 1 1.5 2 3 4 5 6 8 10)
echo "$HDR" > results/graph2_loss_sweep.csv
for cfg in "${CONFIGS[@]}"; do
  read -r K R <<< "$cfg"
  for P in "${LOSSES[@]}"; do
    "$SIM" "$K" "$R" "$SYM" "$FILE" "$P" "$TRIALS" \
           "$RATE" "$RTT" "$RTO_MIN" "$RTO_CAP" "$SEED" \
      >> results/graph2_loss_sweep.csv
  done
done
echo "wrote results/graph2_loss_sweep.csv (configs: ${CONFIGS[*]})"

# sanity: no recovery failures anywhere (real decoder must verify byte-exact)
fails=$(awk -F, 'NR>1 && $12>0 {s+=$12} END{print s+0}' \
        results/graph1_geometry.csv results/graph2_loss_sweep.csv)
echo "total recover_failures across all points: $fails"
[ "$fails" = "0" ] || { echo "FAIL: codec produced recovery errors"; exit 1; }
echo "OK — all recoveries byte-verified by the real decoder"
