# FEC Algorithm: GF(256) Polynomial Erasure Recovery

This document explains the Forward Error Correction (FEC) algorithm implemented in this project at:

- include/fec.h
- src/fec.c
- tests/fec-selftest.c

The implementation follows a Reed-Solomon style polynomial-evaluation design over GF(256), optimized for packet-oriented erasure recovery.

## 1. Design Goal

Given `k` source packets of equal length, generate redundancy so that missing data can be reconstructed without retransmission.

Current implementation supports:

- Single redundant packet generation at a chosen field point.
- Fast recovery of one missing source coefficient packet from surviving source packets + one redundant packet.
- General interpolation path that reconstructs all coefficients from `k` point-value packets.

## 2. Data Model

For one block of `k` source packets, each packet is treated as one coefficient of a polynomial:

P(x) = a0 + a1 x + a2 x^2 + ... + a(k-1) x^(k-1)

where:

- `a_i` is packet `i`.
- All operations are in GF(256).
- Each byte offset is processed independently, so the same polynomial math is applied lane-by-lane across packet bytes.

If there are 4 source packets, that is a degree-3 polynomial.

## 3. Why GF(256)

GF(256) is the standard finite field for byte-level coding:

- Every symbol is exactly one byte (0..255).
- Addition is XOR (fast and safe).
- Multiplication/division are done via lookup tables.

Benefits:

- No integer overflow behavior to reason about at algorithm level.
- Constant-time field ops (table lookups + XOR).
- Good CPU efficiency for packet processing.

## 4. Field Arithmetic Implementation

Context type:

- `struct fec_rs_ctx` stores:
  - `log_lut[256]`
  - `exp_lut[512]`

Initialization:

- `fec_rs_init()` builds tables using primitive polynomial `0x11d`.
- `exp_lut` is duplicated to length 512 so multiplication can avoid modulo 255 in hot paths.

Core identities used:

- add(a, b) = a XOR b
- mul(a, b) = exp(log(a) + log(b)) for non-zero inputs
- div(a, b) = exp(log(a) - log(b)) for non-zero denominator

## 5. Redundancy Generation

API:

- `fec_poly_encode_redundant(ctx, src_packets, source_count, packet_len, redundant_x, redundant_packet_out)`

What it computes:

- One extra point value `P(x_r)` where `x_r = redundant_x`.

Per byte offset `j`:

- `redundant[j] = sum(i=0..k-1) src[i][j] * x_r^i` in GF(256).

Interpretation:

- This redundant packet is one additional point on the same data polynomial curve.

## 6. Fast Single-Missing Recovery

API:

- `fec_poly_recover_missing_coefficient(ctx, src_packets_with_one_null, source_count, packet_len, missing_index, redundant_x, redundant_packet, recovered_packet_out)`

Assume coefficient `a_m` is missing and we know `P(x_r)`.

Rearrangement:

- `P(x_r) = known_sum + a_m * x_r^m`
- `a_m = (P(x_r) + known_sum) / x_r^m`

(`+` is XOR in GF(2^8), so subtraction is the same operation.)

This gives direct recovery of one missing source coefficient packet.

## 7. General Interpolation Path

API:

- `fec_poly_interpolate_coefficients(ctx, points_x, points_y, point_count, packet_len, coeff_packets_out)`

Inputs:

- `point_count = k` distinct x-values.
- `points_y[i] = P(points_x[i])` packet.

Process per byte lane:

1. Compute Newton divided differences over GF(256).
2. Build polynomial in Newton basis.
3. Convert to power basis coefficients.
4. Write recovered coefficients to output packets.

This path reconstructs the exact original coefficient packets when provided enough valid points.

## 8. Correctness Guarantees and Limits

Current guarantees:

- Exact byte-for-byte reconstruction for supported erasure patterns.
- Input validation checks for null pointers, range limits, and duplicate interpolation x-values.

Current limits in this module:

- `source_count` and `point_count` are limited to 255 (GF(256) symbol space).
- Fast recovery API is for one missing coefficient with one redundant packet.
- Multi-loss recovery requires either more redundant points and matrix solve or repeated interpolation setup.

## 9. Complexity Notes

Let:

- `k` = number of source packets
- `L` = packet length in bytes

Costs:

- Redundant generation: O(k * L)
- Single-missing direct recovery: O(k * L)
- Full interpolation: O(k^2 * L)

For small `k` in low-latency transport workloads, these are practical and efficient.

## 10. Test Coverage

`tests/fec-selftest.c` validates two critical paths:

1. Single missing-packet recovery:
   - Build redundancy from known coefficients.
   - Remove one packet.
   - Recover and compare to original.

2. Interpolation reconstruction:
   - Evaluate same polynomial at 4 distinct x points.
   - Interpolate back to coefficients.
   - Compare all reconstructed coefficients to originals.

Run with:

    make fec-test

Expected output:

- `FEC selftest passed`

## 11. Integration Guidance

Typical sender flow:

1. Choose `k` source packets of equal length.
2. Choose `x_r` (non-zero and not colliding with other chosen evaluation points).
3. Call `fec_poly_encode_redundant()`.
4. Transmit source packets + redundant packet metadata (`x_r`, block id, packet index).

Typical receiver flow (single loss case):

1. Identify missing source index in the block.
2. Gather surviving source packets and redundant packet.
3. Call `fec_poly_recover_missing_coefficient()`.
4. Continue in-order delivery.

If recovery is not possible with direct path, fallback to interpolation using enough points.

## 12. Future Extensions

Natural next upgrades:

- Multiple redundant packets per block (`n-k > 1`).
- Decoder for multiple erasures via Vandermonde solve or Gaussian elimination in GF(256).
- SIMD acceleration for byte-lane multiply/XOR loops.
- Optional systematic RS block framing metadata helpers.

## 13. Summary

This implementation provides a practical, byte-safe GF(256) polynomial FEC core that matches the required model:

- Source packets as polynomial coefficients.
- Redundancy from additional polynomial evaluations.
- Loss recovery via finite-field algebra and interpolation.
- Lookup-table arithmetic for CPU efficiency.
