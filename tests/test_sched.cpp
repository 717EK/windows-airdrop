/* Scheduler tests on a simulated clock: nodes on the mock radio, stepped from
 * one due time to the next, so seconds of AWDL run in milliseconds. */
extern "C" {
#include "awdl_sched.h"
#include "rx.h"
#include "peers.h"
#include "election.h"
#include "sync.h"
#include "schedule.h"
#include "frame.h"
#include "ieee80211.h"
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
	void TearDown() override { awdl_clock_set(nullptr); }

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
	EXPECT_EQ(awdl_sched_host_tx(&a.s, f.data(), (int) f.size()), 0);
	EXPECT_EQ(awdl_sched_host_tx(&a.s, f.data(), (int) f.size()), -1) << "one unicast in flight, as core.c";
	EXPECT_EQ(awdl_sched_host_tx(&a.s, f.data(), 13), -1) << "shorter than an Ethernet header";
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
