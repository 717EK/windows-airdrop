/* AWDL scheduler: the timing half of OWL's daemon/core.c, without libev,
 * libpcap or a TAP device. The caller owns the loop: call awdl_sched_tick()
 * at (or after) the time it returns, feed captured frames to
 * awdl_sched_radio_rx(), and frames from the virtual NIC to awdl_sched_host_tx().
 * Time comes from clock_time_us(), which awdl_clock_set() can replace. */
#ifndef AWDL_SCHED_H
#define AWDL_SCHED_H

#include <stdint.h>
#include "radio.h"
#include "state.h"
#include "circular_buffer.h"
#include "ba_reorder.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AWDL_SCHED_IDLE UINT64_MAX
#define AWDL_UNICAST_QUEUE 32

/* Ethernet frame from the AWDL link, for the virtual NIC. */
typedef void (*awdl_deliver_cb)(void *ctx, const uint8_t *eth, int len);

struct awdl_sched {
    unsigned service_port; /* 0: no AirDrop service; otherwise append native Bonjour records to MIF. */
	struct awdl_state awdl_state; /* named as in core.c, so its chanseq code ports unchanged */
	struct ieee80211_state ieee80211_state;
	const struct RadioBackend *radio;
	void *radio_ctx;
	awdl_deliver_cb deliver;
	void *deliver_ctx;
	struct buf *unicast[AWDL_UNICAST_QUEUE];
	unsigned unicast_count;
	unsigned addba_answered; /* ADDBA Requests answered, including explicit declines */
	bool rx_aggregation; /* experimental: enable only for traced throughput comparisons */
	struct ba_reorder ba; /* receive reordering for the accepted Block Ack session */
	struct ether_addr ba_peer;
	unsigned ba_tid, bar_seen, delba_seen;
	struct ether_addr receive_peer;
	uint64_t receive_peer_seen; /* last large IPv6 unicast delivered to this node */
	cbuf_handle_t tx_queue_multicast;
	/* Absolute due times in us, AWDL_SCHED_IDLE when not armed. */
	uint64_t due_chan, due_psf, due_mif, due_peers, due_unicast, due_multicast;
};

/* Replace the clock OWL's core and the scheduler read (NULL: back to the monotonic clock). */
void awdl_clock_set(uint64_t (*now_us)(void));

void awdl_sched_init(struct awdl_sched *s, const char *hostname, const struct ether_addr *self,
                     struct awdl_chan chan, const struct RadioBackend *radio, void *radio_ctx,
                     awdl_deliver_cb deliver, void *deliver_ctx);
void awdl_sched_free(struct awdl_sched *s);

/* Run every timer that is due. Returns the absolute time of the next one. */
uint64_t awdl_sched_tick(struct awdl_sched *s);

/* Diagnostic: one directed MIF, soliciting a MAC ACK from a recently heard peer.
 * Caller must rate-limit and ensure the radio is still on that peer's channel. */
int awdl_sched_probe(struct awdl_sched *s, const struct ether_addr *peer);

/* One frame from the radio (radiotap + 802.11). Returns OWL's RX_* result. */
int awdl_sched_radio_rx(struct awdl_sched *s, const uint8_t *frame, int len);

/* One Ethernet frame from the virtual NIC. Returns 0 if queued, -1 if the
 * queue is full (try again after the next tick) or the frame is malformed. */
int awdl_sched_host_tx(struct awdl_sched *s, const uint8_t *eth, int len);

/* Channel matching for a radio held on one channel. A phone on home Wi-Fi often
 * advertises AWDL slots mostly on its infrastructure channel (e.g. 36) and only
 * two on the social channel 149, so a radio fixed on 149 misses most of them. */
#define AWDL_MATCH_CHANNELS 2      /* candidates: 149 (home, social) and 36 */
#define AWDL_MATCH_MARGIN 2        /* move only for at least this many more slots */
#define AWDL_MATCH_STABLE 3        /* ... seen on this many consecutive checks (~1 s apart) */

struct awdl_chan_match {
	unsigned current, home, pending;
	int stable;
	int gave_up; /* a move away stopped helping: stay home for the rest of the session */
};

/* Count only the active bulk sender's slots, never unrelated nearby devices.
 * Returns 1 for a validated sender heard within 5 s, otherwise 0. */
int awdl_sched_count_slots(struct awdl_sched *s, const unsigned *candidates, int n, int *slots);

/* One matching decision. Returns the channel to retune to, or 0 to stay. */
unsigned awdl_chan_match_step(struct awdl_chan_match *m, const unsigned *candidates, const int *slots,
                              int n, int peers, uint64_t now);

/* After the radio has retuned: make chan our channel and re-derive the advertised sequence. */
void awdl_sched_set_channel(struct awdl_sched *s, unsigned chan);

#ifdef __cplusplus
}
#endif

#endif
