#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "fec.h"

#define SRC_COUNT 4
#define PKT_LEN 64

#define RAND_MAX_K 16
#define RAND_MAX_K_INTERP 12
#define RAND_MAX_LEN 128

static uint8_t random_nonzero_byte(void)
{
    return (uint8_t)(1 + (rand() % 255));
}

static int check_randomized_cases(int iterations)
{
    struct fec_rs_ctx ctx;
    uint8_t src_storage[RAND_MAX_K][RAND_MAX_LEN];
    uint8_t redundant[RAND_MAX_LEN];
    uint8_t recovered[RAND_MAX_LEN];
    const uint8_t *src_ptrs[RAND_MAX_K];
    const uint8_t *src_with_hole[RAND_MAX_K];

    uint8_t coeff_storage[RAND_MAX_K_INTERP][RAND_MAX_LEN];
    uint8_t eval_storage[RAND_MAX_K_INTERP][RAND_MAX_LEN];
    uint8_t out_storage[RAND_MAX_K_INTERP][RAND_MAX_LEN];
    const uint8_t *coeff_ptrs[RAND_MAX_K_INTERP];
    const uint8_t *y_ptrs[RAND_MAX_K_INTERP];
    uint8_t *out_ptrs[RAND_MAX_K_INTERP];
    uint8_t xvals[RAND_MAX_K_INTERP];
    if (fec_rs_init(&ctx) != 0) return -1;

    for (int t = 0; t < iterations; t++) {
        int k = 1 + (rand() % RAND_MAX_K);
        int len = 1 + (rand() % RAND_MAX_LEN);
        int missing = rand() % k;
        uint8_t xr = random_nonzero_byte();

        for (int i = 0; i < k; i++) {
            for (int j = 0; j < len; j++) {
                src_storage[i][j] = (uint8_t)(rand() & 0xff);
            }
            src_ptrs[i] = src_storage[i];
            src_with_hole[i] = src_storage[i];
        }

        if (fec_poly_encode_redundant(&ctx, src_ptrs, k, (size_t)len, xr, redundant) != 0) return -1;
        src_with_hole[missing] = NULL;

        if (fec_poly_recover_missing_coefficient(
                &ctx,
                src_with_hole,
                k,
                (size_t)len,
                missing,
                xr,
                redundant,
                recovered) != 0) {
            return -1;
        }

        if (memcmp(recovered, src_storage[missing], (size_t)len) != 0) return -1;

        {
            int k_interp = 2 + (rand() % (RAND_MAX_K_INTERP - 1));
            int len_interp = 1 + (rand() % RAND_MAX_LEN);
            uint8_t used[256] = { 0 };

            for (int i = 0; i < k_interp; i++) {
                for (int j = 0; j < len_interp; j++) {
                    coeff_storage[i][j] = (uint8_t)(rand() & 0xff);
                }
                coeff_ptrs[i] = coeff_storage[i];
                y_ptrs[i] = eval_storage[i];
                out_ptrs[i] = out_storage[i];

                do {
                    xvals[i] = random_nonzero_byte();
                } while (used[xvals[i]]);
                used[xvals[i]] = 1;
            }

            for (int i = 0; i < k_interp; i++) {
                if (fec_poly_encode_redundant(&ctx, coeff_ptrs, k_interp, (size_t)len_interp, xvals[i], eval_storage[i]) != 0) return -1;
            }

            if (fec_poly_interpolate_coefficients(&ctx, xvals, y_ptrs, k_interp, (size_t)len_interp, out_ptrs) != 0) return -1;

            for (int i = 0; i < k_interp; i++) {
                if (memcmp(out_storage[i], coeff_storage[i], (size_t)len_interp) != 0) return -1;
            }
        }
    }

    return 0;
}

static int check_encode_recover_varied_sizes(void)
{
    struct fec_rs_ctx ctx;
    uint8_t src_storage[8][PKT_LEN];
    uint8_t redundant[PKT_LEN];
    uint8_t recovered[PKT_LEN];
    const uint8_t *src_ptrs[8];
    const uint8_t *src_with_hole[8];
    int k_values[] = { 1, 2, 4, 8 };

    if (fec_rs_init(&ctx) != 0) return -1;

    for (int kv = 0; kv < (int)(sizeof(k_values) / sizeof(k_values[0])); kv++) {
        int k = k_values[kv];
        uint8_t xr = (uint8_t)(3 + kv * 5);

        for (int i = 0; i < k; i++) {
            for (int j = 0; j < PKT_LEN; j++) {
                src_storage[i][j] = (uint8_t)(i * 29 + j * 13 + kv * 7 + 1);
            }
            src_ptrs[i] = src_storage[i];
            src_with_hole[i] = src_storage[i];
        }

        if (fec_poly_encode_redundant(&ctx, src_ptrs, k, PKT_LEN, xr, redundant) != 0) return -1;

        for (int missing = 0; missing < k; missing++) {
            src_with_hole[missing] = NULL;

            if (fec_poly_recover_missing_coefficient(
                    &ctx,
                    src_with_hole,
                    k,
                    PKT_LEN,
                    missing,
                    xr,
                    redundant,
                    recovered) != 0) {
                return -1;
            }

            if (memcmp(recovered, src_storage[missing], PKT_LEN) != 0) return -1;
            src_with_hole[missing] = src_storage[missing];
        }
    }

    return 0;
}

static int check_interpolation_varied_sizes(void)
{
    struct fec_rs_ctx ctx;
    uint8_t coeff_storage[6][PKT_LEN];
    uint8_t eval_storage[6][PKT_LEN];
    uint8_t out_storage[6][PKT_LEN];
    const uint8_t *coeff_ptrs[6];
    const uint8_t *y_ptrs[6];
    uint8_t *out_ptrs[6];
    int k_values[] = { 2, 3, 4, 6 };

    if (fec_rs_init(&ctx) != 0) return -1;

    for (int kv = 0; kv < (int)(sizeof(k_values) / sizeof(k_values[0])); kv++) {
        int k = k_values[kv];
        uint8_t xvals[6] = { 1, 2, 5, 11, 17, 29 };

        for (int i = 0; i < k; i++) {
            for (int j = 0; j < PKT_LEN; j++) {
                coeff_storage[i][j] = (uint8_t)(i * 17 + j * 9 + 2 * kv + 3);
            }
            coeff_ptrs[i] = coeff_storage[i];
            y_ptrs[i] = eval_storage[i];
            out_ptrs[i] = out_storage[i];
        }

        for (int i = 0; i < k; i++) {
            if (fec_poly_encode_redundant(&ctx, coeff_ptrs, k, PKT_LEN, xvals[i], eval_storage[i]) != 0) return -1;
        }

        if (fec_poly_interpolate_coefficients(&ctx, xvals, y_ptrs, k, PKT_LEN, out_ptrs) != 0) return -1;

        for (int i = 0; i < k; i++) {
            if (memcmp(out_storage[i], coeff_storage[i], PKT_LEN) != 0) return -1;
        }
    }

    return 0;
}

static int check_expected_failure_cases(void)
{
    struct fec_rs_ctx ctx;
    uint8_t pkt0[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t pkt1[8] = { 9, 10, 11, 12, 13, 14, 15, 16 };
    uint8_t redundant[8];
    uint8_t recovered[8];
    const uint8_t *src2[2] = { pkt0, pkt1 };
    const uint8_t *src_with_hole[2] = { pkt0, NULL };
    uint8_t x_dup[2] = { 5, 5 };
    const uint8_t *yvals[2] = { pkt0, pkt1 };
    uint8_t out0[8];
    uint8_t out1[8];
    uint8_t *outv[2] = { out0, out1 };

    if (fec_rs_init(&ctx) != 0) return -1;

    if (fec_poly_encode_redundant(&ctx, src2, 2, sizeof(pkt0), 7, redundant) != 0) return -1;

    /* Invalid missing index should fail. */
    if (fec_poly_recover_missing_coefficient(&ctx, src_with_hole, 2, sizeof(pkt0), 2, 7, redundant, recovered) == 0) return -1;

    /* Null context should fail. */
    if (fec_poly_encode_redundant(NULL, src2, 2, sizeof(pkt0), 7, redundant) == 0) return -1;

    /* Duplicate interpolation x-points should fail. */
    if (fec_poly_interpolate_coefficients(&ctx, x_dup, yvals, 2, sizeof(pkt0), outv) == 0) return -1;

    /* source_count > 255 should fail. */
    if (fec_poly_encode_redundant(&ctx, src2, 256, sizeof(pkt0), 7, redundant) == 0) return -1;

    /* With x=0, missing index > 0 is unrecoverable in this direct formula path. */
    if (fec_poly_encode_redundant(&ctx, src2, 2, sizeof(pkt0), 0, redundant) != 0) return -1;
    if (fec_poly_recover_missing_coefficient(&ctx, src_with_hole, 2, sizeof(pkt0), 1, 0, redundant, recovered) == 0) return -1;

    return 0;
}

static int check_zero_length_packets(void)
{
    struct fec_rs_ctx ctx;
    uint8_t a = 1;
    uint8_t b = 2;
    uint8_t out = 0;
    const uint8_t *src[2] = { &a, &b };

    if (fec_rs_init(&ctx) != 0) return -1;

    /* Zero-length packets should be a no-op success. */
    if (fec_poly_encode_redundant(&ctx, src, 2, 0, 3, &out) != 0) return -1;

    return 0;
}

static int check_lut_consistency(void)
{
    struct fec_rs_ctx ctx;
    uint8_t seen[256] = { 0 };

    if (fec_rs_init(&ctx) != 0) return -1;

    /* exp and log must be inverses for all nonzero elements. */
    for (int i = 0; i < 255; i++) {
        uint8_t e = ctx.exp_lut[i];
        if (e == 0) return -1;
        if (ctx.log_lut[e] != (uint8_t)i) return -1;
    }

    /* exp table wrapping: exp[i+255] == exp[i] for double-size table. */
    for (int i = 0; i < 255; i++) {
        if (ctx.exp_lut[i + 255] != ctx.exp_lut[i]) return -1;
    }

    /* All 255 nonzero elements must appear exactly once in exp[0..254]. */
    for (int i = 0; i < 255; i++) {
        if (seen[ctx.exp_lut[i]]) return -1;
        seen[ctx.exp_lut[i]] = 1;
    }
    if (seen[0]) return -1;
    for (int v = 1; v < 256; v++) {
        if (!seen[v]) return -1;
    }

    return 0;
}

static int check_redundant_x_zero(void)
{
    struct fec_rs_ctx ctx;
    uint8_t pkt0[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
    uint8_t pkt1[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t redundant[8];
    uint8_t recovered[8];
    const uint8_t *src[2] = { pkt0, pkt1 };
    const uint8_t *src_hole[2] = { NULL, pkt1 };

    if (fec_rs_init(&ctx) != 0) return -1;

    /* P(0) should equal the constant coefficient a_0 = pkt0. */
    if (fec_poly_encode_redundant(&ctx, src, 2, sizeof(pkt0), 0, redundant) != 0) return -1;
    if (memcmp(redundant, pkt0, sizeof(pkt0)) != 0) return -1;

    /* Recovering missing_index=0 with x_r=0 should succeed (weight = 0^0 = 1). */
    if (fec_poly_recover_missing_coefficient(&ctx, src_hole, 2, sizeof(pkt0), 0, 0, redundant, recovered) != 0) return -1;
    if (memcmp(recovered, pkt0, sizeof(pkt0)) != 0) return -1;

    return 0;
}

static int check_large_k(void)
{
    struct fec_rs_ctx ctx;
    int k = 255;
    size_t len = 2;
    uint8_t storage[255][2];
    uint8_t redundant[2];
    uint8_t recovered[2];
    const uint8_t *ptrs[255];
    const uint8_t *ptrs_hole[255];

    if (fec_rs_init(&ctx) != 0) return -1;

    for (int i = 0; i < k; i++) {
        storage[i][0] = (uint8_t)(i + 1);
        storage[i][1] = (uint8_t)(i * 3 + 7);
        ptrs[i] = storage[i];
        ptrs_hole[i] = storage[i];
    }

    if (fec_poly_encode_redundant(&ctx, ptrs, k, len, 3, redundant) != 0) return -1;

    /* Test recovering at multiple positions across the range. */
    int test_indices[] = { 0, 1, 127, 254 };
    for (int t = 0; t < 4; t++) {
        int missing = test_indices[t];
        ptrs_hole[missing] = NULL;

        if (fec_poly_recover_missing_coefficient(&ctx, ptrs_hole, k, len, missing, 3, redundant, recovered) != 0)
            return -1;
        if (memcmp(recovered, storage[missing], len) != 0)
            return -1;

        ptrs_hole[missing] = storage[missing];
    }

    return 0;
}

static int check_interpolation_k1(void)
{
    struct fec_rs_ctx ctx;
    uint8_t coeff[8] = { 42, 99, 0, 255, 128, 1, 200, 77 };
    uint8_t eval[8];
    uint8_t out[8];
    const uint8_t *coeff_ptr = coeff;
    const uint8_t *y_ptr = eval;
    uint8_t *out_ptr = out;
    uint8_t x = 7;

    if (fec_rs_init(&ctx) != 0) return -1;

    /* k=1: P(x) = a_0 (constant). P(7) should equal a_0. */
    if (fec_poly_encode_redundant(&ctx, &coeff_ptr, 1, sizeof(coeff), x, eval) != 0) return -1;
    if (memcmp(eval, coeff, sizeof(coeff)) != 0) return -1;

    /* Interpolation from one point should recover the constant coefficient. */
    if (fec_poly_interpolate_coefficients(&ctx, &x, &y_ptr, 1, sizeof(coeff), &out_ptr) != 0) return -1;
    if (memcmp(out, coeff, sizeof(coeff)) != 0) return -1;

    return 0;
}

static int check_multi_loss_recovery(void)
{
    struct fec_rs_ctx ctx;
    int k = 4;
    size_t len = 32;
    uint8_t src_data[4][32];
    uint8_t evals[6][32];
    const uint8_t *src_ptrs[4];
    uint8_t x_vals[6] = { 1, 3, 7, 11, 19, 23 };
    const uint8_t *y_ptrs[4];
    uint8_t *out_ptrs[4];
    uint8_t out[4][32];

    if (fec_rs_init(&ctx) != 0) return -1;

    for (int i = 0; i < k; i++) {
        for (size_t j = 0; j < len; j++)
            src_data[i][j] = (uint8_t)(i * 41 + j * 17 + 5);
        src_ptrs[i] = src_data[i];
    }

    /* Generate k+2 = 6 evaluation points. */
    for (int i = 0; i < 6; i++) {
        if (fec_poly_encode_redundant(&ctx, src_ptrs, k, len, x_vals[i], evals[i]) != 0)
            return -1;
    }

    /* Simulate losing points 0 and 1: use points 2,3,4,5. */
    uint8_t sub_x[4] = { x_vals[2], x_vals[3], x_vals[4], x_vals[5] };
    y_ptrs[0] = evals[2]; y_ptrs[1] = evals[3]; y_ptrs[2] = evals[4]; y_ptrs[3] = evals[5];
    for (int i = 0; i < k; i++) out_ptrs[i] = out[i];

    if (fec_poly_interpolate_coefficients(&ctx, sub_x, y_ptrs, k, len, out_ptrs) != 0)
        return -1;

    /* All original coefficients must be perfectly recovered. */
    for (int i = 0; i < k; i++) {
        if (memcmp(out[i], src_data[i], len) != 0)
            return -1;
    }

    return 0;
}

static int check_recover_single_missing(void)
{
    struct fec_rs_ctx ctx;
    uint8_t src[SRC_COUNT][PKT_LEN];
    uint8_t redundant[PKT_LEN];
    uint8_t recovered[PKT_LEN];
    const uint8_t *src_ptrs[SRC_COUNT] = {
        src[0], src[1], src[2], src[3]
    };
    const uint8_t *src_with_hole[SRC_COUNT] = {
        src[0], NULL, src[2], src[3]
    };
    uint8_t redundant_x = 9;

    /* Initialize GF arithmetic context used by encode/recover operations. */
    if (fec_rs_init(&ctx) != 0) return -1;

    /* Deterministic packet payloads make test failures reproducible. */
    for (int i = 0; i < SRC_COUNT; i++) {
        for (int j = 0; j < PKT_LEN; j++) {
            src[i][j] = (uint8_t)(i * 37 + j * 11 + 3);
        }
    }

    /* Generate one redundant packet as P(x_r). */
    if (fec_poly_encode_redundant(&ctx, src_ptrs, SRC_COUNT, PKT_LEN, redundant_x, redundant) != 0) return -1;

    /* Simulate one missing coefficient packet and recover it from redundancy. */
    if (fec_poly_recover_missing_coefficient(
            &ctx,
            src_with_hole,
            SRC_COUNT,
            PKT_LEN,
            1,
            redundant_x,
            redundant,
            recovered) != 0) {
        return -1;
    }

    /* Recovered packet must exactly match the original missing packet. */
    if (memcmp(recovered, src[1], PKT_LEN) != 0) return -1;

    return 0;
}

static int check_interpolation(void)
{
    struct fec_rs_ctx ctx;
    uint8_t coeff[SRC_COUNT][PKT_LEN];
    uint8_t eval0[PKT_LEN], eval1[PKT_LEN], eval2[PKT_LEN], eval3[PKT_LEN];
    uint8_t out0[PKT_LEN], out1[PKT_LEN], out2[PKT_LEN], out3[PKT_LEN];

    const uint8_t *coeff_ptrs[SRC_COUNT] = {
        coeff[0], coeff[1], coeff[2], coeff[3]
    };
    uint8_t x[SRC_COUNT] = { 1, 2, 5, 11 };
    const uint8_t *y_ptrs[SRC_COUNT] = {
        eval0, eval1, eval2, eval3
    };
    uint8_t *coeff_out[SRC_COUNT] = {
        out0, out1, out2, out3
    };

    /* Initialize GF arithmetic context used by interpolation path. */
    if (fec_rs_init(&ctx) != 0) return -1;

    /* Treat these as polynomial coefficients to be reconstructed later. */
    for (int i = 0; i < SRC_COUNT; i++) {
        for (int j = 0; j < PKT_LEN; j++) {
            coeff[i][j] = (uint8_t)(i * 19 + j * 7 + 5);
        }
    }

    /* Build k point evaluations of the same polynomial at distinct x values. */
    if (fec_poly_encode_redundant(&ctx, coeff_ptrs, SRC_COUNT, PKT_LEN, x[0], eval0) != 0) return -1;
    if (fec_poly_encode_redundant(&ctx, coeff_ptrs, SRC_COUNT, PKT_LEN, x[1], eval1) != 0) return -1;
    if (fec_poly_encode_redundant(&ctx, coeff_ptrs, SRC_COUNT, PKT_LEN, x[2], eval2) != 0) return -1;
    if (fec_poly_encode_redundant(&ctx, coeff_ptrs, SRC_COUNT, PKT_LEN, x[3], eval3) != 0) return -1;

    /* Recover all original coefficients from the point set by interpolation. */
    if (fec_poly_interpolate_coefficients(&ctx, x, y_ptrs, SRC_COUNT, PKT_LEN, coeff_out) != 0) return -1;

    /* Interpolated coefficients must match the original coefficient packets. */
    if (memcmp(out0, coeff[0], PKT_LEN) != 0) return -1;
    if (memcmp(out1, coeff[1], PKT_LEN) != 0) return -1;
    if (memcmp(out2, coeff[2], PKT_LEN) != 0) return -1;
    if (memcmp(out3, coeff[3], PKT_LEN) != 0) return -1;

    return 0;
}

int main(int argc, char **argv)
{
    int random_iterations = 0;
    unsigned int random_seed = 0xC0DEC0DEu;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--random") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --random\n");
                return 2;
            }
            random_iterations = (int)strtol(argv[++i], NULL, 10);
            if (random_iterations < 0) {
                fprintf(stderr, "--random must be >= 0\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--seed") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --seed\n");
                return 2;
            }
            random_seed = (unsigned int)strtoul(argv[++i], NULL, 10);
        } else {
            fprintf(stderr, "Usage: %s [--random N] [--seed S]\n", argv[0]);
            return 2;
        }
    }

    /* Verify fast single-loss recovery path. */
    if (check_recover_single_missing() != 0) {
        fprintf(stderr, "FEC selftest failed: single-missing recovery\n");
        return 1;
    }

    /* Verify generic interpolation-based reconstruction path. */
    if (check_interpolation() != 0) {
        fprintf(stderr, "FEC selftest failed: interpolation\n");
        return 1;
    }

    if (check_encode_recover_varied_sizes() != 0) {
        fprintf(stderr, "FEC selftest failed: varied-size encode/recover\n");
        return 1;
    }

    if (check_interpolation_varied_sizes() != 0) {
        fprintf(stderr, "FEC selftest failed: varied-size interpolation\n");
        return 1;
    }

    if (check_expected_failure_cases() != 0) {
        fprintf(stderr, "FEC selftest failed: expected-failure cases\n");
        return 1;
    }

    if (check_zero_length_packets() != 0) {
        fprintf(stderr, "FEC selftest failed: zero-length packets\n");
        return 1;
    }

    if (check_lut_consistency() != 0) {
        fprintf(stderr, "FEC selftest failed: LUT consistency\n");
        return 1;
    }

    if (check_redundant_x_zero() != 0) {
        fprintf(stderr, "FEC selftest failed: redundant x=0\n");
        return 1;
    }

    if (check_large_k() != 0) {
        fprintf(stderr, "FEC selftest failed: large k=255\n");
        return 1;
    }

    if (check_interpolation_k1() != 0) {
        fprintf(stderr, "FEC selftest failed: interpolation k=1\n");
        return 1;
    }

    if (check_multi_loss_recovery() != 0) {
        fprintf(stderr, "FEC selftest failed: multi-loss recovery\n");
        return 1;
    }

    if (random_iterations > 0) {
        srand(random_seed);
        if (check_randomized_cases(random_iterations) != 0) {
            fprintf(stderr, "FEC selftest failed: randomized cases (seed=%u, iterations=%d)\n", random_seed, random_iterations);
            return 1;
        }
    }

    printf("FEC selftest passed\n");
    return 0;
}
