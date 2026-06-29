#include "tcp_optparse.h"

void tcp_walk_options(const uint8_t *ptr, uint8_t optlen,
                      struct tcp_parsed_opts *out)
{
    /* The option area can be up to 40 bytes (data offset is 4 bits ⇒ 60-byte
     * header − 20). Walk it as TCP defines it: kind 0 ends the list, kind 1
     * (NOOP) is a single padding byte, and every other option is length-
     * prefixed ([kind][len][...]). Advancing by the option's own length keeps
     * ptr and optlen in lockstep, so an option we don't special-case (or one
     * that appears before the one we want) never desynchronises the walk. */
    while (optlen > 0) {
        uint8_t kind = *ptr;

        if (kind == TCP_OPT_END) {
            break;                       /* end of option list */
        }

        if (kind == TCP_OPT_NOOP) {
            ptr += 1;                    /* single-byte padding, no length */
            optlen -= 1;
            continue;
        }

        /* Length-prefixed option: the length byte must be present and the
         * whole option must fit in what remains. Anything else is malformed
         * wire data — stop rather than over-read. */
        if (optlen < 2) break;
        uint8_t len = ptr[1];
        if (len < 2 || len > optlen) break;

        switch (kind) {
        case TCP_OPT_MSS:
            if (len == TCP_OPTLEN_MSS) {
                uint16_t mss = (uint16_t)((ptr[2] << 8) | ptr[3]);
                if (mss > 536 && mss <= 1460) {
                    out->mss = mss;
                }
            }
            break;
        case TCP_OPT_SACK_OK:
            out->sack_ok = 1;
            break;
        case TCP_OPT_TS:
            out->ts_seen = 1;
            break;
        case TCP_OPT_FEC_PERM: {
            const struct tcp_opt_fec_perm *fp =
                (const struct tcp_opt_fec_perm *) ptr;
            uint16_t sym_len = (uint16_t)((ptr[6] << 8) | ptr[7]);

            /* Engage only on EXACT geometry agreement (version, k, r, sym_len).
             * Any mismatch leaves fec_perm = 0 ⇒ FEC stays off (never corrupts;
             * worst case is plain TCP). */
            if (len == TCP_OPTLEN_FEC_PERM &&
                fp->version == FEC_WIRE_VERSION &&
                fp->k == TCP_FEC_DEFAULT_K &&
                fp->r == TCP_FEC_DEFAULT_R &&
                sym_len == TCP_FEC_DEFAULT_SYM_LEN) {
                out->fec_perm = 1;
            }
            break;
        }
        default:
            /* Unrecognised but well-formed option: skip it by its length. */
            break;
        }

        ptr += len;
        optlen -= len;
    }
}
