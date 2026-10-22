#include "ba_reorder.h"

#include <stdlib.h>
#include <string.h>

#define SEQ_MOD 4096u
#define SEQ_HALF 2048u

static unsigned seq_diff(uint16_t a, uint16_t b) { return (unsigned) (a - b) & (SEQ_MOD - 1); }

void ba_reorder_start(struct ba_reorder *r, uint16_t ssn, unsigned size) {
	for (unsigned i = 0; i < BA_REORDER_SLOTS; i++) free(r->slot[i].data);
	memset(r, 0, sizeof(*r));
	r->active = 1;
	r->head = ssn & (SEQ_MOD - 1);
	r->size = size && size <= BA_REORDER_SLOTS ? size : BA_REORDER_SLOTS;
}

/* Release the frame at head (if held) and advance head by one. */
static void release_head(struct ba_reorder *r, ba_release_cb cb, void *ctx) {
	unsigned i = r->head % BA_REORDER_SLOTS;
	if (r->slot[i].data) {
		cb(ctx, r->slot[i].data, r->slot[i].len);
		free(r->slot[i].data);
		r->slot[i].data = NULL;
		r->stored--;
		r->reordered++;
	}
	r->head = (r->head + 1) & (SEQ_MOD - 1);
}

static void release_ready(struct ba_reorder *r, ba_release_cb cb, void *ctx) {
	while (r->stored && r->slot[r->head % BA_REORDER_SLOTS].data)
		release_head(r, cb, ctx);
}

/* Move head to new_head, releasing held frames in order and skipping holes. */
static void advance_to(struct ba_reorder *r, uint16_t new_head, ba_release_cb cb, void *ctx) {
	while (r->head != new_head) {
		if (!r->slot[r->head % BA_REORDER_SLOTS].data) r->holes_skipped++;
		release_head(r, cb, ctx);
	}
}

void ba_reorder_stop(struct ba_reorder *r, ba_release_cb cb, void *ctx) {
	if (!r->active) return;
	for (unsigned n = 0; r->stored && n < SEQ_MOD; n++)
		release_head(r, cb, ctx);
	r->active = 0;
}

void ba_reorder_rx(struct ba_reorder *r, uint16_t seq, const uint8_t *frame, int len, uint64_t now,
                   ba_release_cb cb, void *ctx) {
	seq &= SEQ_MOD - 1;
	unsigned d = seq_diff(seq, r->head);
	if (d >= SEQ_HALF) { r->duplicates++; return; } /* before the window: already released */
	if (d >= r->size) { /* beyond the window: slide it so seq is its last slot */
		advance_to(r, (uint16_t) ((seq - r->size + 1) & (SEQ_MOD - 1)), cb, ctx);
		release_ready(r, cb, ctx); /* a held frame may now sit at the new head */
	}
	if (seq == r->head && !r->slot[seq % BA_REORDER_SLOTS].data) {
		cb(ctx, frame, len);
		r->in_order++;
		r->head = (r->head + 1) & (SEQ_MOD - 1);
		release_ready(r, cb, ctx);
		return;
	}
	unsigned i = seq % BA_REORDER_SLOTS;
	if (r->slot[i].data) { r->duplicates++; return; }
	r->slot[i].data = malloc((size_t) len);
	if (!r->slot[i].data) return;
	memcpy(r->slot[i].data, frame, (size_t) len);
	r->slot[i].len = len;
	r->slot[i].t = now;
	r->stored++;
}

void ba_reorder_bar(struct ba_reorder *r, uint16_t ssn, ba_release_cb cb, void *ctx) {
	ssn &= SEQ_MOD - 1;
	if (seq_diff(ssn, r->head) >= SEQ_HALF) return; /* stale request */
	advance_to(r, ssn, cb, ctx);
	release_ready(r, cb, ctx);
}

void ba_reorder_timeout(struct ba_reorder *r, uint64_t now, ba_release_cb cb, void *ctx) {
	if (!r->stored) return;
	/* Any expired held frame gives up the holes before it. Use the furthest
	 * expired one: a late-arriving earlier frame must not hide an older
	 * expired successor, and held frames passed on the way are still
	 * released in order by advance_to. */
	for (unsigned k = r->size - 1; k >= 1; k--) {
		uint16_t seq = (r->head + k) & (SEQ_MOD - 1);
		unsigned i = seq % BA_REORDER_SLOTS;
		if (!r->slot[i].data || now - r->slot[i].t < BA_REORDER_TIMEOUT_US) continue;
		advance_to(r, seq, cb, ctx);
		release_ready(r, cb, ctx);
		return;
	}
}
