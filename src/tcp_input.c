#include "syshead.h"
#include "tcp.h"
#include "tcp_data.h"
#include "skbuff.h"
#include "sock.h"

static int tcp_parse_opts(struct tcp_sock *tsk, struct tcphdr *th)
{
    uint8_t *ptr = th->data;
    uint8_t optlen = tcp_hlen(th) - 20;
    struct tcp_opt_mss *opt_mss = NULL;
    uint8_t sack_seen = 0;
    uint8_t tsopt_seen = 0;
    uint8_t fec_seen = 0;

    while (optlen > 0 && optlen < 20) {
        switch (*ptr) {
        case TCP_OPT_MSS:
            opt_mss = (struct tcp_opt_mss *)ptr;
            uint16_t mss = ntohs(opt_mss->mss);

            if (mss > 536 && mss <= 1460) {
                tsk->smss = mss;
            }

            ptr += sizeof(struct tcp_opt_mss);
            optlen -= 4;
            break;
        case TCP_OPT_NOOP:
            ptr += 1;
            optlen--;
            break;
        case TCP_OPT_SACK_OK:
            sack_seen = 1;
            optlen--;
            break;
        case TCP_OPT_TS:
            tsopt_seen = 1;
            optlen--;
            break;
        case TCP_OPT_FEC_PERM: {
            struct tcp_opt_fec_perm *fp = (struct tcp_opt_fec_perm *) ptr;

            /* Bounds: the full 8-byte option must lie within the option area.
             * If not, the option stream is malformed — stop parsing. */
            if (optlen < TCP_OPTLEN_FEC_PERM) {
                optlen = 0;
                break;
            }

            /* Engage only on EXACT geometry agreement (version, k, r, sym_len).
             * Any mismatch leaves fec_seen = 0 ⇒ FEC stays off (never corrupts;
             * worst case is plain TCP). */
            if (fp->version == FEC_WIRE_VERSION &&
                fp->k == TCP_FEC_DEFAULT_K &&
                fp->r == TCP_FEC_DEFAULT_R &&
                ntohs(fp->sym_len) == TCP_FEC_DEFAULT_SYM_LEN) {
                fec_seen = 1;
            }

            ptr += TCP_OPTLEN_FEC_PERM;
            optlen -= TCP_OPTLEN_FEC_PERM;
            break;
        }
        default:
            print_err("Unrecognized TCPOPT\n");
            optlen--;
            break;
        }
    }

    if (!tsopt_seen) {
        tsk->tsopt = 0;
    }

    if (sack_seen && tsk->sackok) {
        // There's room for 4 sack blocks without TS OPT
        if (tsk->tsopt) tsk->sacks_allowed = 3;
        else tsk->sacks_allowed = 4;
    } else {
        tsk->sackok = 0;
    }

    /* FEC engages only if we offered it AND the peer agreed on geometry. */
    if (fec_seen && tsk->fecok) {
        tsk->fecok = 1;
    } else {
        tsk->fecok = 0;
    }

    return 0;
}

/*
 * Acks all segments from retransmissionn queue that are "older"
 * than current unacknowledged sequence
 */ 
static int tcp_clean_rto_queue(struct sock *sk, uint32_t una)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct sk_buff *skb;
    int rc = 0;
    
    while ((skb = skb_peek(&sk->write_queue)) != NULL) {
        if (skb->seq > 0 && skb->end_seq <= una) {
            /* skb fully acknowledged */
            skb_dequeue(&sk->write_queue);
            skb->refcnt--;
            free_skb(skb);
            if (tsk->inflight > 0) {
                tsk->inflight--;
            }
        } else {
            break;
        }
    };

    if (skb == NULL || tsk->inflight == 0) {
        /* No unacknowledged skbs, stop rto timer */
        tcp_stop_rto_timer(tsk);
    }

    return rc;
}

static inline int __tcp_drop(struct sock *sk, struct sk_buff *skb)
{
    free_skb(skb);
    return 0;
}

static int tcp_verify_segment(struct tcp_sock *tsk, struct tcphdr *th, struct sk_buff *skb)
{
    struct tcb *tcb = &tsk->tcb;

    if (skb->dlen > 0 && tcb->rcv_wnd == 0) return 0;

    if (th->seq < tcb->rcv_nxt ||
        th->seq > (tcb->rcv_nxt + tcb->rcv_wnd)) {
        tcpsock_dbg("Received invalid segment", (&tsk->sk));
        return 0;
    }

    return 1;
}

/* TCP RST received */
static void tcp_reset(struct sock *sk)
{
    sk->poll_events = (POLLOUT | POLLWRNORM | POLLERR | POLLHUP);
    switch (sk->state) {
    case TCP_SYN_SENT:
        sk->err = -ECONNREFUSED;
        break;
    case TCP_CLOSE_WAIT:
        sk->err = -EPIPE;
        break;
    case TCP_CLOSE:
        return;
    default:
        sk->err = -ECONNRESET;
        break;
    }

    tcp_done(sk);
}

static inline int tcp_discard(struct tcp_sock *tsk, struct sk_buff *skb, struct tcphdr *th)
{
    free_skb(skb);
    return 0;
}

static int tcp_listen(struct tcp_sock *tsk, struct sk_buff *skb, struct tcphdr *th)
{
    free_skb(skb);
    return 0;
}

static int tcp_synsent(struct tcp_sock *tsk, struct sk_buff *skb, struct tcphdr *th)
{
    struct tcb *tcb = &tsk->tcb;
    struct sock *sk = &tsk->sk;

    tcpsock_dbg("state is synsent", sk);
    
    if (th->ack) {
        if (th->ack_seq <= tcb->iss || th->ack_seq > tcb->snd_nxt) {
            tcpsock_dbg("ACK is unacceptable", sk);
            
            if (th->rst) goto discard;
            goto reset_and_discard;
        }

        if (th->ack_seq < tcb->snd_una || th->ack_seq > tcb->snd_nxt) {
            tcpsock_dbg("ACK is unacceptable", sk);
            goto reset_and_discard;
        }
    }

    /* ACK is acceptable */
    
    if (th->rst) {
        tcp_reset(&tsk->sk);
        goto discard;
    }

    /* third check the security and precedence -> ignored */

    /* fourth check the SYN bit */
    if (!th->syn) {
        goto discard;
    }

    tcb->rcv_nxt = th->seq + 1;
    tcb->irs = th->seq;
    if (th->ack) {
        tcb->snd_una = th->ack_seq;
        /* Any packets in RTO queue that are acknowledged here should be removed */
        tcp_clean_rto_queue(sk, tcb->snd_una);
    }

    if (tcb->snd_una > tcb->iss) {
        tcp_set_state(sk, TCP_ESTABLISHED);
        tcb->snd_una = tcb->snd_nxt;
        tsk->backoff = 0;
        /* RFC 6298: Sender SHOULD set RTO <- 1 second */
        tsk->rto = 1000;
        tcp_send_ack(&tsk->sk);
        tcp_rearm_user_timeout(&tsk->sk);
        tcp_parse_opts(tsk, th);

        /* Negotiation resolved: allocate codec state if both sides agreed. */
        if (tsk->fecok) {
            if (tcp_fec_enable(tsk) != 0) {
                tsk->fecok = 0;   /* alloc/geometry failure ⇒ fall back to TCP */
                print_err("FEC: enable failed; continuing without FEC\n");
            }
        }

        sock_connected(sk);
    } else {
        tcp_set_state(sk, TCP_SYN_RECEIVED);
        tcb->snd_una = tcb->iss;
        /* Resolve MSS/SACK/FEC from the peer's SYN before replying, so the
         * SYN/ACK echoes FEC-PERM only if the peer actually offered it. */
        tcp_parse_opts(tsk, th);
        tcp_send_synack(&tsk->sk);
    }
    
discard:
    tcp_drop(sk, skb);
    return 0;
reset_and_discard:
    //TODO reset
    tcp_drop(sk, skb);
    return 0;
}

static int tcp_closed(struct tcp_sock *tsk, struct sk_buff *skb, struct tcphdr *th)
{
    /*
      All data in the incoming segment is discarded.  An incoming
      segment containing a RST is discarded.  An incoming segment not
      containing a RST causes a RST to be sent in response.  The
      acknowledgment and sequence field values are selected to make the
      reset sequence acceptable to the TCP that sent the offending
      segment.

      If the ACK bit is off, sequence number zero is used,

        <SEQ=0><ACK=SEG.SEQ+SEG.LEN><CTL=RST,ACK>

      If the ACK bit is on,

        <SEQ=SEG.ACK><CTL=RST>

      Return.
    */

    int rc = -1;

    tcpsock_dbg("state is closed", (&tsk->sk));

    if (th->rst) {
        tcp_discard(tsk, skb, th);
        rc = 0;
        goto out;
    }

    if (th->ack) {
 
    } else {
        
    
    }
    
    rc = tcp_send_reset(tsk);
    free_skb(skb);

out:
    return rc;
}

/* ============================================================= *
 *  FEC receive path (Priority #1, receiver half)               *
 * ============================================================= */

/*
 * Inject a reconstructed source segment into the reassembly as if it had
 * arrived from the wire. A synthetic skb is built with a zeroed header region
 * (so tcp_data_dequeue reads psh = 0) and the recovered payload; feeding it to
 * tcp_data_queue advances rcv_nxt and drains the out-of-order queue exactly
 * like a real in-order segment. Caller holds the socket write lock.
 */
static void tcp_fec_splice(struct tcp_sock *tsk, uint32_t seq,
                           const uint8_t *data, int dlen)
{
    int reserve = ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN;
    struct sk_buff *skb = alloc_skb(reserve + dlen);

    if (!skb) return;

    memcpy(skb->head + reserve, data, (size_t) dlen);
    skb->payload = skb->head + reserve;
    skb->seq = seq;
    skb->dlen = dlen;
    skb->end_seq = seq + (uint32_t) dlen;

    /* th arg is unused by tcp_data_queue; the synthetic header is zeroed. */
    tcp_data_queue(tsk, NULL, skb);
}

/* Find the RX block slot for block_id, or claim one (evicting the oldest). */
static struct tcp_fec_block_rx *tcp_fec_rx_slot(struct tcp_fec *fec, uint32_t block_id)
{
    struct tcp_fec_block_rx *blk, *victim = NULL;

    for (int i = 0; i < TCP_FEC_RX_BLOCKS; i++) {
        blk = &fec->rx[i];
        if (blk->active && blk->block_id == block_id) return blk;
    }

    for (int i = 0; i < TCP_FEC_RX_BLOCKS; i++) {
        blk = &fec->rx[i];
        if (!blk->active) { victim = blk; break; }
        if (!victim || blk->block_id < victim->block_id) victim = blk;
    }

    if (!victim->symbol_buf) {
        victim->symbol_buf = calloc((size_t)(fec->k + fec->r), fec->sym_len);
        if (!victim->symbol_buf) return NULL;
    }

    victim->active = 1;
    victim->recovered = 0;
    victim->blk_k = 0;
    victim->block_id = block_id;
    victim->base_seq = 0;
    victim->tail_len = 0;
    victim->src_count = 0;
    victim->red_count = 0;
    memset(victim->src_present, 0, sizeof(victim->src_present));
    memset(victim->red_present, 0, sizeof(victim->red_present));
    return victim;
}

/*
 * Attempt to reconstruct any missing source symbols of a block and splice
 * them back in. No-op until the block is recoverable (>= k symbols present).
 */
static void tcp_fec_rx_recover(struct tcp_sock *tsk, struct tcp_fec_block_rx *blk)
{
    struct tcp_fec *fec = tsk->fec;
    size_t sym = fec->sym_len;
    int r = fec->r;
    int k = blk->blk_k ? blk->blk_k : fec->k;   /* assume full block if unknown */

    if (blk->recovered) return;
    if (k <= 0 || k > fec->k) return;           /* short/odd blocks beyond v1 -> TCP */

    int present = 0;
    for (int i = 0; i < k; i++)
        if (blk->src_present[i]) present++;
    int missing = k - present;

    if (missing == 0) { blk->recovered = 1; return; }   /* nothing lost */
    if (blk->red_count < missing) return;               /* not yet recoverable */

    struct fec_decoder dec;
    if (fec_decoder_init(&dec, fec->ctx, k, r, sym, NULL) != 0) return;

    for (int i = 0; i < k; i++)
        if (blk->src_present[i])
            fec_decoder_add_source(&dec, i, blk->symbol_buf + (size_t) i * sym);
    for (int j = 0; j < r; j++)
        if (blk->red_present[j])
            fec_decoder_add_redundant(&dec, j,
                blk->symbol_buf + (size_t)(fec->k + j) * sym);

    if (!fec_decoder_recoverable(&dec)) return;

    /* Reconstruct missing sources directly into their staging slots. */
    uint8_t *out[FEC_BLK_MAX_K] = { 0 };
    for (int i = 0; i < k; i++)
        out[i] = blk->src_present[i] ? NULL : (blk->symbol_buf + (size_t) i * sym);

    if (fec_decoder_recover(&dec, out) != 0) return;

    /* Splice each recovered symbol in seq order so rcv_nxt chains forward. */
    for (int i = 0; i < k; i++) {
        if (blk->src_present[i]) continue;

        int dlen = (int) sym;
        if (i == k - 1 && blk->tail_len > 0 && blk->tail_len < sym)
            dlen = blk->tail_len;

        tcp_fec_splice(tsk, blk->base_seq + (uint32_t) i * sym,
                       blk->symbol_buf + (size_t) i * sym, dlen);
        blk->src_present[i] = 1;
        blk->src_count++;
    }

    blk->recovered = 1;
    tcp_send_ack(&tsk->sk);   /* advertise the advanced rcv_nxt to the sender */
}

/*
 * Intercept FEC-tagged segments. Source segments are registered with the
 * block decoder and then delivered normally (return 0). Repair segments are
 * consumed (return 1) — they never enter the data stream. Returns 0 for
 * anything not FEC-tagged so normal processing continues.
 */
static int tcp_fec_input(struct sock *sk, struct tcphdr *th, struct sk_buff *skb)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcp_fec *fec = tsk->fec;
    struct tcp_opt_fec *opt;
    struct tcp_fec_block_rx *blk;
    size_t sym = fec->sym_len;

    /* Our peer writes the 8-byte FEC option first and alone on coded segments. */
    if (tcp_hlen(th) < TCP_HDR_LEN + TCP_OPTLEN_FEC) return 0;
    if (th->data[0] != TCP_OPT_FEC || th->data[1] != TCP_OPTLEN_FEC) return 0;

    opt = (struct tcp_opt_fec *) th->data;
    blk = tcp_fec_rx_slot(fec, ntohl(opt->block_id));
    if (!blk) return 0;   /* OOM -> fall back to plain TCP */

    if (opt->type == FEC_SYM_SOURCE) {
        int idx = opt->index;
        if (idx < 0 || idx >= fec->k) return 0;

        if (!blk->src_present[idx]) {
            int dlen = (int) skb->dlen;
            if (dlen > (int) sym) dlen = (int) sym;

            uint8_t *slot = blk->symbol_buf + (size_t) idx * sym;
            memset(slot, 0, sym);
            memcpy(slot, skb->payload, (size_t) dlen);   /* payload already skips opt */
            blk->src_present[idx] = 1;
            blk->src_count++;

            /* base_seq is derivable from any source: seq - index*sym_len. */
            blk->base_seq = skb->seq - (uint32_t) idx * sym;
            /* A short final source reveals the block's real size. */
            if (dlen < (int) sym) {
                blk->tail_len = (uint16_t) dlen;
                blk->blk_k = (uint8_t)(idx + 1);
            }
        }

        tcp_fec_rx_recover(tsk, blk);
        return 0;   /* deliver the source segment as normal stream data */
    }

    if (opt->type == FEC_SYM_REPAIR) {
        struct fec_repair_hdr *rh = (struct fec_repair_hdr *) skb->payload;
        int idx = opt->index;

        if ((int) skb->dlen < (int)(sizeof(*rh) + sym) ||
            idx < 0 || idx >= fec->r) {
            free_skb(skb);
            return 1;
        }

        blk->base_seq = ntohl(rh->base_seq);
        blk->tail_len = ntohs(rh->tail_len);
        blk->blk_k = rh->k;

        if (!blk->red_present[idx]) {
            uint8_t *slot = blk->symbol_buf + (size_t)(fec->k + idx) * sym;
            memcpy(slot, skb->payload + sizeof(*rh), sym);
            blk->red_present[idx] = 1;
            blk->red_count++;
        }

        tcp_fec_rx_recover(tsk, blk);
        free_skb(skb);   /* repair is out-of-band, never delivered to the stream */
        return 1;
    }

    return 0;
}

/*
 * Follows RFC793 "Segment Arrives" section closely
 */
int tcp_input_state(struct sock *sk, struct tcphdr *th, struct sk_buff *skb)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;

    tcpsock_dbg("input state", sk);

    switch (sk->state) {
    case TCP_CLOSE:
        return tcp_closed(tsk, skb, th);
    case TCP_LISTEN:
        return tcp_listen(tsk, skb, th);
    case TCP_SYN_SENT:
        return tcp_synsent(tsk, skb, th);
    }

    /* FEC: intercept tagged segments before the window check, so out-of-band
     * repair segments are not rejected as out-of-window. Source segments are
     * registered with the block decoder, then fall through to normal delivery. */
    if (tsk->fec && tcp_fec_input(sk, th, skb)) {
        return 0;
    }

    /* "Otherwise" section in RFC793 */

    /* first check sequence number */
    if (!tcp_verify_segment(tsk, th, skb)) {
        /* RFC793: If an incoming segment is not acceptable, an acknowledgment
         * should be sent in reply (unless the RST bit is set, if so drop
         *  the segment and return): */
        if (!th->rst) {
            tcp_send_ack(sk);
        }
        return_tcp_drop(sk, skb);
    }
    
    /* second check the RST bit */
    if (th->rst) {
        free_skb(skb);
        tcp_enter_time_wait(sk);
        tsk->sk.ops->recv_notify(&tsk->sk);
        return 0;
    }
    
    /* third check security and precedence */
    // Not implemented

    /* fourth check the SYN bit */
    if (th->syn) {
        /* RFC 5961 Section 4.2 */
        tcp_send_challenge_ack(sk, skb);
        return_tcp_drop(sk, skb);
    }
    
    /* fifth check the ACK field */
    if (!th->ack) {
        return_tcp_drop(sk, skb);
    }

    // ACK bit is on
    switch (sk->state) {
    case TCP_SYN_RECEIVED:
        if (tcb->snd_una <= th->ack_seq && th->ack_seq < tcb->snd_nxt) {
            tcp_set_state(sk, TCP_ESTABLISHED);
        } else {
            return_tcp_drop(sk, skb);
        }
    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
    case TCP_CLOSE_WAIT:
    case TCP_CLOSING:
    case TCP_LAST_ACK:
        if (tcb->snd_una < th->ack_seq && th->ack_seq <= tcb->snd_nxt) {
            tcb->snd_una = th->ack_seq;
            /* Any segments on the retransmission queue which are thereby
               entirely acknowledged are removed. */
            tcp_rtt(tsk);
            tcp_clean_rto_queue(sk, tcb->snd_una);
        }

        if (th->ack_seq < tcb->snd_una) {
            // If the ACK is a duplicate, it can be ignored
            return_tcp_drop(sk, skb);
        }

        if (th->ack_seq > tcb->snd_nxt) {
            // If the ACK acks something not yet sent, then send an ACK, drop segment
            // and return
            // TODO: Dropping the seg here, why would I respond with an ACK? Linux
            // does not respond either
            //tcp_send_ack(&tsk->sk);
            return_tcp_drop(sk, skb);
        }

        if (tcb->snd_una < th->ack_seq && th->ack_seq <= tcb->snd_nxt) {
            // TODO: Send window should be updated
        }

        break;
    }

    /* If the write queue is empty, it means our FIN was acked */
    if (skb_queue_empty(&sk->write_queue)) {
        switch (sk->state) {
        case TCP_FIN_WAIT_1:
            tcp_set_state(sk, TCP_FIN_WAIT_2);
        case TCP_FIN_WAIT_2:
            break;
        case TCP_CLOSING:
            /* In addition to the processing for the ESTABLISHED state, if
             * the ACK acknowledges our FIN then enter the TIME-WAIT state,
               otherwise ignore the segment. */
            tcp_set_state(sk, TCP_TIME_WAIT);
            break;
        case TCP_LAST_ACK:
            /* The only thing that can arrive in this state is an acknowledgment of our FIN.  
             * If our FIN is now acknowledged, delete the TCB, enter the CLOSED state, and return. */
            free_skb(skb);
            return tcp_done(sk);
        case TCP_TIME_WAIT:
            /* TODO: The only thing that can arrive in this state is a
               retransmission of the remote FIN.  Acknowledge it, and restart
               the 2 MSL timeout. */
            if (tcb->rcv_nxt == th->seq) {
                tcpsock_dbg("Remote FIN retransmitted?", sk);
//                tcb->rcv_nxt += 1;
                tsk->flags |= TCP_FIN;
                tcp_send_ack(sk);
            }
            break;
        }
    }
    
    /* sixth, check the URG bit */
    if (th->urg) {

    }

    int expected = skb->seq == tcb->rcv_nxt;

    /* seventh, process the segment txt */
    switch (sk->state) {
    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
        if (th->psh || skb->dlen > 0) {
            tcp_data_queue(tsk, th, skb);
        }
                
        break;
    case TCP_CLOSE_WAIT:
    case TCP_CLOSING:
    case TCP_LAST_ACK:
    case TCP_TIME_WAIT:
        /* This should not occur, since a FIN has been received from the
           remote side.  Ignore the segment text. */
        break;
    }

    /* eighth, check the FIN bit */
    if (th->fin && expected) {
        tcpsock_dbg("Received in-sequence FIN", sk);

        switch (sk->state) {
        case TCP_CLOSE:
        case TCP_LISTEN:
        case TCP_SYN_SENT:
            // Do not process, since SEG.SEQ cannot be validated
            goto drop_and_unlock;
        }

        tcb->rcv_nxt += 1;
        tsk->flags |= TCP_FIN;
        sk->poll_events |= (POLLIN | POLLPRI | POLLRDNORM | POLLRDBAND);
        
        tcp_send_ack(sk);
        tsk->sk.ops->recv_notify(&tsk->sk);

        switch (sk->state) {
        case TCP_SYN_RECEIVED:
        case TCP_ESTABLISHED:
            tcp_set_state(sk, TCP_CLOSE_WAIT);
            break;
        case TCP_FIN_WAIT_1:
            /* If our FIN has been ACKed (perhaps in this segment), then
               enter TIME-WAIT, start the time-wait timer, turn off the other
               timers; otherwise enter the CLOSING state. */
            if (skb_queue_empty(&sk->write_queue)) {
                tcp_enter_time_wait(sk);
            } else {
                tcp_set_state(sk, TCP_CLOSING);
            }

            break;
        case TCP_FIN_WAIT_2:
            /* Enter the TIME-WAIT state.  Start the time-wait timer, turn
               off the other timers. */
            tcp_enter_time_wait(sk);
            break;
        case TCP_CLOSE_WAIT:
        case TCP_CLOSING:
        case TCP_LAST_ACK:
            /* Remain in the state */
            break;
        case TCP_TIME_WAIT:
            /* TODO: Remain in the TIME-WAIT state.  Restart the 2 MSL time-wait
               timeout. */
            break;
        }
    }

    /* Congestion control and delacks */
    switch (sk->state) {
    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
        if (expected) {
            tcp_stop_delack_timer(tsk);

            int pending = min(skb_queue_len(&sk->write_queue), 3);
            /* RFC1122:  A TCP SHOULD implement a delayed ACK, but an ACK should not
             * be excessively delayed; in particular, the delay MUST be less than
             * 0.5 seconds, and in a stream of full-sized segments there SHOULD 
             * be an ACK for at least every second segment. */
            if (tsk->inflight == 0 && pending > 0) {
                tcp_send_next(sk, pending);
                tsk->inflight += pending;
                tcp_rearm_rto_timer(tsk);
            } else if (th->psh || (skb->dlen > 1000 && ++tsk->delacks > 1)) {
                tsk->delacks = 0;
                tcp_send_ack(sk);
            } else if (skb->dlen > 0) {
                tsk->delack = timer_add(200, &tcp_send_delack, &tsk->sk);
            }
        }
    }

    free_skb(skb);

unlock:
    return 0;
drop_and_unlock:
    tcp_drop(sk, skb);
    goto unlock;
}

int tcp_receive(struct tcp_sock *tsk, void *buf, int len)
{
    int rlen = 0;
    int curlen = 0;
    struct sock *sk = &tsk->sk;
    struct socket *sock = sk->sock;

    memset(buf, 0, len);

    while (rlen < len) {
        curlen = tcp_data_dequeue(tsk, buf + rlen, len - rlen);

        rlen += curlen;

        if (tsk->flags & TCP_PSH) {

            tsk->flags &= ~TCP_PSH;
            break;
        }

        if (tsk->flags & TCP_FIN || rlen == len) break;

        if (sock->flags & O_NONBLOCK) {
            if (rlen == 0) {
                rlen = -EAGAIN;
            } 
            
            break;
        } else {
            pthread_mutex_lock(&tsk->sk.recv_wait.lock);
            socket_release(sock);
            wait_sleep(&tsk->sk.recv_wait);
            pthread_mutex_unlock(&tsk->sk.recv_wait.lock);
            socket_wr_acquire(sock);
        }
    }

    if (rlen >= 0) tcp_rearm_user_timeout(sk);
    
    return rlen;
}
