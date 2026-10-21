/* Port of OWL's daemon/core.c scheduling (jedbillyb/owl 832d70f + omdrop-owl
 * patches) from libev timers to due times. Same decisions, same guards.
 * GPL-3.0-or-later, as the code it derives from. */
#include <stdio.h>
#include <string.h>

#include "awdl_sched.h"
#include "awdl_service.h"
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

static void ba_release(void *ctx, const uint8_t *frame, int len);

static void radio_send(struct awdl_sched *s, const uint8_t *buf, int len) {
	if (s->radio->transmit(s->radio_ctx, buf, (unsigned int) len) < 0)
		log_warn("radio: transmit of %d bytes failed", len);
}

static int build_action(struct awdl_sched *s, uint8_t *buf, enum awdl_action_type type) {
    int len = awdl_init_full_action_frame(buf, &s->awdl_state, &s->ieee80211_state, type);
    if (len < 0 || !s->service_port) return len;
    if (s->ieee80211_state.fcs) return -1; /* Native service mode uses the radio's hardware FCS. */
    unsigned rt = buf[2] | buf[3] << 8;
    for (unsigned off = rt + sizeof(struct ieee80211_hdr) + sizeof(struct awdl_action); off + 3 <= (unsigned)len;) {
        unsigned size = 3 + (buf[off + 1] | buf[off + 2] << 8);
        if (size > (unsigned)len - off) return -1;
        if (buf[off] == AWDL_SERVICE_PARAMETERS_TLV) {
            uint8_t filter[14];
            int n = awdl_service_filter(filter, sizeof filter);
            if (len - size + n > 65535) return -1;
            memmove(buf + off + n, buf + off + size, len - off - size);
            memcpy(buf + off, filter, n);
            len += n - size;
            break;
        }
        off += size;
    }
    if (type != AWDL_ACTION_MIF) return len;
    int n = awdl_service_records(buf + len, 65535 - len, s->awdl_state.self_address.ether_addr_octet,
                                 s->awdl_state.name, s->service_port);
    return n < 0 ? -1 : len + n;
}

static void send_action(struct awdl_sched *s, enum awdl_action_type type) {
	uint8_t buf[65535];
	int len = build_action(s, buf, type);
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
	struct ieee80211_tx_rate saved_rate = awdl_data_tx_rate;
	/* Discovery multicast stays at the compatible rate even in the faster data mode. */
	if (dst.ether_addr_octet[0] & 1)
		awdl_data_tx_rate = (struct ieee80211_tx_rate){ .legacy = 12, .mcs = -1, .sgi = 0 };
	int len = awdl_init_full_data_frame(frame, &src, &dst, buf_data(buf), buf_len(buf),
	                                    &s->awdl_state, &s->ieee80211_state);
	awdl_data_tx_rate = saved_rate;
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
	/* Find an eligible frame without moving blocked frames past each other. */
	for (unsigned i = 0; i < s->unicast_count; i++) {
		struct buf *frame = s->unicast[i];
		read_ether_addr(frame, ETHER_DST_OFFSET, &dst);
		if (compare_ether_addr(&dst, &awdl->self_address) == 0) { /* back to self */
			if (s->deliver)
				s->deliver(s->deliver_ctx, buf_data(frame), buf_len(frame));
		} else if (awdl_peer_get(awdl->peers.peers, &dst, &peer) < 0) {
			log_debug("drop frame to non-peer %s", ether_ntoa(&dst));
		} else {
			in = awdl_can_send_unicast_in(awdl, peer, now, AWDL_UNICAST_GUARD_TU);
			if (in != 0) { /* not now: retry when the window opens */
				if (in < 0) /* at the end of a slot, within the guard */
					in = -in + usec_to_sec(ieee80211_tu_to_usec(AWDL_UNICAST_GUARD_TU));
				uint64_t retry = after(now, in);
				if (retry < s->due_unicast) s->due_unicast = retry;
				continue;
			}
			send_data(s, frame);
			awdl->stats.tx_data_unicast++;
		}
		buf_free(frame);
		memmove(s->unicast + i, s->unicast + i + 1, (s->unicast_count - i - 1) * sizeof(*s->unicast));
		s->unicast_count--;
		if (s->unicast_count) s->due_unicast = now;
		return; /* At most one packet per tick; do not flood the USB transmit queue. */
	}
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

int awdl_sched_count_slots(struct awdl_sched *s, const unsigned *candidates, int n, int *slots) {
	struct awdl_peer *p;

	for (int c = 0; c < n; c++)
		slots[c] = 0;
	uint64_t now = clock_time_us();
	if (!s->receive_peer_seen || now < s->receive_peer_seen || now - s->receive_peer_seen > 5000000 ||
	    awdl_peer_get(s->awdl_state.peers.peers, &s->receive_peer, &p) != PEERS_OK ||
	    !p->is_valid || !p->has_sequence) return 0;
	for (int i = 0; i < AWDL_CHANSEQ_LENGTH; i++) {
		unsigned num = awdl_chan_num(p->sequence[i], p->sequence_enc);
		for (int c = 0; c < n; c++)
			if (num == candidates[c]) slots[c]++;
	}
	return 1;
}

unsigned awdl_chan_match_step(struct awdl_chan_match *m, const unsigned *candidates, const int *slots,
                              int n, int peers, uint64_t now) {
	int best = 0, current_slots = 0;

	(void) now; /* decisions are per check; the caller paces checks about 1 s apart */
	if (!peers && m->current == m->home) {
		m->pending = 0;
		m->stable = 0;
		return 0;
	}
	/* Away with no peer counts as "not helping" below (0 >= 0): losing the phone
	 * after moving is the failure seen live, and 149 is where discovery works. */
	int home_slots = 0;
	for (int c = 0; c < n; c++) {
		if (slots[c] > slots[best])
			best = c;
		if (candidates[c] == m->current)
			current_slots = slots[c];
		if (candidates[c] == m->home)
			home_slots = slots[c];
	}
	unsigned target = 0;
	if (m->current != m->home) {
		/* Away: the phone may drop its slots on our new channel once we arrive (seen
		 * live: 36 x6 fell to 36 x1). Go home as soon as away no longer beats it. */
		if (home_slots >= current_slots)
			target = m->home;
	} else if (!m->gave_up && candidates[best] != m->home && slots[best] >= home_slots + AWDL_MATCH_MARGIN) {
		target = candidates[best];
	}
	if (!target) {
		m->pending = 0;
		m->stable = 0;
		return 0;
	}
	/* Hysteresis: a sequence that flickers for one check must not move the radio. */
	if (m->pending == target)
		m->stable++;
	else {
		m->pending = target;
		m->stable = 1;
	}
	if (m->stable < AWDL_MATCH_STABLE)
		return 0;
	m->pending = 0;
	m->stable = 0;
	if (target == m->home)
		m->gave_up = 1; /* one failed try per session; retrying would just oscillate */
	return target;
}

void awdl_sched_set_channel(struct awdl_sched *s, unsigned chan) {
	struct awdl_state *awdl = &s->awdl_state;
	struct awdl_chan c = {{{(uint8_t) chan, chan > 14 ? 0x80 : 0x51}}};

	awdl->channel.master = c;
	awdl->channel.current = c;
	awdl_chanseq_init_static(awdl->channel.sequence, &c);
	awdl->channel.enc = AWDL_CHAN_ENC_OPCLASS;
	awdl->channel.sequence_from_peer = 0;
	awdl->channel.pinned_chan = 0;
	awdl_adopt_chanseq(s); /* advertise the slots we can now serve on the new channel */
}

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
	for (unsigned i = 0; i < s->unicast_count; i++) buf_free(s->unicast[i]);
	s->unicast_count = 0;
	while (circular_buf_get(s->tx_queue_multicast, &b, 0) == 0)
		buf_free(b);
	circular_buf_free(s->tx_queue_multicast);
	awdl_peers_free(s->awdl_state.peers.peers);
	ba_reorder_start(&s->ba, 0, 0); /* frees held frames */
	s->ba.active = 0;
}

#define DUE(t) ((t) != AWDL_SCHED_IDLE && (t) <= now)
#define MIN(a, b) ((a) < (b) ? (a) : (b))

uint64_t awdl_sched_tick(struct awdl_sched *s) {
	uint64_t now = clock_time_us();

	if (s->ba.active)
		ba_reorder_timeout(&s->ba, now, ba_release, s);

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

/*
 * Block Ack session setup (IEEE 802.11 category 3). Accepting ADDBA made the
 * test phone aggregate, but did not improve file goodput. Decline by default;
 * --rx-aggregation enables controlled experiments with the hardware responder.
 * Returns 1 when an ADDBA Request addressed to us was answered, 0 otherwise.
 */
static int answer_addba(struct awdl_sched *s, const uint8_t *frame, int len) {
	if (len < 4) return 0;
	unsigned rt = frame[2] | frame[3] << 8;
	if (rt < 8 || len < (int) rt + 24 + 9) return 0;
    const uint8_t *hdr = frame + rt;
	if (hdr[0] != 0xd0) return 0; /* management, action */
	if (memcmp(hdr + 4, s->awdl_state.self_address.ether_addr_octet, 6)) return 0;
	const uint8_t *body = hdr + 24;
	if (body[0] != 3 || body[1] != 0) return 0; /* Block Ack, ADDBA Request */
	uint8_t token = body[2];
	unsigned params = body[3] | body[4] << 8;
	unsigned tid = (params >> 2) & 0xf;
	unsigned buffer = params >> 6;
	if (!buffer || buffer > 64) buffer = 64;
	struct ether_addr peer;
	memcpy(&peer, hdr + 10, 6);

	uint8_t out[128];
	int n = ieee80211_init_radiotap_header(out);
	n += ieee80211_init_awdl_action_hdr(out + n, &s->awdl_state.self_address, &peer, &s->ieee80211_state);
	unsigned reply = (params & 1) | 2 /* immediate Block Ack */ | (tid << 2) | (buffer << 6);
	/* Aggregation stalled physical transfers; decline until explicitly tested. */
	int accepted = s->rx_aggregation && (params & 2);
	uint8_t resp[9] = {3, 1, token, accepted ? 0 : 37, 0, (uint8_t) reply, (uint8_t) (reply >> 8), 0, 0};
	memcpy(out + n, resp, sizeof resp);
	n += sizeof resp;
	radio_send(s, out, n);
	s->addba_answered++;
	if (accepted) {
		/* Starting Sequence Control follows the 2-byte timeout. */
		uint16_t ssn = (uint16_t) ((body[7] | body[8] << 8) >> 4);
		ba_reorder_stop(&s->ba, ba_release, s);
		ba_reorder_start(&s->ba, ssn, buffer);
		s->ba_peer = peer;
		s->ba_tid = tid;
	}
	/* printf: the desktop captures the link's stdout, OWL's log_info does not reach it. */
	printf("ADDBA request from %s tid %u buffer %u: %s with buffer %u\n", ether_ntoa(&peer), tid,
	       params >> 6, accepted ? "accepted" : "declined (experimental RX aggregation off)", buffer);
	fflush(stdout);
	return 1;
}

static int process_frame(struct awdl_sched *s, const uint8_t *frame, int len);

static void ba_release(void *ctx, const uint8_t *frame, int len) {
	process_frame((struct awdl_sched *) ctx, frame, len);
}

/* Frames of the accepted Block Ack session go through the reorder buffer;
 * returns 1 when the frame was consumed here. */
static int ba_session_rx(struct awdl_sched *s, const uint8_t *frame, int len) {
	if (!s->ba.active || len < 4) return 0;
	unsigned rt = frame[2] | frame[3] << 8;
	if (rt < 8 || len < (int) rt + 20) return 0;
	const uint8_t *hdr = frame + rt;
	if (memcmp(hdr + 4, s->awdl_state.self_address.ether_addr_octet, 6) || memcmp(hdr + 10, &s->ba_peer, 6))
		return 0;
	if (hdr[0] == 0x84) { /* BlockAckReq: control (2) | TID in bits 12-15, then SSC */
		if ((unsigned) (hdr[17] >> 4) != s->ba_tid) return 1;
		s->bar_seen++;
		ba_reorder_bar(&s->ba, (uint16_t) ((hdr[18] | hdr[19] << 8) >> 4), ba_release, s);
		return 1;
	}
	if (hdr[0] == 0xd0 && len >= (int) rt + 30 && hdr[24] == 3 && hdr[25] == 2) { /* DELBA */
		s->delba_seen++;
		ba_reorder_stop(&s->ba, ba_release, s);
		printf("DELBA from %s: Block Ack session ended\n", ether_ntoa(&s->ba_peer));
		fflush(stdout);
		return 1;
	}
	if (hdr[0] != 0x88 || len < (int) rt + 26 || (hdr[24] & 0x0f) != s->ba_tid)
		return 0; /* not QoS data of this session */
	uint16_t seq = (uint16_t) ((hdr[22] | hdr[23] << 8) >> 4);
	ba_reorder_rx(&s->ba, seq, frame, len, clock_time_us(), ba_release, s);
	return 1;
}

int awdl_sched_radio_rx(struct awdl_sched *s, const uint8_t *frame, int len) {
	if (answer_addba(s, frame, len))
		return RX_OK;
	if (ba_session_rx(s, frame, len))
		return RX_OK;
	return process_frame(s, frame, len);
}

static int process_frame(struct awdl_sched *s, const uint8_t *frame, int len) {
	struct buf *data_arr[MAX_NUM_AMPDU];
	struct buf **data = &data_arr[0];
	const struct buf *b = buf_new_const(frame, len);
	int result = awdl_rx(b, &data, data_arr + MAX_NUM_AMPDU, &s->awdl_state);

	/* A failed aggregate may already have produced partial outputs. */
	for (struct buf **d = data_arr; d < data; d++) {
		if (result == RX_OK && buf_len(*d) > 1024) {
			const uint8_t *eth = buf_data(*d);
			if (eth[12] == 0x86 && eth[13] == 0xdd &&
			    !memcmp(eth, s->awdl_state.self_address.ether_addr_octet, 6)) {
				memcpy(&s->receive_peer, eth + 6, 6);
				s->receive_peer_seen = clock_time_us();
			}
		}
		if (result == RX_OK && s->deliver)
			s->deliver(s->deliver_ctx, buf_data(*d), buf_len(*d));
		buf_free(*d);
	}
	if (result < RX_OK) {
		s->awdl_state.stats.rx_unknown++;
	}
	buf_free(b);
	return result;
}

int awdl_sched_host_tx(struct awdl_sched *s, const uint8_t *eth, int len) {
	if (len < ETHER_LENGTH || len > ETHER_MAX_LEN)
		return -1;
	bool multicast = eth[ETHER_DST_OFFSET] & 0x01;
	if (multicast ? circular_buf_full(s->tx_queue_multicast) : s->unicast_count == AWDL_UNICAST_QUEUE)
		return -1;

	struct buf *b = buf_new_owned(len);
	memcpy((uint8_t *) buf_data(b), eth, len);
	uint64_t now = clock_time_us();
	if (multicast) {
		circular_buf_put(s->tx_queue_multicast, b);
		if (s->due_multicast == AWDL_SCHED_IDLE)
			s->due_multicast = now;
	} else {
		s->unicast[s->unicast_count++] = b;
		s->due_unicast = now;
	}
	return 0;
}

int awdl_sched_probe(struct awdl_sched *s, const struct ether_addr *peer) {
    if (!peer || (peer->ether_addr_octet[0] & 1) ||
        !memcmp(peer, &s->awdl_state.self_address, sizeof(*peer))) return -1;
    uint8_t frame[65535];
    struct ether_addr saved = s->awdl_state.dst;
    s->awdl_state.dst = *peer;
    int len = build_action(s, frame, AWDL_ACTION_MIF);
    s->awdl_state.dst = saved;
    if (len < 0) return -1;
    return s->radio->transmit(s->radio_ctx, frame, (unsigned)len) < 0 ? -1 : 0;
}
