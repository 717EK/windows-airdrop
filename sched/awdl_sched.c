/* Port of OWL's daemon/core.c scheduling (jedbillyb/owl 832d70f + omdrop-owl
 * patches) from libev timers to due times. Same decisions, same guards.
 * GPL-3.0-or-later, as the code it derives from. */
#include <string.h>

#include "awdl_sched.h"
#include "log.h"
#include "wire.h"
#include "rx.h"
#include "tx.h"
#include "peers.h"
#include "election.h"
#include "schedule.h"
#include "sync.h"

#define ETHER_LENGTH 14
#define ETHER_DST_OFFSET 0
#define ETHER_SRC_OFFSET 6
#define ETHER_ETHERTYPE_OFFSET 12
#define MAX_NUM_AMPDU 16
#define MULTICAST_QUEUE 16

static uint64_t after(uint64_t now, double sec) {
	return now + sec_to_usec(sec);
}

static void radio_send(struct awdl_sched *s, const uint8_t *buf, int len) {
	if (s->radio->transmit(s->radio_ctx, buf, (unsigned int) len) < 0)
		log_warn("radio: transmit of %d bytes failed", len);
}

static void send_action(struct awdl_sched *s, enum awdl_action_type type) {
	uint8_t buf[65535];
	int len = awdl_init_full_action_frame(buf, &s->awdl_state, &s->ieee80211_state, type);
	if (len < 0)
		return;
	radio_send(s, buf, len);
	s->awdl_state.stats.tx_action++;
}

/* Consumes buf: strips its Ethernet header and sends the rest as AWDL data. */
static void send_data(struct awdl_sched *s, const struct buf *buf) {
	uint8_t frame[65535];
	struct ether_addr src, dst;

	read_ether_addr(buf, ETHER_DST_OFFSET, &dst);
	read_ether_addr(buf, ETHER_SRC_OFFSET, &src);
	buf_strip(buf, ETHER_LENGTH);
	int len = awdl_init_full_data_frame(frame, &src, &dst, buf_data(buf), buf_len(buf),
	                                    &s->awdl_state, &s->ieee80211_state);
	s->awdl_state.stats.tx_data++;
	radio_send(s, frame, len);
}

static void switch_channel(struct awdl_sched *s, uint64_t now) {
	struct awdl_state *awdl = &s->awdl_state;
	int slot = awdl_sync_current_eaw(now, &awdl->sync) % AWDL_CHANSEQ_LENGTH;
	struct awdl_chan chan_new = awdl->channel.sequence[slot];
	int num_new = awdl_chan_num(chan_new, awdl->channel.enc);
	int num_old = awdl_chan_num(awdl->channel.current, awdl->channel.enc);

	if (num_new && num_new != num_old) {
		/* As in core.c: only believe we moved if the radio took the request. */
		if (s->radio->set_channel(s->radio_ctx, (unsigned int) num_new) < 0)
			log_warn("could not switch to channel %d (slot %d), radio still on %d", num_new, slot, num_old);
		else
			awdl->channel.current = chan_new;
	}
	s->due_chan = now + awdl_sync_next_aw_us(now, &awdl->sync);
}

static void send_mif(struct awdl_sched *s, uint64_t now) {
	struct awdl_state *awdl = &s->awdl_state;
	uint64_t next_aw = awdl_sync_next_aw_us(now, &awdl->sync);
	uint64_t eaw_len = awdl->sync.presence_mode * awdl->sync.aw_period;

	if (awdl_chan_num(awdl->channel.current, awdl->channel.enc) > 0)
		send_action(s, AWDL_ACTION_MIF);
	/* next one in the middle of the next EAW */
	s->due_mif = now + next_aw + ieee80211_tu_to_usec(eaw_len / 2);
}

static void send_unicast(struct awdl_sched *s, uint64_t now) {
	struct awdl_state *awdl = &s->awdl_state;
	struct awdl_peer *peer;
	struct ether_addr dst;
	double in = 0;

	s->due_unicast = AWDL_SCHED_IDLE;
	if (!s->next)
		return;

	read_ether_addr(s->next, ETHER_DST_OFFSET, &dst);
	if (compare_ether_addr(&dst, &awdl->self_address) == 0) { /* back to self */
		if (s->deliver)
			s->deliver(s->deliver_ctx, buf_data(s->next), buf_len(s->next));
	} else if (awdl_peer_get(awdl->peers.peers, &dst, &peer) < 0) {
		log_debug("drop frame to non-peer %s", ether_ntoa(&dst));
	} else {
		in = awdl_can_send_unicast_in(awdl, peer, now, AWDL_UNICAST_GUARD_TU);
		if (in != 0) { /* not now: retry when the window opens */
			if (in < 0) /* at the end of a slot, within the guard */
				in = -in + usec_to_sec(ieee80211_tu_to_usec(AWDL_UNICAST_GUARD_TU));
			s->due_unicast = after(now, in);
			return;
		}
		send_data(s, s->next);
		awdl->stats.tx_data_unicast++;
	}
	buf_free(s->next);
	s->next = NULL;
}

static void send_multicast(struct awdl_sched *s, uint64_t now) {
	struct awdl_state *awdl = &s->awdl_state;
	double in = 0;

	s->due_multicast = AWDL_SCHED_IDLE;
	if (circular_buf_empty(s->tx_queue_multicast))
		return;

	/* With -F the first AW may be all a peer spends on ours, so only the unicast guard applies. */
	int guard = awdl->channel.fixed_radio ? AWDL_UNICAST_GUARD_TU : AWDL_MULTICAST_GUARD_TU;
	in = awdl_can_send_in(awdl, now, guard);
	if (awdl_is_multicast_eaw(awdl, now) && in == 0) {
		void *next;
		circular_buf_get(s->tx_queue_multicast, &next, 0);
		send_data(s, next);
		buf_free(next);
		awdl->stats.tx_data_multicast++;
		in = 0;
	} else if (in == 0) { /* try again next EAW */
		in = usec_to_sec(ieee80211_tu_to_usec(64));
	} else if (in < 0) { /* at the end of a slot, within the guard */
		in = -in + usec_to_sec(ieee80211_tu_to_usec(guard));
	}
	if (!circular_buf_empty(s->tx_queue_multicast))
		s->due_multicast = after(now, in);
}

/* ---- Copied verbatim from OWL daemon/core.c (patched), only the state type renamed. ---- */

/*
 * Pick the peer whose advertised channel sequence should drive ours: the
 * elected sync master if it is in the peer table and has sent us a sequence.
 *
 * Kept deliberately simple. An earlier version preferred "the peer that most
 * recently sent us data" instead, which flapped between the phone and a
 * bystander at the 1 Hz tick and made throughput 6x worse (FINDINGS 17). Under
 * PIN the question mostly stops mattering anyway, because every Apple device in
 * the room names the same social channel and so maps to the same pin.
 */
static struct awdl_peer *awdl_chanseq_source(struct awdl_state *awdl) {
	struct awdl_peer *master;

	if (awdl_election_is_sync_master(&awdl->election, &awdl->self_address))
		return NULL;
	if (awdl_peer_get(awdl->peers.peers, &awdl->election.sync_addr, &master) < 0)
		return NULL; /* elected master is not in the peer table */
	if (!master->has_sequence)
		return NULL; /* no chanseq TLV seen from it yet; keep what we have */
	return master;
}

/*
 * Derive our own channel sequence from a peer's, according to the configured
 * strategy, so that we are on-channel during that peer's availability windows.
 * Reverts to our own static sequence when we are our own sync master again
 * (e.g. the master aged out of the peer table).
 *
 * The peer's sequence is stored as raw TLV bytes, so its encoding must be
 * adopted along with it -- decoding those bytes with a different encoding
 * yields plausible-looking but wrong channel numbers.
 */
static void awdl_adopt_chanseq(struct awdl_sched *state) {
	struct awdl_state *awdl = &state->awdl_state;
	struct awdl_peer *src;
	struct awdl_chan seq[AWDL_CHANSEQ_LENGTH];
	enum awdl_chan_encoding enc;
	uint64_t now = clock_time_us();

	src = awdl_chanseq_source(awdl);

	if (!src) {
		if (awdl->channel.sequence_from_peer) {
			log_info("no sync master, reverting to static channel sequence");
			awdl_chanseq_init_static(awdl->channel.sequence, &awdl->channel.master);
			awdl->channel.enc = AWDL_CHAN_ENC_OPCLASS;
			awdl->channel.sequence_from_peer = 0;
			awdl->channel.pinned_chan = 0;
			memset(&awdl->channel.seq_src, 0, sizeof(awdl->channel.seq_src));
		}
		return;
	}

	switch (awdl->channel.strategy) {
		case AWDL_CHANSEQ_PIN: {
			/* Sit on the peer's dominant social channel in every slot. We have no
			 * infra association to service on the monitor vif, so there is nothing
			 * to gain by ever leaving it -- and everything to lose, because an
			 * iPhone widens its own sequence from 2 slots to as many as 12 during a
			 * transfer and we want to be present for all of them without waiting
			 * for a 1 Hz tick to notice the change. Being on one channel in all 16
			 * slots is also phase-invariant, so the FINDINGS 17 failure (right
			 * channels, wrong times) cannot recur here by construction. */
			uint8_t want = awdl_chanseq_dominant_chan(src->sequence, src->sequence_enc,
			                                          awdl_chan_num(awdl->channel.master,
			                                                        AWDL_CHAN_ENC_OPCLASS));
			struct awdl_chan chan;

			/* -F: the radio never leaves -c's channel, so that is the one we are on
			 * in every slot, whatever the peer favours. */
			if (awdl->channel.fixed_radio)
				want = awdl_chan_num(awdl->channel.master, AWDL_CHAN_ENC_OPCLASS);

			if (!want)
				return; /* peer advertised nothing usable; keep what we have */
			if (awdl->channel.pinned_chan == want)
				return; /* already pinned there -- no log spam, no rewrite */

			chan = (struct awdl_chan) {{{want, want > 14 ? 0x80 : 0x51}}};
			awdl_chanseq_init_static(seq, &chan);
			enc = AWDL_CHAN_ENC_OPCLASS;
			awdl->channel.pinned_chan = want;
			log_info("pinning channel %d in all slots (dominant channel of %s (%s))",
			         want, ether_ntoa(&src->addr), src->name);
			break;
		}
		case AWDL_CHANSEQ_ROTATE: {
			/* Right channels at the right times: the peer's slot array is written
			 * in the peer's availability-window phase, but awdl_switch_channel()
			 * indexes our sequence in *ours*. Rotating by the difference puts the
			 * peer's slots where our clock will actually look for them.
			 *
			 * This is the correct form of the fix that FINDINGS 17 retracted. Note
			 * it is a no-op whenever the source is the elected master, since our
			 * phase is defined by that peer and the delta is then zero -- which is
			 * exactly why upstream never needed it. */
			int delta = ((int) awdl_sync_current_eaw(awdl_peer_time(now, src), &awdl->sync) -
			             (int) awdl_sync_current_eaw(now, &awdl->sync)) % AWDL_CHANSEQ_LENGTH;

			if (delta < 0)
				delta += AWDL_CHANSEQ_LENGTH;

			/* Hysteresis: require two consecutive ticks to agree before moving.
			 * Without it a peer whose phase sits near a window boundary makes the
			 * delta oscillate between two neighbouring values, rewriting the
			 * sequence every second -- the same flapping that made the FINDINGS 17
			 * attempt worse, arriving by a different route. */
			if (!awdl->channel.rot_valid) {
				awdl->channel.rot_delta = delta;
				awdl->channel.rot_valid = 1;
			} else if (delta != awdl->channel.rot_delta) {
				if (delta == awdl->channel.rot_delta_pending) {
					log_debug("rotation delta %d -> %d (confirmed)", awdl->channel.rot_delta, delta);
					awdl->channel.rot_delta = delta;
				} else {
					awdl->channel.rot_delta_pending = delta;
					return; /* unconfirmed: leave the sequence alone this tick */
				}
			}
			awdl_chanseq_rotate(seq, src->sequence, awdl->channel.rot_delta);
			enc = src->sequence_enc;
			break;
		}
		case AWDL_CHANSEQ_WIDEN: {
			/* The peer's own sequence, widened into some of its empty slots.
			 * §25 found the phone answers a sequence copied from it and ignores
			 * one pinned to a single channel; this is the ladder between those,
			 * so the width at which it stops answering is a measurement rather
			 * than a guess. */
			int filled = awdl_chanseq_widen(seq, src->sequence, src->sequence_enc,
			                                awdl->channel.widen_max);
			uint8_t ours = awdl_chan_num(awdl->channel.master, AWDL_CHAN_ENC_OPCLASS);
			int kept = ours ? awdl_chanseq_count_chan(src->sequence, src->sequence_enc, ours) : 0;

			enc = src->sequence_enc;
			if (filled)
				log_debug("widened %s's sequence by %d slot(s)", ether_ntoa(&src->addr), filled);

			/* Report the overlap even though we do not act on it here. Widening
			 * changes what we ADVERTISE; it cannot move the radio, which a
			 * P2P-GO chanctx holds on one channel. So "can this peer reach us"
			 * has the same answer under every strategy, and until now it was
			 * only ever measured under intersect -- which meant that selecting
			 * widen silently blinded the one line that carries it, and anything
			 * parsing this log went on reporting a state it was no longer
			 * measuring.
			 *
			 * Control flow differs from intersect: widen has nothing to withhold,
			 * so with zero overlap it reports and continues rather than bailing.
			 *
			 * The debug line retains the literal "intersect:" prefix because it is
			 * a wire format with a consumer (airdropd greps for it), not a
			 * description, even though nothing was intersected here. */
			if (!kept) {
				uint8_t want = awdl_chanseq_dominant_chan(src->sequence, src->sequence_enc, 0);

				log_info("NO OVERLAP with %s on ch %d - peer wants ch %d",
				         ether_ntoa(&src->addr), ours, want);
			} else {
				log_debug("intersect: %d/%d slots overlap %s on ch %d",
				          kept, AWDL_CHANSEQ_LENGTH, ether_ntoa(&src->addr), ours);
			}
			break;
		}
		case AWDL_CHANSEQ_INTERSECT: {
			/* Advertise only the slots we can actually serve. Our channel is the
			 * static one from -c, which is what the P2P-GO chanctx holds the radio
			 * on; awdl_switch_channel() cannot move off it, so every other slot in
			 * the peer's sequence is a slot we would be claiming and then missing. */
			uint8_t ours = awdl_chan_num(awdl->channel.master, AWDL_CHAN_ENC_OPCLASS);
			int kept = awdl_chanseq_intersect(seq, src->sequence, src->sequence_enc, ours);

			if (awdl->channel.fixed_radio) {
				/* -F: we are on our channel in every slot, so the honest sequence
				 * has every slot in which any peer is there, not only the
				 * master's. It matters because a peer sends to us only in slots
				 * that both sequences give to the channel, and the peer sending
				 * is rarely the master: an iPhone mid-transfer advertised
				 *   149 x8, 6,6,149,6,6,6,149,149
				 * while its master, an idle iPad, had channel 6 in slots 0 and 8.
				 * Following the iPad: 27 kB/s. Following the phone: 150 kB/s.
				 * Bytes are still only ever copied from a peer's own sequence, and
				 * only from peers whose slots line up with ours: a peer from
				 * another cluster means a different slot by the same index. */
				struct awdl_peer *p;
				awdl_peers_it_t it = awdl_peers_it_new(awdl->peers.peers);

				while (awdl_peers_it_next(it, &p) == PEERS_OK) {
					int64_t off_tu;

					if (p == src || !p->is_valid || !p->has_sequence || !p->has_sync_offset ||
					    p->sequence_enc != src->sequence_enc)
						continue;
					/* phase difference, within one period of 16 slots of 64 TU */
					off_tu = ((p->sync_offset / 1024) % 1024 + 1024 + 512) % 1024 - 512;
					if (off_tu > 16 || off_tu < -16)
						continue;
					for (int i = 0; i < AWDL_CHANSEQ_LENGTH; i++) {
						if (!awdl_chan_num(seq[i], src->sequence_enc) &&
						    awdl_chan_num(p->sequence[i], p->sequence_enc) == ours) {
							seq[i] = p->sequence[i];
							kept++;
						}
					}
				}
				awdl_peers_it_free(it);
			}

			if (!kept) {
				/* No overlap at all this tick. Keep whatever we last advertised
				 * rather than announcing an all-empty sequence, which says "never
				 * available" and would have the phone stop addressing us entirely.
				 *
				 * This is the silent-failure case: everything downstream looks
				 * healthy (owl up, awdl0 up, opendrop listening, mDNS leaving the
				 * radio) while not one slot can carry a frame to this peer. It is
				 * what "it just stopped appearing" looked like from outside, so it
				 * is logged at INFO with the channel we WOULD need, loudly enough
				 * for airdropd to notice and act on it. */
				uint8_t want = awdl_chanseq_dominant_chan(src->sequence, src->sequence_enc, 0);

				log_info("NO OVERLAP with %s on ch %d - peer wants ch %d",
				         ether_ntoa(&src->addr), ours, want);
				return;
			}
			log_debug("intersect: %d/%d slots overlap %s on ch %d",
			          kept, AWDL_CHANSEQ_LENGTH, ether_ntoa(&src->addr), ours);
			enc = src->sequence_enc;
			break;
		}
		case AWDL_CHANSEQ_VERBATIM:
		default:
			memcpy(seq, src->sequence, sizeof(seq));
			enc = src->sequence_enc;
			break;
	}

	if (awdl->channel.enc != enc || memcmp(awdl->channel.sequence, seq, sizeof(seq))) {
		awdl->channel.enc = enc;
		memcpy(awdl->channel.sequence, seq, sizeof(seq));
		awdl->channel.sequence_from_peer = 1;
		awdl->channel.seq_src = src->addr;
		if (awdl->channel.strategy != AWDL_CHANSEQ_PIN)
			log_info("following channel sequence of sync master %s (%s), enc %d",
			         ether_ntoa(&src->addr), src->name, enc);
	}
}

/* ---- End of copy. ---- */

static void clean_peers(struct awdl_sched *s, uint64_t now) {
	struct awdl_state *awdl = &s->awdl_state;
	uint64_t cutoff = now > awdl->peers.timeout ? now - awdl->peers.timeout : 0;
	awdl_peers_remove(awdl->peers.peers, cutoff,
	                  awdl->peer_remove_cb, awdl->peer_remove_cb_data);
	awdl_election_run(&awdl->election, &awdl->peers);
	awdl_adopt_chanseq(s);
	s->due_peers = now + awdl->peers.clean_interval;
}

void awdl_sched_init(struct awdl_sched *s, const char *hostname, const struct ether_addr *self,
                     struct awdl_chan chan, const struct RadioBackend *radio, void *radio_ctx,
                     awdl_deliver_cb deliver, void *deliver_ctx) {
	uint64_t now = clock_time_us();

	memset(s, 0, sizeof(*s));
	awdl_init_state(&s->awdl_state, hostname, self, chan, now);
	ieee80211_init_state(&s->ieee80211_state);
	s->radio = radio;
	s->radio_ctx = radio_ctx;
	s->deliver = deliver;
	s->deliver_ctx = deliver_ctx;
	s->tx_queue_multicast = circular_buf_init(MULTICAST_QUEUE);

	/* As awdl_schedule(): channel, MIF and TX run at once, PSF and cleanup after one interval. */
	s->due_chan = now;
	s->due_mif = now;
	s->due_psf = now + ieee80211_tu_to_usec(s->awdl_state.psf_interval);
	s->due_peers = now + s->awdl_state.peers.clean_interval;
	s->due_unicast = AWDL_SCHED_IDLE;
	s->due_multicast = AWDL_SCHED_IDLE;
}

void awdl_sched_free(struct awdl_sched *s) {
	void *b;
	if (s->next)
		buf_free(s->next);
	while (circular_buf_get(s->tx_queue_multicast, &b, 0) == 0)
		buf_free(b);
	circular_buf_free(s->tx_queue_multicast);
	awdl_peers_free(s->awdl_state.peers.peers);
}

#define DUE(t) ((t) != AWDL_SCHED_IDLE && (t) <= now)
#define MIN(a, b) ((a) < (b) ? (a) : (b))

uint64_t awdl_sched_tick(struct awdl_sched *s) {
	uint64_t now = clock_time_us();

	/* Channel first, so frames sent in this tick go out on the slot's channel. */
	if (DUE(s->due_chan))
		switch_channel(s, now);
	if (DUE(s->due_peers))
		clean_peers(s, now);
	if (DUE(s->due_psf)) {
		send_action(s, AWDL_ACTION_PSF);
		s->due_psf = now + ieee80211_tu_to_usec(s->awdl_state.psf_interval);
	}
	if (DUE(s->due_mif))
		send_mif(s, now);
	if (DUE(s->due_unicast))
		send_unicast(s, now);
	if (DUE(s->due_multicast))
		send_multicast(s, now);

	return MIN(MIN(MIN(s->due_chan, s->due_peers), MIN(s->due_psf, s->due_mif)),
	           MIN(s->due_unicast, s->due_multicast));
}

int awdl_sched_radio_rx(struct awdl_sched *s, const uint8_t *frame, int len) {
	struct buf *data_arr[MAX_NUM_AMPDU];
	struct buf **data = &data_arr[0];
	const struct buf *b = buf_new_const(frame, len);
	int result = awdl_rx(b, &data, &s->awdl_state);

	if (result == RX_OK) {
		for (struct buf **d = &data_arr[0]; d < data; d++) {
			if (s->deliver)
				s->deliver(s->deliver_ctx, buf_data(*d), buf_len(*d));
			buf_free(*d);
		}
	} else if (result < RX_OK) {
		s->awdl_state.stats.rx_unknown++;
	}
	buf_free(b);
	return result;
}

int awdl_sched_host_tx(struct awdl_sched *s, const uint8_t *eth, int len) {
	if (len < ETHER_LENGTH || len > ETHER_MAX_LEN)
		return -1;
	bool multicast = eth[ETHER_DST_OFFSET] & 0x01;
	if (multicast ? circular_buf_full(s->tx_queue_multicast) : s->next != NULL)
		return -1;

	struct buf *b = buf_new_owned(len);
	memcpy((uint8_t *) buf_data(b), eth, len);
	uint64_t now = clock_time_us();
	if (multicast) {
		circular_buf_put(s->tx_queue_multicast, b);
		if (s->due_multicast == AWDL_SCHED_IDLE)
			s->due_multicast = now;
	} else {
		s->next = b;
		s->due_unicast = now;
	}
	return 0;
}
