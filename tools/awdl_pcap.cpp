/* Replay a capture (pcap, DLT 105: 802.11 without radiotap, as awdlcap writes)
 * through OWL's AWDL parser and report what OWL learns about each peer.
 *
 *   awdl_pcap <capture.pcap>...
 */
extern "C" {
#include "election.h"
#include "peers.h"
#include "rx.h"
#include "state.h"
}

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static const struct ether_addr SELF = {{0x02, 0x00, 0x00, 0x00, 0x00, 0x99}};

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: awdl_pcap <capture.pcap>...\n");
		return 2;
	}
	struct awdl_state st;
	awdl_init_state(&st, "replay", &SELF, CHAN_OPCLASS_6, clock_time_us());
	st.filter_rssi = 0; /* no radiotap signal field in the capture */

	std::map<int, int> results;
	int awdl = 0;
	for (int a = 1; a < argc; a++) {
		FILE *f = fopen(argv[a], "rb");
		uint32_t gh[6];
		if (!f || fread(gh, 4, 6, f) != 6 || gh[0] != 0xa1b2c3d4 || gh[5] != 105) {
			fprintf(stderr, "%s: not a DLT 105 pcap\n", argv[a]);
			return 1;
		}
		uint32_t rh[4];
		while (fread(rh, 4, 4, f) == 4) {
			/* minimal radiotap header: version 0, length 8, no fields (needs patches/win-02) */
			std::vector<uint8_t> frame(8 + rh[2]);
			frame[2] = 8;
			if (fread(frame.data() + 8, 1, rh[2], f) != rh[2])
				break;
			const uint8_t *d = frame.data() + 8;
			if (rh[2] < 29 || d[0] != 0xd0 || d[24] != 127 || memcmp(d + 25, "\x00\x17\xf2", 3) || d[28] != 8)
				continue;
			awdl++;
			const struct buf *b = buf_new_const(frame.data(), (int) frame.size());
			struct buf **data = nullptr;
			results[awdl_rx(b, &data, &st)]++;
			buf_free(b);
		}
		fclose(f);
	}

	printf("%d AWDL action frames replayed; OWL results:", awdl);
	for (auto &[r, n] : results)
		printf(" %s=%d", r == RX_OK ? "OK" : std::to_string(r).c_str(), n);
	printf("\npeers OWL tracked: %d\n", awdl_peers_length(st.peers.peers));

	struct awdl_peer *p;
	awdl_peers_it_t it = awdl_peers_it_new(st.peers.peers);
	while (awdl_peers_it_next(it, &p) == PEERS_OK) {
		char s[512];
		awdl_peer_print(p, s, sizeof s);
		printf("  %s\n", s);
	}
	awdl_peers_it_free(it);

	awdl_election_run(&st.election, &st.peers);
	char e[256];
	awdl_election_tree_print(&st.election, e, sizeof e);
	printf("election: %s\n", e);
	return 0;
}
