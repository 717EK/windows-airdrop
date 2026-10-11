/* Radio backend interface (dev plan section 4). A real backend sets a
 * capability only after a physical test has shown it, not because a driver
 * header exposes it. Frames are radiotap + 802.11, as OWL builds and parses them. */
#ifndef RADIO_H
#define RADIO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct RadioCapabilities {
	bool monitor_capture;
	bool raw_80211_injection;
	bool channel_control;
	bool concurrent_station_monitor;
	bool reliable_unicast_ack;
};

struct RadioBackend {
	int (*open)(void *ctx);
	/* Returns the frame length, 0 when nothing is waiting, or -1 if it doesn't fit in cap. */
	int (*capture)(void *ctx, unsigned char *buf, unsigned int cap);
	int (*transmit)(void *ctx, const unsigned char *frame, unsigned int len);
	int (*set_channel)(void *ctx, unsigned int channel);
	int (*capabilities)(void *ctx, struct RadioCapabilities *out);
	void (*close)(void *ctx);
};

/* Mock: every endpoint on one shared in-memory medium hears every frame the
 * others transmit on the same channel. No timing, loss or ACKs. The medium is
 * a ring: an endpoint that falls more than MOCK_MEDIUM_FRAMES behind loses the oldest. */
#define MOCK_MEDIUM_FRAMES 64
#define MOCK_FRAME_MAX 2048

struct mock_frame {
	int sender;
	unsigned int channel;
	unsigned int len;
	unsigned char data[MOCK_FRAME_MAX];
};

struct mock_medium {
	struct mock_frame frames[MOCK_MEDIUM_FRAMES];
	unsigned int count; /* frames ever sent; frame i lives at frames[i % MOCK_MEDIUM_FRAMES] */
};

struct mock_radio {
	struct mock_medium *medium;
	int id;
	unsigned int channel;
	unsigned int next; /* index of the next frame on the medium to look at */
	bool is_open;
};

extern const struct RadioBackend mock_radio_backend;

#ifdef __cplusplus
}
#endif

#endif
