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

#ifdef __cplusplus
extern "C" {
#endif

#define AWDL_SCHED_IDLE UINT64_MAX

/* Ethernet frame from the AWDL link, for the virtual NIC. */
typedef void (*awdl_deliver_cb)(void *ctx, const uint8_t *eth, int len);

struct awdl_sched {
	struct awdl_state awdl_state; /* named as in core.c, so its chanseq code ports unchanged */
	struct ieee80211_state ieee80211_state;
	const struct RadioBackend *radio;
	void *radio_ctx;
	awdl_deliver_cb deliver;
	void *deliver_ctx;
	struct buf *next; /* the one pending unicast frame, as in core.c */
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

/* One frame from the radio (radiotap + 802.11). Returns OWL's RX_* result. */
int awdl_sched_radio_rx(struct awdl_sched *s, const uint8_t *frame, int len);

/* One Ethernet frame from the virtual NIC. Returns 0 if queued, -1 if the
 * queue is full (try again after the next tick) or the frame is malformed. */
int awdl_sched_host_tx(struct awdl_sched *s, const uint8_t *eth, int len);

#ifdef __cplusplus
}
#endif

#endif
