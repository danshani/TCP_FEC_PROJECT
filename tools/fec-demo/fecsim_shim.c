/*
 * fecsim_shim — a flat C API over the project's REAL GF(256) codec
 * (src/fec.c + src/fec_codec.c), compiled to a shared library and called from
 * the Python demo backend via ctypes.
 *
 * The point of the live demo is that the encode/decode you watch on screen is
 * THIS code — the same systematic Reed-Solomon engine that runs in the stack —
 * not a JavaScript reimplementation. Python only decides the loss pattern and
 * drives the animation; every parity byte and every recovered byte comes from
 * here.
 *
 * Build (see tools/fec-demo/Makefile or build_demo.sh):
 *   gcc -shared -fPIC -I include tools/fec-demo/fecsim_shim.c \
 *       src/fec_codec.c src/fec.c -o build/libfecsim.so
 */
#include <string.h>
#include "fec.h"
#include "fec_codec.h"

/* One shared GF(256) context, initialised on first use. */
static struct fec_rs_ctx g_ctx;
static int g_ready = 0;

static int ensure_ctx(void)
{
    if (!g_ready) {
        if (fec_rs_init(&g_ctx) != 0) return -1;
        g_ready = 1;
    }
    return 0;
}

/* Geometry limits exposed to the caller (mirror fec_codec.h). */
int fecsim_max_k(void) { return FEC_BLK_MAX_K; }
int fecsim_max_r(void) { return FEC_BLK_MAX_R; }

/*
 * Encode r parity symbols from k source symbols.
 *   src        : k * sym contiguous source bytes
 *   parity_out : r * sym writable bytes (parity j at j*sym)
 * Returns 0 on success.
 */
int fecsim_encode(const unsigned char *src, int k, int r, int sym,
                  unsigned char *parity_out)
{
    if (ensure_ctx() != 0) return -1;
    if (k < 1 || k > FEC_BLK_MAX_K || r < 1 || r > FEC_BLK_MAX_R || sym < 1)
        return -2;

    struct fec_encoder enc;
    uint8_t *red_ptrs[FEC_BLK_MAX_R];

    if (fec_encoder_init(&enc, &g_ctx, k, r, (size_t)sym, NULL) != 0) return -3;
    for (int i = 0; i < k; i++)
        if (fec_encoder_add_source(&enc, src + (size_t)i * sym) != 0) return -4;
    for (int j = 0; j < r; j++)
        red_ptrs[j] = parity_out + (size_t)j * sym;
    return fec_encoder_generate(&enc, red_ptrs) == 0 ? 0 : -5;
}

/*
 * Recover missing sources from the symbols that survived the channel.
 *   symbols : (k+r) * sym buffer. Source i at i*sym, parity j at (k+j)*sym.
 *             Lost symbols may hold anything (ignored via the mask).
 *   present : (k+r) flags; 1 = received, 0 = lost. Sources first, then parity.
 *   out     : k * sym writable buffer; recovered source bytes are written for
 *             missing indices (present sources are copied through too).
 * Returns 0 if all k sources are now available, 1 if unrecoverable
 * (too few symbols survived), negative on error.
 */
int fecsim_recover(const unsigned char *symbols, const unsigned char *present,
                   int k, int r, int sym, unsigned char *out)
{
    if (ensure_ctx() != 0) return -1;
    if (k < 1 || k > FEC_BLK_MAX_K || r < 1 || r > FEC_BLK_MAX_R || sym < 1)
        return -2;

    struct fec_decoder dec;
    if (fec_decoder_init(&dec, &g_ctx, k, r, (size_t)sym, NULL) != 0) return -3;

    int have_src = 0;
    for (int i = 0; i < k; i++) {
        const unsigned char *p = symbols + (size_t)i * sym;
        if (present[i]) {
            fec_decoder_add_source(&dec, i, p);
            memcpy(out + (size_t)i * sym, p, sym);   /* copy survivors through */
            have_src++;
        }
    }
    for (int j = 0; j < r; j++)
        if (present[k + j])
            fec_decoder_add_redundant(&dec, j, symbols + (size_t)(k + j) * sym);

    if (have_src == k) return 0;                     /* nothing lost */
    if (!fec_decoder_recoverable(&dec)) return 1;    /* too few symbols */

    uint8_t *outp[FEC_BLK_MAX_K] = {0};
    for (int i = 0; i < k; i++)
        outp[i] = present[i] ? NULL : (out + (size_t)i * sym);
    if (fec_decoder_recover(&dec, outp) != 0) return -4;
    return 0;
}
