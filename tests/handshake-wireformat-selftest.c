/*
 * Wire-format + negotiation oracle for the FEC handshake.
 *
 * This does NOT link the live stack (the option builders/parsers are static
 * and pull in the whole TCP/socket/pthread tree). Instead it pins down the
 * on-wire contract the integration must honour, using the REAL shared structs
 * from include/fec_frame.h:
 *
 *   - struct sizes / packing of the three wire structs
 *   - exact SYN/ACK option byte layout (MSS, FEC-PERM, SACK), 4-byte aligned,
 *     within the 40-byte option ceiling
 *   - FEC negotiation gate: accept only on exact (version,k,r,sym_len) match
 *   - per-segment FEC option + repair header byte-order round-trips
 *
 * If the live code drifts from this contract, the bytes here are what a
 * tcpdump capture should be diffed against.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include "fec_frame.h"

/* Mirror the stack's option kinds we interleave with FEC. */
#define OPT_MSS      2
#define OPTLEN_MSS   4
#define OPT_NOOP     1
#define OPT_SACK_OK  4
#define OPTLEN_SACK  2
#define OPT_MAX      40

struct mss_opt { uint8_t kind, len; uint16_t mss; } __attribute__((packed));

static int failures = 0;
#define CHECK(cond, msg) do {                                   \
        if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    } while (0)

/* Build a SYN/ACK option block exactly like tcp_write_syn_options:
 * MSS, then FEC-PERM (if fec), then SACK (NOP NOP SACK_OK len), NOP-padded
 * to a 4-byte boundary. Returns the option length in bytes. */
static int build_synack_opts(uint8_t *buf, uint16_t mss, int sack, int fec,
                             uint8_t k, uint8_t r, uint16_t sym_len)
{
    int i = 0;
    struct mss_opt *m = (struct mss_opt *) &buf[i];
    m->kind = OPT_MSS; m->len = OPTLEN_MSS; m->mss = htons(mss);
    i += OPTLEN_MSS;

    if (fec) {
        struct tcp_opt_fec_perm *fp = (struct tcp_opt_fec_perm *) &buf[i];
        fp->kind = TCP_OPT_FEC_PERM; fp->len = TCP_OPTLEN_FEC_PERM;
        fp->version = FEC_WIRE_VERSION; fp->k = k; fp->r = r;
        fp->flags = 0; fp->sym_len = htons(sym_len);
        i += TCP_OPTLEN_FEC_PERM;
    }

    if (sack) {
        buf[i++] = OPT_NOOP; buf[i++] = OPT_NOOP;
        buf[i++] = OPT_SACK_OK; buf[i++] = OPTLEN_SACK;
    }

    while (i % 4) buf[i++] = OPT_NOOP;
    return i;
}

/* Parser mirroring tcp_parse_opts's FEC gate. FEC-PERM is placed before SACK
 * on the wire precisely because the real SACK_OK case does not advance the
 * pointer; here we parse linearly and return whether a geometry-matching
 * FEC-PERM was seen. */
static int parse_fec_offer(const uint8_t *buf, int len)
{
    int i = 0, fec_ok = 0;
    while (i < len) {
        uint8_t kind = buf[i];
        if (kind == OPT_NOOP) { i++; continue; }
        if (kind == OPT_MSS)     { i += OPTLEN_MSS; continue; }
        if (kind == OPT_SACK_OK) { i += OPTLEN_SACK; continue; }
        if (kind == TCP_OPT_FEC_PERM) {
            if (len - i < TCP_OPTLEN_FEC_PERM) break;
            const struct tcp_opt_fec_perm *fp =
                (const struct tcp_opt_fec_perm *) &buf[i];
            if (fp->version == FEC_WIRE_VERSION &&
                fp->k == TCP_FEC_DEFAULT_K &&
                fp->r == TCP_FEC_DEFAULT_R &&
                ntohs(fp->sym_len) == TCP_FEC_DEFAULT_SYM_LEN)
                fec_ok = 1;
            i += TCP_OPTLEN_FEC_PERM;
            continue;
        }
        break; /* unknown */
    }
    return fec_ok;
}

int main(void)
{
    /* 1. Struct sizes / packing. */
    CHECK(sizeof(struct tcp_opt_fec_perm) == 8, "tcp_opt_fec_perm size != 8");
    CHECK(sizeof(struct tcp_opt_fec)      == 8, "tcp_opt_fec size != 8");
    CHECK(sizeof(struct fec_repair_hdr)   == 16, "fec_repair_hdr size != 16");
    CHECK(TCP_OPTLEN_FEC_PERM == 8, "TCP_OPTLEN_FEC_PERM != 8");
    CHECK(TCP_OPT_FEC_PERM == 30, "TCP_OPT_FEC_PERM != 30");
    CHECK(TCP_OPT_FEC == 31, "TCP_OPT_FEC != 31");

    /* 2. Exact SYN/ACK byte layout for a fully-negotiated handshake. */
    uint8_t buf[64];
    int n = build_synack_opts(buf, 1460, 1, 1,
                              TCP_FEC_DEFAULT_K, TCP_FEC_DEFAULT_R,
                              TCP_FEC_DEFAULT_SYM_LEN);
    const uint8_t expect[] = {
        0x02,0x04,0x05,0xB4,                          /* MSS 1460        */
        0x1E,0x08,0x01,0x08,0x02,0x00,0x02,0x00,      /* FEC-PERM        */
        0x01,0x01,0x04,0x02                           /* NOP NOP SACK-OK */
    };
    CHECK(n == 16, "negotiated SYN/ACK optlen != 16");
    CHECK(n % 4 == 0, "optlen not 4-byte aligned");
    CHECK(n <= OPT_MAX, "optlen exceeds 40-byte ceiling");
    CHECK(memcmp(buf, expect, sizeof(expect)) == 0, "SYN/ACK byte layout mismatch");

    /* 3. Negotiation gate: accept on exact match, reject on each mismatch. */
    CHECK(parse_fec_offer(buf, n) == 1, "matching geometry not accepted");

    /* absent FEC-PERM */
    int n2 = build_synack_opts(buf, 1460, 1, 0, 0, 0, 0);
    CHECK(n2 == 8, "MSS+SACK optlen != 8");
    CHECK(parse_fec_offer(buf, n2) == 0, "FEC accepted when not offered");

    /* version mismatch */
    n = build_synack_opts(buf, 1460, 1, 1, TCP_FEC_DEFAULT_K, TCP_FEC_DEFAULT_R,
                          TCP_FEC_DEFAULT_SYM_LEN);
    ((struct tcp_opt_fec_perm *)&buf[4])->version = 99;
    CHECK(parse_fec_offer(buf, n) == 0, "version mismatch accepted");

    /* k mismatch */
    n = build_synack_opts(buf, 1460, 1, 1, (uint8_t)(TCP_FEC_DEFAULT_K + 1),
                          TCP_FEC_DEFAULT_R, TCP_FEC_DEFAULT_SYM_LEN);
    CHECK(parse_fec_offer(buf, n) == 0, "k mismatch accepted");

    /* r mismatch */
    n = build_synack_opts(buf, 1460, 1, 1, TCP_FEC_DEFAULT_K,
                          (uint8_t)(TCP_FEC_DEFAULT_R + 1), TCP_FEC_DEFAULT_SYM_LEN);
    CHECK(parse_fec_offer(buf, n) == 0, "r mismatch accepted");

    /* sym_len mismatch */
    n = build_synack_opts(buf, 1460, 1, 1, TCP_FEC_DEFAULT_K, TCP_FEC_DEFAULT_R,
                          (uint16_t)(TCP_FEC_DEFAULT_SYM_LEN + 1));
    CHECK(parse_fec_offer(buf, n) == 0, "sym_len mismatch accepted");

    /* 4. Per-segment FEC option round-trip. */
    uint8_t ob[8];
    struct tcp_opt_fec *opt = (struct tcp_opt_fec *) ob;
    opt->kind = TCP_OPT_FEC; opt->len = TCP_OPTLEN_FEC;
    opt->type = FEC_SYM_REPAIR; opt->index = 17;
    opt->block_id = htonl(0xDEADBEEF);
    CHECK(opt->kind == 31 && opt->type == FEC_SYM_REPAIR && opt->index == 17,
          "tcp_opt_fec fields corrupt");
    CHECK(ntohl(opt->block_id) == 0xDEADBEEF, "tcp_opt_fec block_id round-trip");

    /* 5. Repair header round-trip. */
    uint8_t rb[16];
    struct fec_repair_hdr *rh = (struct fec_repair_hdr *) rb;
    rh->version = FEC_WIRE_VERSION; rh->k = 8; rh->r = 2; rh->repair_index = 1;
    rh->block_id = htonl(0x01020304);
    rh->base_seq = htonl(0xCAFEF00D);
    rh->sym_len  = htons(TCP_FEC_DEFAULT_SYM_LEN);
    rh->tail_len = htons(333);
    CHECK(rh->k == 8 && rh->r == 2 && rh->repair_index == 1, "repair hdr fields corrupt");
    CHECK(ntohl(rh->block_id) == 0x01020304, "repair block_id round-trip");
    CHECK(ntohl(rh->base_seq) == 0xCAFEF00D, "repair base_seq round-trip");
    CHECK(ntohs(rh->sym_len) == TCP_FEC_DEFAULT_SYM_LEN, "repair sym_len round-trip");
    CHECK(ntohs(rh->tail_len) == 333, "repair tail_len round-trip");

    if (failures == 0) {
        printf("Handshake wire-format selftest passed\n");
        return 0;
    }
    printf("Handshake wire-format selftest FAILED (%d)\n", failures);
    return 1;
}