/* awdl_netif: the address derivation always runs; the adapter round trip runs
 * only where a TAP-Windows6 adapter exists and the tests run as administrator. */
extern "C" {
#include "awdl_netif.h"
}

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>
#include "gtest/gtest.h"

TEST(netif, rfc4291_link_local_matches_eui64) {
	/* RFC 4291 appendix A style: flip the U/L bit, insert ff:fe. */
	struct ether_addr mac = {{0x00, 0x1b, 0x63, 0x84, 0x45, 0xe6}};
	uint8_t want[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0x1b, 0x63, 0xff, 0xfe, 0x84, 0x45, 0xe6};
	uint8_t got[16];
	awdl_rfc4291_addr(&mac, got);
	EXPECT_EQ(memcmp(got, want, 16), 0);

	struct ether_addr local = {{0x02, 0, 0, 0, 0, 0x0a}}; /* locally administered, as AWDL uses */
	awdl_rfc4291_addr(&local, got);
	EXPECT_EQ(got[8], 0x00);
	EXPECT_EQ(got[15], 0x0a);
}

static uint16_t icmpv6_checksum(const uint8_t *src, const uint8_t *dst, const uint8_t *icmp, size_t len) {
	uint32_t sum = 0;
	auto add = [&](const uint8_t *p, size_t n) {
		for (size_t i = 0; i + 1 < n; i += 2)
			sum += (p[i] << 8) | p[i + 1];
		if (n & 1)
			sum += p[n - 1] << 8;
	};
	uint8_t tail[8] = {0, 0, (uint8_t) (len >> 8), (uint8_t) len, 0, 0, 0, 58};
	add(src, 16);
	add(dst, 16);
	add(tail, 8);
	add(icmp, len);
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t) ~sum;
}

/* Neighbour solicitation from `peer` for our own address, unicast to us. */
static std::vector<uint8_t> neighbour_solicitation(const ether_addr &peer, const ether_addr &self) {
	uint8_t src[16], dst[16];
	awdl_rfc4291_addr(&peer, src);
	awdl_rfc4291_addr(&self, dst);

	std::vector<uint8_t> icmp = {135, 0, 0, 0, 0, 0, 0, 0};
	icmp.insert(icmp.end(), dst, dst + 16);                       /* target */
	icmp.push_back(1);                                              /* source link-layer address option */
	icmp.push_back(1);
	icmp.insert(icmp.end(), peer.ether_addr_octet, peer.ether_addr_octet + 6);
	uint16_t ck = icmpv6_checksum(src, dst, icmp.data(), icmp.size());
	icmp[2] = ck >> 8;
	icmp[3] = ck & 0xff;

	std::vector<uint8_t> f(self.ether_addr_octet, self.ether_addr_octet + 6);
	f.insert(f.end(), peer.ether_addr_octet, peer.ether_addr_octet + 6);
	f.insert(f.end(), {0x86, 0xdd, 0x60, 0, 0, 0, 0, (uint8_t) icmp.size(), 58, 255});
	f.insert(f.end(), src, src + 16);
	f.insert(f.end(), dst, dst + 16);
	f.insert(f.end(), icmp.begin(), icmp.end());
	return f;
}

/* The path AWDL traffic takes: a peer's frame written into awdl0, the Windows
 * stack's answer read back out, addressed to the peer by its permanent
 * neighbour entry. Neighbour discovery passes the firewall on any profile. */
TEST(netif, tap_round_trip_with_windows_stack) {
	struct awdl_netif *nif;
	if (awdl_netif_open(&nif, nullptr) != 0)
		GTEST_SKIP() << "no TAP-Windows6 adapter (see docs/virtual-adapter.md)";
	if (awdl_netif_up(nif) != 0) {
		awdl_netif_close(nif);
		GTEST_SKIP() << "awdl_netif_up failed: run the tests as administrator";
	}

	struct ether_addr self, peer = {{0x02, 0x77, 0x77, 0x77, 0x77, 0x01}};
	awdl_netif_mac(nif, &self);
	ASSERT_EQ(awdl_netif_neighbor_add(nif, &peer), 0);

	auto ns = neighbour_solicitation(peer, self);
	bool answered = false;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
	auto next_send = std::chrono::steady_clock::now();
	uint8_t buf[2048];
	while (!answered && std::chrono::steady_clock::now() < deadline) {
		if (std::chrono::steady_clock::now() >= next_send) { /* the address may still be settling */
			ASSERT_EQ(awdl_netif_write(nif, ns.data(), (int) ns.size()), 0);
			next_send += std::chrono::seconds(1);
		}
		int len = awdl_netif_read(nif, buf, sizeof(buf));
		ASSERT_GE(len, 0);
		if (len == 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			continue;
		}
		/* Neighbour advertisement (136) to the peer's MAC */
		answered = len >= 14 + 40 + 24 && !memcmp(buf, peer.ether_addr_octet, 6) &&
		           buf[12] == 0x86 && buf[13] == 0xdd && buf[14 + 6] == 58 && buf[14 + 40] == 136;
	}
	EXPECT_TRUE(answered) << "Windows never answered the neighbour solicitation through the TAP adapter";
	EXPECT_EQ(awdl_netif_neighbor_remove(nif, &peer), 0);
	awdl_netif_close(nif);
}
