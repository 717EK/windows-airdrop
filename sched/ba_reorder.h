/* Block Ack receive reordering for one session (one peer, one TID), as an
 * 802.11 receiver must do once it accepts ADDBA: MPDUs of an A-MPDU that the
 * sender retransmits after a BlockAck hole arrive after later ones, and
 * handing them up in arrival order makes TCP see loss. Frames are held by
 * sequence number and released in order; a hole is skipped after a timeout or
 * when a BlockAckReq / the window moves past it. Duplicates are dropped. */
#ifndef BA_REORDER_H
#define BA_REORDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BA_REORDER_SLOTS 64
#define BA_REORDER_TIMEOUT_US 100000 /* mac80211's HT_RX_REORDER_BUF_TIMEOUT */

typedef void (*ba_release_cb)(void *ctx, const uint8_t *frame, int len);

struct ba_reorder {
	int active;
	uint16_t head; /* next expected sequence number */
	unsigned size; /* negotiated buffer size, 1..64 */
	unsigned stored;
	struct { uint8_t *data; int len; uint64_t t; } slot[BA_REORDER_SLOTS];
	/* counters */
	unsigned in_order, reordered, duplicates, holes_skipped;
};

void ba_reorder_start(struct ba_reorder *r, uint16_t ssn, unsigned size);
void ba_reorder_stop(struct ba_reorder *r, ba_release_cb cb, void *ctx);
/* Takes a copy of the frame when it has to be held. */
void ba_reorder_rx(struct ba_reorder *r, uint16_t seq, const uint8_t *frame, int len, uint64_t now,
                   ba_release_cb cb, void *ctx);
/* BlockAckReq: everything before ssn is final. */
void ba_reorder_bar(struct ba_reorder *r, uint16_t ssn, ba_release_cb cb, void *ctx);
/* Skip holes whose oldest held successor has waited longer than the timeout. */
void ba_reorder_timeout(struct ba_reorder *r, uint64_t now, ba_release_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
