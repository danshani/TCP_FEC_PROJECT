#ifndef _FEC_CODEC_H
#define _FEC_CODEC_H

#include "fec.h"

/*
 * FEC block codec — encoder/decoder that wraps the GF(256) polynomial FEC
 * engine into a practical block-oriented workflow for TCP segment protection.
 *
 * Uses systematic evaluation coding (standard Reed-Solomon approach):
 *   - Source symbols are evaluations P(x_i) at x_i = 1, 2, ..., k.
 *   - Redundant symbols are evaluations P(x_j) at x_j = k+1, k+2, ..., k+r.
 *   - The polynomial P of degree k-1 passes through all source data points.
 *   - Source data is transmitted unchanged (systematic code).
 *
 * Recovery from any k of the n=k+r symbols is always possible via
 * Lagrange interpolation from k distinct evaluation points.
 */

#define FEC_BLK_MAX_K 128
#define FEC_BLK_MAX_R 32

/* ---- Encoder ---- */

struct fec_encoder {
    const struct fec_rs_ctx *ctx;
    int k;
    int r;
    size_t sym_len;
    uint8_t src_x[FEC_BLK_MAX_K];
    uint8_t red_x[FEC_BLK_MAX_R];
    int filled;
    const uint8_t *src[FEC_BLK_MAX_K];
};

/*
 * Initialize encoder.
 * red_x: evaluation points for redundant symbols (must be distinct, nonzero).
 *        If NULL, uses default values 1, 2, ..., r.
 * Returns 0 on success.
 */
int fec_encoder_init(struct fec_encoder *enc, const struct fec_rs_ctx *ctx,
                     int k, int r, size_t sym_len, const uint8_t *red_x);

/* Add the next source symbol (call k times in order, index 0..k-1). */
int fec_encoder_add_source(struct fec_encoder *enc, const uint8_t *data);

/*
 * Generate all r redundant symbols.
 * redundant_out must have r entries, each pointing to sym_len writable bytes.
 * All k sources must have been added first.
 */
int fec_encoder_generate(const struct fec_encoder *enc,
                         uint8_t *const *redundant_out);

/* Reset encoder for the next block (keeps k, r, sym_len, red_x). */
void fec_encoder_reset(struct fec_encoder *enc);

/* ---- Decoder ---- */

struct fec_decoder {
    const struct fec_rs_ctx *ctx;
    int k;
    int r;
    size_t sym_len;
    uint8_t src_x[FEC_BLK_MAX_K];
    uint8_t red_x[FEC_BLK_MAX_R];
    uint8_t src_present[FEC_BLK_MAX_K];
    uint8_t red_present[FEC_BLK_MAX_R];
    int src_count;
    int red_count;
    const uint8_t *src[FEC_BLK_MAX_K];
    const uint8_t *red[FEC_BLK_MAX_R];
};

/*
 * Initialize decoder. red_x must match the encoder's values.
 * If NULL, uses default values 1, 2, ..., r.
 */
int fec_decoder_init(struct fec_decoder *dec, const struct fec_rs_ctx *ctx,
                     int k, int r, size_t sym_len, const uint8_t *red_x);

/* Add a received source symbol at the given index (0..k-1). */
int fec_decoder_add_source(struct fec_decoder *dec, int index,
                           const uint8_t *data);

/* Add a received redundant symbol at the given index (0..r-1). */
int fec_decoder_add_redundant(struct fec_decoder *dec, int index,
                              const uint8_t *data);

/* Returns 1 if all k source symbols are present (no recovery needed). */
int fec_decoder_complete(const struct fec_decoder *dec);

/* Returns 1 if enough data to recover all missing sources. */
int fec_decoder_recoverable(const struct fec_decoder *dec);

/*
 * Recover missing source symbols.
 * recovered_out is an array of k pointers. Only entries for missing
 * source indices are written (each must point to sym_len writable bytes).
 * Present-source entries are not touched.
 * Returns 0 on success.
 */
int fec_decoder_recover(const struct fec_decoder *dec,
                        uint8_t *const *recovered_out);

/* Reset decoder for the next block (keeps k, r, sym_len, red_x). */
void fec_decoder_reset(struct fec_decoder *dec);

#endif
