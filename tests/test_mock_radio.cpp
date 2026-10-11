/* Two OWL nodes on the mock radio: frames OWL builds on one side go through
 * the RadioBackend interface and OWL's parser on the other. No Wi-Fi hardware. */
extern "C" {
#include "radio.h"
#include "rx.h"
#include "tx.h"
#include "peers.h"
#include "channel.h"
#include "ieee80211.h"
}

#include <set>
#include <vector>
#include "gtest/gtest.h"

static const struct ether_addr ADDR_A = {{0x02, 0, 0, 0, 0, 0x0a}};
static const struct ether_addr ADDR_B = {{0x02, 0, 0, 0, 0, 0x0b}};

struct Node {
	struct awdl_state awdl;
	struct ieee80211_state ieee;
	struct mock_radio radio;

	Node(struct mock_medium *m, int id, const struct ether_addr *addr) : radio{m, id, 6, 0, false} {
		awdl_init_state(&awdl, id == 1 ? "node-a" : "node-b", addr, CHAN_OPCLASS_6, 0);
		awdl.filter_rssi = 0; /* the mock's radiotap carries no signal strength */
		ieee80211_init_state(&ieee);
		mock_radio_backend.open(&radio);
	}
};

static std::vector<uint8_t> build_action(Node &n, enum awdl_action_type type) {
	std::vector<uint8_t> buf(MOCK_FRAME_MAX);
	buf.resize(awdl_init_full_action_frame(buf.data(), &n.awdl, &n.ieee, type));
	return buf;
}

static int receive(Node &n, std::vector<uint8_t> frame) {
	const struct buf *b = buf_new_const(frame.data(), (int) frame.size());
	struct buf **data = NULL;
	int r = awdl_rx(b, &data, &n.awdl);
	buf_free(b);
	return r;
}

TEST(mock_radio, action_frame_discovers_peer) {
	struct mock_medium medium = {};
	Node a(&medium, 1, &ADDR_A), b(&medium, 2, &ADDR_B);

	std::vector<uint8_t> mif = build_action(a, AWDL_ACTION_MIF);
	ASSERT_GT(mock_radio_backend.transmit(&a.radio, mif.data(), mif.size()), 0);

	std::vector<uint8_t> got(MOCK_FRAME_MAX);
	int len = mock_radio_backend.capture(&b.radio, got.data(), got.size());
	ASSERT_EQ(len, (int) mif.size());
	got.resize(len);

	EXPECT_EQ(receive(b, got), RX_OK);
	struct awdl_peer *peer;
	EXPECT_EQ(awdl_peer_get(b.awdl.peers.peers, &ADDR_A, &peer), PEERS_OK);
	EXPECT_EQ(awdl_peers_length(b.awdl.peers.peers), 1);
}

TEST(mock_radio, sender_does_not_hear_itself_and_channels_isolate) {
	struct mock_medium medium = {};
	Node a(&medium, 1, &ADDR_A), b(&medium, 2, &ADDR_B);
	mock_radio_backend.set_channel(&b.radio, 149);

	std::vector<uint8_t> psf = build_action(a, AWDL_ACTION_PSF);
	mock_radio_backend.transmit(&a.radio, psf.data(), psf.size());

	uint8_t buf[MOCK_FRAME_MAX];
	EXPECT_EQ(mock_radio_backend.capture(&a.radio, buf, sizeof(buf)), 0);
	EXPECT_EQ(mock_radio_backend.capture(&b.radio, buf, sizeof(buf)), 0);
}

TEST(mock_radio, own_frame_is_ignored) {
	struct mock_medium medium = {};
	Node a(&medium, 1, &ADDR_A);
	EXPECT_EQ(receive(a, build_action(a, AWDL_ACTION_MIF)), RX_IGNORE_FROM_SELF);
}

/* Offsets where a TLV ends: an action frame cut there is a shorter valid frame,
 * since AWDL frames carry no overall length (and OWL sends no FCS by default). */
static std::set<size_t> tlv_boundaries(const std::vector<uint8_t> &f) {
	std::set<size_t> ends;
	size_t p = (f[2] | f[3] << 8) + sizeof(struct ieee80211_hdr) + sizeof(struct awdl_action);
	ends.insert(p);
	while (p + 3 <= f.size()) {
		p += 3 + (f[p + 1] | f[p + 2] << 8);
		ends.insert(p);
	}
	return ends;
}

/* Section 10: parse safely, reject truncated input. A frame cut anywhere
 * except a TLV boundary must be refused, without crashing. */
TEST(mock_radio, truncated_frames_are_rejected) {
	struct mock_medium medium = {};
	Node a(&medium, 1, &ADDR_A);
	std::vector<uint8_t> mif = build_action(a, AWDL_ACTION_MIF);
	std::set<size_t> ok = tlv_boundaries(mif);
	ASSERT_TRUE(ok.count(mif.size())); /* the walk lands exactly on the end */

	for (size_t n = 0; n < mif.size(); n++) {
		Node fresh(&medium, 2, &ADDR_B);
		std::vector<uint8_t> cut(mif.begin(), mif.begin() + n);
		int r = receive(fresh, cut);
		if (!ok.count(n))
			EXPECT_NE(r, RX_OK) << "prefix length " << n;
	}
}

/* Corrupt one byte at a time: must never crash (run under ASan/valgrind on Linux for more). */
TEST(mock_radio, single_byte_corruption_does_not_crash) {
	struct mock_medium medium = {};
	Node a(&medium, 1, &ADDR_A);
	std::vector<uint8_t> mif = build_action(a, AWDL_ACTION_MIF);

	for (size_t i = 0; i < mif.size(); i++) {
		for (int v : {0x00, 0xff}) {
			Node b(&medium, 2, &ADDR_B);
			std::vector<uint8_t> bad = mif;
			bad[i] = (uint8_t) v;
			receive(b, bad);
		}
	}
	SUCCEED();
}
