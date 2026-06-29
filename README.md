# TCP‑FEC — Forward Error Correction for TCP

> A systematic Reed–Solomon erasure code bolted into the transport layer, so a receiver
> **rebuilds lost packets locally instead of waiting a full round‑trip for a retransmission.**

TCP recovers from packet loss under one assumption: that loss means **congestion**. On
physically noisy links (Wi‑Fi, cellular) that assumption is wrong — packets are corrupted by
transient noise, not queue overflow — and TCP pays for the misdiagnosis in **latency**, because
recovering even a single lost segment costs a full RTT retransmission that stalls every byte
queued behind it (head‑of‑line blocking).

This project adds **Forward Error Correction (FEC)** to TCP as a *proactive* alternative to
reactive retransmission. The sender transmits a little redundancy ahead of time; the receiver
reconstructs losses immediately, with **zero round‑trips**.

Built on the [Level‑IP](https://github.com/saminiir/level-ip) userspace TCP/IP stack.

---

## How it works

**Systematic Reed–Solomon over GF(256).** Each block of `k` source segments defines a
polynomial; `r` parity segments are further evaluations of that same polynomial. Any `k` of the
`n = k + r` coded symbols reconstruct the whole block via Lagrange interpolation — so the
receiver can lose **any `r`** symbols per block and recover with no retransmission.

```
 k source packets  =  points P(1) … P(k)     (your real data, sent unchanged — "systematic")
 r parity packets  =  points P(k+1) … P(k+r) (computed from ALL sources)
 receive any k of the n points  →  interpolate P  →  rebuild the missing ones
```

**Default geometry:** `k = 8`, `r = 2`, `sym_len = 512` (25 % overhead, survives any 2 of 10).

**Negotiated, backward‑compatible.** FEC is advertised as a standard TCP option (`FEC‑PERM`)
during the SYN/SYN‑ACK handshake and engages **only** when both peers agree on identical
geometry. Against a peer that doesn't understand the option, the connection degrades gracefully
to plain TCP. Parity is injected **out‑of‑band** (it consumes no TCP sequence space), so the
original byte stream is untouched and a non‑FEC receiver reads it normally.

**Why userspace.** The standard TCP header reserves only 40 bytes for options — too little for
per‑packet coding metadata — and writing finite‑field buffer code inside the kernel is
unforgiving. The mechanism was therefore moved out of the kernel and integrated into the
Level‑IP userspace stack, which interfaces with the host through a virtual **TAP** device.

```
 App ── userspace lib ── Level‑IP stack ──┬── FEC Encoder (tcp_output) ──┐
   (standard TCP, unchanged)              └── FEC Decoder (tcp_input)  ──┘── TAP ── wire
```

---

## Quick start

> The self‑tests and the demo build with just `gcc` / `python3` and run on any Linux (or WSL).
> Running the **full stack** (`make`) additionally needs Linux with `CAP_NET_ADMIN` and TUN/TAP.

### Run the test suite

```bash
make fec-test            # GF(256) field primitives + single-coefficient recovery + interpolation
make fec-codec-test      # systematic block codec (encode/decode), byte-exact, edge cases
make fec-receiver-test   # receiver-side recovery pipeline + seq-number mapping
make handshake-test      # wire-format oracle for the handshake option structs
```

### Run the interactive demo

A browser visualiser driven by the **real** GF(256) codec (`build/libfecsim.so`, a thin shim
over `src/fec.c` + `src/fec_codec.c`). Drop a file, pick `k`/`r`/loss, and watch lost packets
get rebuilt — then compare completion time against TCP.

```bash
bash tools/fec-demo/run_demo.sh      # builds the .so, starts a server on :8088
# open http://localhost:8088
```

### Reproduce the performance graphs

```bash
make fec-sim                         # build the trace-driven simulator
bash tools/run-fec-experiments.sh    # writes results/graph{1,2,3}_*.csv
python3 tools/plot_fec.py            # writes results/*.svg
```

---

## Results

FEC is **not** a universal win — it is a deliberate **trade of bandwidth for latency**. The
simulator drives the *real* codec (every recovery is byte‑verified) under a calibrated model of
this stack's loss‑recovery timing, against two baselines: plain RTO‑only TCP (this stack's real
behaviour) and an idealised SACK fast‑recovery TCP (real‑world TCP).

| Packet loss | Plain TCP (RTO‑only) | Idealised TCP (SACK) | FEC (k=8, r=2) |
|---|---|---|---|
| 0 % | 0.26 s | 0.26 s | 0.31 s *(pays 25 % overhead)* |
| 1 % | 5.5 s | 0.44 s | **0.36 s** *(wins both)* |

FEC beats RTO‑only TCP by 1–2 orders of magnitude at any loss, and beats idealised SACK‑TCP in a
**low‑loss band** (the regime of real Wi‑Fi/cellular links). The winning band is tunable through
block geometry — higher `r/k` survives higher loss at the cost of more bandwidth.

---

## Project structure

```
src/fec.c, fec.h               GF(256) field arithmetic + polynomial interpolation
src/fec_codec.c, fec_codec.h   systematic block encoder / decoder
src/tcp_output.c               FEC encode + parity emission (sender path)
src/tcp_input.c                FEC option parsing, negotiation, decode + splice (receiver path)
include/fec_frame.h            on-the-wire option / repair-header structs
tests/                         unit, integration, fuzz/sanitizer self-tests
tools/fec-demo/                interactive browser demo (real codec via ctypes)
tools/fec-sim.c, plot_fec.py   trace-driven simulator + plotting
results/                       generated CSVs + SVG graphs
Documentation/fec-algorithm.md the GF(256) algorithm, explained
```

---

## Verification & limitations

Correctness is established by a layered pyramid that runs the **real** code at every level:
unit tests for the field math, an integration test for the receiver data‑path, and randomized
fuzz/sanitizer runs (**> 900,000 blocks, zero byte‑level recovery failures, zero memory
errors**).

**Honest gap:** end‑to‑end recovery between two separate FEC‑speaking hosts on a live wire was
not demonstrated. The current Level‑IP environment hard‑codes a single TAP interface and IP, so
it cannot host two FEC peers at once, and an independent IPC issue blocks large transfers. The
recovery logic itself is proven byte‑exact across three independent layers — what remains is a
**systems‑integration milestone, not an algorithm or codec defect.**

## Roadmap

- `listen`/`accept` support + removing the hard‑coded TAP/IP/socket → run two peers on one host
- Fix the IPC issue blocking large transfers
- Live two‑peer over `tc netem`‑induced loss — close the verification gap above

---

## References

- RFC 3452 / RFC 6363 — IETF FEC building‑block and framework
- RFC 9265 — Forward Erasure Correction and congestion‑control interaction
- Reed–Solomon erasure codes over finite fields (Lagrange‑interpolation recovery)

## Credits

- **Author:** Dan Moshe Shani — final‑year project, Software Engineering, Azrieli College of Engineering, Jerusalem
- **Academic supervisor:** Dr. Shimrit Tzur‑David
- **Built on:** [Level‑IP](https://github.com/saminiir/level-ip) by Sami Niiranen

## License

MIT — see [LICENSE.md](LICENSE.md). Inherited from the upstream Level‑IP project.
