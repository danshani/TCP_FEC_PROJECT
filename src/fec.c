#include "fec.h"

#include <stdlib.h>
#include <string.h>

int fec_rs_init(struct fec_rs_ctx *ctx)
{
    uint16_t x = 1;

    if (ctx == NULL) return -1;

    memset(ctx->exp_lut, 0, sizeof(ctx->exp_lut));
    memset(ctx->log_lut, 0, sizeof(ctx->log_lut));

    /* Build GF(256) tables from primitive element powers under 0x11d. */
    for (int i = 0; i < 255; i++) {
        ctx->exp_lut[i] = (uint8_t)x;
        ctx->log_lut[x] = i;

        x <<= 1;
        if (x & 0x100) {
            x ^= FEC_GF_PRIMITIVE_POLY;
        }
    }

    /* Duplicate exponents so addition in multiplication can skip modulo ops. */
    for (int i = 255; i < 512; i++) {
        ctx->exp_lut[i] = ctx->exp_lut[i - 255];
    }

    return 0;
}

int fec_poly_encode_redundant(
    const struct fec_rs_ctx *ctx,
    const uint8_t *const *src_packets,
    int source_count,
    size_t packet_len,
    uint8_t redundant_x,
    uint8_t *redundant_packet_out)
{
    if (ctx == NULL || src_packets == NULL || redundant_packet_out == NULL) return -1;
    if (source_count <= 0 || source_count > 255) return -1;

    for (int i = 0; i < source_count; i++) {
        if (src_packets[i] == NULL) return -1;
    }

    /*
     * Horner's method: P(x) = a_0 + x*(a_1 + x*(... + x*a_{k-1}))
     * Uses k-1 GF multiplications instead of 2k-1 in the power-tracking approach.
     */
    for (size_t off = 0; off < packet_len; off++) {
        uint8_t y = src_packets[source_count - 1][off];
        for (int i = source_count - 2; i >= 0; i--)
            y = gf256_add(gf256_mul(ctx, y, redundant_x), src_packets[i][off]);

        redundant_packet_out[off] = y;
    }

    return 0;
}

int fec_poly_recover_missing_coefficient(
    const struct fec_rs_ctx *ctx,
    const uint8_t *const *src_packets,
    int source_count,
    size_t packet_len,
    int missing_index,
    uint8_t redundant_x,
    const uint8_t *redundant_packet,
    uint8_t *recovered_packet_out)
{
    uint8_t missing_weight = 1;

    if (ctx == NULL || src_packets == NULL || redundant_packet == NULL || recovered_packet_out == NULL) return -1;
    if (source_count <= 0 || source_count > 255) return -1;
    if (missing_index < 0 || missing_index >= source_count) return -1;

    for (int i = 0; i < source_count; i++) {
        if (i != missing_index && src_packets[i] == NULL) return -1;
    }

    /* Compute x_r^missing_index: the coefficient weight in P(x_r). */
    for (int i = 0; i < missing_index; i++) {
        missing_weight = gf256_mul(ctx, missing_weight, redundant_x);
    }

    if (missing_weight == 0) return -1;

    /* Precompute inverse once to replace per-byte division with multiplication. */
    uint8_t inv_weight = gf256_inv(ctx, missing_weight);

    for (size_t off = 0; off < packet_len; off++) {
        uint8_t known_sum = 0;
        uint8_t x_pow = 1;

        /* Sum known terms in P(x_r), skipping the missing coefficient term. */
        for (int i = 0; i < source_count; i++) {
            if (i != missing_index) {
                known_sum = gf256_add(known_sum, gf256_mul(ctx, src_packets[i][off], x_pow));
            }
            x_pow = gf256_mul(ctx, x_pow, redundant_x);
        }

        /* a_m = (redundant ^ known_sum) * inv(x_r^m) */
        recovered_packet_out[off] = gf256_mul(ctx, gf256_add(redundant_packet[off], known_sum), inv_weight);
    }

    return 0;
}

int fec_poly_interpolate_coefficients(
    const struct fec_rs_ctx *ctx,
    const uint8_t *points_x,
    const uint8_t *const *points_y,
    int point_count,
    size_t packet_len,
    uint8_t *const *coeff_packets_out)
{
    int n = point_count;
    uint8_t *omega = NULL;
    uint8_t *qi = NULL;
    uint8_t *T = NULL;

    if (ctx == NULL || points_x == NULL || points_y == NULL || coeff_packets_out == NULL) return -1;
    if (n <= 0 || n > 255) return -1;

    for (int i = 0; i < n; i++) {
        if (points_y[i] == NULL || coeff_packets_out[i] == NULL) return -1;
        for (int j = i + 1; j < n; j++) {
            if (points_x[i] == points_x[j]) return -1;
        }
    }

    omega = calloc((size_t)(n + 1), 1);
    qi = calloc((size_t)n, 1);
    T = calloc((size_t)(n * n), 1);

    if (!omega || !qi || !T) {
        free(omega); free(qi); free(T);
        return -1;
    }

    /*
     * Precompute Lagrange interpolation matrix T[p][i] such that for each byte:
     *   coeff[p] = sum_i T[p][i] * y[i]
     *
     * This separates the x-dependent precomputation (O(n^2)) from the per-byte
     * reconstruction (O(n^2) table lookups), enabling fast inner loops.
     */

    /* Build master polynomial omega(x) = prod_{j=0}^{n-1}(x + x_j). */
    omega[0] = 1;
    for (int j = 0; j < n; j++) {
        uint8_t xj = points_x[j];
        for (int k = j + 1; k >= 1; k--)
            omega[k] = gf256_add(omega[k - 1], gf256_mul(ctx, omega[k], xj));
        omega[0] = gf256_mul(ctx, omega[0], xj);
    }

    /* For each point i, compute the Lagrange basis polynomial L_i(x). */
    for (int i = 0; i < n; i++) {
        uint8_t xi = points_x[i];
        uint8_t si;

        /* Synthetic division: qi(x) = omega(x) / (x + x_i). */
        qi[n - 1] = omega[n];
        for (int k = n - 2; k >= 0; k--)
            qi[k] = gf256_add(omega[k + 1], gf256_mul(ctx, qi[k + 1], xi));

        /* Barycentric weight: s_i = prod_{j!=i}(x_i + x_j). */
        si = 1;
        for (int j = 0; j < n; j++) {
            if (j != i)
                si = gf256_mul(ctx, si, gf256_add(xi, points_x[j]));
        }

        if (si == 0) {
            free(omega); free(qi); free(T);
            return -1;
        }

        uint8_t inv_si = gf256_inv(ctx, si);

        for (int p = 0; p < n; p++)
            T[p * n + i] = gf256_mul(ctx, qi[p], inv_si);
    }

    free(omega);
    free(qi);

    /*
     * Apply T to each byte lane using per-element multiplication tables.
     * A 256-byte table replaces the 3-LUT-access gf256_mul with one lookup,
     * yielding ~3x throughput on the hot inner loop over packet bytes.
     */
    for (int p = 0; p < n; p++)
        memset(coeff_packets_out[p], 0, packet_len);

    for (int i = 0; i < n; i++) {
        for (int p = 0; p < n; p++) {
            uint8_t t = T[p * n + i];
            if (t == 0) continue;

            uint8_t mul_t[256];
            mul_t[0] = 0;
            for (int v = 1; v < 256; v++)
                mul_t[v] = gf256_mul(ctx, t, (uint8_t)v);

            for (size_t off = 0; off < packet_len; off++)
                coeff_packets_out[p][off] ^= mul_t[points_y[i][off]];
        }
    }

    free(T);
    return 0;
}
