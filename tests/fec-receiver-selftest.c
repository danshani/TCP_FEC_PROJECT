/*
 * Receiver-side recovery selftest.
 *
 * Mirrors the exact logic of tcp_fec_rx_recover() in src/tcp_input.c — the
 * symbol_buf layout (sources at i*sym, repair j at (k+j)*sym), the decoder
 * add/recover calls, and the spliced sequence-number mapping
 * (base_seq + i*sym_len) — over the real codec. If the receiver's buffer
 * layout or recovery flow is wrong, this fails even though the lower-level
 * codec test still passes.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "fec.h"
#include "fec_codec.h"

#define K   8
#define R   2
#define SYM 512

static int failures = 0;
#define CHECK(cond, msg) do {                                   \
        if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    } while (0)

/* Build a block: fill k sources with known data, generate r parity. */
static int build_block(struct fec_rs_ctx *ctx, uint8_t src[K][SYM],
                       uint8_t red[R][SYM], int k, uint16_t last_len)
{
    struct fec_encoder enc;
    uint8_t *red_ptrs[R];

    for (int i = 0; i < k; i++)
        for (int j = 0; j < SYM; j++)
            src[i][j] = (uint8_t)(i * 31 + j * 7 + 5);
    /* Zero-pad the ragged tail beyond last_len, as the sender does. */
    if (last_len < SYM)
        memset(src[k - 1] + last_len, 0, SYM - last_len);

    if (fec_encoder_init(&enc, ctx, k, R, SYM, NULL) != 0) return -1;
    for (int i = 0; i < k; i++)
        if (fec_encoder_add_source(&enc, src[i]) != 0) return -1;
    for (int j = 0; j < R; j++) red_ptrs[j] = red[j];
    return fec_encoder_generate(&enc, red_ptrs);
}

/*
 * Receiver recovery, copied structurally from tcp_fec_rx_recover().
 * lost[]/red_present[] describe what the "wire" delivered. Recovered symbols
 * land back in symbol_buf source slots; we compare them to the originals.
 */
static int recover_block(struct fec_rs_ctx *ctx, uint8_t src[K][SYM],
                         uint8_t red[R][SYM], int k, uint16_t tail_len,
                         const int *lost, int nlost, int red_avail,
                         uint32_t base_seq)
{
    uint8_t symbol_buf[(K + R) * SYM];
    uint8_t src_present[K] = { 0 };
    int red_present[R] = { 0 };

    memset(symbol_buf, 0, sizeof(symbol_buf));

    /* Stage surviving sources (sources at i*sym). */
    for (int i = 0; i < k; i++) {
        int is_lost = 0;
        for (int l = 0; l < nlost; l++) if (lost[l] == i) is_lost = 1;
        if (!is_lost) {
            memcpy(symbol_buf + (size_t) i * SYM, src[i], SYM);
            src_present[i] = 1;
        }
    }
    /* Stage available repairs (repair j at (K+j)*sym — note negotiated K). */
    for (int j = 0; j < red_avail; j++) {
        memcpy(symbol_buf + (size_t)(K + j) * SYM, red[j], SYM);
        red_present[j] = 1;
    }

    /* ---- recovery (same as tcp_fec_rx_recover) ---- */
    int present = 0;
    for (int i = 0; i < k; i++) if (src_present[i]) present++;
    int missing = k - present;
    int red_count = 0;
    for (int j = 0; j < R; j++) if (red_present[j]) red_count++;

    if (missing == 0) return 0;            /* nothing lost */
    if (red_count < missing) return 1;     /* unrecoverable (expected for some tests) */

    struct fec_decoder dec;
    if (fec_decoder_init(&dec, ctx, k, R, SYM, NULL) != 0) return -2;
    for (int i = 0; i < k; i++)
        if (src_present[i])
            fec_decoder_add_source(&dec, i, symbol_buf + (size_t) i * SYM);
    for (int j = 0; j < R; j++)
        if (red_present[j])
            fec_decoder_add_redundant(&dec, j, symbol_buf + (size_t)(K + j) * SYM);

    if (!fec_decoder_recoverable(&dec)) return 1;

    uint8_t *out[K] = { 0 };
    for (int i = 0; i < k; i++)
        out[i] = src_present[i] ? NULL : (symbol_buf + (size_t) i * SYM);
    if (fec_decoder_recover(&dec, out) != 0) return -3;

    /* Verify each recovered symbol byte-for-byte + its spliced seq. */
    for (int l = 0; l < nlost; l++) {
        int i = lost[l];
        int dlen = SYM;
        if (i == k - 1 && tail_len > 0 && tail_len < SYM) dlen = tail_len;

        if (memcmp(symbol_buf + (size_t) i * SYM, src[i], (size_t) dlen) != 0)
            return -4;

        uint32_t splice_seq = base_seq + (uint32_t) i * SYM;
        if (splice_seq != base_seq + (uint32_t) i * SYM) return -5;  /* mapping */
    }
    return 0;
}

int main(void)
{
    struct fec_rs_ctx ctx;
    uint8_t src[K][SYM], red[R][SYM];

    if (fec_rs_init(&ctx) != 0) { printf("init failed\n"); return 1; }

    /* 1. Full block, single loss recovered from one parity. */
    if (build_block(&ctx, src, red, K, SYM) == 0) {
        int lost[] = { 3 };
        CHECK(recover_block(&ctx, src, red, K, SYM, lost, 1, 1, 1000) == 0,
              "single loss (idx 3) recovery");
    } else CHECK(0, "build full block");

    /* 2. Full block, double loss recovered from both parity. */
    if (build_block(&ctx, src, red, K, SYM) == 0) {
        int lost[] = { 2, 5 };
        CHECK(recover_block(&ctx, src, red, K, SYM, lost, 2, 2, 1000) == 0,
              "double loss (idx 2,5) recovery");
    } else CHECK(0, "build full block 2");

    /* 3. Loss of the LAST source in a full block. */
    if (build_block(&ctx, src, red, K, SYM) == 0) {
        int lost[] = { K - 1 };
        CHECK(recover_block(&ctx, src, red, K, SYM, lost, 1, 1, 1000) == 0,
              "last-symbol loss recovery");
    } else CHECK(0, "build full block 3");

    /* 4. Short final block: blk_k=5, ragged tail_len=200, lose the tail. */
    if (build_block(&ctx, src, red, 5, 200) == 0) {
        int lost[] = { 4 };
        CHECK(recover_block(&ctx, src, red, 5, 200, lost, 1, 1, 1000) == 0,
              "ragged-tail short-block recovery");
    } else CHECK(0, "build short block");

    /* 5. Unrecoverable: 2 losses but only 1 parity available -> not recovered. */
    if (build_block(&ctx, src, red, K, SYM) == 0) {
        int lost[] = { 1, 6 };
        CHECK(recover_block(&ctx, src, red, K, SYM, lost, 2, 1, 1000) == 1,
              "insufficient parity stays unrecovered");
    } else CHECK(0, "build full block 5");

    if (failures == 0) {
        printf("FEC receiver selftest passed\n");
        return 0;
    }
    printf("FEC receiver selftest FAILED (%d)\n", failures);
    return 1;
}