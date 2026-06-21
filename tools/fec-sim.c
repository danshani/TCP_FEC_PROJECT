/*
 * fec-sim — trace-driven completion-time simulator for the TCP-FEC project.
 *
 * Goal: produce the presentation graphs (completion time vs. geometry, and
 * completion time vs. loss rate) using NUMBERS THAT ARE REAL where it matters:
 *
 *   - Erasure recovery is performed by the project's ACTUAL GF(256) decoder
 *     (src/fec.c + src/fec_codec.c). For every block that FEC claims to
 *     recover, we run fec_decoder_recover() and verify the bytes match the
 *     originals. recover_failures MUST stay 0 — if it doesn't, the codec is
 *     broken and the graph is meaningless. This is not a combinatorial model
 *     of recovery; it is the recovery code itself.
 *
 *   - The TCP timing model uses THIS STACK's real loss-recovery behaviour:
 *       * Initial/min RTO = 1000 ms        (src/tcp_input.c:228)
 *       * RTO doubles per consecutive loss  (src/tcp_output.c:451, RFC 6298)
 *       * No fast-retransmit / no SACK fast-recovery: duplicate ACKs are
 *         ignored (src/tcp_input.c:591), so the ONLY loss-recovery mechanism
 *         is the RTO timer. Every lost segment therefore stalls the transfer
 *         for at least one full RTO.
 *
 * Model of a transfer of S source segments (S = ceil(file_bytes / sym_len)):
 *
 *   plain TCP : no parity on the wire. Each source segment is lost
 *               independently w.p. p; each loss costs a chain of RTO stalls
 *               (1000, 2000, ... until a retransmit gets through). Head-of-line
 *               blocking makes these stalls additive to completion time.
 *
 *   FEC       : segments are grouped into blocks of k; r parity symbols are
 *               added (extra wire bytes -> r/k bandwidth overhead). A block of
 *               n=k+r symbols survives iff it loses <= r symbols (received>=k),
 *               in which case the real decoder reconstructs the lost sources
 *               LOCALLY with zero RTO. A block that loses > r symbols recovers
 *               nothing via FEC, so each of its lost sources falls back to the
 *               same RTO-stall chain as plain TCP.
 *
 *   completion time = serialization (bytes/rate) + 1 RTT + sum of RTO stalls.
 *
 * Output: one CSV row per invocation (averaged over `trials` Monte-Carlo runs):
 *   k,r,sym_len,file_bytes,loss_pct,trials,t_tcp_ms,t_fec_ms,
 *   fec_overhead_pct,busted_blocks_avg,recover_failures
 *
 * Loop over geometries / loss values in a driver script and concatenate.
 *
 * Build:  see the `fec-sim` target in the Makefile.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fec.h"
#include "fec_codec.h"

/* ---- deterministic PRNG (xorshift32) so a given seed is reproducible ---- */
static uint32_t rng_state = 0;
static inline uint32_t rng_u32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}
/* uniform in [0,1) */
static inline double rng_unit(void)
{
    return (double)(rng_u32() >> 8) / 16777216.0;   /* 24 random bits */
}
static inline int coin(double p) { return rng_unit() < p; }

/*
 * Stall (ms) incurred recovering ONE segment that was just lost, under this
 * stack's RTO-only recovery: wait one RTO and retransmit; if that retransmit
 * is also lost (prob p) wait a doubled RTO and try again, capped at 60 s.
 */
static double rto_stall(double p, double rto_min, double rto_cap)
{
    double stall = 0.0;
    double cur = rto_min;
    for (;;) {
        stall += cur;                 /* wait this RTO, then retransmit */
        if (!coin(p)) break;          /* retransmit delivered */
        cur *= 2.0;                   /* RFC 6298 backoff */
        if (cur > rto_cap) cur = rto_cap;
    }
    return stall;
}

/*
 * Idealised-TCP stall (ms) for one lost segment, assuming fast-retransmit /
 * SACK fast-recovery (which Level-IP does NOT have, but production TCP does).
 * An isolated loss is detected by duplicate ACKs and retransmitted in ~1 RTT.
 * Tail losses (no following segments to trigger dup ACKs) and a lost
 * retransmit fall back to the RTO chain. This is the conservative baseline:
 * real SACK would batch multiple losses in a window into a single RTT.
 */
static double fast_stall(int is_tail, double p, double rtt,
                         double rto_min, double rto_cap)
{
    if (is_tail) return rto_stall(p, rto_min, rto_cap);
    double stall = rtt;                 /* fast retransmit: ~1 RTT */
    double cur = rto_min;
    while (coin(p)) {                   /* the retransmit was also lost */
        stall += cur;
        cur *= 2.0;
        if (cur > rto_cap) cur = rto_cap;
    }
    return stall;
}

int main(int argc, char **argv)
{
    if (argc != 12) {
        fprintf(stderr,
            "usage: %s k r sym_len file_bytes loss_pct trials "
            "rate_bps rtt_ms rto_min_ms rto_cap_ms seed\n", argv[0]);
        return 2;
    }
    int      k          = atoi(argv[1]);
    int      r          = atoi(argv[2]);
    size_t   sym_len    = (size_t)strtoul(argv[3], NULL, 10);
    size_t   file_bytes = (size_t)strtoull(argv[4], NULL, 10);
    double   loss_pct   = atof(argv[5]);
    int      trials     = atoi(argv[6]);
    double   rate_bps   = atof(argv[7]);
    double   rtt_ms     = atof(argv[8]);
    double   rto_min    = atof(argv[9]);
    double   rto_cap    = atof(argv[10]);
    rng_state           = (uint32_t)strtoul(argv[11], NULL, 10);
    if (rng_state == 0) rng_state = 0x9e3779b9u;

    double p = loss_pct / 100.0;

    if (k < 1 || k > FEC_BLK_MAX_K || r < 1 || r > FEC_BLK_MAX_R ||
        sym_len == 0 || trials < 1) {
        fprintf(stderr, "bad parameters\n");
        return 2;
    }

    struct fec_rs_ctx ctx;
    if (fec_rs_init(&ctx) != 0) { fprintf(stderr, "fec_rs_init failed\n"); return 1; }

    size_t S = (file_bytes + sym_len - 1) / sym_len;   /* source segments */
    if (S == 0) S = 1;
    size_t nblocks = (S + k - 1) / k;

    /* per-block scratch */
    uint8_t *srcbuf = malloc((size_t)k * sym_len);
    uint8_t *redbuf = malloc((size_t)r * sym_len);
    uint8_t *outbuf = malloc((size_t)k * sym_len);
    if (!srcbuf || !redbuf || !outbuf) { fprintf(stderr, "oom\n"); return 1; }

    double sum_t_tcp = 0.0, sum_t_tcp_fast = 0.0, sum_t_fec = 0.0, sum_busted = 0.0;
    long   recover_failures = 0;

    /* serialization time (ms): FEC sends r parity per block on top of sources */
    double bytes_tcp = (double)S * sym_len;
    double bytes_fec = (double)(S + r * nblocks) * sym_len;
    double serialize_tcp = bytes_tcp / rate_bps * 8000.0;   /* bytes->bits->ms */
    double serialize_fec = bytes_fec / rate_bps * 8000.0;
    double fec_overhead_pct = (bytes_fec / bytes_tcp - 1.0) * 100.0;

    for (int t = 0; t < trials; t++) {
        double tcp_stall = 0.0, tcp_fast_stall = 0.0, fec_stall = 0.0;
        long   busted = 0;

        /* ---- plain TCP (this stack): every loss is one RTO chain ----
         * ---- idealised TCP: same losses, but fast-retransmit (~1 RTT) ---- */
        for (size_t i = 0; i < S; i++)
            if (coin(p)) {
                tcp_stall      += rto_stall(p, rto_min, rto_cap);
                tcp_fast_stall += fast_stall(i >= S - 3, p, rtt_ms, rto_min, rto_cap);
            }

        /* ---- FEC: per block, real encode + erasure + real decode/verify ---- */
        for (size_t b = 0; b < nblocks; b++) {
            int blk_k = (b == nblocks - 1) ? (int)(S - b * k) : k;
            if (blk_k < 1) blk_k = 1;

            /* deterministic source content for this block */
            for (int i = 0; i < blk_k; i++)
                for (size_t j = 0; j < sym_len; j++)
                    srcbuf[i * sym_len + j] =
                        (uint8_t)((b * 131 + i * 31 + j * 7 + 5) & 0xff);

            /* real systematic encode of r parity */
            struct fec_encoder enc;
            uint8_t *red_ptrs[FEC_BLK_MAX_R];
            if (fec_encoder_init(&enc, &ctx, blk_k, r, sym_len, NULL) != 0) {
                fprintf(stderr, "enc init failed\n"); return 1;
            }
            for (int i = 0; i < blk_k; i++)
                fec_encoder_add_source(&enc, srcbuf + (size_t)i * sym_len);
            for (int j = 0; j < r; j++) red_ptrs[j] = redbuf + (size_t)j * sym_len;
            if (fec_encoder_generate(&enc, red_ptrs) != 0) {
                fprintf(stderr, "encode failed\n"); return 1;
            }

            /* draw erasures over the n = blk_k + r wire symbols */
            uint8_t src_lost[FEC_BLK_MAX_K] = {0};
            uint8_t red_lost[FEC_BLK_MAX_R] = {0};
            int lost_src = 0, lost_red = 0;
            for (int i = 0; i < blk_k; i++) if (coin(p)) { src_lost[i] = 1; lost_src++; }
            for (int j = 0; j < r; j++)     if (coin(p)) { red_lost[j] = 1; lost_red++; }

            int received = (blk_k + r) - (lost_src + lost_red);

            if (lost_src == 0) continue;          /* all sources arrived */

            if (received >= blk_k) {
                /* FEC recovers locally: run the REAL decoder and verify bytes */
                struct fec_decoder dec;
                if (fec_decoder_init(&dec, &ctx, blk_k, r, sym_len, NULL) != 0) {
                    fprintf(stderr, "dec init failed\n"); return 1;
                }
                for (int i = 0; i < blk_k; i++)
                    if (!src_lost[i])
                        fec_decoder_add_source(&dec, i, srcbuf + (size_t)i * sym_len);
                for (int j = 0; j < r; j++)
                    if (!red_lost[j])
                        fec_decoder_add_redundant(&dec, j, redbuf + (size_t)j * sym_len);

                if (!fec_decoder_recoverable(&dec)) { recover_failures++; continue; }

                uint8_t *out[FEC_BLK_MAX_K] = {0};
                for (int i = 0; i < blk_k; i++)
                    out[i] = src_lost[i] ? (outbuf + (size_t)i * sym_len) : NULL;
                if (fec_decoder_recover(&dec, out) != 0) { recover_failures++; continue; }

                for (int i = 0; i < blk_k; i++)
                    if (src_lost[i] &&
                        memcmp(outbuf + (size_t)i * sym_len,
                               srcbuf + (size_t)i * sym_len, sym_len) != 0)
                        recover_failures++;
                /* recovered locally -> zero RTO stall */
            } else {
                /* > r losses: FEC recovers nothing; lost sources need RTO */
                busted++;
                for (int i = 0; i < blk_k; i++)
                    if (src_lost[i]) fec_stall += rto_stall(p, rto_min, rto_cap);
            }
        }

        sum_t_tcp      += serialize_tcp + rtt_ms + tcp_stall;
        sum_t_tcp_fast += serialize_tcp + rtt_ms + tcp_fast_stall;
        sum_t_fec      += serialize_fec + rtt_ms + fec_stall;
        sum_busted     += busted;
    }

    double t_tcp      = sum_t_tcp / trials;
    double t_tcp_fast = sum_t_tcp_fast / trials;
    double t_fec      = sum_t_fec / trials;
    double busted_avg = sum_busted / trials;

    /* CSV row */
    printf("%d,%d,%zu,%zu,%.4f,%d,%.2f,%.2f,%.2f,%.2f,%.3f,%ld\n",
           k, r, sym_len, file_bytes, loss_pct, trials,
           t_tcp, t_tcp_fast, t_fec, fec_overhead_pct, busted_avg, recover_failures);

    free(srcbuf); free(redbuf); free(outbuf);
    return recover_failures ? 3 : 0;
}
