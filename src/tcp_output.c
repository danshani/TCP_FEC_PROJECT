#include "syshead.h"
#include "utils.h"
#include "tcp.h"
#include "ip.h"
#include "skbuff.h"
#include "timer.h"
#include "fec_frame.h"

static void *tcp_retransmission_timeout(void *arg);

static struct sk_buff *tcp_alloc_skb(int optlen, int size)
{
    int reserved = ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN + optlen + size;
    struct sk_buff *skb = alloc_skb(reserved);

    skb_reserve(skb, reserved);
    skb->protocol = IP_TCP;
    skb->dlen = size;
    skb->seq = 0;

    return skb;
}

static int tcp_write_syn_options(struct tcphdr *th, struct tcp_options *opts, int optlen)
{
    struct tcp_opt_mss *opt_mss = (struct tcp_opt_mss *) th->data;
    uint32_t i = 0;

    opt_mss->kind = TCP_OPT_MSS;
    opt_mss->len = TCP_OPTLEN_MSS;
    opt_mss->mss = htons(opts->mss);

    i += sizeof(struct tcp_opt_mss);

    /* FEC-permitted is written BEFORE the SACK option: the existing SACK_OK
     * parse case does not advance the option pointer, so any option after it
     * would be shadowed. Placing FEC first keeps it parseable. */
    if (opts->fec) {
        struct tcp_opt_fec_perm *fp = (struct tcp_opt_fec_perm *) &th->data[i];
        fp->kind    = TCP_OPT_FEC_PERM;
        fp->len     = TCP_OPTLEN_FEC_PERM;
        fp->version = FEC_WIRE_VERSION;
        fp->k       = TCP_FEC_DEFAULT_K;
        fp->r       = TCP_FEC_DEFAULT_R;
        fp->flags   = 0;
        fp->sym_len = htons(TCP_FEC_DEFAULT_SYM_LEN);
        i += TCP_OPTLEN_FEC_PERM;
    }

    if (opts->sack) {
        th->data[i++] = TCP_OPT_NOOP;
        th->data[i++] = TCP_OPT_NOOP;
        th->data[i++] = TCP_OPT_SACK_OK;
        th->data[i++] = TCP_OPTLEN_SACK;
    }

    th->hl = TCP_DOFFSET + (optlen / 4);

    return 0;
}

static int tcp_syn_options(struct sock *sk, struct tcp_options *opts)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    int optlen = 0;

    opts->mss = tsk->rmss;
    optlen += TCP_OPTLEN_MSS;

    if (tsk->sackok) {
        opts->sack = 1;
        optlen += TCP_OPT_NOOP * 2;
        optlen += TCP_OPTLEN_SACK;
    } else {
        opts->sack = 0;
    }

    if (tsk->fecok) {
        opts->fec = 1;
        optlen += TCP_OPTLEN_FEC_PERM;   /* 8 bytes, already 4-byte aligned */
    } else {
        opts->fec = 0;
    }

    return optlen;
}

static int tcp_write_options(struct tcp_sock *tsk, struct tcphdr *th)
{
    uint8_t *ptr = th->data;

    /* FEC source/repair segments pre-write their 8-byte option at build time
     * as the first (and only) option. The send path must not overwrite it;
     * SACK is never combined with FEC on the sender data path. The option
     * area is zero-filled at alloc, so data[0] is TCP_OPT_FEC iff we wrote one. */
    if (th->data[0] == TCP_OPT_FEC) return 0;

    if (!tsk->sackok || tsk->sacks[0].left == 0) return 0;

    *ptr++ = TCP_OPT_NOOP;
    *ptr++ = TCP_OPT_NOOP;
    *ptr++ = TCP_OPT_SACK;
    *ptr++ = 2 + tsk->sacklen * 8;

    struct tcp_sack_block *sb = (struct tcp_sack_block *)ptr;

    for (int i = tsk->sacklen - 1; i >= 0; i--) {
        sb->left = htonl(tsk->sacks[i].left);
        sb->right = htonl(tsk->sacks[i].right);
        tsk->sacks[i].left = 0;
        tsk->sacks[i].right = 0;

        sb += 1;
        ptr += sizeof(struct tcp_sack_block);
    }

    tsk->sacklen = 0;

    return 0;
}

static int tcp_transmit_skb(struct sock *sk, struct sk_buff *skb, uint32_t seq)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;
    struct tcphdr *thdr = tcp_hdr(skb);

    /* No options were previously set */
    if (thdr->hl == 0) thdr->hl = TCP_DOFFSET;

    skb_push(skb, thdr->hl * 4);

    thdr->sport = sk->sport;
    thdr->dport = sk->dport;
    thdr->seq = seq;
    thdr->ack_seq = tcb->rcv_nxt;
    thdr->rsvd = 0;
    thdr->win = tcb->rcv_wnd;
    thdr->csum = 0;
    thdr->urp = 0;

    if (thdr->hl > 5) {
        tcp_write_options(tsk, thdr);
    }

    tcp_out_dbg(thdr, sk, skb);

    thdr->sport = htons(thdr->sport);
    thdr->dport = htons(thdr->dport);
    thdr->seq = htonl(thdr->seq);
    thdr->ack_seq = htonl(thdr->ack_seq);
    thdr->win = htons(thdr->win);
    thdr->csum = htons(thdr->csum);
    thdr->urp = htons(thdr->urp);
    thdr->csum = tcp_v4_checksum(skb, htonl(sk->saddr), htonl(sk->daddr));
    
    return ip_output(sk, skb);
}

static int tcp_queue_transmit_skb(struct sock *sk, struct sk_buff *skb)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;
    struct tcphdr * th = tcp_hdr(skb);
    int rc = 0;
    
    if (skb_queue_empty(&sk->write_queue)) {
        tcp_rearm_rto_timer(tsk);
    }

    if (tsk->inflight == 0) {
        /* Store sequence information into the socket buffer */
        rc = tcp_transmit_skb(sk, skb, tcb->snd_nxt);
        tsk->inflight++;
        skb->seq = tcb->snd_nxt;
        tcb->snd_nxt += skb->dlen;
        skb->end_seq = tcb->snd_nxt;

        if (th->fin) tcb->snd_nxt++;
    }

    skb_queue_tail(&sk->write_queue, skb);

    return rc;
}

int tcp_send_synack(struct sock *sk)
{
    if (sk->state != TCP_SYN_SENT) {
        print_err("TCP synack: Socket was not in correct state (SYN_SENT)\n");
        return 1;
    }

    struct sk_buff *skb;
    struct tcphdr *th;
    struct tcb * tcb = &tcp_sk(sk)->tcb;
    int rc = 0;

    skb = tcp_alloc_skb(0, 0);
    th = tcp_hdr(skb);

    th->syn = 1;
    th->ack = 1;

    rc = tcp_transmit_skb(sk, skb, tcb->snd_nxt);
    free_skb(skb);

    return rc;
}

/* Routine for timer-invoked delayed acknowledgment */
void *tcp_send_delack(void *arg)
{
    struct sock *sk = (struct sock *) arg;
    socket_wr_acquire(sk->sock);

    struct tcp_sock *tsk = tcp_sk(sk);
    tsk->delacks = 0;
    tcp_release_delack_timer(tsk);
    tcp_send_ack(sk);

    socket_release(sk->sock);

    return NULL;
}

int tcp_send_next(struct sock *sk, int amount)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;
    struct tcphdr *th;
    struct sk_buff *next;
    struct list_head *item, *tmp;
    int i = 0;

    list_for_each_safe(item, tmp, &sk->write_queue.head) {
        if (++i > amount) break;
        next = list_entry(item, struct sk_buff, list);

        if (next == NULL) return -1;

        skb_reset_header(next);
        tcp_transmit_skb(sk, next, tcb->snd_nxt);

        next->seq = tcb->snd_nxt;
        tcb->snd_nxt += next->dlen;
        next->end_seq = tcb->snd_nxt;

        th = tcp_hdr(next);
        if (th->fin) tcb->snd_nxt++;
    }
    
    return 0;
}

static int tcp_options_len(struct sock *sk)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    uint8_t optlen = 0;

    if (tsk->sackok && tsk->sacklen > 0) {
        for (int i = 0; i < tsk->sacklen; i++) {
            if (tsk->sacks[i].left != 0) {
                optlen += 8;
            }
        }

        optlen += 2;
    }

    while (optlen % 4 > 0) optlen++;

    return optlen;
}

int tcp_send_ack(struct sock *sk)
{
    if (sk->state == TCP_CLOSE) return 0;
    
    struct sk_buff *skb;
    struct tcphdr *th;
    struct tcb *tcb = &tcp_sk(sk)->tcb;
    int rc = 0;
    int optlen = tcp_options_len(sk);

    skb = tcp_alloc_skb(optlen, 0);
    
    th = tcp_hdr(skb);
    th->ack = 1;
    th->hl = TCP_DOFFSET + (optlen / 4);

    rc = tcp_transmit_skb(sk, skb, tcb->snd_nxt);
    free_skb(skb);

    return rc;
}

static int tcp_send_syn(struct sock *sk)
{
    if (sk->state != TCP_SYN_SENT && sk->state != TCP_CLOSE && sk->state != TCP_LISTEN) {
        print_err("Socket was not in correct state (closed or listen)\n");
        return 1;
    }

    struct sk_buff *skb;
    struct tcphdr *th;
    struct tcp_options opts = { 0 };
    int tcp_options_len = 0;

    tcp_options_len = tcp_syn_options(sk, &opts);
    skb = tcp_alloc_skb(tcp_options_len, 0);
    th = tcp_hdr(skb);

    tcp_write_syn_options(th, &opts, tcp_options_len);
    sk->state = TCP_SYN_SENT;
    th->syn = 1;

    return tcp_queue_transmit_skb(sk, skb);
}

int tcp_send_fin(struct sock *sk)
{
    if (sk->state == TCP_CLOSE) return 0;

    struct sk_buff *skb;
    struct tcphdr *th;
    int rc = 0;

    skb = tcp_alloc_skb(0, 0);
    
    th = tcp_hdr(skb);
    th->fin = 1;
    th->ack = 1;

    rc = tcp_queue_transmit_skb(sk, skb);

    return rc;
}

void tcp_select_initial_window(uint32_t *rcv_wnd)
{
    *rcv_wnd = 44477;
}

static void tcp_notify_user(struct sock *sk)
{
    switch (sk->state) {
    case TCP_CLOSE_WAIT:
        wait_wakeup(&sk->sock->sleep);
        break;
    }
}

static void *tcp_connect_rto(void *arg)
{
    struct tcp_sock *tsk = (struct tcp_sock *) arg;
    struct tcb *tcb = &tsk->tcb;
    struct sock *sk = &tsk->sk;

    socket_wr_acquire(sk->sock);
    tcp_release_rto_timer(tsk);

    if (sk->state == TCP_SYN_SENT) {
        if (tsk->backoff > TCP_CONN_RETRIES) {
            tsk->sk.err = -ETIMEDOUT;
            sk->poll_events |= (POLLOUT | POLLERR | POLLHUP);
            tcp_done(sk);
        } else {
            struct sk_buff *skb = write_queue_head(sk);

            if (skb) {
                skb_reset_header(skb);
                tcp_transmit_skb(sk, skb, tcb->snd_una);
            
                tsk->backoff++;
                tcp_rearm_rto_timer(tsk);
            }
         }
    } else {
        print_err("TCP connect RTO triggered even when not in SYNSENT\n");
    }

    socket_release(sk->sock);

    return NULL;
}

static void *tcp_retransmission_timeout(void *arg)
{
    struct tcp_sock *tsk = (struct tcp_sock *) arg;
    struct tcb *tcb = &tsk->tcb;
    struct sock *sk = &tsk->sk;

    socket_wr_acquire(sk->sock);

    tcp_release_rto_timer(tsk);

    struct sk_buff *skb = write_queue_head(sk);

    if (!skb) {
        tsk->backoff = 0;
        tcpsock_dbg("TCP RTO queue empty, notifying user", sk);
        tcp_notify_user(sk);
        goto unlock;
    }

    struct tcphdr *th = tcp_hdr(skb);
    skb_reset_header(skb);
    
    tcp_transmit_skb(sk, skb, tcb->snd_una);
    /* RFC 6298: 2.5 Maximum value MAY be placed on RTO, provided it is at least
       60 seconds */
    if (tsk->rto > 60000) {
        tcp_done(sk);

        tsk->sk.err = -ETIMEDOUT;
        sk->poll_events |= (POLLOUT | POLLERR | POLLHUP);

        socket_release(sk->sock);
        return NULL;
    } else {
        /* RFC 6298: Section 5.5 double RTO time */
        tsk->rto *= 2;
        tsk->backoff++;
        tsk->retransmit = timer_add(tsk->rto, &tcp_retransmission_timeout, tsk);

        if (th->fin) {
            tcp_handle_fin_state(sk);
        }
    }

unlock:
    socket_release(sk->sock);

    return NULL;
}

void tcp_rearm_rto_timer(struct tcp_sock *tsk)
{
    struct sock *sk = &tsk->sk;
    tcp_release_rto_timer(tsk);

    if (sk->state == TCP_SYN_SENT) {
        tsk->retransmit = timer_add(TCP_SYN_BACKOFF << tsk->backoff, &tcp_connect_rto, tsk);
    } else {
        tsk->retransmit = timer_add(tsk->rto, &tcp_retransmission_timeout, tsk);
    }
}

int tcp_connect(struct sock *sk)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;
    int rc = 0;
    
    tsk->tcp_header_len = sizeof(struct tcphdr);
    tcb->iss = generate_iss();
    tcb->snd_wnd = 0;
    tcb->snd_wl1 = 0;

    tcb->snd_una = tcb->iss;
    tcb->snd_up = tcb->iss;
    tcb->snd_nxt = tcb->iss;
    tcb->rcv_nxt = 0;

    tcp_select_initial_window(&tsk->tcb.rcv_wnd);

    rc = tcp_send_syn(sk);
    tcb->snd_nxt++;
    
    return rc;
}

/* ============================================================= *
 *  FEC transmit path (Priority #1)                              *
 * ============================================================= */

/* GF(256) tables are read-only after init and identical for every
 * connection, so they are built once and shared by pointer. */
static struct fec_rs_ctx fec_shared_ctx;
static int fec_shared_ready = 0;

int tcp_fec_init_shared(void)
{
    if (fec_shared_ready) return 0;
    if (fec_rs_init(&fec_shared_ctx) != 0) return -1;
    fec_shared_ready = 1;
    return 0;
}

/* Allocate per-connection FEC state. Idempotent. */
int tcp_fec_enable(struct tcp_sock *tsk)
{
    struct tcp_fec *fec;
    uint8_t  k = TCP_FEC_DEFAULT_K;
    uint8_t  r = TCP_FEC_DEFAULT_R;
    uint16_t sym_len = TCP_FEC_DEFAULT_SYM_LEN;

    if (!tsk || !fec_shared_ready) return -1;
    if (tsk->fec) return 0;                       /* already enabled */

    if (k == 0 || k > FEC_BLK_MAX_K) return -1;
    if (r == 0 || r > FEC_BLK_MAX_R) return -1;
    if ((int)k + (int)r > 255)       return -1;
    if (sym_len == 0)                return -1;

    fec = calloc(1, sizeof(*fec));
    if (!fec) return -1;

    fec->k = k;
    fec->r = r;
    fec->sym_len = sym_len;
    fec->ctx = &fec_shared_ctx;

    /* Lazily-sized staging buffers (Priority #2 will pool these). */
    fec->tx_src_buf    = calloc((size_t)k, sym_len);
    fec->tx_repair_buf = calloc((size_t)r, sym_len);
    if (!fec->tx_src_buf || !fec->tx_repair_buf) goto fail;

    if (fec_encoder_init(&fec->enc, fec->ctx, k, r, sym_len, NULL) != 0)
        goto fail;

    tsk->fec = fec;
    return 0;

fail:
    free(fec->tx_src_buf);
    free(fec->tx_repair_buf);
    free(fec);
    return -1;
}

/* Free per-connection FEC state. */
void tcp_fec_release(struct tcp_sock *tsk)
{
    if (!tsk || !tsk->fec) return;
    free(tsk->fec->tx_src_buf);
    free(tsk->fec->tx_repair_buf);
    /* RX symbol_buf[] is owned/freed by the receiver path (not yet present). */
    free(tsk->fec);
    tsk->fec = NULL;
}

/* Write the 8-byte per-symbol FEC option at offset 0 of the option area. */
static void tcp_write_fec_option(struct tcphdr *th, uint8_t type,
                                 uint8_t index, uint32_t block_id)
{
    struct tcp_opt_fec *opt = (struct tcp_opt_fec *) th->data;
    opt->kind     = TCP_OPT_FEC;
    opt->len      = TCP_OPTLEN_FEC;
    opt->type     = type;
    opt->index    = index;
    opt->block_id = htonl(block_id);
}

/* Build and transmit ONE out-of-band repair segment:
 *   [FEC REPAIR option][fec_repair_hdr][sym_len parity bytes]
 * Sent directly via tcp_transmit_skb — never queued, never advances snd_nxt,
 * never retransmitted. Caller holds the socket write lock. */
static int tcp_fec_send_repair(struct sock *sk, struct tcp_fec *fec,
                               uint8_t blk_k, uint8_t repair_index,
                               uint16_t tail_len, const uint8_t *parity)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct sk_buff  *skb;
    struct tcphdr   *th;
    struct fec_repair_hdr *rh;
    int payload = (int)sizeof(struct fec_repair_hdr) + (int)fec->sym_len;
    int rc;

    skb = tcp_alloc_skb(TCP_OPTLEN_FEC, payload);
    th  = tcp_hdr(skb);

    /* Option first (build time), so tcp_write_options leaves it intact. */
    tcp_write_fec_option(th, FEC_SYM_REPAIR, repair_index, fec->tx_block_id);
    th->hl  = TCP_DOFFSET + (TCP_OPTLEN_FEC / 4);
    th->ack = 1;

    /* Reveal the payload region and fill header + parity. */
    skb_push(skb, payload);
    rh = (struct fec_repair_hdr *) skb->data;
    rh->version      = FEC_WIRE_VERSION;
    rh->k            = blk_k;
    rh->r            = fec->r;
    rh->repair_index = repair_index;
    rh->block_id     = htonl(fec->tx_block_id);
    rh->base_seq     = htonl(fec->tx_base_seq);
    rh->sym_len      = htons(fec->sym_len);
    rh->tail_len     = htons(tail_len);
    memcpy(skb->data + sizeof(struct fec_repair_hdr), parity, fec->sym_len);

    /* Stamp current snd_nxt as a reference only; DO NOT advance it. */
    rc = tcp_transmit_skb(sk, skb, tsk->tcb.snd_nxt);
    free_skb(skb);
    return rc;
}

/* Generate parity for the current block (full k, or short k'=blk_k) and emit
 * r repair segments, then reset for the next block. The source pointers are
 * already collected in fec->enc.src[0..blk_k-1]. Caller holds the write lock. */
static int tcp_fec_emit_block(struct sock *sk, struct tcp_fec *fec,
                              uint8_t blk_k, uint16_t tail_len)
{
    uint8_t *red_ptrs[FEC_BLK_MAX_R];
    struct fec_encoder tmp;
    const struct fec_encoder *enc;

    if (blk_k == 0)      return 0;       /* nothing staged */
    if (blk_k > fec->k)  return -1;      /* bounds guard   */

    for (int j = 0; j < fec->r; j++)
        red_ptrs[j] = fec->tx_repair_buf + (size_t)j * fec->sym_len;

    if (blk_k == fec->k) {
        enc = &fec->enc;                 /* full block */
    } else {
        /* Short final block: re-encode with k' = blk_k, reusing the source
         * pointers already gathered (no re-copy). */
        if (fec_encoder_init(&tmp, fec->ctx, blk_k, fec->r,
                             fec->sym_len, NULL) != 0)
            return -1;
        for (int i = 0; i < blk_k; i++)
            if (fec_encoder_add_source(&tmp, fec->enc.src[i]) != 0)
                return -1;
        enc = &tmp;
    }

    if (fec_encoder_generate(enc, red_ptrs) != 0)
        return -1;

    for (uint8_t j = 0; j < fec->r; j++)
        if (tcp_fec_send_repair(sk, fec, blk_k, j, tail_len,
                                fec->tx_repair_buf + (size_t)j * fec->sym_len) != 0)
            return -1;

    fec->tx_block_id++;
    fec->tx_filled = 0;
    fec_encoder_reset(&fec->enc);
    return 0;
}

/* Flush a partial in-progress block (stream terminating or paused). No-op if
 * no block in progress. Caller MUST hold the socket write lock. */
int tcp_fec_flush(struct tcp_sock *tsk)
{
    struct tcp_fec *fec = tsk ? tsk->fec : NULL;

    if (!fec || fec->tx_filled == 0) return 0;
    return tcp_fec_emit_block(&tsk->sk, fec, fec->tx_filled, fec->tx_tail_len);
}

int tcp_send(struct tcp_sock *tsk, const void *buf, int len)
{
    struct sk_buff *skb;
    struct tcphdr *th;
    int slen = len;
    struct tcp_fec *fec = tsk->fec;                  /* NULL ⇒ FEC not negotiated */
    int mss = fec ? (int) fec->sym_len : tsk->smss;  /* FEC: one segment = one symbol */
    int optlen = fec ? TCP_OPTLEN_FEC : 0;
    int dlen = 0;

    while (slen > 0) {
        dlen = slen > mss ? mss : slen;
        slen -= dlen;

        skb = tcp_alloc_skb(optlen, dlen);

        /* ---- FEC: tag this segment as a source symbol --------------- */
        if (fec) {
            /* Defensive bounds: a completed block must have been flushed,
             * so tx_filled is always < k here. Force-flush if not. */
            if (fec->tx_filled >= fec->k) {
                print_err("FEC: block slot overflow, forcing flush\n");
                tcp_fec_emit_block(&tsk->sk, fec, fec->tx_filled,
                                   fec->tx_tail_len);
            }

            /* First symbol pins the block's base data sequence number, so the
             * receiver can map recovered symbol i to base_seq + i*sym_len. */
            if (fec->tx_filled == 0)
                fec->tx_base_seq = tsk->tcb.snd_nxt;

            th = tcp_hdr(skb);
            tcp_write_fec_option(th, FEC_SYM_SOURCE,
                                 (uint8_t) fec->tx_filled, fec->tx_block_id);
            th->hl = TCP_DOFFSET + (TCP_OPTLEN_FEC / 4);
        }

        /* Payload follows the option area (geometry handled by tcp_alloc_skb). */
        skb_push(skb, dlen);
        memcpy(skb->data, buf, dlen);
        buf += dlen;

        th = tcp_hdr(skb);
        th->ack = 1;
        if (slen == 0) th->psh = 1;

        /* ---- FEC: register the symbol with the encoder ------------- */
        if (fec) {
            const uint8_t *sym_ptr;

            if (dlen == (int) fec->sym_len) {
                /* Full symbol → zero-copy. skb->data currently points at the
                 * payload; the buffer cannot be freed while we hold the write
                 * lock (RX/ACK free path takes the same lock). The captured
                 * pointer stays valid even after tcp_transmit_skb repositions
                 * skb->data, because the bytes do not move. */
                sym_ptr = skb->data;
                fec->tx_tail_len = fec->sym_len;
            } else {
                /* Ragged tail → copy into zero-padded staging so the coding
                 * math reads a full sym_len while only dlen bytes hit the wire.
                 * Bounds: dlen < sym_len, slot is exactly sym_len. */
                uint8_t *slot = fec->tx_src_buf +
                                (size_t) fec->tx_filled * fec->sym_len;
                memset(slot, 0, fec->sym_len);
                memcpy(slot, skb->data, (size_t) dlen);
                sym_ptr = slot;
                fec->tx_tail_len = (uint16_t) dlen;
            }

            if (fec_encoder_add_source(&fec->enc, sym_ptr) != 0) {
                print_err("FEC: encoder add_source failed\n");
            } else {
                fec->tx_filled++;
            }
        }

        if (tcp_queue_transmit_skb(&tsk->sk, skb) == -1) {
            perror("Error on TCP skb queueing");
        }

        /* ---- FEC: full block complete → generate parity synchronously,
         * while every source skb is still queued and lock-protected. */
        if (fec && fec->tx_filled == fec->k) {
            if (tcp_fec_emit_block(&tsk->sk, fec, fec->k,
                                   fec->tx_tail_len) != 0)
                print_err("FEC: block emit failed\n");
        }
    }

    tcp_rearm_user_timeout(&tsk->sk);

    return len;
}

int tcp_send_reset(struct tcp_sock *tsk)
{
    struct sk_buff *skb;
    struct tcphdr *th;
    struct tcb *tcb;
    int rc = 0;

    skb = tcp_alloc_skb(0, 0);
    th = tcp_hdr(skb);
    tcb = &tsk->tcb;

    th->rst = 1;
    tcb->snd_una = tcb->snd_nxt;

    rc = tcp_transmit_skb(&tsk->sk, skb, tcb->snd_nxt);
    free_skb(skb);

    return rc;
}

int tcp_send_challenge_ack(struct sock *sk, struct sk_buff *skb)
{
    // TODO: implement me
    return 0;
}

int tcp_queue_fin(struct sock *sk)
{
    struct sk_buff *skb;
    struct tcphdr *th;
    int rc = 0;

    /* Emit parity for any partial in-progress FEC block before teardown. */
    tcp_fec_flush(tcp_sk(sk));

    skb = tcp_alloc_skb(0, 0);
    th = tcp_hdr(skb);

    th->fin = 1;
    th->ack = 1;

    tcpsock_dbg("Queueing fin", sk);
    
    rc = tcp_queue_transmit_skb(sk, skb);

    return rc;
}
