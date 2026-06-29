/*
 * Regression test for the TCP option walker (tcp_walk_options).
 *
 * These layouts all broke the previous parser, which (a) skipped the whole
 * option area once it was >= 20 bytes and (b) failed to advance past
 * SACK-OK / Timestamp / unknown options, desynchronising the walk. Each case
 * below asserts that an option appearing AFTER such an option is still seen,
 * and that malformed/truncated input is handled without over-reading.
 *
 * Self-contained: builds with `gcc tests/optparse-selftest.c src/tcp_optparse.c`
 * (no networking headers, runs anywhere).
 */
#include <stdio.h>
#include <string.h>
#include "tcp_optparse.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

/* Helper: append a well-formed FEC-PERM option with matching geometry. */
static int put_fec_perm(uint8_t *p)
{
    p[0] = TCP_OPT_FEC_PERM;
    p[1] = TCP_OPTLEN_FEC_PERM;
    p[2] = FEC_WIRE_VERSION;
    p[3] = TCP_FEC_DEFAULT_K;
    p[4] = TCP_FEC_DEFAULT_R;
    p[5] = 0;                                  /* flags */
    p[6] = (TCP_FEC_DEFAULT_SYM_LEN >> 8) & 0xff;
    p[7] = TCP_FEC_DEFAULT_SYM_LEN & 0xff;
    return TCP_OPTLEN_FEC_PERM;
}

static int put_mss(uint8_t *p, uint16_t mss)
{
    p[0] = TCP_OPT_MSS;
    p[1] = TCP_OPTLEN_MSS;
    p[2] = (mss >> 8) & 0xff;
    p[3] = mss & 0xff;
    return TCP_OPTLEN_MSS;
}

int main(void)
{
    /* 1. FEC-PERM placed AFTER SACK-OK must still be detected. The old parser
     *    stalled on SACK-OK (decremented optlen without advancing ptr) and
     *    never reached the FEC option. */
    {
        uint8_t opt[40]; int n = 0;
        opt[n++] = TCP_OPT_SACK_OK; opt[n++] = TCP_OPTLEN_SACK;
        n += put_fec_perm(opt + n);
        struct tcp_parsed_opts o = {0};
        tcp_walk_options(opt, n, &o);
        CHECK(o.sack_ok, "1: SACK-OK seen");
        CHECK(o.fec_perm, "1: FEC-PERM after SACK-OK seen");
    }

    /* 2. FEC-PERM placed AFTER a Timestamp option (10 bytes) must be detected.
     *    The old parser advanced TS by only 1 byte, desynchronising the walk. */
    {
        uint8_t opt[40]; int n = 0;
        opt[n++] = TCP_OPT_TS; opt[n++] = 10;
        memset(opt + n, 0xAB, 8); n += 8;       /* TS payload */
        n += put_fec_perm(opt + n);
        struct tcp_parsed_opts o = {0};
        tcp_walk_options(opt, n, &o);
        CHECK(o.ts_seen, "2: Timestamp seen");
        CHECK(o.fec_perm, "2: FEC-PERM after Timestamp seen");
    }

    /* 3. Total option area >= 20 bytes: old `optlen < 20` bound skipped it all.
     *    MSS + TS + SACK-OK + FEC-PERM = 4 + 10 + 2 + 8 = 24 bytes. */
    {
        uint8_t opt[40]; int n = 0;
        n += put_mss(opt + n, 1000);
        opt[n++] = TCP_OPT_TS; opt[n++] = 10; memset(opt + n, 0, 8); n += 8;
        opt[n++] = TCP_OPT_SACK_OK; opt[n++] = TCP_OPTLEN_SACK;
        n += put_fec_perm(opt + n);
        CHECK(n >= 20, "3: option area is >= 20 bytes");
        struct tcp_parsed_opts o = {0};
        tcp_walk_options(opt, n, &o);
        CHECK(o.mss == 1000, "3: MSS parsed");
        CHECK(o.ts_seen, "3: Timestamp seen");
        CHECK(o.sack_ok, "3: SACK-OK seen");
        CHECK(o.fec_perm, "3: FEC-PERM seen in a 24-byte option area");
    }

    /* 4. Unknown (but well-formed) option must be skipped by its length, not
     *    stall the walk; a following FEC-PERM is still found. */
    {
        uint8_t opt[40]; int n = 0;
        opt[n++] = 250; opt[n++] = 4; opt[n++] = 0; opt[n++] = 0;  /* unknown, len 4 */
        n += put_fec_perm(opt + n);
        struct tcp_parsed_opts o = {0};
        tcp_walk_options(opt, n, &o);
        CHECK(o.fec_perm, "4: FEC-PERM after unknown option seen");
    }

    /* 5. NOOP padding and END-of-list are handled. */
    {
        uint8_t opt[40]; int n = 0;
        opt[n++] = TCP_OPT_NOOP; opt[n++] = TCP_OPT_NOOP;
        n += put_fec_perm(opt + n);
        opt[n++] = TCP_OPT_END;
        opt[n++] = TCP_OPT_SACK_OK; opt[n++] = TCP_OPTLEN_SACK; /* after END: ignored */
        struct tcp_parsed_opts o = {0};
        tcp_walk_options(opt, n, &o);
        CHECK(o.fec_perm, "5: FEC-PERM after NOOPs seen");
        CHECK(!o.sack_ok, "5: option after END-of-list ignored");
    }

    /* 6. Truncated option (length overruns the area) stops cleanly, no
     *    over-read, and does not falsely report the option. */
    {
        uint8_t opt[40]; int n = 0;
        opt[n++] = TCP_OPT_MSS; opt[n++] = TCP_OPTLEN_MSS; opt[n++] = 0x05; /* only 3 of 4 bytes */
        struct tcp_parsed_opts o = {0};
        tcp_walk_options(opt, n, &o);
        CHECK(o.mss == 0, "6: truncated MSS not accepted");
    }

    /* 7. FEC-PERM with wrong geometry must NOT engage FEC. */
    {
        uint8_t opt[40]; int n = 0;
        n += put_fec_perm(opt + n);
        opt[3] = TCP_FEC_DEFAULT_K + 1;          /* corrupt k */
        struct tcp_parsed_opts o = {0};
        tcp_walk_options(opt, n, &o);
        CHECK(!o.fec_perm, "7: mismatched geometry leaves FEC off");
    }

    if (failures == 0) {
        printf("optparse selftest passed\n");
        return 0;
    }
    printf("optparse selftest FAILED (%d)\n", failures);
    return 1;
}
