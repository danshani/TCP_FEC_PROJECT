#ifndef TCP_OPTPARSE_H_
#define TCP_OPTPARSE_H_

#include <stdint.h>
#include "fec_frame.h"   /* TCP_OPT_FEC_PERM, geometry defaults, struct tcp_opt_fec_perm */

/*
 * TCP option kinds and lengths. These live here (rather than in tcp.h) so the
 * wire-format option walker below can be compiled and unit-tested in complete
 * isolation from the rest of the stack. tcp.h includes this header, so every
 * existing user keeps seeing the same definitions.
 */
#define TCP_OPT_END     0
#define TCP_OPT_NOOP    1
#define TCP_OPT_MSS     2
#define TCP_OPTLEN_MSS  4
#define TCP_OPT_SACK_OK 4
#define TCP_OPT_SACK    5
#define TCP_OPTLEN_SACK 2
#define TCP_OPT_TS      8

struct tcp_opt_mss {
    uint8_t kind;
    uint8_t len;
    uint16_t mss;
} __attribute__((packed));

/* Result of walking a TCP option area. Pure data — no stack types. */
struct tcp_parsed_opts {
    uint8_t  sack_ok;    /* SACK-permitted option present                   */
    uint8_t  ts_seen;    /* Timestamp option present                        */
    uint8_t  fec_perm;   /* FEC-permitted present AND geometry matches ours */
    uint16_t mss;        /* advertised MSS if a valid one was seen, else 0  */
};

/*
 * Walk a TCP option area ([kind] / [kind][len][...]) of optlen bytes starting
 * at opts, recording what we found into *out (which the caller must zero).
 *
 * Robust against malformed/truncated wire data: a missing length byte or an
 * option whose length overruns the area stops the walk instead of over-reading.
 * Multi-byte options are skipped by their own length so the walk stays in sync
 * regardless of option ordering. Reads multi-byte fields manually (big-endian)
 * so this unit has no libc networking dependency and tests build anywhere.
 */
void tcp_walk_options(const uint8_t *opts, uint8_t optlen,
                      struct tcp_parsed_opts *out);

#endif
