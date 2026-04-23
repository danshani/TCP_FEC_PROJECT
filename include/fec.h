#ifndef _FEC_H
#define _FEC_H

#include <stdint.h>
#include <stddef.h>

#define FEC_GF_PRIMITIVE_POLY 0x11d

struct fec_rs_ctx {
    uint8_t exp_lut[512];
    uint8_t log_lut[256];
};

/* GF(256) arithmetic primitives using log/exp LUT. */

static inline uint8_t gf256_add(uint8_t a, uint8_t b)
{
    return a ^ b;
}

static inline uint8_t gf256_mul(const struct fec_rs_ctx *ctx, uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) return 0;
    return ctx->exp_lut[ctx->log_lut[a] + ctx->log_lut[b]];
}

static inline uint8_t gf256_div(const struct fec_rs_ctx *ctx, uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) return 0;
    int diff = (int)ctx->log_lut[a] - (int)ctx->log_lut[b];
    if (diff < 0) diff += 255;
    return ctx->exp_lut[diff];
}

static inline uint8_t gf256_inv(const struct fec_rs_ctx *ctx, uint8_t a)
{
    if (a == 0) return 0;
    return ctx->exp_lut[255 - ctx->log_lut[a]];
}

/* Initialize GF(256) lookup tables. Must be called before other FEC APIs. */
int fec_rs_init(struct fec_rs_ctx *ctx);

/* Generate one redundant packet: y = P(redundant_x), where source packets are polynomial coefficients. */
int fec_poly_encode_redundant(
    const struct fec_rs_ctx *ctx,
    const uint8_t *const *src_packets,
    int source_count,
    size_t packet_len,
    uint8_t redundant_x,
    uint8_t *redundant_packet_out);

/*
 * Recover one missing source coefficient packet using surviving source packets and one redundant packet.
 * Returns 0 on success.
 */
int fec_poly_recover_missing_coefficient(
    const struct fec_rs_ctx *ctx,
    const uint8_t *const *src_packets,
    int source_count,
    size_t packet_len,
    int missing_index,
    uint8_t redundant_x,
    const uint8_t *redundant_packet,
    uint8_t *recovered_packet_out);

/*
 * Interpolate polynomial coefficients from k points (x_i, y_i), where each y_i is a packet buffer.
 * points_x must contain distinct GF(256) values.
 */
int fec_poly_interpolate_coefficients(
    const struct fec_rs_ctx *ctx,
    const uint8_t *points_x,
    const uint8_t *const *points_y,
    int point_count,
    size_t packet_len,
    uint8_t *const *coeff_packets_out);

#endif
