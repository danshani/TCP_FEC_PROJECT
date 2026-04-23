#include "fec_codec.h"

#include <stdlib.h>
#include <string.h>

/* ---- Encoder ---- */

int fec_encoder_init(struct fec_encoder *enc, const struct fec_rs_ctx *ctx,
                     int k, int r, size_t sym_len, const uint8_t *red_x)
{
    if (!enc || !ctx) return -1;
    if (k <= 0 || k > FEC_BLK_MAX_K) return -1;
    if (r <= 0 || r > FEC_BLK_MAX_R) return -1;
    if (k + r > 255) return -1;

    memset(enc, 0, sizeof(*enc));
    enc->ctx = ctx;
    enc->k = k;
    enc->r = r;
    enc->sym_len = sym_len;

    /* Source x-values: 1, 2, ..., k. */
    for (int i = 0; i < k; i++)
        enc->src_x[i] = (uint8_t)(i + 1);

    /* Redundant x-values: configurable, default k+1, k+2, ..., k+r. */
    for (int i = 0; i < r; i++)
        enc->red_x[i] = red_x ? red_x[i] : (uint8_t)(k + i + 1);

    /* Validate: all x-values (source + redundant) must be distinct and nonzero. */
    uint8_t seen[256] = { 0 };
    for (int i = 0; i < k; i++) {
        if (seen[enc->src_x[i]]) return -1;
        seen[enc->src_x[i]] = 1;
    }
    for (int i = 0; i < r; i++) {
        if (enc->red_x[i] == 0) return -1;
        if (seen[enc->red_x[i]]) return -1;
        seen[enc->red_x[i]] = 1;
    }

    return 0;
}

int fec_encoder_add_source(struct fec_encoder *enc, const uint8_t *data)
{
    if (!enc || !data) return -1;
    if (enc->filled >= enc->k) return -1;
    enc->src[enc->filled++] = data;
    return 0;
}

int fec_encoder_generate(const struct fec_encoder *enc,
                         uint8_t *const *redundant_out)
{
    int k = enc->k;
    size_t sym_len = enc->sym_len;
    uint8_t *coeff_mem = NULL;
    uint8_t **coeffs = NULL;
    const uint8_t **coeff_ptrs = NULL;
    int rc = -1;

    if (!enc || !redundant_out) return -1;
    if (enc->filled != k) return -1;
    for (int j = 0; j < enc->r; j++)
        if (!redundant_out[j]) return -1;

    coeff_mem = calloc((size_t)k, sym_len ? sym_len : 1);
    coeffs = calloc((size_t)k, sizeof(uint8_t *));
    coeff_ptrs = calloc((size_t)k, sizeof(const uint8_t *));
    if (!coeff_mem || !coeffs || !coeff_ptrs) goto done;

    for (int i = 0; i < k; i++) {
        coeffs[i] = coeff_mem + (size_t)i * sym_len;
        coeff_ptrs[i] = coeffs[i];
    }

    /*
     * Step 1: Interpolate — find polynomial P of degree k-1 such that
     * P(src_x[i]) = source_data[i] for each source symbol.
     */
    if (fec_poly_interpolate_coefficients(
            enc->ctx, enc->src_x, enc->src, k, sym_len, coeffs) != 0)
        goto done;

    /*
     * Step 2: Evaluate P at each redundant x-value to produce parity symbols.
     * Source data is transmitted unchanged (systematic code).
     */
    for (int j = 0; j < enc->r; j++) {
        if (fec_poly_encode_redundant(
                enc->ctx, coeff_ptrs, k, sym_len,
                enc->red_x[j], redundant_out[j]) != 0)
            goto done;
    }

    rc = 0;
done:
    free(coeff_mem);
    free(coeffs);
    free(coeff_ptrs);
    return rc;
}

void fec_encoder_reset(struct fec_encoder *enc)
{
    if (!enc) return;
    enc->filled = 0;
    memset(enc->src, 0, sizeof(enc->src));
}

/* ---- Decoder ---- */

int fec_decoder_init(struct fec_decoder *dec, const struct fec_rs_ctx *ctx,
                     int k, int r, size_t sym_len, const uint8_t *red_x)
{
    if (!dec || !ctx) return -1;
    if (k <= 0 || k > FEC_BLK_MAX_K) return -1;
    if (r <= 0 || r > FEC_BLK_MAX_R) return -1;
    if (k + r > 255) return -1;

    memset(dec, 0, sizeof(*dec));
    dec->ctx = ctx;
    dec->k = k;
    dec->r = r;
    dec->sym_len = sym_len;

    for (int i = 0; i < k; i++)
        dec->src_x[i] = (uint8_t)(i + 1);

    for (int i = 0; i < r; i++)
        dec->red_x[i] = red_x ? red_x[i] : (uint8_t)(k + i + 1);

    return 0;
}

int fec_decoder_add_source(struct fec_decoder *dec, int index,
                           const uint8_t *data)
{
    if (!dec || !data) return -1;
    if (index < 0 || index >= dec->k) return -1;
    if (dec->src_present[index]) return -1;
    dec->src[index] = data;
    dec->src_present[index] = 1;
    dec->src_count++;
    return 0;
}

int fec_decoder_add_redundant(struct fec_decoder *dec, int index,
                              const uint8_t *data)
{
    if (!dec || !data) return -1;
    if (index < 0 || index >= dec->r) return -1;
    if (dec->red_present[index]) return -1;
    dec->red[index] = data;
    dec->red_present[index] = 1;
    dec->red_count++;
    return 0;
}

int fec_decoder_complete(const struct fec_decoder *dec)
{
    if (!dec) return 0;
    return dec->src_count == dec->k;
}

int fec_decoder_recoverable(const struct fec_decoder *dec)
{
    if (!dec) return 0;
    int missing = dec->k - dec->src_count;
    return dec->red_count >= missing;
}

int fec_decoder_recover(const struct fec_decoder *dec,
                        uint8_t *const *recovered_out)
{
    int k = dec->k;
    size_t sym_len = dec->sym_len;
    int missing = k - dec->src_count;
    uint8_t *coeff_mem = NULL;
    uint8_t **coeffs = NULL;
    const uint8_t **coeff_ptrs = NULL;
    int rc = -1;

    if (!dec || !recovered_out) return -1;
    if (missing == 0) return 0;
    if (dec->red_count < missing) return -1;

    /* Validate output buffers for missing sources. */
    for (int i = 0; i < k; i++)
        if (!dec->src_present[i] && !recovered_out[i]) return -1;

    /*
     * Collect exactly k evaluation points from surviving symbols.
     * Prefer source symbols first, then fill with redundant.
     */
    uint8_t x_vals[FEC_BLK_MAX_K];
    const uint8_t *y_vals[FEC_BLK_MAX_K];
    int count = 0;

    for (int i = 0; i < k && count < k; i++) {
        if (dec->src_present[i]) {
            x_vals[count] = dec->src_x[i];
            y_vals[count] = dec->src[i];
            count++;
        }
    }
    for (int j = 0; j < dec->r && count < k; j++) {
        if (dec->red_present[j]) {
            x_vals[count] = dec->red_x[j];
            y_vals[count] = dec->red[j];
            count++;
        }
    }
    if (count < k) return -1;

    coeff_mem = calloc((size_t)k, sym_len ? sym_len : 1);
    coeffs = calloc((size_t)k, sizeof(uint8_t *));
    coeff_ptrs = calloc((size_t)k, sizeof(const uint8_t *));
    if (!coeff_mem || !coeffs || !coeff_ptrs) goto done;

    for (int i = 0; i < k; i++) {
        coeffs[i] = coeff_mem + (size_t)i * sym_len;
        coeff_ptrs[i] = coeffs[i];
    }

    /*
     * Step 1: Lagrange interpolation from k survivor points → polynomial
     * coefficients. Always succeeds because points have distinct x-values.
     */
    if (fec_poly_interpolate_coefficients(
            dec->ctx, x_vals, y_vals, k, sym_len, coeffs) != 0)
        goto done;

    /*
     * Step 2: Evaluate the recovered polynomial at each missing source
     * x-value to reconstruct the lost data.
     */
    for (int i = 0; i < k; i++) {
        if (!dec->src_present[i]) {
            if (fec_poly_encode_redundant(
                    dec->ctx, coeff_ptrs, k, sym_len,
                    dec->src_x[i], recovered_out[i]) != 0)
                goto done;
        }
    }

    rc = 0;
done:
    free(coeff_mem);
    free(coeffs);
    free(coeff_ptrs);
    return rc;
}

void fec_decoder_reset(struct fec_decoder *dec)
{
    if (!dec) return;
    dec->src_count = 0;
    dec->red_count = 0;
    memset(dec->src_present, 0, sizeof(dec->src_present));
    memset(dec->red_present, 0, sizeof(dec->red_present));
    memset(dec->src, 0, sizeof(dec->src));
    memset(dec->red, 0, sizeof(dec->red));
}
