#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "fec.h"
#include "fec_codec.h"

#define SYM_LEN 64

/* ---- Test 1: No loss — all sources present ---- */
static int check_no_loss(void)
{
    struct fec_rs_ctx ctx;
    struct fec_encoder enc;
    struct fec_decoder dec;
    int k = 4, r = 2;
    uint8_t src[4][SYM_LEN];
    uint8_t red[2][SYM_LEN];
    uint8_t *red_ptrs[2] = { red[0], red[1] };

    if (fec_rs_init(&ctx) != 0) return -1;
    if (fec_encoder_init(&enc, &ctx, k, r, SYM_LEN, NULL) != 0) return -1;
    if (fec_decoder_init(&dec, &ctx, k, r, SYM_LEN, NULL) != 0) return -1;

    for (int i = 0; i < k; i++) {
        for (int j = 0; j < SYM_LEN; j++)
            src[i][j] = (uint8_t)(i * 37 + j * 11 + 3);
        if (fec_encoder_add_source(&enc, src[i]) != 0) return -1;
    }

    if (fec_encoder_generate(&enc, red_ptrs) != 0) return -1;

    for (int i = 0; i < k; i++) {
        if (fec_decoder_add_source(&dec, i, src[i]) != 0) return -1;
    }

    if (!fec_decoder_complete(&dec)) return -1;
    if (!fec_decoder_recoverable(&dec)) return -1;

    /* Recover should be a no-op. */
    uint8_t *out[4] = { NULL, NULL, NULL, NULL };
    if (fec_decoder_recover(&dec, out) != 0) return -1;

    return 0;
}

/* ---- Test 2: Single loss at each position ---- */
static int check_single_loss(void)
{
    struct fec_rs_ctx ctx;
    int k = 4, r = 2;
    uint8_t src[4][SYM_LEN];
    uint8_t red[2][SYM_LEN];
    uint8_t recovered[SYM_LEN];

    if (fec_rs_init(&ctx) != 0) return -1;

    for (int i = 0; i < k; i++)
        for (int j = 0; j < SYM_LEN; j++)
            src[i][j] = (uint8_t)(i * 23 + j * 7 + 5);

    /* Encode. */
    struct fec_encoder enc;
    uint8_t *red_ptrs[2] = { red[0], red[1] };
    if (fec_encoder_init(&enc, &ctx, k, r, SYM_LEN, NULL) != 0) return -1;
    for (int i = 0; i < k; i++)
        if (fec_encoder_add_source(&enc, src[i]) != 0) return -1;
    if (fec_encoder_generate(&enc, red_ptrs) != 0) return -1;

    /* Lose each source in turn and recover. */
    for (int lost = 0; lost < k; lost++) {
        struct fec_decoder dec;
        if (fec_decoder_init(&dec, &ctx, k, r, SYM_LEN, NULL) != 0) return -1;

        for (int i = 0; i < k; i++) {
            if (i != lost)
                if (fec_decoder_add_source(&dec, i, src[i]) != 0) return -1;
        }
        if (fec_decoder_add_redundant(&dec, 0, red[0]) != 0) return -1;

        if (fec_decoder_complete(&dec)) return -1; /* should NOT be complete */
        if (!fec_decoder_recoverable(&dec)) return -1;

        uint8_t *out[4];
        for (int i = 0; i < k; i++) out[i] = (i == lost) ? recovered : NULL;

        if (fec_decoder_recover(&dec, out) != 0) return -1;
        if (memcmp(recovered, src[lost], SYM_LEN) != 0) return -1;
    }

    return 0;
}

/* ---- Test 3: Double loss ---- */
static int check_double_loss(void)
{
    struct fec_rs_ctx ctx;
    int k = 4, r = 2;
    uint8_t src[4][SYM_LEN];
    uint8_t red[2][SYM_LEN];
    uint8_t rec0[SYM_LEN], rec1[SYM_LEN];

    if (fec_rs_init(&ctx) != 0) return -1;

    for (int i = 0; i < k; i++)
        for (int j = 0; j < SYM_LEN; j++)
            src[i][j] = (uint8_t)(i * 41 + j * 13 + 1);

    struct fec_encoder enc;
    uint8_t *red_ptrs[2] = { red[0], red[1] };
    if (fec_encoder_init(&enc, &ctx, k, r, SYM_LEN, NULL) != 0) return -1;
    for (int i = 0; i < k; i++)
        if (fec_encoder_add_source(&enc, src[i]) != 0) return -1;
    if (fec_encoder_generate(&enc, red_ptrs) != 0) return -1;

    /* Lose sources 0 and 2. */
    struct fec_decoder dec;
    if (fec_decoder_init(&dec, &ctx, k, r, SYM_LEN, NULL) != 0) return -1;
    if (fec_decoder_add_source(&dec, 1, src[1]) != 0) return -1;
    if (fec_decoder_add_source(&dec, 3, src[3]) != 0) return -1;
    if (fec_decoder_add_redundant(&dec, 0, red[0]) != 0) return -1;
    if (fec_decoder_add_redundant(&dec, 1, red[1]) != 0) return -1;

    if (!fec_decoder_recoverable(&dec)) return -1;

    uint8_t *out[4] = { rec0, NULL, rec1, NULL };
    if (fec_decoder_recover(&dec, out) != 0) return -1;
    if (memcmp(rec0, src[0], SYM_LEN) != 0) return -1;
    if (memcmp(rec1, src[2], SYM_LEN) != 0) return -1;

    return 0;
}

/* ---- Test 4: Triple loss (k=6, r=3) ---- */
static int check_triple_loss(void)
{
    struct fec_rs_ctx ctx;
    int k = 6, r = 3;
    uint8_t src[6][SYM_LEN];
    uint8_t red[3][SYM_LEN];

    if (fec_rs_init(&ctx) != 0) return -1;

    for (int i = 0; i < k; i++)
        for (int j = 0; j < SYM_LEN; j++)
            src[i][j] = (uint8_t)(i * 17 + j * 31 + 9);

    struct fec_encoder enc;
    uint8_t *red_ptrs[3] = { red[0], red[1], red[2] };
    if (fec_encoder_init(&enc, &ctx, k, r, SYM_LEN, NULL) != 0) return -1;
    for (int i = 0; i < k; i++)
        if (fec_encoder_add_source(&enc, src[i]) != 0) return -1;
    if (fec_encoder_generate(&enc, red_ptrs) != 0) return -1;

    /* Lose sources 1, 3, 5. */
    struct fec_decoder dec;
    uint8_t rec1[SYM_LEN], rec3[SYM_LEN], rec5[SYM_LEN];

    if (fec_decoder_init(&dec, &ctx, k, r, SYM_LEN, NULL) != 0) return -1;
    if (fec_decoder_add_source(&dec, 0, src[0]) != 0) return -1;
    if (fec_decoder_add_source(&dec, 2, src[2]) != 0) return -1;
    if (fec_decoder_add_source(&dec, 4, src[4]) != 0) return -1;
    for (int j = 0; j < r; j++)
        if (fec_decoder_add_redundant(&dec, j, red[j]) != 0) return -1;

    if (!fec_decoder_recoverable(&dec)) return -1;

    uint8_t *out[6] = { NULL, rec1, NULL, rec3, NULL, rec5 };
    if (fec_decoder_recover(&dec, out) != 0) return -1;
    if (memcmp(rec1, src[1], SYM_LEN) != 0) return -1;
    if (memcmp(rec3, src[3], SYM_LEN) != 0) return -1;
    if (memcmp(rec5, src[5], SYM_LEN) != 0) return -1;

    return 0;
}

/* ---- Test 5: All sources lost — recover from redundant only ---- */
static int check_all_lost(void)
{
    struct fec_rs_ctx ctx;
    int k = 3, r = 3;
    uint8_t src[3][SYM_LEN];
    uint8_t red[3][SYM_LEN];
    uint8_t rec[3][SYM_LEN];

    if (fec_rs_init(&ctx) != 0) return -1;

    for (int i = 0; i < k; i++)
        for (int j = 0; j < SYM_LEN; j++)
            src[i][j] = (uint8_t)(i * 53 + j * 19 + 7);

    struct fec_encoder enc;
    uint8_t *red_ptrs[3] = { red[0], red[1], red[2] };
    if (fec_encoder_init(&enc, &ctx, k, r, SYM_LEN, NULL) != 0) return -1;
    for (int i = 0; i < k; i++)
        if (fec_encoder_add_source(&enc, src[i]) != 0) return -1;
    if (fec_encoder_generate(&enc, red_ptrs) != 0) return -1;

    /* Zero sources present — all from redundant. */
    struct fec_decoder dec;
    if (fec_decoder_init(&dec, &ctx, k, r, SYM_LEN, NULL) != 0) return -1;
    for (int j = 0; j < r; j++)
        if (fec_decoder_add_redundant(&dec, j, red[j]) != 0) return -1;

    if (!fec_decoder_recoverable(&dec)) return -1;

    uint8_t *out[3] = { rec[0], rec[1], rec[2] };
    if (fec_decoder_recover(&dec, out) != 0) return -1;
    for (int i = 0; i < k; i++)
        if (memcmp(rec[i], src[i], SYM_LEN) != 0) return -1;

    return 0;
}

/* ---- Test 6: Minimal block (k=1, r=1) ---- */
static int check_k1_r1(void)
{
    struct fec_rs_ctx ctx;
    uint8_t src[SYM_LEN];
    uint8_t red[SYM_LEN];
    uint8_t rec[SYM_LEN];

    if (fec_rs_init(&ctx) != 0) return -1;

    for (int j = 0; j < SYM_LEN; j++)
        src[j] = (uint8_t)(j * 3 + 42);

    struct fec_encoder enc;
    uint8_t *red_ptr = red;
    if (fec_encoder_init(&enc, &ctx, 1, 1, SYM_LEN, NULL) != 0) return -1;
    if (fec_encoder_add_source(&enc, src) != 0) return -1;
    if (fec_encoder_generate(&enc, &red_ptr) != 0) return -1;

    /* k=1: P(x) = d_0 (constant). Redundant at x=2 should also equal d_0. */
    if (memcmp(red, src, SYM_LEN) != 0) return -1;

    /* Lose the single source and recover. */
    struct fec_decoder dec;
    if (fec_decoder_init(&dec, &ctx, 1, 1, SYM_LEN, NULL) != 0) return -1;
    if (fec_decoder_add_redundant(&dec, 0, red) != 0) return -1;

    if (!fec_decoder_recoverable(&dec)) return -1;

    uint8_t *out = rec;
    if (fec_decoder_recover(&dec, &out) != 0) return -1;
    if (memcmp(rec, src, SYM_LEN) != 0) return -1;

    return 0;
}

/* ---- Test 7: Large block (k=64, r=4) ---- */
static int check_large_block(void)
{
    struct fec_rs_ctx ctx;
    int k = 64, r = 4;
    size_t len = 128;

    if (fec_rs_init(&ctx) != 0) return -1;

    uint8_t (*src)[128] = calloc((size_t)k, len);
    uint8_t (*red_data)[128] = calloc((size_t)r, len);
    uint8_t *red_ptrs[4];
    if (!src || !red_data) { free(src); free(red_data); return -1; }

    for (int i = 0; i < k; i++)
        for (size_t j = 0; j < len; j++)
            src[i][j] = (uint8_t)(i * 7 + j * 3 + 1);

    struct fec_encoder enc;
    for (int j = 0; j < r; j++) red_ptrs[j] = red_data[j];
    if (fec_encoder_init(&enc, &ctx, k, r, len, NULL) != 0) return -1;
    for (int i = 0; i < k; i++)
        if (fec_encoder_add_source(&enc, src[i]) != 0) return -1;
    if (fec_encoder_generate(&enc, red_ptrs) != 0) return -1;

    /* Lose sources 0, 15, 32, 63 (4 losses = max). */
    int lost[] = { 0, 15, 32, 63 };
    int nlost = 4;
    uint8_t rec_data[4][128];

    struct fec_decoder dec;
    if (fec_decoder_init(&dec, &ctx, k, r, len, NULL) != 0) return -1;

    for (int i = 0; i < k; i++) {
        int is_lost = 0;
        for (int l = 0; l < nlost; l++)
            if (lost[l] == i) { is_lost = 1; break; }
        if (!is_lost)
            if (fec_decoder_add_source(&dec, i, src[i]) != 0) return -1;
    }
    for (int j = 0; j < r; j++)
        if (fec_decoder_add_redundant(&dec, j, red_data[j]) != 0) return -1;

    if (!fec_decoder_recoverable(&dec)) return -1;

    uint8_t **out = calloc((size_t)k, sizeof(uint8_t *));
    if (!out) { free(src); free(red_data); return -1; }
    for (int l = 0; l < nlost; l++)
        out[lost[l]] = rec_data[l];

    if (fec_decoder_recover(&dec, out) != 0) { free(out); free(src); free(red_data); return -1; }

    for (int l = 0; l < nlost; l++)
        if (memcmp(rec_data[l], src[lost[l]], len) != 0) { free(out); free(src); free(red_data); return -1; }

    free(out);
    free(src);
    free(red_data);
    return 0;
}

/* ---- Test 8: Insufficient redundancy ---- */
static int check_insufficient(void)
{
    struct fec_rs_ctx ctx;
    int k = 4, r = 1;
    uint8_t src[4][SYM_LEN];
    uint8_t red[SYM_LEN];

    if (fec_rs_init(&ctx) != 0) return -1;

    for (int i = 0; i < k; i++)
        for (int j = 0; j < SYM_LEN; j++)
            src[i][j] = (uint8_t)(i + j);

    struct fec_encoder enc;
    uint8_t *red_ptr = red;
    if (fec_encoder_init(&enc, &ctx, k, r, SYM_LEN, NULL) != 0) return -1;
    for (int i = 0; i < k; i++)
        if (fec_encoder_add_source(&enc, src[i]) != 0) return -1;
    if (fec_encoder_generate(&enc, &red_ptr) != 0) return -1;

    /* Lose 2 sources with only 1 redundant — should be unrecoverable. */
    struct fec_decoder dec;
    if (fec_decoder_init(&dec, &ctx, k, r, SYM_LEN, NULL) != 0) return -1;
    if (fec_decoder_add_source(&dec, 0, src[0]) != 0) return -1;
    if (fec_decoder_add_source(&dec, 1, src[1]) != 0) return -1;
    if (fec_decoder_add_redundant(&dec, 0, red) != 0) return -1;

    if (fec_decoder_recoverable(&dec)) return -1; /* should NOT be recoverable */

    uint8_t rec0[SYM_LEN], rec1[SYM_LEN];
    uint8_t *out[4] = { NULL, NULL, rec0, rec1 };
    if (fec_decoder_recover(&dec, out) == 0) return -1; /* should fail */

    return 0;
}

/* ---- Test 9: Custom x-values ---- */
static int check_custom_xvals(void)
{
    struct fec_rs_ctx ctx;
    int k = 4, r = 2;
    /* Custom x-values: must not collide with source x-values 1..4. */
    uint8_t red_x[2] = { 17, 251 };
    uint8_t src[4][SYM_LEN];
    uint8_t red[2][SYM_LEN];
    uint8_t rec[SYM_LEN];

    if (fec_rs_init(&ctx) != 0) return -1;

    for (int i = 0; i < k; i++)
        for (int j = 0; j < SYM_LEN; j++)
            src[i][j] = (uint8_t)(i * 61 + j * 29 + 11);

    struct fec_encoder enc;
    uint8_t *red_ptrs[2] = { red[0], red[1] };
    if (fec_encoder_init(&enc, &ctx, k, r, SYM_LEN, red_x) != 0) return -1;
    for (int i = 0; i < k; i++)
        if (fec_encoder_add_source(&enc, src[i]) != 0) return -1;
    if (fec_encoder_generate(&enc, red_ptrs) != 0) return -1;

    /* Single loss with custom x-values. */
    struct fec_decoder dec;
    if (fec_decoder_init(&dec, &ctx, k, r, SYM_LEN, red_x) != 0) return -1;
    for (int i = 1; i < k; i++)
        if (fec_decoder_add_source(&dec, i, src[i]) != 0) return -1;
    if (fec_decoder_add_redundant(&dec, 0, red[0]) != 0) return -1;

    uint8_t *out[4] = { rec, NULL, NULL, NULL };
    if (fec_decoder_recover(&dec, out) != 0) return -1;
    if (memcmp(rec, src[0], SYM_LEN) != 0) return -1;

    /* Double loss with custom x-values. */
    uint8_t rec2[SYM_LEN], rec3[SYM_LEN];
    fec_decoder_reset(&dec);
    if (fec_decoder_add_source(&dec, 0, src[0]) != 0) return -1;
    if (fec_decoder_add_source(&dec, 1, src[1]) != 0) return -1;
    if (fec_decoder_add_redundant(&dec, 0, red[0]) != 0) return -1;
    if (fec_decoder_add_redundant(&dec, 1, red[1]) != 0) return -1;

    uint8_t *out2[4] = { NULL, NULL, rec2, rec3 };
    if (fec_decoder_recover(&dec, out2) != 0) return -1;
    if (memcmp(rec2, src[2], SYM_LEN) != 0) return -1;
    if (memcmp(rec3, src[3], SYM_LEN) != 0) return -1;

    return 0;
}

/* ---- Test 10: Encoder reset and reuse ---- */
static int check_reset(void)
{
    struct fec_rs_ctx ctx;
    int k = 2, r = 1;
    uint8_t src_a[2][SYM_LEN], src_b[2][SYM_LEN];
    uint8_t red_a[SYM_LEN], red_b[SYM_LEN];
    uint8_t rec[SYM_LEN];

    if (fec_rs_init(&ctx) != 0) return -1;

    for (int j = 0; j < SYM_LEN; j++) {
        src_a[0][j] = (uint8_t)(j + 1);
        src_a[1][j] = (uint8_t)(j + 50);
        src_b[0][j] = (uint8_t)(j + 100);
        src_b[1][j] = (uint8_t)(j + 200);
    }

    struct fec_encoder enc;
    uint8_t *rp;

    /* Block A. */
    if (fec_encoder_init(&enc, &ctx, k, r, SYM_LEN, NULL) != 0) return -1;
    for (int i = 0; i < k; i++)
        if (fec_encoder_add_source(&enc, src_a[i]) != 0) return -1;
    rp = red_a;
    if (fec_encoder_generate(&enc, &rp) != 0) return -1;

    /* Reset and encode block B. */
    fec_encoder_reset(&enc);
    for (int i = 0; i < k; i++)
        if (fec_encoder_add_source(&enc, src_b[i]) != 0) return -1;
    rp = red_b;
    if (fec_encoder_generate(&enc, &rp) != 0) return -1;

    /* Verify block A recovery still works. */
    struct fec_decoder dec;
    if (fec_decoder_init(&dec, &ctx, k, r, SYM_LEN, NULL) != 0) return -1;
    if (fec_decoder_add_source(&dec, 0, src_a[0]) != 0) return -1;
    if (fec_decoder_add_redundant(&dec, 0, red_a) != 0) return -1;
    uint8_t *out[2] = { NULL, rec };
    if (fec_decoder_recover(&dec, out) != 0) return -1;
    if (memcmp(rec, src_a[1], SYM_LEN) != 0) return -1;

    /* Decoder reset and verify block B recovery. */
    fec_decoder_reset(&dec);
    if (fec_decoder_add_source(&dec, 1, src_b[1]) != 0) return -1;
    if (fec_decoder_add_redundant(&dec, 0, red_b) != 0) return -1;
    uint8_t *out2[2] = { rec, NULL };
    if (fec_decoder_recover(&dec, out2) != 0) return -1;
    if (memcmp(rec, src_b[0], SYM_LEN) != 0) return -1;

    return 0;
}

/* ---- Test 11: Error parameter validation ---- */
static int check_errors(void)
{
    struct fec_rs_ctx ctx;
    struct fec_encoder enc;
    struct fec_decoder dec;
    uint8_t buf[8] = { 0 };
    uint8_t *ptr = buf;

    if (fec_rs_init(&ctx) != 0) return -1;

    /* NULL context. */
    if (fec_encoder_init(&enc, NULL, 4, 2, 8, NULL) == 0) return -1;
    if (fec_decoder_init(&dec, NULL, 4, 2, 8, NULL) == 0) return -1;

    /* k out of range. */
    if (fec_encoder_init(&enc, &ctx, 0, 2, 8, NULL) == 0) return -1;
    if (fec_encoder_init(&enc, &ctx, 129, 2, 8, NULL) == 0) return -1;

    /* r out of range. */
    if (fec_encoder_init(&enc, &ctx, 4, 0, 8, NULL) == 0) return -1;
    if (fec_encoder_init(&enc, &ctx, 4, 33, 8, NULL) == 0) return -1;

    /* k+r > 255. */
    if (fec_encoder_init(&enc, &ctx, 128, 128, 8, NULL) == 0) return -1;

    /* Duplicate x-values. */
    uint8_t dup_x[2] = { 5, 5 };
    if (fec_encoder_init(&enc, &ctx, 4, 2, 8, dup_x) == 0) return -1;

    /* Zero x-value. */
    uint8_t zero_x[2] = { 0, 1 };
    if (fec_encoder_init(&enc, &ctx, 4, 2, 8, zero_x) == 0) return -1;

    /* Add source to uninitialized / overflow. */
    if (fec_encoder_init(&enc, &ctx, 1, 1, 8, NULL) != 0) return -1;
    if (fec_encoder_add_source(&enc, buf) != 0) return -1;
    if (fec_encoder_add_source(&enc, buf) == 0) return -1; /* overflow */

    /* Generate before all sources added. */
    fec_encoder_reset(&enc);
    if (fec_encoder_generate(&enc, &ptr) == 0) return -1;

    /* Decoder: duplicate source add. */
    if (fec_decoder_init(&dec, &ctx, 2, 1, 8, NULL) != 0) return -1;
    if (fec_decoder_add_source(&dec, 0, buf) != 0) return -1;
    if (fec_decoder_add_source(&dec, 0, buf) == 0) return -1; /* dup */

    /* Decoder: invalid index. */
    if (fec_decoder_add_source(&dec, 5, buf) == 0) return -1;
    if (fec_decoder_add_redundant(&dec, 5, buf) == 0) return -1;

    return 0;
}

/* ---- Test 12: Randomized fuzz ---- */
#define RAND_MAX_K 32
#define RAND_MAX_R 8
#define RAND_MAX_LEN 128

static int check_randomized(int iterations)
{
    struct fec_rs_ctx ctx;
    struct fec_encoder enc;
    struct fec_decoder dec;

    uint8_t src_storage[RAND_MAX_K][RAND_MAX_LEN];
    uint8_t red_storage[RAND_MAX_R][RAND_MAX_LEN];
    uint8_t rec_storage[RAND_MAX_K][RAND_MAX_LEN];
    uint8_t *red_ptrs[RAND_MAX_R];
    uint8_t *out_ptrs[RAND_MAX_K];

    if (fec_rs_init(&ctx) != 0) return -1;

    for (int t = 0; t < iterations; t++) {
        int k = 2 + (rand() % (RAND_MAX_K - 1));
        int max_r = 255 - k;
        if (max_r > RAND_MAX_R) max_r = RAND_MAX_R;
        int r = 1 + (rand() % max_r);
        int len = 1 + (rand() % RAND_MAX_LEN);

        /* Generate random source data. */
        for (int i = 0; i < k; i++)
            for (int j = 0; j < len; j++)
                src_storage[i][j] = (uint8_t)(rand() & 0xff);

        /* Encode. */
        if (fec_encoder_init(&enc, &ctx, k, r, (size_t)len, NULL) != 0) return -1;
        for (int i = 0; i < k; i++)
            if (fec_encoder_add_source(&enc, src_storage[i]) != 0) return -1;
        for (int j = 0; j < r; j++) red_ptrs[j] = red_storage[j];
        if (fec_encoder_generate(&enc, red_ptrs) != 0) return -1;

        /* Random number of losses (0 to min(k, r)). */
        int max_loss = k < r ? k : r;
        int nloss = rand() % (max_loss + 1);

        /* Shuffle source indices and pick first nloss as lost. */
        int indices[RAND_MAX_K];
        for (int i = 0; i < k; i++) indices[i] = i;
        for (int i = k - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int tmp = indices[i];
            indices[i] = indices[j];
            indices[j] = tmp;
        }

        uint8_t lost_mask[RAND_MAX_K] = { 0 };
        for (int l = 0; l < nloss; l++)
            lost_mask[indices[l]] = 1;

        /* Decode. */
        if (fec_decoder_init(&dec, &ctx, k, r, (size_t)len, NULL) != 0) return -1;
        for (int i = 0; i < k; i++) {
            if (!lost_mask[i])
                if (fec_decoder_add_source(&dec, i, src_storage[i]) != 0) return -1;
        }

        /* Add just enough redundant to cover losses. */
        int red_needed = nloss;
        for (int j = 0; j < r && red_needed > 0; j++) {
            if (fec_decoder_add_redundant(&dec, j, red_storage[j]) != 0) return -1;
            red_needed--;
        }

        if (nloss == 0) {
            if (!fec_decoder_complete(&dec)) return -1;
        } else {
            if (!fec_decoder_recoverable(&dec)) return -1;
        }

        for (int i = 0; i < k; i++)
            out_ptrs[i] = lost_mask[i] ? rec_storage[i] : NULL;

        if (fec_decoder_recover(&dec, out_ptrs) != 0) return -1;

        for (int i = 0; i < k; i++) {
            if (lost_mask[i]) {
                if (memcmp(rec_storage[i], src_storage[i], (size_t)len) != 0)
                    return -1;
            }
        }
    }

    return 0;
}

/* ---- Test 13: Double loss at every pair combination ---- */
static int check_double_loss_all_pairs(void)
{
    struct fec_rs_ctx ctx;
    int k = 5, r = 2;
    uint8_t src[5][SYM_LEN];
    uint8_t red[2][SYM_LEN];

    if (fec_rs_init(&ctx) != 0) return -1;

    for (int i = 0; i < k; i++)
        for (int j = 0; j < SYM_LEN; j++)
            src[i][j] = (uint8_t)(i * 47 + j * 23 + 13);

    struct fec_encoder enc;
    uint8_t *red_ptrs[2] = { red[0], red[1] };
    if (fec_encoder_init(&enc, &ctx, k, r, SYM_LEN, NULL) != 0) return -1;
    for (int i = 0; i < k; i++)
        if (fec_encoder_add_source(&enc, src[i]) != 0) return -1;
    if (fec_encoder_generate(&enc, red_ptrs) != 0) return -1;

    /* Test every pair of lost sources. */
    for (int a = 0; a < k; a++) {
        for (int b = a + 1; b < k; b++) {
            struct fec_decoder dec;
            uint8_t rec_a[SYM_LEN], rec_b[SYM_LEN];

            if (fec_decoder_init(&dec, &ctx, k, r, SYM_LEN, NULL) != 0) return -1;
            for (int i = 0; i < k; i++) {
                if (i != a && i != b)
                    if (fec_decoder_add_source(&dec, i, src[i]) != 0) return -1;
            }
            if (fec_decoder_add_redundant(&dec, 0, red[0]) != 0) return -1;
            if (fec_decoder_add_redundant(&dec, 1, red[1]) != 0) return -1;

            if (!fec_decoder_recoverable(&dec)) return -1;

            uint8_t *out[5] = { NULL, NULL, NULL, NULL, NULL };
            out[a] = rec_a;
            out[b] = rec_b;

            if (fec_decoder_recover(&dec, out) != 0) return -1;
            if (memcmp(rec_a, src[a], SYM_LEN) != 0) return -1;
            if (memcmp(rec_b, src[b], SYM_LEN) != 0) return -1;
        }
    }

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

    if (check_no_loss() != 0) {
        fprintf(stderr, "FAIL: no-loss\n");
        return 1;
    }

    if (check_single_loss() != 0) {
        fprintf(stderr, "FAIL: single-loss\n");
        return 1;
    }

    if (check_double_loss() != 0) {
        fprintf(stderr, "FAIL: double-loss\n");
        return 1;
    }

    if (check_triple_loss() != 0) {
        fprintf(stderr, "FAIL: triple-loss\n");
        return 1;
    }

    if (check_all_lost() != 0) {
        fprintf(stderr, "FAIL: all-lost\n");
        return 1;
    }

    if (check_k1_r1() != 0) {
        fprintf(stderr, "FAIL: k1-r1\n");
        return 1;
    }

    if (check_large_block() != 0) {
        fprintf(stderr, "FAIL: large-block\n");
        return 1;
    }

    if (check_insufficient() != 0) {
        fprintf(stderr, "FAIL: insufficient\n");
        return 1;
    }

    if (check_custom_xvals() != 0) {
        fprintf(stderr, "FAIL: custom-xvals\n");
        return 1;
    }

    if (check_reset() != 0) {
        fprintf(stderr, "FAIL: reset\n");
        return 1;
    }

    if (check_errors() != 0) {
        fprintf(stderr, "FAIL: errors\n");
        return 1;
    }

    if (check_double_loss_all_pairs() != 0) {
        fprintf(stderr, "FAIL: double-loss-all-pairs\n");
        return 1;
    }

    if (random_iterations > 0) {
        srand(random_seed);
        if (check_randomized(random_iterations) != 0) {
            fprintf(stderr, "FAIL: randomized (seed=%u, iterations=%d)\n",
                    random_seed, random_iterations);
            return 1;
        }
    }

    printf("FEC codec selftest passed\n");
    return 0;
}
