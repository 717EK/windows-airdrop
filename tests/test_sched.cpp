/* Scheduler tests on a simulated clock: nodes on the mock radio, stepped from
 * one due time to the next, so seconds of AWDL run in milliseconds. */
extern "C" {
#include "awdl_sched.h"
#include "awdl_service.h"
#include "rx.h"
#include "tx.h"
#include "peers.h"
#include "election.h"
#include "sync.h"
#include "schedule.h"
#include "frame.h"
#include "ieee80211.h"
#include <radiotap.h>
}

#include <algorithm>
#include <sstream>
#include <string>
#include <memory>
#include <vector>
#include "gtest/gtest.h"

static uint64_t sim_now;
static uint64_t sim_clock() { return sim_now; }

/* Wraps the mock to record what the scheduler asks of the radio. */
struct Recorder {
	struct mock_radio mock;
	std::vector<unsigned int> channels;
	std::vector<std::vector<uint8_t>> sent;
	/* Set to check the unicast gate at the moment a data frame leaves. */
	struct awdl_sched *gate_self = nullptr;
	struct ether_addr gate_peer;
	struct awdl_sched *peer_sched = nullptr; /* the peer's own view, for diagnosis */
	std::vector<std::string> gate_violations;
	int gated = 0;
};

static int rec_open(void *c) { return mock_radio_backend.open(&((Recorder *) c)->mock); }
static int rec_capture(void *c, unsigned char *b, unsigned int cap) {
	return mock_radio_backend.capture(&((Recorder *) c)->mock, b, cap);
}
static int rec_transmit(void *c, const unsigned char *f, unsigned int len) {
	((Recorder *) c)->sent.emplace_back(f, f + len);
	Recorder *r = (Recorder *) c;
	size_t rt = f[2] | f[3] << 8;
	if (r->gate_self && ((f[rt] | f[rt + 1] << 8) & IEEE80211_FCTL_FTYPE) == IEEE80211_FTYPE_DATA) {
		struct awdl_peer *p;
		r->gated++;
		if (awdl_peer_get(r->gate_self->awdl_state.peers.peers, &r->gate_peer, &p) != PEERS_OK ||
		    awdl_can_send_unicast_in(&r->gate_self->awdl_state, p, sim_now, AWDL_UNICAST_GUARD_TU) != 0) {
			std::ostringstream o;
			o << "t=" << sim_now;
			if (r->peer_sched)
				o << " peer slot " << awdl_sync_current_eaw(sim_now, &r->peer_sched->awdl_state.sync) % 16
				  << " next_aw " << awdl_sync_next_aw_us(sim_now, &r->peer_sched->awdl_state.sync);
			o << " our slot " << awdl_sync_current_eaw(sim_now, &r->gate_self->awdl_state.sync) % 16
			  << " next_aw " << awdl_sync_next_aw_us(sim_now, &r->gate_self->awdl_state.sync);
			r->gate_violations.push_back(o.str());
		}
	}
	return mock_radio_backend.transmit(&((Recorder *) c)->mock, f, len);
}
static int rec_set_channel(void *c, unsigned int ch) {
	((Recorder *) c)->channels.push_back(ch);
	return mock_radio_backend.set_channel(&((Recorder *) c)->mock, ch);
}
static int rec_caps(void *c, struct RadioCapabilities *o) { return mock_radio_backend.capabilities(&((Recorder *) c)->mock, o); }
static void rec_close(void *c) { mock_radio_backend.close(&((Recorder *) c)->mock); }
static const struct RadioBackend recorder_backend = {rec_open, rec_capture, rec_transmit, rec_set_channel, rec_caps, rec_close};

struct Node {
	Recorder radio;
	struct awdl_sched s;
	std::vector<std::vector<uint8_t>> delivered;
	struct ether_addr addr;

	Node(struct mock_medium *m, int id) : radio{{m, id, 0, 0, false}, {}, {}} {
		addr = {{0x02, 0, 0, 0, 0, (uint8_t) id}};
		recorder_backend.open(&radio);
		awdl_sched_init(&s, "node", &addr, CHAN_OPCLASS_6, &recorder_backend, &radio, deliver, this);
		s.awdl_state.filter_rssi = 0; /* the mock carries no signal strength */
	}
	~Node() { awdl_sched_free(&s); }

	static void deliver(void *ctx, const uint8_t *eth, int len) {
		((Node *) ctx)->delivered.emplace_back(eth, eth + len);
	}
	int peers() { return awdl_peers_length(s.awdl_state.peers.peers); }
	bool knows(const Node &o) {
		struct awdl_peer *p;
		return awdl_peer_get(s.awdl_state.peers.peers, &o.addr, &p) == PEERS_OK;
	}
};

struct Sim : ::testing::Test {
	struct mock_medium medium = {};
	std::vector<std::unique_ptr<Node>> nodes;

	void SetUp() override {
		sim_now = 10000000; /* 10 s: keeps "now - timeout" arithmetic away from zero */
		awdl_clock_set(sim_clock);
	}
	void TearDown() override {
		awdl_clock_set(nullptr);
		awdl_data_tx_rate = { .legacy = 12, .mcs = -1, .sgi = 0 };
	}

	Node &add() {
		nodes.emplace_back(new Node(&medium, (int) nodes.size() + 1));
		return *nodes.back();
	}

	/* Tick every node, hand each the frames on its channel, jump to the next due time. */
	void run_for(uint64_t us, std::vector<Node *> active = {}) {
		if (active.empty())
			for (auto &n : nodes)
				active.push_back(n.get());
		uint64_t end = sim_now + us;
		for (int guard = 0; guard < 1000000; guard++) {
			uint64_t next = AWDL_SCHED_IDLE;
			for (Node *n : active)
				next = std::min(next, awdl_sched_tick(&n->s));
			uint8_t buf[MOCK_FRAME_MAX];
			int len;
			for (Node *n : active)
				while ((len = recorder_backend.capture(&n->radio, buf, sizeof(buf))) > 0)
					awdl_sched_radio_rx(&n->s, buf, len);
			if (next > end) {
				sim_now = end;
				return;
			}
			ASSERT_GE(next, sim_now) << "scheduler asked to run in the past";
			sim_now = std::max(next, sim_now + 1);
		}
		FAIL() << "scheduler did not advance";
	}
};

static std::vector<uint8_t> eth_frame(const struct ether_addr &dst, const struct ether_addr &src, const char *text) {
	std::vector<uint8_t> f(dst.ether_addr_octet, dst.ether_addr_octet + 6);
	f.insert(f.end(), src.ether_addr_octet, src.ether_addr_octet + 6);
	f.push_back(0x86); /* IPv6, the only ethertype OWL carries */
	f.push_back(0xdd);
	f.insert(f.end(), text, text + strlen(text));
	return f;
}

static int count_action(const Recorder &r, enum awdl_action_type type) {
	int n = 0;
	for (auto &f : r.sent) {
		size_t rt = f[2] | f[3] << 8;
		const struct ieee80211_hdr *h = (const struct ieee80211_hdr *) &f[rt];
		if ((le16toh(h->frame_control) & IEEE80211_FCTL_FTYPE) != IEEE80211_FTYPE_MGMT)
			continue;
		const struct awdl_action *a = (const struct awdl_action *) &f[rt + sizeof(*h)];
		n += a->subtype == type;
	}
	return n;
}

TEST_F(Sim, two_nodes_discover_each_other_and_agree_on_master) {
	Node &a = add();
	sim_now += 37000; /* B starts 37 ms later, out of phase */
	Node &b = add();
	run_for(3000000);

	EXPECT_TRUE(a.knows(b));
	EXPECT_TRUE(b.knows(a));
	EXPECT_EQ(memcmp(&a.s.awdl_state.election.sync_addr, &b.s.awdl_state.election.sync_addr, 6), 0)
	    << "both nodes should follow the same sync master";
	/* The slave adopted the master's phase: both agree on the current window. */
	EXPECT_EQ(awdl_sync_current_eaw(sim_now, &a.s.awdl_state.sync),
	          awdl_sync_current_eaw(sim_now, &b.s.awdl_state.sync));
}

TEST_F(Sim, psf_and_mif_cadence) {
	Node &a = add();
	run_for(1000000);

	double psf_us = a.s.awdl_state.psf_interval * 1024.0;
	int psf = count_action(a.radio, AWDL_ACTION_PSF);
	EXPECT_NEAR(psf, 1000000 / psf_us, 1.0);
	/* One MIF per extended availability window. */
	double eaw_us = a.s.awdl_state.sync.presence_mode * a.s.awdl_state.sync.aw_period * 1024.0;
	EXPECT_NEAR(count_action(a.radio, AWDL_ACTION_MIF), 1000000 / eaw_us, 2.0);
}

TEST_F(Sim, unicast_reaches_peer_unchanged) {
	Node &a = add(), &b = add();
	run_for(2000000);
	ASSERT_TRUE(a.knows(b));

	auto f = eth_frame(b.addr, a.addr, "hello over awdl");
	ASSERT_EQ(awdl_sched_host_tx(&a.s, f.data(), (int) f.size()), 0);
	run_for(1000000);

	ASSERT_EQ(b.delivered.size(), 1u);
	EXPECT_EQ(b.delivered[0], f);
	EXPECT_EQ(a.s.awdl_state.stats.tx_data_unicast, 1u);
}

/* B is on channel 6 only in odd slots, A always. Frames arrive at every phase
 * of two slots: each must wait for B's next channel-6 slot, outside the guard
 * at its edges, and still arrive (the mock drops frames sent while B is away). */
TEST_F(Sim, unicast_waits_for_peer_window_and_guard) {
	Node &a = add(), &b = add();
	run_for(2000000);
	struct awdl_peer *peer;
	ASSERT_EQ(awdl_peer_get(a.s.awdl_state.peers.peers, &b.addr, &peer), PEERS_OK);

	/* Freeze election and sequence adoption, which would overwrite B's sequence. */
	a.s.due_peers = b.s.due_peers = AWDL_SCHED_IDLE;
	for (int i = 0; i < AWDL_CHANSEQ_LENGTH; i++)
		b.s.awdl_state.channel.sequence[i] = i % 2 ? CHAN_OPCLASS_6 : CHAN_OPCLASS_149;
	run_for(500000); /* let A hear B's new sequence */
	a.radio.gate_self = &a.s;
	a.radio.gate_peer = b.addr;
	a.radio.peer_sched = &b.s;

	const int n = 64; /* 2 TU apart: covers two 64 TU slots, guards included */
	for (int k = 0; k < n; k++) {
		run_for(100000 + 2048 * k);
		auto f = eth_frame(b.addr, a.addr, "phase");
		ASSERT_EQ(awdl_sched_host_tx(&a.s, f.data(), (int) f.size()), 0);
		run_for(200000);
	}
	EXPECT_EQ(b.delivered.size(), (size_t) n);
	EXPECT_EQ(a.radio.gated, n);
	for (auto &v : a.radio.gate_violations)
		ADD_FAILURE() << "data frame sent outside B's window: " << v;
}

TEST_F(Sim, multicast_reaches_peer) {
	Node &a = add(), &b = add();
	run_for(2000000);

	struct ether_addr all_nodes = {{0x33, 0x33, 0, 0, 0, 1}};
	auto f = eth_frame(all_nodes, a.addr, "neighbour solicitation");
	ASSERT_EQ(awdl_sched_host_tx(&a.s, f.data(), (int) f.size()), 0);
	run_for(1000000);

	ASSERT_EQ(b.delivered.size(), 1u);
	EXPECT_EQ(b.delivered[0], f);
}

TEST_F(Sim, unicast_to_stranger_is_dropped_and_to_self_loops_back) {
	Node &a = add();
	struct ether_addr stranger = {{0x02, 9, 9, 9, 9, 9}};
	auto lost = eth_frame(stranger, a.addr, "x");
	ASSERT_EQ(awdl_sched_host_tx(&a.s, lost.data(), (int) lost.size()), 0);
	run_for(100000);
	EXPECT_EQ(a.s.awdl_state.stats.tx_data, 0u);

	auto self = eth_frame(a.addr, a.addr, "me");
	ASSERT_EQ(awdl_sched_host_tx(&a.s, self.data(), (int) self.size()), 0);
	run_for(100000);
	ASSERT_EQ(a.delivered.size(), 1u);
	EXPECT_EQ(a.delivered[0], self);
}

TEST_F(Sim, host_tx_backpressure_and_validation) {
	Node &a = add(), &b = add();
	auto f = eth_frame(b.addr, a.addr, "one");
	for (unsigned i = 0; i < AWDL_UNICAST_QUEUE; i++)
		EXPECT_EQ(awdl_sched_host_tx(&a.s, f.data(), (int) f.size()), 0);
	EXPECT_EQ(awdl_sched_host_tx(&a.s, f.data(), (int) f.size()), -1) << "bounded unicast queue";
	EXPECT_EQ(awdl_sched_host_tx(&a.s, f.data(), 13), -1) << "shorter than an Ethernet header";
}

TEST_F(Sim, faster_unicast_keeps_discovery_and_multicast_at_compatible_rate) {
    Node &a = add(), &b = add();
    run_for(2000000);
    awdl_data_tx_rate = { .legacy = 24, .mcs = -1, .sgi = 0 };
    a.radio.sent.clear();
    auto unicast = eth_frame(b.addr, a.addr, "faster data");
    struct ether_addr group = {{0x33, 0x33, 0, 0, 0, 1}};
    auto multicast = eth_frame(group, a.addr, "discovery");
    ASSERT_EQ(awdl_sched_host_tx(&a.s, unicast.data(), unicast.size()), 0);
    ASSERT_EQ(awdl_sched_host_tx(&a.s, multicast.data(), multicast.size()), 0);
    run_for(1000000);
    unsigned unicast_count = 0, multicast_count = 0, actions = 0;
    for (const auto &frame : a.radio.sent) {
        unsigned rt = frame[2] | frame[3] << 8;
        ASSERT_GE(rt, 9u);
        EXPECT_NE(frame[4] & (1u << IEEE80211_RADIOTAP_RATE), 0);
        bool data = (frame[rt] & IEEE80211_FCTL_FTYPE) == IEEE80211_FTYPE_DATA;
        bool group_address = frame[rt + 4] & 1;
        EXPECT_EQ(frame[8], data && !group_address ? 48 : 24);
        if (!data) actions++;
        else if (group_address) multicast_count++;
        else unicast_count++;
    }
    EXPECT_EQ(unicast_count, 1u);
    EXPECT_EQ(multicast_count, 1u);
    EXPECT_GT(actions, 0u);
    EXPECT_EQ(awdl_data_tx_rate.legacy, 24);
}

TEST_F(Sim, waiting_peer_does_not_block_ready_peer_and_keeps_its_packet_order) {
    Node &a = add(), &b = add(), &c = add();
    run_for(2000000);
    struct awdl_peer *blocked, *ready;
    ASSERT_EQ(awdl_peer_get(a.s.awdl_state.peers.peers, &b.addr, &blocked), PEERS_OK);
    ASSERT_EQ(awdl_peer_get(a.s.awdl_state.peers.peers, &c.addr, &ready), PEERS_OK);
    for (auto &node : nodes) {
        node->s.due_chan = node->s.due_psf = node->s.due_mif = node->s.due_peers = AWDL_SCHED_IDLE;
        recorder_backend.set_channel(&node->radio, 6);
        node->s.awdl_state.channel.current = CHAN_OPCLASS_6;
        for (auto &channel : node->s.awdl_state.channel.sequence) channel = CHAN_OPCLASS_6;
    }
    for (unsigned i = 0; i < AWDL_CHANSEQ_LENGTH; ++i) {
        blocked->sequence[i] = CHAN_OPCLASS_149;
        ready->sequence[i] = CHAN_OPCLASS_6;
    }
    auto first = eth_frame(b.addr, a.addr, "first");
    auto other = eth_frame(c.addr, a.addr, "ready");
    auto second = eth_frame(b.addr, a.addr, "second");
    ASSERT_EQ(awdl_sched_host_tx(&a.s, first.data(), first.size()), 0);
    ASSERT_EQ(awdl_sched_host_tx(&a.s, other.data(), other.size()), 0);
    ASSERT_EQ(awdl_sched_host_tx(&a.s, second.data(), second.size()), 0);
    run_for(10000);
    ASSERT_EQ(c.delivered.size(), 1u);
    EXPECT_EQ(c.delivered[0], other);
    // The mock delivers overheard data too; the real TAP runner filters by destination.
    auto keep_addressed_to_b = [&] {
        b.delivered.erase(std::remove_if(b.delivered.begin(), b.delivered.end(), [&](const auto &frame) {
            return memcmp(frame.data(), b.addr.ether_addr_octet, 6) != 0;
        }), b.delivered.end());
    };
    keep_addressed_to_b();
    EXPECT_TRUE(b.delivered.empty());
    for (auto &channel : blocked->sequence) channel = CHAN_OPCLASS_6;
    // A newly queued packet makes the scheduler reconsider the changed window.
    auto third = eth_frame(b.addr, a.addr, "third");
    ASSERT_EQ(awdl_sched_host_tx(&a.s, third.data(), third.size()), 0);
    run_for(100000);
    keep_addressed_to_b();
    ASSERT_EQ(b.delivered.size(), 3u);
    EXPECT_EQ(b.delivered[0], first);
    EXPECT_EQ(b.delivered[1], second);
    EXPECT_EQ(b.delivered[2], third);
}

TEST_F(Sim, radio_follows_channel_sequence) {
	Node &a = add();
	for (int i = 0; i < AWDL_CHANSEQ_LENGTH; i++)
		a.s.awdl_state.channel.sequence[i] = i % 2 ? CHAN_OPCLASS_149 : CHAN_OPCLASS_6;
	a.radio.channels.clear();

	/* A slot is one extended availability window: presence_mode AWs. */
	double period_us = AWDL_CHANSEQ_LENGTH * a.s.awdl_state.sync.presence_mode *
	                   a.s.awdl_state.sync.aw_period * 1024.0;
	run_for((uint64_t) period_us);

	/* Alternating slots: one switch per slot, always to the other channel. */
	EXPECT_NEAR((double) a.radio.channels.size(), AWDL_CHANSEQ_LENGTH, 1.0);
	for (size_t i = 1; i < a.radio.channels.size(); i++)
		EXPECT_NE(a.radio.channels[i], a.radio.channels[i - 1]);
}

TEST_F(Sim, silent_peer_times_out) {
	Node &a = add(), &b = add();
	run_for(2000000);
	ASSERT_TRUE(a.knows(b));

	uint64_t wait = a.s.awdl_state.peers.timeout + 2 * a.s.awdl_state.peers.clean_interval;
	run_for(wait, {&a}); /* B goes quiet */
	EXPECT_EQ(a.peers(), 0);
}

TEST_F(Sim, directed_probe_is_parseable_and_preserves_group_announcements) {
    auto &a = add(); auto &b = add();
    auto group = a.s.awdl_state.dst;
    EXPECT_EQ(awdl_sched_probe(&a.s, &b.addr), 0);
    ASSERT_EQ(a.radio.sent.size(), 1u);
    const auto &frame = a.radio.sent.back();
    size_t rt = frame[2] | frame[3] << 8;
    ASSERT_GE(frame.size(), rt + 24);
    EXPECT_EQ(memcmp(frame.data() + rt + 4, &b.addr, 6), 0);
    EXPECT_EQ(memcmp(frame.data() + rt + 10, &a.addr, 6), 0);
    EXPECT_EQ(awdl_sched_radio_rx(&b.s, frame.data(), frame.size()), RX_OK);
    EXPECT_TRUE(b.knows(a));
    EXPECT_EQ(memcmp(&group, &a.s.awdl_state.dst, 6), 0);
    EXPECT_LT(awdl_sched_probe(&a.s, &a.addr), 0);
    EXPECT_LT(awdl_sched_probe(&a.s, &group), 0);
    EXPECT_EQ(a.radio.sent.size(), 1u);
}

TEST_F(Sim, addba_declined_by_default_and_accepted_only_in_experimental_mode) {
    auto &a = add(); auto &b = add();
    // Radiotap (8 bytes) + management action header + ADDBA Request body from b to a.
    std::vector<uint8_t> req = {0, 0, 8, 0, 0, 0, 0, 0, 0xd0, 0, 0, 0};
    req.insert(req.end(), (uint8_t *) &a.addr, (uint8_t *) &a.addr + 6);
    req.insert(req.end(), (uint8_t *) &b.addr, (uint8_t *) &b.addr + 6);
    req.insert(req.end(), {0, 0x25, 0, 0xff, 0x94, 0x73, 0x10, 0});
    unsigned params = 1 | 2 | (5 << 2) | (32 << 6); // A-MSDU, immediate, TID 5, buffer 32
    req.insert(req.end(), {3, 0, 0x2a, (uint8_t) params, (uint8_t) (params >> 8), 0, 0, 0x10, 0});
    size_t before = a.radio.sent.size();
    EXPECT_FALSE(a.s.rx_aggregation);
    ASSERT_EQ(awdl_sched_radio_rx(&a.s, req.data(), req.size()), RX_OK);
    ASSERT_EQ(a.radio.sent.size(), before + 1);
    const auto &decline = a.radio.sent.back();
    unsigned drt = decline[2] | decline[3] << 8;
    ASSERT_GE(decline.size(), drt + 24 + 9);
    EXPECT_EQ(decline[drt + 27], 37);
    EXPECT_EQ(decline[drt + 28], 0);
    a.s.rx_aggregation = true;
    EXPECT_EQ(awdl_sched_radio_rx(&a.s, req.data(), req.size()), RX_OK);
    EXPECT_EQ(a.s.addba_answered, 2u);
    ASSERT_EQ(a.radio.sent.size(), before + 2);
    const auto &resp = a.radio.sent.back();
    size_t rt = resp[2] | resp[3] << 8;
    ASSERT_GE(resp.size(), rt + 24 + 9);
    EXPECT_EQ(resp[rt], 0xd0);
    EXPECT_EQ(memcmp(resp.data() + rt + 4, &b.addr, 6), 0);
    EXPECT_EQ(memcmp(resp.data() + rt + 10, &a.addr, 6), 0);
    const uint8_t *body = resp.data() + rt + 24;
    EXPECT_EQ(body[0], 3); EXPECT_EQ(body[1], 1); EXPECT_EQ(body[2], 0x2a);
    EXPECT_EQ(body[3] | body[4] << 8, 0); // status success
    unsigned got = body[5] | body[6] << 8;
    EXPECT_EQ((got >> 2) & 0xf, 5u);
    EXPECT_EQ(got >> 6, 32u);
    EXPECT_TRUE(got & 2);
    // Not addressed to a: ignored, nothing sent.
    req[rt + 4] ^= 1;
    EXPECT_NE(awdl_sched_radio_rx(&a.s, req.data(), req.size()), RX_OK);
    EXPECT_EQ(a.radio.sent.size(), before + 2);
}

TEST_F(Sim, accepted_block_ack_session_reorders_data_and_honours_bar_and_delba) {
    auto &a = add(); auto &b = add();
    a.s.rx_aggregation = true;
    auto hdr = [&](uint8_t fc0, std::vector<uint8_t> tail) {
        std::vector<uint8_t> f = {0, 0, 8, 0, 0, 0, 0, 0, fc0, 0, 0, 0};
        f.insert(f.end(), (uint8_t *) &a.addr, (uint8_t *) &a.addr + 6);
        f.insert(f.end(), (uint8_t *) &b.addr, (uint8_t *) &b.addr + 6);
        f.insert(f.end(), tail.begin(), tail.end());
        return f;
    };
    unsigned params = 2 | (5 << 2) | (32 << 6);
    auto req = hdr(0xd0, {0, 0x25, 0, 0xff, 0x94, 0x73, 0, 0,
                          3, 0, 1, (uint8_t) params, (uint8_t) (params >> 8), 0, 0, 0x10, 0}); // SSN 1
    ASSERT_EQ(awdl_sched_radio_rx(&a.s, req.data(), req.size()), RX_OK);
    ASSERT_TRUE(a.s.ba.active);
    EXPECT_EQ(a.s.ba.head, 1);
    auto data = [&](uint16_t seq, uint8_t tid) {
        uint16_t sc = seq << 4;
        return hdr(0x88, {0, 0x25, 0, 0xff, 0x94, 0x73, (uint8_t) sc, (uint8_t) (sc >> 8), tid, 0, 0xaa, 0xbb});
    };
    auto d3 = data(3, 5), d2 = data(2, 5), other = data(2, 6);
    awdl_sched_radio_rx(&a.s, d3.data(), d3.size());
    awdl_sched_radio_rx(&a.s, other.data(), other.size()); // another TID bypasses the buffer
    EXPECT_EQ(a.s.ba.stored, 1u);                          // 3 waits for 1
    awdl_sched_radio_rx(&a.s, d2.data(), d2.size());
    EXPECT_EQ(a.s.ba.stored, 2u);                          // 2 and 3 wait for 1
    uint16_t ssc = 2 << 4;
    auto bar = hdr(0x84, {0x04, 5 << 4, (uint8_t) ssc, (uint8_t) (ssc >> 8)}); // BAR TID 5, SSN 2
    EXPECT_EQ(awdl_sched_radio_rx(&a.s, bar.data(), bar.size()), RX_OK);
    EXPECT_EQ(a.s.bar_seen, 1u);
    EXPECT_EQ(a.s.ba.stored, 0u);
    EXPECT_EQ(a.s.ba.head, 4);
    EXPECT_EQ(a.s.ba.reordered, 2u);
    auto other_delba = hdr(0xd0, {0, 0x25, 0, 0xff, 0x94, 0x73, 0, 0, 3, 2, 0, 0x60, 1, 0}); // TID 6
    EXPECT_EQ(awdl_sched_radio_rx(&a.s, other_delba.data(), other_delba.size()), RX_OK);
    EXPECT_TRUE(a.s.ba.active);
    EXPECT_EQ(a.s.delba_seen, 0u);
    auto delba = hdr(0xd0, {0, 0x25, 0, 0xff, 0x94, 0x73, 0, 0, 3, 2, 0, 0x50, 1, 0}); // TID 5
    EXPECT_EQ(awdl_sched_radio_rx(&a.s, delba.data(), delba.size()), RX_OK);
    EXPECT_EQ(a.s.delba_seen, 1u);
    EXPECT_FALSE(a.s.ba.active);
}

TEST_F(Sim, native_service_records_are_in_mif_and_survive_owl_parsing) {
    auto &a = add(); auto &b = add();
    strcpy(a.s.awdl_state.name, "001122334455");
    a.s.service_port = 8771;
    EXPECT_EQ(awdl_sched_probe(&a.s, &b.addr), 0);
    const auto &frame = a.radio.sent.back();
    auto flags = std::search(frame.begin(), frame.end(), "flags=136", "flags=136" + 9);
    EXPECT_NE(flags, frame.end());
    EXPECT_EQ(awdl_sched_radio_rx(&b.s, frame.data(), frame.size()), RX_OK);
    EXPECT_TRUE(b.knows(a));
}

TEST_F(Sim, native_service_wire_fields_and_output_capacity) {
    const uint8_t mac[6] = {0, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t data[512]{};
    EXPECT_LT(awdl_service_records(data, 1, mac, "001122334455", 8771), 0);
    EXPECT_EQ(data[0], 0);
    int len = awdl_service_records(data, sizeof data, mac, "001122334455", 8771);
    ASSERT_GT(len, 0);
    unsigned offset = 0;
    const unsigned kinds[] = {12, 33, 16};
    for (unsigned kind : kinds) {
        ASSERT_LT(offset + 10, (unsigned)len);
        EXPECT_EQ(data[offset], 2);
        unsigned size = data[offset+1] | data[offset+2] << 8;
        unsigned name_len = data[offset+3] | data[offset+4] << 8;
        ASSERT_LE(offset + 3 + size, (unsigned)len);
        EXPECT_EQ(data[offset + 4 + name_len], kind);
        if (kind == 33) {
            unsigned payload = offset + 9 + name_len;
            EXPECT_EQ(data[payload + 4], 8771 >> 8);
            EXPECT_EQ(data[payload + 5], 8771 & 255);
        }
        offset += size + 3;
    }
    EXPECT_EQ(offset, (unsigned)len);
    EXPECT_LT(awdl_service_records(data, sizeof data, mac, "bad.host", 8771), 0);
}

TEST_F(Sim, service_filter_encodes_sparse_bloom_and_capacity) {
    uint8_t data[14]{};
    EXPECT_LT(awdl_service_filter(data, 13), 0);
    EXPECT_EQ(data[0], 0);
    ASSERT_EQ(awdl_service_filter(data, sizeof data), 14);
    EXPECT_EQ(data[0], AWDL_SERVICE_PARAMETERS_TLV);
    EXPECT_EQ(data[1], 11);
    unsigned mask = data[8] | data[9] << 8 | data[10] << 16 | data[11] << 24;
    EXPECT_EQ(mask, (1u << 13) | (1u << 19));
    EXPECT_EQ(data[12], 4);
    EXPECT_EQ(data[13], 64);
}


TEST_F(Sim, mif_accepts_zero_padding_but_rejects_nonzero_tail) {
    auto &a = add(); auto &b = add();
    for (int tail : {0, 1, 2}) {
        uint8_t frame[2048];
        int n = awdl_init_full_action_frame(frame, &a.s.awdl_state, &a.s.ieee80211_state, AWDL_ACTION_MIF);
        memset(frame + n, 0, tail);
        EXPECT_EQ(awdl_sched_radio_rx(&b.s, frame, n + tail), RX_OK);
    }
    uint8_t frame[2048];
    int n = awdl_init_full_action_frame(frame, &a.s.awdl_state, &a.s.ieee80211_state, AWDL_ACTION_MIF);
    frame[n] = 1;
    EXPECT_EQ(awdl_sched_radio_rx(&b.s, frame, n + 1), RX_UNEXPECTED_FORMAT);
    frame[n] = 0; frame[n+1] = 1;
    EXPECT_EQ(awdl_sched_radio_rx(&b.s, frame, n + 2), RX_UNEXPECTED_FORMAT);
}

TEST_F(Sim, data_keeps_peer_alive_after_action_frames_stop) {
    auto &a = add(); auto &b = add();
    run_for(2000000);
    ASSERT_TRUE(a.knows(b));
    EXPECT_EQ(a.s.awdl_state.peers.timeout, 5000000u);
    uint8_t frame[2048];
    const uint8_t payload[] = {0x60, 0, 0, 0};
    int n = awdl_init_full_data_frame(frame, &b.addr, &a.addr, payload, sizeof payload,
                                      &b.s.awdl_state, &b.s.ieee80211_state);
    for (int i = 0; i < 4; ++i) {
        sim_now += 3000000;
        ASSERT_EQ(awdl_sched_radio_rx(&a.s, frame, n), RX_OK);
        awdl_peers_remove(a.s.awdl_state.peers.peers, sim_now - a.s.awdl_state.peers.timeout, nullptr, nullptr);
        ASSERT_TRUE(a.knows(b));
    }
    sim_now += a.s.awdl_state.peers.timeout + 1;
    awdl_peers_remove(a.s.awdl_state.peers.peers, sim_now - a.s.awdl_state.peers.timeout, nullptr, nullptr);
    EXPECT_FALSE(a.knows(b));
}

TEST_F(Sim, aggregate_cannot_overrun_output_array) {
    auto &a = add(); auto &b = add();
    run_for(2000000);
    uint8_t full[2048];
    const uint8_t payload[] = {0x60, 0, 0, 0};
    int n = awdl_init_full_data_frame(full, &b.addr, &a.addr, payload, sizeof payload,
                                      &b.s.awdl_state, &b.s.ieee80211_state);
    unsigned rt = full[2] | full[3] << 8;
    unsigned start = rt + sizeof(struct ieee80211_hdr);
    unsigned len = n - start;
    std::vector<uint8_t> aggregate;
    for (int i = 0; i < 17; ++i) {
        aggregate.insert(aggregate.end(), a.addr.ether_addr_octet, a.addr.ether_addr_octet + 6);
        aggregate.insert(aggregate.end(), b.addr.ether_addr_octet, b.addr.ether_addr_octet + 6);
        aggregate.push_back(len >> 8); aggregate.push_back(len & 255);
        aggregate.insert(aggregate.end(), full + start, full + n);
        if (i != 16) aggregate.insert(aggregate.end(), (4 - ((14 + len) % 4)) % 4, 0);
    }
    struct buf *outputs[17] = {};
    auto sentinel = reinterpret_cast<struct buf *>(uintptr_t(1));
    outputs[16] = sentinel;
    struct buf **next = outputs;
    auto input = buf_new_const(aggregate.data(), aggregate.size());
    EXPECT_EQ(awdl_rx_data_amsdu(input, &next, outputs + 16, &b.addr, &a.addr, &a.s.awdl_state), RX_TOO_MANY_FRAMES);
    EXPECT_EQ(next, outputs + 16);
    EXPECT_EQ(outputs[16], sentinel);
    for (auto p = outputs; p < next; ++p) buf_free(*p);
    buf_free(input);
    // The scheduler must discard the entire overflowing aggregate, including partial outputs.
    std::vector<uint8_t> wire(full, full + start);
    wire[rt] |= IEEE80211_STYPE_QOS_DATA;
    wire.push_back(IEEE80211_QOS_CTL_A_MSDU_PRESENT); wire.push_back(0);
    wire.insert(wire.end(), aggregate.begin(), aggregate.end());
    EXPECT_EQ(awdl_sched_radio_rx(&a.s, wire.data(), wire.size()), RX_TOO_MANY_FRAMES);
    EXPECT_TRUE(a.delivered.empty());
}

TEST_F(Sim, oversized_data_payload_is_rejected_without_output) {
    auto &a = add(); auto &b = add();
    run_for(2000000);
    std::vector<uint8_t> payload(ETHER_MAX_LEN, 0x60), frame(4096);
    int n = awdl_init_full_data_frame(frame.data(), &b.addr, &a.addr, payload.data(), payload.size(),
                                      &b.s.awdl_state, &b.s.ieee80211_state);
    EXPECT_EQ(awdl_sched_radio_rx(&a.s, frame.data(), n), RX_UNEXPECTED_FORMAT);
    EXPECT_TRUE(a.delivered.empty());
}

static const unsigned match_candidates[AWDL_MATCH_CHANNELS] = {149, 36};

TEST(channel_match, moves_only_after_a_stable_clear_advantage) {
    struct awdl_chan_match m = {149, 149, 0, 0, 0};
    int phone_on_wifi[] = {2, 6};   /* the large-send session: 149 x2, 36 x6 */
    int close_call[] = {3, 4};      /* one slot better: below the margin */
    EXPECT_EQ(awdl_chan_match_step(&m, match_candidates, close_call, 2, 1, 1), 0u);
    EXPECT_EQ(awdl_chan_match_step(&m, match_candidates, phone_on_wifi, 2, 1, 2), 0u);
    EXPECT_EQ(awdl_chan_match_step(&m, match_candidates, phone_on_wifi, 2, 1, 3), 0u);
    EXPECT_EQ(awdl_chan_match_step(&m, match_candidates, phone_on_wifi, 2, 1, 4), 36u);
}

TEST(channel_match, a_flicker_resets_the_count) {
    struct awdl_chan_match m = {149, 149, 0, 0, 0};
    int favour_36[] = {2, 6}, even[] = {4, 4};
    awdl_chan_match_step(&m, match_candidates, favour_36, 2, 1, 1);
    awdl_chan_match_step(&m, match_candidates, favour_36, 2, 1, 2);
    EXPECT_EQ(awdl_chan_match_step(&m, match_candidates, even, 2, 1, 3), 0u);
    EXPECT_EQ(awdl_chan_match_step(&m, match_candidates, favour_36, 2, 1, 4), 0u);
}

TEST(channel_match, stays_while_the_phone_widens_our_channel) {
    struct awdl_chan_match m = {36, 149, 0, 0, 0};
    int widened_36[] = {2, 12};
    for (int i = 0; i < 5; i++)
        EXPECT_EQ(awdl_chan_match_step(&m, match_candidates, widened_36, 2, 1, i), 0u);
    EXPECT_FALSE(m.gave_up);
}

TEST(channel_match, reverts_when_away_stops_helping_and_then_gives_up) {
    struct awdl_chan_match m = {36, 149, 0, 0, 0};
    int dropped[] = {2, 1};           /* live 0.2.3 test: the phone cut 36 to one slot after we moved */
    EXPECT_EQ(awdl_chan_match_step(&m, match_candidates, dropped, 2, 1, 1), 0u);
    EXPECT_EQ(awdl_chan_match_step(&m, match_candidates, dropped, 2, 1, 2), 0u);
    EXPECT_EQ(awdl_chan_match_step(&m, match_candidates, dropped, 2, 1, 3), 149u);
    EXPECT_TRUE(m.gave_up);
    m.current = 149;
    int favour_36[] = {2, 6};
    for (int i = 0; i < 10; i++)      /* no second try this session, however tempting */
        EXPECT_EQ(awdl_chan_match_step(&m, match_candidates, favour_36, 2, 1, 10 + i), 0u);
}

TEST(channel_match, losing_the_phone_while_away_reverts_within_three_checks) {
    struct awdl_chan_match m = {36, 149, 0, 0, 0};
    int none[] = {0, 0}, back[] = {2, 6};
    EXPECT_EQ(awdl_chan_match_step(&m, match_candidates, none, 2, 0, 1), 0u);
    EXPECT_EQ(awdl_chan_match_step(&m, match_candidates, back, 2, 1, 2), 0u); /* a flap back resets */
    EXPECT_EQ(awdl_chan_match_step(&m, match_candidates, none, 2, 0, 3), 0u);
    EXPECT_EQ(awdl_chan_match_step(&m, match_candidates, none, 2, 0, 4), 0u);
    EXPECT_EQ(awdl_chan_match_step(&m, match_candidates, none, 2, 0, 5), 149u);
    EXPECT_TRUE(m.gave_up);
}

TEST(channel_match, nothing_happens_at_home_without_peers) {
    struct awdl_chan_match m = {149, 149, 0, 0, 0};
    int none[] = {0, 0};
    for (int i = 0; i < 5; i++)
        EXPECT_EQ(awdl_chan_match_step(&m, match_candidates, none, 2, 0, i), 0u);
}

TEST_F(Sim, bulk_unicast_selects_sender_but_metadata_and_multicast_do_not) {
    auto &a = add(); auto &b = add(); auto &c = add();
    run_for(2000000);
    auto small = eth_frame(a.addr, c.addr, "discovery metadata");
    ASSERT_EQ(awdl_sched_host_tx(&c.s, small.data(), small.size()), 0);
    run_for(1000000);
    EXPECT_EQ(a.s.receive_peer_seen, 0u);
    std::string payload(1200, 'x');
    auto bulk = eth_frame(a.addr, b.addr, payload.c_str());
    ASSERT_EQ(awdl_sched_host_tx(&b.s, bulk.data(), bulk.size()), 0);
    run_for(1000000);
    ASSERT_GT(a.s.receive_peer_seen, 0u);
    EXPECT_EQ(memcmp(&a.s.receive_peer, &b.addr, sizeof b.addr), 0);
    struct ether_addr group = {{0x33, 0x33, 0, 0, 0, 1}};
    auto multicast = eth_frame(group, c.addr, payload.c_str());
    ASSERT_EQ(awdl_sched_host_tx(&c.s, multicast.data(), multicast.size()), 0);
    ASSERT_EQ(awdl_sched_host_tx(&c.s, small.data(), small.size()), 0);
    run_for(1000000);
    EXPECT_EQ(memcmp(&a.s.receive_peer, &b.addr, sizeof b.addr), 0);
}

TEST_F(Sim, slot_counts_and_new_channel_follow_only_active_sender) {
    auto &a = add();
    struct ether_addr phone = {{0x1e, 0x07, 0x09, 0xdc, 0x4a, 0x8e}};
    struct awdl_peer *p;
    awdl_peer_add(a.s.awdl_state.peers.peers, &phone, sim_now, nullptr, nullptr);
    ASSERT_EQ(awdl_peer_get(a.s.awdl_state.peers.peers, &phone, &p), PEERS_OK);
    const unsigned seq[AWDL_CHANSEQ_LENGTH] = {36, 36, 149, 0, 0, 0, 0, 36, 6, 36, 149, 36, 0, 0, 0, 36};
    for (int i = 0; i < AWDL_CHANSEQ_LENGTH; i++)
        p->sequence[i] = (struct awdl_chan) {{{(uint8_t) seq[i], seq[i] > 14 ? (uint8_t) 0x80 : (uint8_t) 0x51}}};
    p->sequence_enc = AWDL_CHAN_ENC_OPCLASS;
    int slots[AWDL_MATCH_CHANNELS];
    p->is_valid = 0; p->has_sequence = 1;
    EXPECT_EQ(awdl_sched_count_slots(&a.s, match_candidates, 2, slots), 0); /* unvalidated peers do not count */
    p->is_valid = 1;
    EXPECT_EQ(awdl_sched_count_slots(&a.s, match_candidates, 2, slots), 0); /* nearby is not an active sender */
    a.s.receive_peer = phone;
    a.s.receive_peer_seen = sim_now;
    EXPECT_EQ(awdl_sched_count_slots(&a.s, match_candidates, 2, slots), 1);
    EXPECT_EQ(slots[0], 2);
    EXPECT_EQ(slots[1], 6);
    struct ether_addr mac = {{2, 1, 2, 3, 4, 5}};
    struct awdl_peer *other;
    awdl_peer_add(a.s.awdl_state.peers.peers, &mac, sim_now, nullptr, nullptr);
    ASSERT_EQ(awdl_peer_get(a.s.awdl_state.peers.peers, &mac, &other), PEERS_OK);
    other->is_valid = other->has_sequence = 1;
    other->sequence_enc = AWDL_CHAN_ENC_OPCLASS;
    struct awdl_chan other_channel = {{{149, 0x80}}};
    awdl_chanseq_init_static(other->sequence, &other_channel);
    EXPECT_EQ(awdl_sched_count_slots(&a.s, match_candidates, 2, slots), 1);
    EXPECT_EQ(slots[0], 2); EXPECT_EQ(slots[1], 6);
    sim_now += 5000001;
    EXPECT_EQ(awdl_sched_count_slots(&a.s, match_candidates, 2, slots), 0);
    EXPECT_EQ(slots[0], 0); EXPECT_EQ(slots[1], 0);
    awdl_sched_set_channel(&a.s, 36);
    EXPECT_EQ(awdl_chan_num(a.s.awdl_state.channel.master, AWDL_CHAN_ENC_OPCLASS), 36);
    EXPECT_EQ(awdl_chan_num(a.s.awdl_state.channel.current, a.s.awdl_state.channel.enc), 36);
}

namespace {
struct Released {
    std::vector<int> seqs;
    static void cb(void *ctx, const uint8_t *frame, int len) {
        ASSERT_EQ(len, 2);
        static_cast<Released *>(ctx)->seqs.push_back(frame[0] | frame[1] << 8);
    }
};
void feed(ba_reorder &r, Released &out, uint16_t seq, uint64_t now = 0) {
    uint8_t f[2] = {(uint8_t) seq, (uint8_t) (seq >> 8)};
    ba_reorder_rx(&r, seq, f, 2, now, Released::cb, &out);
}
} // namespace

TEST(BaReorder, releases_in_sequence_order_and_drops_duplicates) {
    ba_reorder r{}; Released out;
    ba_reorder_start(&r, 0, 64);
    feed(r, out, 0); feed(r, out, 2); feed(r, out, 3); feed(r, out, 1); feed(r, out, 2); feed(r, out, 0);
    EXPECT_EQ(out.seqs, (std::vector<int>{0, 1, 2, 3}));
    EXPECT_EQ(r.duplicates, 2u);
    EXPECT_EQ(r.stored, 0u);
    ba_reorder_stop(&r, Released::cb, &out);
}

TEST(BaReorder, timeout_and_block_ack_request_skip_holes) {
    ba_reorder r{}; Released out;
    ba_reorder_start(&r, 0, 64);
    feed(r, out, 2, 1000);
    ba_reorder_timeout(&r, 1000 + BA_REORDER_TIMEOUT_US - 1, Released::cb, &out);
    EXPECT_TRUE(out.seqs.empty());
    ba_reorder_timeout(&r, 1000 + BA_REORDER_TIMEOUT_US, Released::cb, &out);
    EXPECT_EQ(out.seqs, (std::vector<int>{2}));
    EXPECT_EQ(r.holes_skipped, 2u);
    feed(r, out, 5); feed(r, out, 6);
    ba_reorder_bar(&r, 5, Released::cb, &out);
    EXPECT_EQ(out.seqs, (std::vector<int>{2, 5, 6}));
    ba_reorder_bar(&r, 1, Released::cb, &out); // stale: ignored
    EXPECT_EQ(r.head, 7);
    ba_reorder_stop(&r, Released::cb, &out);
}

TEST(BaReorder, window_slides_and_wraps_sequence_space) {
    ba_reorder r{}; Released out;
    ba_reorder_start(&r, 4094, 4);
    feed(r, out, 4095); feed(r, out, 4094); feed(r, out, 0);
    EXPECT_EQ(out.seqs, (std::vector<int>{4094, 4095, 0}));
    feed(r, out, 3);                 // holds 3, head stays 1
    feed(r, out, 9);                 // beyond window 1..4: slides head to 6, releasing 3
    EXPECT_EQ(out.seqs, (std::vector<int>{4094, 4095, 0, 3}));
    EXPECT_EQ(r.head, 6);
    ba_reorder_stop(&r, Released::cb, &out); // flushes 9
    EXPECT_EQ(out.seqs.back(), 9);
    EXPECT_FALSE(r.active);
}
