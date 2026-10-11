#include <string.h>
#include "radio.h"

static int mock_open(void *ctx) {
	struct mock_radio *r = ctx;
	r->next = r->medium->count; /* hear only what is sent from now on */
	r->is_open = true;
	return 0;
}

static int mock_capture(void *ctx, unsigned char *buf, unsigned int cap) {
	struct mock_radio *r = ctx;
	if (!r->is_open)
		return -1;
	while (r->next < r->medium->count) {
		const struct mock_frame *f = &r->medium->frames[r->next++];
		if (f->sender == r->id || f->channel != r->channel)
			continue;
		if (f->len > cap)
			return -1;
		memcpy(buf, f->data, f->len);
		return (int) f->len;
	}
	return 0;
}

static int mock_transmit(void *ctx, const unsigned char *frame, unsigned int len) {
	struct mock_radio *r = ctx;
	struct mock_medium *m = r->medium;
	/* ponytail: fixed-size medium, a ring buffer if a test ever needs more than 64 frames */
	if (!r->is_open || len > MOCK_FRAME_MAX || m->count == MOCK_MEDIUM_FRAMES)
		return -1;
	struct mock_frame *f = &m->frames[m->count++];
	f->sender = r->id;
	f->channel = r->channel;
	f->len = len;
	memcpy(f->data, frame, len);
	return (int) len;
}

static int mock_set_channel(void *ctx, unsigned int channel) {
	((struct mock_radio *) ctx)->channel = channel;
	return 0;
}

static int mock_capabilities(void *ctx, struct RadioCapabilities *out) {
	(void) ctx;
	/* Simulated, so everything "works". Never copy this into a real backend. */
	*out = (struct RadioCapabilities) {true, true, true, true, true};
	return 0;
}

static void mock_close(void *ctx) {
	((struct mock_radio *) ctx)->is_open = false;
}

const struct RadioBackend mock_radio_backend = {
	mock_open, mock_capture, mock_transmit, mock_set_channel, mock_capabilities, mock_close,
};
