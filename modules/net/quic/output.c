// SPDX-License-Identifier: GPL-2.0-or-later
/* QUIC kernel implementation
 * (C) Copyright Red Hat Corp. 2023
 *
 * This file is part of the QUIC kernel implementation
 *
 * Initialization/cleanup for QUIC protocol support.
 *
 * Written or modified by:
 *    Xin Long <lucien.xin@gmail.com>
 */

#include "socket.h"

static int quic_outq_limit_check(struct sock *sk, u8 type, u16 frame_len)
{
	struct quic_path_group *paths = quic_paths(sk);
	struct quic_packet *packet = quic_packet(sk);
	struct quic_outqueue *outq = quic_outq(sk);
	u16 len;

	if (outq->single && outq->count)
		return -1;

	/* congestion control */
	if (!outq->single && quic_frame_ack_eliciting(type) && !quic_frame_ping(type)) {
		len = quic_packet_frame_len(packet) + frame_len;
		if (outq->inflight + len > outq->window)
			return -1;
	}

	/* amplificationlimit */
	if (quic_is_serv(sk) && !quic_path_validated(paths)) {
		len = quic_packet_len(packet) + frame_len + quic_packet_taglen(packet);
		if (quic_path_ampl_sndlen(paths) + len > quic_path_ampl_rcvlen(paths) * 3) {
			quic_path_set_blocked(paths, 1);
			return -1;
		}
	}

	return 0;
}

static int quic_outq_transmit_flush(struct sock *sk)
{
	struct quic_packet *packet = quic_packet(sk);
	struct quic_outqueue *outq = quic_outq(sk);
	int count = outq->count;

	outq->count = 0;
	if (!quic_packet_empty(packet))
		count += quic_packet_create(sk);
	quic_packet_flush(sk);

	return count;
}

static void quic_outq_transmit_ctrl(struct sock *sk, u8 level)
{
	struct quic_pnspace *space = quic_pnspace(sk, level);
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_frame *frame, *next;
	struct list_head *head;

	if (!quic_crypto_send_ready(quic_crypto(sk, level)))
		return;

	if (quic_pnspace_need_sack(space)) {
		if (!quic_outq_transmit_frame(sk, QUIC_FRAME_ACK, &level,
					      quic_pnspace_sack_path(space), true))
			quic_pnspace_set_need_sack(space, 0);
	}

	head = &outq->control_list;
	list_for_each_entry_safe(frame, next, head, list) {
		if (!frame->level && level)
			break;
		if (frame->level != level)
			continue;
		if (quic_packet_config(sk, frame->level, frame->path))
			break;
		if (quic_outq_limit_check(sk, frame->type, frame->len))
			break;
		if (quic_packet_tail(sk, frame))
			continue;
		outq->count += quic_packet_create(sk);
		next = frame;
	}
}

static void quic_outq_transmit_dgram(struct sock *sk)
{
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_frame *frame, *next;
	struct list_head *head;

	if (!quic_crypto_send_ready(quic_crypto(sk, outq->data_level)))
		return;

	head = &outq->datagram_list;
	list_for_each_entry_safe(frame, next, head, list) {
		if (quic_packet_config(sk, outq->data_level, frame->path))
			break;
		if (quic_outq_limit_check(sk, frame->type, frame->len))
			break;
		if (quic_packet_tail(sk, frame))
			continue;
		outq->count += quic_packet_create(sk);
		next = frame;
	}
}

int quic_outq_flow_control(struct sock *sk, struct quic_stream *stream, u16 bytes, u8 sndblock)
{
	struct quic_outqueue *outq = quic_outq(sk);
	u8 frame, blocked = 0, transmit = 0;

	/* send flow control */
	if (stream->send.bytes + bytes > stream->send.max_bytes) {
		blocked = stream->send.data_blocked;
		if (!blocked && stream->send.last_max_bytes < stream->send.max_bytes) {
			frame = QUIC_FRAME_STREAM_DATA_BLOCKED;
			if (sndblock && !quic_outq_transmit_frame(sk, frame, stream, 0, true))
				transmit = 1;
			stream->send.last_max_bytes = stream->send.max_bytes;
			stream->send.data_blocked = 1;
		}
		blocked = 1;
	}
	if (outq->bytes + bytes > outq->max_bytes) {
		blocked = outq->data_blocked;
		if (!blocked && outq->last_max_bytes < outq->max_bytes) {
			frame = QUIC_FRAME_DATA_BLOCKED;
			if (sndblock && !quic_outq_transmit_frame(sk, frame, outq, 0, true))
				transmit = 1;
			outq->last_max_bytes = outq->max_bytes;
			outq->data_blocked = 1;
		}
		blocked = 1;
	}

	if (transmit)
		quic_outq_transmit(sk);

	return blocked;
}

u64 quic_outq_wspace(struct sock *sk, struct quic_stream *stream)
{
	struct quic_outqueue *outq = quic_outq(sk);
	u64 len = outq->max_bytes - outq->bytes;

	if (stream) {
		len = min_t(u64, len, sk_stream_wspace(sk));
		len = min_t(u64, len, stream->send.max_bytes - stream->send.bytes);
	}

	return len;
}

static int quic_outq_delay_check(struct sock *sk, u8 level, u8 nodelay)
{
	struct quic_packet *packet = quic_packet(sk);
	struct quic_outqueue *outq = quic_outq(sk);
	u64 pacing_time;

	if (level || outq->close_frame) /* do not delay for early data or closing sockets */
		return 0;

	/* pacing control */
	pacing_time = quic_cong_pacing_time(quic_cong(sk));
	if (pacing_time > ktime_get_ns()) {
		quic_timer_start(sk, QUIC_TIMER_PACE, pacing_time);
		return 1;
	}

	/* nagle algorithm */
	if (nodelay)
		return 0;
	if (!quic_packet_empty(packet))
		return 0;
	if ((quic_config(sk)->stream_data_nodelay || !outq->inflight) &&
	    !outq->force_delay)
		return 0;
	if (outq->stream_list_len > quic_packet_mss(packet))
		return 0;
	return 1;
}

static void quic_outq_transmit_stream(struct sock *sk)
{
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_frame *frame, *next;
	struct list_head *head;

	if (!quic_crypto_send_ready(quic_crypto(sk, outq->data_level)))
		return;

	head = &outq->stream_list;
	list_for_each_entry_safe(frame, next, head, list) {
		if (quic_packet_config(sk, outq->data_level, frame->path))
			break;
		if (quic_outq_limit_check(sk, frame->type, frame->len))
			break;
		if (quic_outq_delay_check(sk, outq->data_level, frame->nodelay))
			break;
		if (quic_packet_tail(sk, frame)) {
			outq->stream_list_len -= frame->len;
			continue;
		}
		outq->count += quic_packet_create(sk);
		next = frame;
	}
}

static void quic_outq_transmit_old(struct sock *sk, u8 level)
{
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_frame *frame, *next;
	struct list_head *head;

	head = &outq->transmitted_list;
	list_for_each_entry_safe(frame, next, head, list) {
		if (!frame->level && level)
			break;
		if (frame->level != level)
			continue;
		if (!quic_frame_retransmittable(frame->type))
			continue;
		if (!quic_crypto_send_ready(quic_crypto(sk, frame->level)))
			break;
		if (quic_packet_config(sk, frame->level, frame->path))
			break;
		if (quic_outq_limit_check(sk, frame->type, frame->len))
			break;
		if (quic_packet_tail(sk, frame))
			continue; /* packed and conintue with the next frame */
		outq->count += quic_packet_create(sk); /* build and xmit the packed frames */
		next = frame; /* go back but still pack the current frame */
	}
}

/* pack and transmit frames from outqueue */
int quic_outq_transmit(struct sock *sk)
{
	quic_outq_transmit_ctrl(sk, QUIC_CRYPTO_INITIAL);
	quic_outq_transmit_ctrl(sk, QUIC_CRYPTO_HANDSHAKE);
	quic_outq_transmit_ctrl(sk, QUIC_CRYPTO_APP);

	quic_outq_transmit_dgram(sk);
	quic_outq_transmit_stream(sk);

	return quic_outq_transmit_flush(sk);
}

static int quic_outq_transmit_single(struct sock *sk, u8 level)
{
	struct quic_outqueue *outq = quic_outq(sk);

	outq->single = 1;
	quic_outq_transmit_ctrl(sk, level);

	if (level == QUIC_CRYPTO_APP) {
		quic_outq_transmit_dgram(sk);
		quic_outq_transmit_stream(sk);
	}

	quic_outq_transmit_old(sk, level);
	outq->single = 0;

	return quic_outq_transmit_flush(sk);
}

static void quic_outq_wfree(int len, struct sock *sk)
{
	if (!len)
		return;

	WARN_ON(refcount_sub_and_test(len, &sk->sk_wmem_alloc));
	sk_wmem_queued_add(sk, -len);
	sk_mem_uncharge(sk, len);

	if (sk_stream_wspace(sk) > 0)
		sk->sk_write_space(sk);
}

static void quic_outq_set_owner_w(int len, struct sock *sk)
{
	if (!len)
		return;

	refcount_add(len, &sk->sk_wmem_alloc);
	sk_wmem_queued_add(sk, len);
	sk_mem_charge(sk, len);
}

int quic_outq_stream_append(struct sock *sk, struct quic_msginfo *info, u8 pack)
{
	struct quic_stream_table *streams = quic_streams(sk);
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_stream *stream = info->stream;
	struct quic_frame *frame;
	struct list_head *head;
	int len, bytes;

	head = &outq->stream_list;
	if (list_empty(head))
		return -1;
	frame = list_last_entry(head, struct quic_frame, list);
	if (frame->stream != stream || frame->nodelay || frame->offset >= 0)
		return -1;

	len = frame->len;
	bytes = quic_frame_stream_append(sk, frame, info, pack);
	if (bytes < 0 || !pack)
		return bytes;

	if (frame->type & QUIC_STREAM_BIT_FIN &&
	    stream->send.state == QUIC_STREAM_SEND_STATE_SEND) {
		if (quic_stream_send_active_id(streams) == stream->id)
			quic_stream_set_send_active_id(streams, -1);
		stream->send.state = QUIC_STREAM_SEND_STATE_SENT;
	}

	stream->send.bytes += bytes;
	stream->send.offset += bytes;

	outq->bytes += bytes;
	outq->stream_list_len += (frame->len - len);
	quic_outq_set_owner_w((int)bytes, sk);

	return bytes;
}

void quic_outq_stream_tail(struct sock *sk, struct quic_frame *frame, bool cork)
{
	struct quic_stream_table *streams = quic_streams(sk);
	struct quic_stream *stream = frame->stream;
	struct quic_outqueue *outq = quic_outq(sk);

	if (stream->send.state == QUIC_STREAM_SEND_STATE_READY)
		stream->send.state = QUIC_STREAM_SEND_STATE_SEND;

	if (frame->type & QUIC_STREAM_BIT_FIN &&
	    stream->send.state == QUIC_STREAM_SEND_STATE_SEND) {
		if (quic_stream_send_active_id(streams) == stream->id)
			quic_stream_set_send_active_id(streams, -1);
		stream->send.state = QUIC_STREAM_SEND_STATE_SENT;
	}

	stream->send.frags++;
	stream->send.bytes += frame->bytes;
	stream->send.offset += frame->bytes;

	outq->bytes += frame->bytes;
	outq->stream_list_len += frame->len;
	quic_outq_set_owner_w((int)frame->bytes, sk);

	list_add_tail(&frame->list, &outq->stream_list);
	if (!cork)
		quic_outq_transmit(sk);
}

void quic_outq_dgram_tail(struct sock *sk, struct quic_frame *frame, bool cork)
{
	quic_outq_set_owner_w((int)frame->bytes, sk);
	list_add_tail(&frame->list, &quic_outq(sk)->datagram_list);
	if (!cork)
		quic_outq_transmit(sk);
}

void quic_outq_ctrl_tail(struct sock *sk, struct quic_frame *frame, bool cork)
{
	struct list_head *head = &quic_outq(sk)->control_list;
	struct quic_frame *pos;

	list_for_each_entry(pos, head, list) {
		if (frame->level) {
			if (!pos->level) {
				head = &pos->list;
				break;
			}
			if (frame->level > pos->level)
				continue;
			if (frame->level < pos->level) {
				head = &pos->list;
				break;
			}
		}
		if (quic_frame_path_probing(frame->type)) {
			head = &pos->list;
			break;
		}
		if (!frame->level)
			break;
	}

	quic_outq_set_owner_w((int)frame->bytes, sk);
	list_add_tail(&frame->list, head);
	if (!cork)
		quic_outq_transmit(sk);
}

void quic_outq_transmitted_tail(struct sock *sk, struct quic_frame *frame)
{
	struct list_head *head = &quic_outq(sk)->transmitted_list;
	struct quic_frame *pos;

	list_for_each_entry(pos, head, list) {
		if (!frame->level) {
			if (pos->level)
				continue;
			goto offset;
		}
		if (!pos->level) {
			head = &pos->list;
			break;
		}
		if (frame->level > pos->level)
			continue;
		if (frame->level < pos->level) {
			head = &pos->list;
			break;
		}
offset:
		if (frame->offset < pos->offset) {
			head = &pos->list;
			break;
		}
	}
	frame->transmitted = 1;
	list_add_tail(&frame->list, head);
}

void quic_outq_packet_sent_tail(struct sock *sk, struct quic_packet_sent *sent)
{
	struct list_head *head = &quic_outq(sk)->packet_sent_list;
	struct quic_packet_sent *pos;

	if (sent->level) {
		list_for_each_entry(pos, head, list) {
			if (!pos->level) {
				head = &pos->list;
				break;
			}
			if (sent->level > pos->level)
				continue;
			if (sent->level < pos->level) {
				head = &pos->list;
				break;
			}
		}
	}
	list_add_tail(&sent->list, head);
}

void quic_outq_transmit_probe(struct sock *sk)
{
	struct quic_pnspace *space = quic_pnspace(sk, QUIC_CRYPTO_APP);
	u32 taglen = quic_packet_taglen(quic_packet(sk));
	struct quic_path_group *paths = quic_paths(sk);
	struct quic_config *c = quic_config(sk);
	struct quic_probeinfo info;
	u32 pathmtu;
	s64 number;

	if (!quic_is_established(sk))
		return;

	if (quic_packet_config(sk, QUIC_CRYPTO_APP, 0))
		return;

	info.size = quic_path_probe_size(paths);
	info.level = QUIC_CRYPTO_APP;
	number = quic_pnspace_next_pn(space);
	if (!quic_outq_transmit_frame(sk, QUIC_FRAME_PING, &info, 0, false)) {
		pathmtu = quic_path_pl_send(paths, number);
		if (pathmtu)
			quic_packet_mss_update(sk, pathmtu + taglen);
	}

	quic_timer_reset(sk, QUIC_TIMER_PMTU, c->plpmtud_probe_interval);
}

void quic_outq_transmit_close(struct sock *sk, u8 type, u32 errcode, u8 level)
{
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_connection_close close = {};

	if (!errcode)
		return;

	close.errcode = errcode;
	close.frame = type;
	quic_inq_event_recv(sk, QUIC_EVENT_CONNECTION_CLOSE, &close);

	quic_outq_set_close_errcode(outq, errcode);
	quic_outq_set_close_frame(outq, type);

	quic_outq_transmit_frame(sk, QUIC_FRAME_CONNECTION_CLOSE, &level, 0, false);
	quic_set_state(sk, QUIC_SS_CLOSED);
}

void quic_outq_transmit_app_close(struct sock *sk)
{
	u32 errcode = QUIC_TRANSPORT_ERROR_APPLICATION;
	u8 type = QUIC_FRAME_CONNECTION_CLOSE, level;
	struct quic_outqueue *outq = quic_outq(sk);

	if (quic_is_established(sk)) {
		level = QUIC_CRYPTO_APP;
		type = QUIC_FRAME_CONNECTION_CLOSE_APP;
		outq->close_frame = type;
		quic_outq_transmit(sk); /* flush data before closing */
	} else if (quic_is_establishing(sk)) {
		level = QUIC_CRYPTO_INITIAL;
		quic_outq_set_close_errcode(outq, errcode);
	} else {
		return;
	}
	/* send close frame only when it's NOT idle timeout or closed by peer */
	quic_outq_transmit_frame(sk, type, &level, 0, false);
}

static void quic_outq_psent_sack_frames(struct sock *sk, struct quic_packet_sent *sent)
{
	struct quic_stream_table *streams = quic_streams(sk);
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_stream_update update;
	struct quic_stream *stream;
	struct quic_frame *frame;
	int acked = 0, i;

	for (i = 0; i < sent->frames; i++) {
		frame = sent->frame_array[i];
		if (list_empty(&frame->list)) {
			quic_frame_put(frame);
			continue;
		}
		quic_frame_put(frame);

		stream = frame->stream;
		if (quic_frame_stream(frame->type)) {
			stream->send.frags--;
			if (!stream->send.frags &&
			    stream->send.state == QUIC_STREAM_SEND_STATE_SENT) {
				update.id = stream->id;
				update.state = QUIC_STREAM_SEND_STATE_RECVD;
				quic_inq_event_recv(sk, QUIC_EVENT_STREAM_UPDATE, &update);

				stream->send.state = update.state;
				quic_stream_send_put(streams, stream, quic_is_serv(sk));
				sk->sk_write_space(sk);
			}
		} else if (quic_frame_reset_stream(frame->type)) {
			update.id = stream->id;
			update.state = QUIC_STREAM_SEND_STATE_RESET_RECVD;
			update.errcode = stream->send.errcode;
			quic_inq_event_recv(sk, QUIC_EVENT_STREAM_UPDATE, &update);

			stream->send.state = update.state;
			quic_stream_send_put(streams, stream, quic_is_serv(sk));
			sk->sk_write_space(sk);
		} else if (quic_frame_stream_data_blocked(frame->type)) {
			stream->send.data_blocked = 0;
		} else if (quic_frame_data_blocked(frame->type)) {
			outq->data_blocked = 0;
		}

		acked += frame->bytes;
		frame->transmitted = 0;
		list_del_init(&frame->list);
		quic_frame_put(frame);
	}
	quic_outq_wfree(acked, sk);
}

static void quic_outq_path_confirm(struct sock *sk, u8 level, s64 largest, s64 smallest)
{
	struct quic_path_group *paths = quic_paths(sk);
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_config *c = quic_config(sk);
	bool raise_timer, complete;
	u32 pathmtu;

	if (quic_path_validated(paths))
		outq->pto_count = 0;

	if (level || !quic_path_pl_confirm(paths, largest, smallest))
		return;

	pathmtu = quic_path_pl_recv(paths, &raise_timer, &complete);
	if (pathmtu)
		quic_packet_mss_update(sk, pathmtu + quic_packet_taglen(quic_packet(sk)));
	if (!complete)
		quic_outq_transmit_probe(sk);
	if (raise_timer) /* reuse probe timer as raise timer */
		quic_timer_reset(sk, QUIC_TIMER_PMTU, (u64)c->plpmtud_probe_interval * 30);
}

void quic_outq_transmitted_sack(struct sock *sk, u8 level, s64 largest, s64 smallest,
				s64 ack_largest, u32 ack_delay)
{
	struct quic_pnspace *space = quic_pnspace(sk, level);
	struct quic_crypto *crypto = quic_crypto(sk, level);
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_cong *cong = quic_cong(sk);
	struct quic_packet_sent *sent, *next;
	u32 pto, acked = 0;

	quic_outq_path_confirm(sk, level, largest, smallest);
	pr_debug("%s: largest: %llu, smallest: %llu\n", __func__, largest, smallest);

	list_for_each_entry_safe_reverse(sent, next, &outq->packet_sent_list, list) {
		if (level != sent->level)
			continue;
		if (sent->number > largest)
			continue;
		if (sent->number < smallest)
			break;

		if (sent->ecn)
			quic_set_sk_ecn(sk, INET_ECN_ECT_0);

		outq->inflight -= sent->frame_len;
		quic_outq_psent_sack_frames(sk, sent);
		quic_pnspace_dec_inflight(space, sent->frame_len);

		if (sent->number == ack_largest) {
			quic_pnspace_set_max_pn_acked_seen(space, sent->number);
			quic_cong_rtt_update(cong, sent->sent_time, ack_delay);

			pto = quic_cong_pto(cong);
			quic_pnspace_set_max_time_limit(space, pto * 2);
			quic_crypto_set_key_update_time(crypto, pto * 2);
		}
		quic_cong_on_packet_acked(cong, sent->sent_time, sent->frame_len, sent->number);
		quic_outq_sync_window(sk, quic_cong_window(cong));

		acked += sent->frame_len;
		list_del(&sent->list);
		kfree(sent);
	}

	quic_cong_on_ack_recv(cong, acked, READ_ONCE(sk->sk_max_pacing_rate));
}

/* GetLossTimeAndSpace() */
static u32 quic_outq_get_loss_time(struct sock *sk, u8 *level)
{
	struct quic_pnspace *s;
	u32 time, t;

	s = quic_pnspace(sk, QUIC_CRYPTO_INITIAL);
	t = quic_pnspace_loss_time(s);
	time = t;
	*level = QUIC_CRYPTO_INITIAL;

	s = quic_pnspace(sk, QUIC_CRYPTO_HANDSHAKE);
	t = quic_pnspace_loss_time(s);
	if (t && (!time || time > t)) {
		time = t;
		*level = QUIC_CRYPTO_HANDSHAKE;
	}

	s = quic_pnspace(sk, QUIC_CRYPTO_APP);
	t = quic_pnspace_loss_time(s);
	if (t && (!time || time > t)) {
		time = t;
		*level = QUIC_CRYPTO_APP;
	}

	return time;
}

/* GetPtoTimeAndSpace() */
static u32 quic_outq_get_pto_time(struct sock *sk, u8 *level)
{
	u32 duration, t, time = 0, now = jiffies_to_usecs(jiffies);
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_pnspace *s;

	duration = quic_cong_pto(quic_cong(sk)) * (1 << outq->pto_count);

	if (!outq->inflight) {
		*level = QUIC_CRYPTO_INITIAL;
		if (quic_crypto_send_ready(quic_crypto(sk, QUIC_CRYPTO_HANDSHAKE)))
			*level = QUIC_CRYPTO_HANDSHAKE;
		return now + duration;
	}

	s = quic_pnspace(sk, QUIC_CRYPTO_INITIAL);
	if (quic_pnspace_inflight(s)) {
		t = quic_pnspace_last_sent_time(s) + duration;
		time = t;
		*level = QUIC_CRYPTO_INITIAL;
	}

	s = quic_pnspace(sk, QUIC_CRYPTO_HANDSHAKE);
	if (quic_pnspace_inflight(s)) {
		t = quic_pnspace_last_sent_time(s) + duration;
		if (!time || time > t) {
			time = t;
			*level = QUIC_CRYPTO_HANDSHAKE;
		}
	}

	if (time)
		return time;

	s =  quic_pnspace(sk, QUIC_CRYPTO_APP);
	if (quic_pnspace_inflight(s)) {
		duration += (outq->max_ack_delay * (1 << outq->pto_count));
		t = quic_pnspace_last_sent_time(s) + duration;
		if (!time || time > t) {
			time = t;
			*level = QUIC_CRYPTO_APP;
		}
	}

	return time;
}

/* SetLossDetectionTimer() */
void quic_outq_update_loss_timer(struct sock *sk)
{
	struct quic_path_group *paths = quic_paths(sk);
	struct quic_outqueue *outq = quic_outq(sk);
	u32 time, now = jiffies_to_usecs(jiffies);
	u8 level;

	time = quic_outq_get_loss_time(sk, &level);
	if (time)
		goto out;

	if ((!outq->inflight && quic_is_established(sk)) || quic_path_blocked(paths))
		return quic_timer_stop(sk, QUIC_TIMER_LOSS);

	time = quic_outq_get_pto_time(sk, &level);

out:
	time = (time > now) ? (time - now) : 1;
	quic_timer_reset(sk, QUIC_TIMER_LOSS, time);
}

void quic_outq_sync_window(struct sock *sk, u32 window)
{
	struct quic_outqueue *outq = quic_outq(sk);

	if (outq->window == window)
		return;
	outq->window = window;

	if (sk->sk_userlocks & SOCK_SNDBUF_LOCK)
		return;
	if (sk->sk_sndbuf > (int)window * 2)
		if (sk_stream_wspace(sk) > 0)
			sk->sk_write_space(sk);
	sk->sk_sndbuf = (int)window * 2;
}

/* put the timeout frame back to the corresponding outqueue */
static void quic_outq_retransmit_frame(struct sock *sk, struct quic_frame *frame)
{
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_frame *pos;
	struct list_head *head;

	head = &outq->control_list;
	if (quic_frame_stream(frame->type)) {
		head = &outq->stream_list;

		outq->stream_list_len += frame->len;
	}

	list_for_each_entry(pos, head, list) {
		if (!frame->level) {
			if (pos->level)
				continue;
			goto offset;
		}
		if (!pos->level) {
			head = &pos->list;
			break;
		}
		if (frame->level > pos->level)
			continue;
		if (frame->level < pos->level) {
			head = &pos->list;
			break;
		}
offset:
		if (pos->offset < 0 || frame->offset < pos->offset) {
			head = &pos->list;
			break;
		}
	}
	list_add_tail(&frame->list, head);
	QUIC_INC_STATS(sock_net(sk), QUIC_MIB_FRM_RETRANS);
}

static void quic_outq_psent_retransmit_frames(struct sock *sk, struct quic_packet_sent *sent)
{
	struct quic_frame *frame;
	int bytes = 0, i;

	for (i = 0; i < sent->frames; i++) {
		frame = sent->frame_array[i];
		if (list_empty(&frame->list)) {
			quic_frame_put(frame);
			continue;
		}
		quic_frame_put(frame);

		if (frame->transmitted) {
			list_del_init(&frame->list);

			if (!quic_frame_retransmittable(frame->type)) {
				bytes += frame->bytes;
				quic_frame_put(frame);
				continue;
			}
			frame->transmitted = 0;
			quic_outq_retransmit_frame(sk, frame); /* mark as loss */
		}
	}
	quic_outq_wfree(bytes, sk);
}

/* DetectAndRemoveLostPackets() */
void quic_outq_retransmit_mark(struct sock *sk, u8 level, u8 immediate)
{
	struct quic_pnspace *space = quic_pnspace(sk, level);
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_cong *cong = quic_cong(sk);
	struct quic_packet_sent *sent, *next;
	u32 time, now, pto, loss_time;
	s64 seen;

	seen = quic_pnspace_max_pn_acked_seen(space);
	time = quic_pnspace_max_pn_acked_time(space);
	now = jiffies_to_usecs(jiffies);
	pto = quic_cong_pto(cong);

	quic_pnspace_set_loss_time(space, 0);
	quic_cong_set_time(cong, now);

	list_for_each_entry_safe(sent, next, &outq->packet_sent_list, list) {
		if (level && !sent->level)
			break;
		if (level != sent->level)
			continue;
		if (!immediate && sent->number > seen)
			break;

		if (!immediate && sent->sent_time + pto > now && sent->number + 3 > seen) {
			loss_time = quic_pnspace_loss_time(space);
			if (!loss_time || loss_time > sent->sent_time + pto) {
				loss_time = sent->sent_time + pto;
				quic_pnspace_set_loss_time(space, loss_time);
			}
			break;
		}

		outq->inflight -= sent->frame_len;
		quic_outq_psent_retransmit_frames(sk, sent);
		quic_pnspace_dec_inflight(space, sent->frame_len);

		quic_cong_on_packet_lost(cong, time, sent->frame_len, sent->number);
		quic_outq_sync_window(sk, quic_cong_window(cong));

		list_del(&sent->list);
		kfree(sent);
	}
}

void quic_outq_retransmit_list(struct sock *sk, struct list_head *head)
{
	struct quic_frame *frame, *next;
	int bytes = 0;

	list_for_each_entry_safe(frame, next, head, list) {
		list_del_init(&frame->list);

		if (!quic_frame_retransmittable(frame->type)) {
			bytes += frame->bytes;
			quic_frame_put(frame);
			continue;
		}
		frame->transmitted = 0;
		quic_outq_retransmit_frame(sk, frame);
	}
	quic_outq_wfree(bytes, sk);
}

/* OnLossDetectionTimeout() */
void quic_outq_transmit_pto(struct sock *sk)
{
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_probeinfo info = {};
	u32 time;
	u8 level;

	time = quic_outq_get_loss_time(sk, &level);
	if (time) {
		quic_outq_retransmit_mark(sk, level, 0);
		quic_outq_update_loss_timer(sk);
		quic_outq_transmit(sk);
		return;
	}

	quic_outq_get_pto_time(sk, &level);

	if (quic_outq_transmit_single(sk, level))
		goto out;

	if (quic_packet_config(sk, level, 0))
		goto out;

	if (level) {
		info.level = level;
		info.size = QUIC_MIN_UDP_PAYLOAD;
	}
	quic_outq_transmit_frame(sk, QUIC_FRAME_PING, &info, 0, false);

out:
	if (outq->pto_count < 8)
		outq->pto_count++;
	quic_outq_update_loss_timer(sk);
}

int quic_outq_probe_path_alt(struct sock *sk, u8 cork)
{
	struct quic_conn_id_set *id_set = quic_dest(sk);
	struct quic_path_group *paths = quic_paths(sk);
	u8 path = 1;
	u64 number;

	if (!quic_conn_id_select_alt(id_set, false)) {
		if (quic_path_alt_state(paths, QUIC_PATH_ALT_PENDING))
			return -EINVAL;

		number = quic_conn_id_first_number(id_set);
		if (quic_outq_transmit_frame(sk, QUIC_FRAME_RETIRE_CONNECTION_ID, &number, 0, cork))
			return -ENOMEM;

		quic_path_set_alt_state(paths, QUIC_PATH_ALT_PENDING);
		return 0;
	}

	quic_path_set_alt_state(paths, QUIC_PATH_ALT_PROBING);
	quic_set_sk_ecn(sk, 0); /* clear ecn during path migration */
	quic_outq_transmit_frame(sk, QUIC_FRAME_PATH_CHALLENGE, &path, path, cork);
	quic_timer_reset_path(sk);
	return 0;
}

void quic_outq_update_path(struct sock *sk, u8 path)
{
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_frame *pos;

	list_for_each_entry(pos, &outq->control_list, list)
		pos->path = path;

	list_for_each_entry(pos, &outq->transmitted_list, list)
		pos->path = path;
}

void quic_outq_stream_list_purge(struct sock *sk, struct quic_stream *stream)
{
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_frame *frame, *next;
	int bytes = 0;

	list_for_each_entry_safe(frame, next, &outq->transmitted_list, list) {
		if (frame->stream != stream)
			continue;

		bytes += frame->bytes;
		list_del_init(&frame->list);
		quic_frame_put(frame);
	}

	list_for_each_entry_safe(frame, next, &outq->stream_list, list) {
		if (frame->stream != stream)
			continue;

		outq->stream_list_len -= frame->len;
		bytes += frame->bytes;
		list_del_init(&frame->list);
		quic_frame_put(frame);
	}
	quic_outq_wfree(bytes, sk);
}

int quic_outq_transmit_frame(struct sock *sk, u8 type, void *data, u8 path, u8 cork)
{
	struct quic_frame *frame;

	frame = quic_frame_create(sk, type, data);
	if (!frame)
		return -ENOMEM;

	frame->path = path;
	quic_outq_ctrl_tail(sk, frame, cork);
	return 0;
}

int quic_outq_transmit_new_conn_id(struct sock *sk, u64 prior, u8 path, u8 cork)
{
	struct quic_conn_id_set *id_set = quic_source(sk);
	u32 max, seqno;

	max = quic_conn_id_max_count(id_set) + prior - 1;
	for (seqno = quic_conn_id_last_number(id_set) + 1; seqno <= max; seqno++) {
		if (quic_outq_transmit_frame(sk, QUIC_FRAME_NEW_CONNECTION_ID, &prior,
					     path, true))
			return -ENOMEM;
	}
	if (!cork)
		quic_outq_transmit(sk);
	return 0;
}

int quic_outq_transmit_retire_conn_id(struct sock *sk, u64 prior, u8 path, u8 cork)
{
	struct quic_conn_id_set *id_set = quic_dest(sk);
	u64 seqno;

	for (seqno = quic_conn_id_first_number(id_set); seqno <= prior; seqno++) {
		if (quic_outq_transmit_frame(sk, QUIC_FRAME_RETIRE_CONNECTION_ID, &seqno,
					     path, cork))
			return -ENOMEM;
	}
	if (!cork)
		quic_outq_transmit(sk);
	return 0;
}

static void quic_outq_encrypted_work(struct work_struct *work)
{
	struct quic_sock *qs = container_of(work, struct quic_sock, outq.work);
	struct sock *sk = &qs->inet.sk;
	struct sk_buff_head *head;
	struct quic_crypto_cb *cb;
	struct sk_buff *skb;

	lock_sock(sk);
	head = &sk->sk_write_queue;
	if (sock_flag(sk, SOCK_DEAD)) {
		skb_queue_purge(head);
		goto out;
	}

	skb = skb_dequeue(head);
	while (skb) {
		cb = QUIC_CRYPTO_CB(skb);
		if (quic_packet_config(sk, cb->level, cb->path)) {
			kfree_skb(skb);
			skb = skb_dequeue(head);
			continue;
		}
		/* the skb here is ready to send */
		cb->resume = 1;
		quic_packet_xmit(sk, skb);
		skb = skb_dequeue(head);
	}
	quic_packet_flush(sk);
out:
	release_sock(sk);
	sock_put(sk);
}

void quic_outq_encrypted_tail(struct sock *sk, struct sk_buff *skb)
{
	struct quic_outqueue *outq = quic_outq(sk);

	sock_hold(sk);
	skb_queue_tail(&sk->sk_write_queue, skb);

	if (!schedule_work(&outq->work))
		sock_put(sk);
}

void quic_outq_set_param(struct sock *sk, struct quic_transport_param *p)
{
	struct quic_packet *packet = quic_packet(sk);
	struct quic_outqueue *outq = quic_outq(sk);
	struct quic_cong *cong = quic_cong(sk);
	u32 pmtu;

	if (!p->remote)
		return;

	outq->disable_compatible_version = p->disable_compatible_version;
	outq->disable_1rtt_encryption = p->disable_1rtt_encryption;
	outq->max_datagram_frame_size = p->max_datagram_frame_size;
	outq->max_udp_payload_size = p->max_udp_payload_size;
	outq->ack_delay_exponent = p->ack_delay_exponent;
	outq->max_idle_timeout = p->max_idle_timeout;
	outq->grease_quic_bit = p->grease_quic_bit;
	outq->stateless_reset = p->stateless_reset;
	outq->max_ack_delay = p->max_ack_delay;
	outq->max_data = p->max_data;

	outq->max_bytes = outq->max_data;
	quic_cong_set_max_window(cong, min_t(u64, outq->max_data, S32_MAX / 2));
	quic_cong_set_max_ack_delay(cong, outq->max_ack_delay);

	pmtu = min_t(u32, dst_mtu(__sk_dst_get(sk)), QUIC_PATH_MAX_PMTU);
	quic_packet_mss_update(sk, pmtu - packet->hlen);
}

void quic_outq_get_param(struct sock *sk, struct quic_transport_param *p)
{
	struct quic_outqueue *outq = quic_outq(sk);

	if (!p->remote)
		return;

	p->disable_compatible_version = outq->disable_compatible_version;
	p->disable_1rtt_encryption = outq->disable_1rtt_encryption;
	p->max_datagram_frame_size = outq->max_datagram_frame_size;
	p->max_udp_payload_size = outq->max_udp_payload_size;
	p->ack_delay_exponent = outq->ack_delay_exponent;
	p->max_idle_timeout = outq->max_idle_timeout;
	p->grease_quic_bit = outq->grease_quic_bit;
	p->stateless_reset = outq->stateless_reset;
	p->max_ack_delay = outq->max_ack_delay;
	p->max_data = outq->max_data;
}

void quic_outq_init(struct sock *sk)
{
	struct quic_outqueue *outq = quic_outq(sk);

	INIT_LIST_HEAD(&outq->stream_list);
	INIT_LIST_HEAD(&outq->control_list);
	INIT_LIST_HEAD(&outq->datagram_list);
	INIT_LIST_HEAD(&outq->transmitted_list);
	INIT_LIST_HEAD(&outq->packet_sent_list);
	skb_queue_head_init(&sk->sk_write_queue);
	INIT_WORK(&outq->work, quic_outq_encrypted_work);
}

static void quic_outq_psent_list_purge(struct sock *sk, struct list_head *head)
{
	struct quic_packet_sent *sent, *next;

	list_for_each_entry_safe(sent, next, head, list) {
		quic_outq_psent_sack_frames(sk, sent);
		list_del(&sent->list);
		kfree(sent);
	}
}

static void quic_outq_list_purge(struct sock *sk, struct list_head *head)
{
	struct quic_frame *frame, *next;
	int bytes = 0;

	list_for_each_entry_safe(frame, next, head, list) {
		bytes += frame->bytes;
		list_del_init(&frame->list);
		quic_frame_put(frame);
	}
	quic_outq_wfree(bytes, sk);
}

void quic_outq_free(struct sock *sk)
{
	struct quic_outqueue *outq = quic_outq(sk);

	quic_outq_psent_list_purge(sk, &outq->packet_sent_list);
	quic_outq_list_purge(sk, &outq->transmitted_list);
	quic_outq_list_purge(sk, &outq->datagram_list);
	quic_outq_list_purge(sk, &outq->control_list);
	quic_outq_list_purge(sk, &outq->stream_list);
	kfree(outq->close_phrase);
}
