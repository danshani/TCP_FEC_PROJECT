#ifndef _FEC_FRAME_H
#define _FEC_FRAME_H

#include <stdint.h>
#include "fec_codec.h"   /* FEC_BLK_MAX_K, FEC_BLK_MAX_R, struct fec_rs_ctx,
                          * struct fec_encoder, struct fec_decoder */

/* ---- Protocol identification ---------------------------------------- */

#define FEC_WIRE_VERSION    1

/*
 * New TCP option kinds. Chosen from the experimental/unassigned range and
 * negotiated at SYN time, mirroring how this stack gates SACK.
 */
#define TCP_OPT_FEC_PERM    30   /* SYN only: FEC-permitted + block geometry  */
#define TCP_OPT_FEC         31   /* every coded segment: per-symbol coding tag */

#define TCP_OPTLEN_FEC_PERM 8
#define TCP_OPTLEN_FEC      8

/* ---- Default block geometry (used until/unless negotiation overrides) - */

#define TCP_FEC_DEFAULT_K   8
#define TCP_FEC_DEFAULT_R   2

/*
 * Fixed coding symbol length, negotiated for exact equality. Kept below the
 * minimum SMSS (536) so one full data segment carries exactly one symbol and
 * the 8-byte FEC option never pushes the frame past the link MTU.
 */
#define TCP_FEC_DEFAULT_SYM_LEN 512

/* Role of a coded segment, carried in the per-segment option. */
enum fec_sym_type {
    FEC_SYM_SOURCE = 0,   /* systematic data symbol: app bytes ARE the payload */
    FEC_SYM_REPAIR = 1,   /* parity symbol: fec_repair_hdr + parity bytes       */
};

/* ---- SYN-time negotiation ------------------------------------------- */

/*
 * Advertised in SYN (and, once supported, SYN/ACK). FEC engages for the
 * connection only if BOTH peers offer it with identical (version, k, r,
 * sym_len); any mismatch falls back to plain TCP. A peer that does not
 * understand the option ignores it, so the connection degrades gracefully.
 */
struct tcp_opt_fec_perm {
    uint8_t  kind;        /* TCP_OPT_FEC_PERM                      */
    uint8_t  len;         /* TCP_OPTLEN_FEC_PERM (8)               */
    uint8_t  version;     /* FEC_WIRE_VERSION                      */
    uint8_t  k;           /* target source symbols per block       */
    uint8_t  r;           /* repair symbols per block              */
    uint8_t  flags;       /* reserved, MBZ                         */
    uint16_t sym_len;     /* coding symbol size, bytes (net order) */
} __attribute__((packed));

/* ---- Per-segment coding tag ----------------------------------------- */

/*
 * Present on every FEC-coded segment (source and repair). Lets the receiver
 * place a payload into the correct block slot even when its neighbours are
 * lost. block_id is per-connection and monotonic; index is the slot within
 * the block (0..k-1 for source, 0..r-1 for repair).
 */
struct tcp_opt_fec {
    uint8_t  kind;        /* TCP_OPT_FEC                           */
    uint8_t  len;         /* TCP_OPTLEN_FEC (8)                    */
    uint8_t  type;        /* enum fec_sym_type                     */
    uint8_t  index;       /* symbol index within the block         */
    uint32_t block_id;    /* per-connection block number (net)     */
} __attribute__((packed));

/* ---- Repair payload header ------------------------------------------ */

/*
 * Prefixes the TCP payload of a REPAIR segment, ahead of the parity bytes.
 * Self-describing so a repair that arrives before (or without) its source
 * neighbours is still actionable, and so recovered symbols can be mapped
 * back onto data-stream sequence numbers:
 *
 *     recovered symbol i  ->  TCP seq = base_seq + i * sym_len
 *
 * `k` here is the ACTUAL source count of this block (a flushed final block
 * may be shorter than the negotiated target); `tail_len` is the real byte
 * length of the last source symbol so zero-padding can be stripped.
 */
struct fec_repair_hdr {
    uint8_t  version;     /* FEC_WIRE_VERSION                      */
    uint8_t  k;           /* source symbols in THIS block          */
    uint8_t  r;           /* repair symbols in THIS block          */
    uint8_t  repair_index;/* 0..r-1                                */
    uint32_t block_id;    /* matches tcp_opt_fec.block_id (net)    */
    uint32_t base_seq;    /* TCP seq of source symbol 0 (net)      */
    uint16_t sym_len;     /* padded symbol length (net)            */
    uint16_t tail_len;    /* real bytes in last source symbol (net)*/
} __attribute__((packed));

#endif