/* Bounded native AWDL link runner, optionally connected to a Windows TAP. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rx.h"
#include "awdl_sched.h"
#include "pipe_radio.h"
#include "pipe_protocol.h"
#include "peers.h"
#include "tx.h"
#include "awdl_netif.h"

struct network_context {
    struct awdl_netif *nif;
    uint8_t mac[6];
    unsigned delivered, queued, dropped, write_errors, neighbor_errors;
    int failed;
};

/* Only IPv6 addressed to us or to IPv6 multicast reaches Windows; OWL sends IPv6 only too. */
static int wanted(const struct network_context *net, const uint8_t *frame, int len) {
    if (len < 14 || frame[12] != 0x86 || frame[13] != 0xdd) return 0;
    return !memcmp(frame, net->mac, 6) || (frame[0] == 0x33 && frame[1] == 0x33);
}

static void deliver(void *ctx, const uint8_t *frame, int len) {
    struct network_context *net = ctx;
    if (!wanted(net, frame, len)) net->dropped++;
    /* One rejected frame (e.g. oversized) must not end the session. */
    else if (awdl_netif_write(net->nif, frame, len) < 0) net->write_errors++;
    else net->delivered++;
}

/* Peer names come from the air: keep them on one printable line. */
static const char *printable(const char *name, char *out, size_t cap) {
    size_t i = 0;
    for (; name[i] && i + 1 < cap; i++) out[i] = (unsigned char)name[i] < 0x20 || name[i] == 0x7f ? '?' : name[i];
    out[i] = 0;
    return out;
}

static void peer_added(struct awdl_peer *peer, void *ctx) {
    struct network_context *net = ctx;
    char name[sizeof peer->name];
    printf("peer added %s name=%s device_class=%u version=0x%02x valid=%u\n",
        ether_ntoa(&peer->addr), printable(peer->name, name, sizeof name), peer->devclass, peer->version, peer->is_valid);
    printf("peer channels:");
    for (int i = 0; i < AWDL_CHANSEQ_LENGTH; ++i)
        printf(" %u", awdl_chan_num(peer->sequence[i], peer->sequence_enc));
    printf("\n");
    fflush(stdout);
    if (!net->nif) return;
    if (awdl_netif_neighbor_add(net->nif, &peer->addr) < 0) { net->neighbor_errors++; return; }
    uint8_t ip[16];
    awdl_rfc4291_addr(&peer->addr, ip);
    printf("peer IPv6 ");
    for (int i = 0; i < 16; i += 2) printf("%s%x", i ? ":" : "", ip[i] * 256 + ip[i+1]);
    printf("%%%lu\n", awdl_netif_ifindex(net->nif));
    fflush(stdout);
}

static void peer_removed(struct awdl_peer *peer, void *ctx) {
    struct network_context *net = ctx;
    char name[sizeof peer->name];
    printf("peer removed %s name=%s device_class=%u\n", ether_ntoa(&peer->addr), printable(peer->name, name, sizeof name), peer->devclass);
    fflush(stdout);
    if (net->nif && awdl_netif_neighbor_remove(net->nif, &peer->addr) < 0) net->neighbor_errors++;
}


int main(int argc, char **argv) {
    int probe = 0, listen = 0, fixed = 0, match = 0, rx_aggregation = 0;
    unsigned service_port = 0;
    unsigned data_rate = 12;
    const char *stop_file = NULL;
    const char *tap_name = NULL;
    if (argc < 4) goto usage;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--probe")) probe = 1;
        else if (!strcmp(argv[i], "--listen")) listen = 1;
        else if (!strcmp(argv[i], "--fixed")) fixed = 1;
        else if (!strcmp(argv[i], "--match-channel")) match = 1;
        else if (!strcmp(argv[i], "--rx-aggregation")) rx_aggregation = 1;
        else if (!strcmp(argv[i], "--data-rate") && i + 1 < argc) {
            const char *rate = argv[++i];
            if (!strcmp(rate, "12")) data_rate = 12;
            else if (!strcmp(rate, "24")) data_rate = 24;
            else goto usage;
        }
        else if (!strcmp(argv[i], "--stop-file") && i + 1 < argc) stop_file = argv[++i];
        else if (!strcmp(argv[i], "--tap") && i + 1 < argc) tap_name = argv[++i];
        else if (!strcmp(argv[i], "--service-port") && i + 1 < argc) {
            char *tail; unsigned long port = strtoul(argv[++i], &tail, 10);
            if (*tail || !port || port > 65535) goto usage;
            service_port = port;
        } else goto usage;
    }
    if (probe && listen) goto usage;
    if (rx_aggregation && listen) goto usage;
    if (match && !fixed) goto usage; /* matching moves the one fixed channel */
    awdl_data_tx_rate = (struct ieee80211_tx_rate){ .legacy = data_rate, .mcs = -1, .sgi = 0 };
    char *end;
    unsigned channel = strtoul(argv[2], &end, 10);
    if (*end || !radio_channel_valid(channel)) return 2;
    unsigned seconds = strtoul(argv[3], &end, 10);
    if (*end || seconds < 1 || seconds > 3600) return 2;
    struct pipe_radio r = {0};
    r.name = argv[1]; r.channel = channel; r.fixed_channel = fixed;
    struct network_context net = {0};
    if (tap_name) {
        if (awdl_netif_open(&net.nif, tap_name) < 0 || awdl_netif_up(net.nif) < 0) {
            fprintf(stderr, "TAP setup failed: install TAP-Windows6 and run as administrator (docs/build.md)\n");
            awdl_netif_close(net.nif);
            return 1;
        }
        struct ether_addr mac;
        awdl_netif_mac(net.nif, &mac);
        memcpy(r.mac, &mac, 6);
        memcpy(net.mac, &mac, 6);
    } else {
    /* Random local unicast address per run, shared with the hardware ACK identity. */
    HCRYPTPROV provider;
    if (!CryptAcquireContextA(&provider, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return 1;
    BOOL random_ok = CryptGenRandom(provider, 6, r.mac);
    CryptReleaseContext(provider, 0);
    if (!random_ok) return 1;
    r.mac[0] = (r.mac[0] | 2) & 0xfe;
    }
    if (pipe_radio_backend.open(&r) < 0) { fprintf(stderr, "radio bridge open/handshake failed\n"); awdl_netif_close(net.nif); return 1; }
    struct ether_addr self;
    memcpy(&self, r.mac, 6);
    struct awdl_chan chan = {{{(uint8_t)channel, channel > 14 ? 0x80 : 0x51}}};
    char hostname[32] = "Windows-AWDL";
    if (service_port) snprintf(hostname, sizeof hostname, "%02x%02x%02x%02x%02x%02x", r.mac[0], r.mac[1], r.mac[2], r.mac[3], r.mac[4], r.mac[5]);
    struct awdl_sched s;
    awdl_sched_init(&s, hostname, &self, chan, &pipe_radio_backend, &r, tap_name ? deliver : NULL, &net);
    s.service_port = service_port;
    /* The installed 0.2.5 desktop cannot pass --rx-aggregation; a marker file
     * beside this executable turns the experiment on for that build. */
    {
        char marker[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, marker, sizeof marker);
        char *slash = n && n < sizeof marker ? strrchr(marker, '\\') : NULL;
        if (slash && (size_t) (slash - marker) + sizeof "\\rx-aggregation.enable" <= sizeof marker) {
            strcpy(slash, "\\rx-aggregation.enable");
            if (GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES && !listen) rx_aggregation = 1;
        }
    }
    s.rx_aggregation = rx_aggregation;
    s.listen_only = listen;
    printf("rx_aggregation=%d\n", rx_aggregation);
    s.awdl_state.election.metric_first = 1;
    if (fixed) s.awdl_state.channel.strategy = AWDL_CHANSEQ_INTERSECT;
    /* Keep fixed_radio clear: that upstream option requests TX_NOACK for unicast. */
    s.awdl_state.peer_cb = peer_added; s.awdl_state.peer_cb_data = &net;
    s.awdl_state.peer_remove_cb = peer_removed; s.awdl_state.peer_remove_cb_data = &net;
    if (tap_name) {
        printf("TAP connected; interface index=%lu\n", awdl_netif_ifindex(net.nif));
    }
    s.awdl_state.filter_rssi = 0;
    unsigned received = 0, accepted = 0, ack_to_self = 0, peak_peers = 0;
    int failed = 0;
    unsigned probes = 0;
    uint64_t last_probe = 0;
    if (timeBeginPeriod(1) != TIMERR_NOERROR) {
        fprintf(stderr, "could not request 1 ms scheduling resolution\n");
        awdl_sched_free(&s); pipe_radio_backend.close(&r); awdl_netif_close(net.nif); return 1;
    }
    uint64_t deadline = clock_time_us() + (uint64_t)seconds * 1000000;
    unsigned char frame[RADIO_PIPE_MAX], host_frame[2048];
    int host_pending = 0;
    static const unsigned match_channels[AWDL_MATCH_CHANNELS] = {149, 36};
    struct awdl_chan_match matcher = {channel, channel, 0, 0, 0};
    if (match && channel != 149 && channel != 36) match = 0;
    uint64_t next_match = 0;
    uint64_t last_loop = clock_time_us(), metrics_at = last_loop + 5000000;
    uint64_t loop_us = 0, max_loop_us = 0, tx_late_us = 0;
    unsigned loops = 0, queue_peak = 0;
    printf("local MAC %02x:%02x:%02x:%02x:%02x:%02x; ACK armed=%d; on-air unverified\n",
        r.mac[0], r.mac[1], r.mac[2], r.mac[3], r.mac[4], r.mac[5], r.ack_armed);
    printf("service_port=%u channel=%u fixed=%d data_rate=%u match_channel=%d; TX reports require destination correlation in radio log\n", service_port, channel, fixed, data_rate, match);
    fflush(stdout);
    while (clock_time_us() < deadline && (!stop_file || GetFileAttributesA(stop_file) == INVALID_FILE_ATTRIBUTES)) {
        uint64_t loop_now = clock_time_us(), loop_delta = loop_now - last_loop;
        last_loop = loop_now; loops++; loop_us += loop_delta;
        if (loop_delta > max_loop_us) max_loop_us = loop_delta;
        if (s.due_unicast != AWDL_SCHED_IDLE && loop_now > s.due_unicast && loop_now - s.due_unicast > tx_late_us)
            tx_late_us = loop_now - s.due_unicast;
        uint64_t next = listen ? clock_time_us() + 10000 : awdl_sched_tick(&s);
        if (r.pipe == INVALID_HANDLE_VALUE) { failed = 1; break; }
        unsigned drained = 0;
        /* Bound each drain so busy ambient traffic cannot starve the scheduler. A
         * 1.4 MB/s receive is ~1,000 frames/s in ~2 frame/ms bursts; 32 per ~1 ms
         * loop let the bridge queue overflow (2026-10-19), 256 keeps headroom. */
        for (int i = 0; i < 256; i++) {
            int n = pipe_radio_backend.capture(&r, frame, sizeof frame);
            if (n < 0) { failed = 1; break; }
            if (!n) break;
            received++;
            if (n >= 18 && frame[8] == 0xd4 && !memcmp(frame + 12, r.mac, 6)) ack_to_self++;
            int result = awdl_sched_radio_rx(&s, frame, n);
            if (result == RX_OK) {
                accepted++;
                /* Respond only to a freshly received AWDL action, at most once a second. */
                if (probe && n >= 40 && frame[8] == 0xd0 && clock_time_us() - last_probe >= 1000000) {
                    struct ether_addr peer;
                    memcpy(&peer, frame + 18, 6);
                    /* A failed send is retried next second; a dead bridge is caught by the pipe check. */
                    if (awdl_sched_probe(&s, &peer) == 0) probes++;
                    last_probe = clock_time_us();
                }
            }
            unsigned peers = awdl_peers_length(s.awdl_state.peers.peers);
            if (peers > peak_peers) peak_peers = peers;
            drained++;
            /* During a transfer a unicast (TCP ACK) is due almost always, so breaking
             * on `next` after one frame capped RX at ~1 frame per loop (~1,000/s) and
             * the bridge queue backed up ~1,000 frames (2026-10-19). Take at least 16. */
            if (i >= 15 && clock_time_us() >= next) break;
        }
        if (failed || net.failed) { failed = 1; break; }
        if (match && clock_time_us() >= next_match) {
            int slots[AWDL_MATCH_CHANNELS];
            int peers = awdl_sched_count_slots(&s, match_channels, AWDL_MATCH_CHANNELS, slots);
            unsigned to = awdl_chan_match_step(&matcher, match_channels, slots, AWDL_MATCH_CHANNELS, peers, clock_time_us());
            next_match = clock_time_us() + 1000000;
            if (to) {
                r.fixed_channel = 0; /* the only sanctioned move of the fixed channel */
                int moved = pipe_radio_backend.set_channel(&r, to) == 0;
                r.fixed_channel = 1;
                if (r.pipe == INVALID_HANDLE_VALUE) { failed = 1; break; }
                if (moved) {
                    printf("channel %s %u -> %u (peer slots: 149=%d 36=%d, peers=%d)\n",
                        matcher.gave_up ? "match reverted" : "matched", matcher.current, to, slots[0], slots[1], peers);
                    matcher.current = to;
                    awdl_sched_set_channel(&s, to);
                } else {
                    printf("channel match to %u refused by radio; staying on %u\n", to, matcher.current);
                    next_match = clock_time_us() + 30000000; /* back off rather than retry every few seconds */
                }
                fflush(stdout);
            }
        }
        if (net.nif && !listen) {
            for (int i = 0; i < 32; i++) {
                if (!host_pending) {
                    host_pending = awdl_netif_read(net.nif, host_frame, sizeof host_frame);
                    if (host_pending < 0) { failed = 1; break; }
                    if (!host_pending) break;
                    if (host_pending < 14 || host_pending > ETHER_MAX_LEN || host_frame[12] != 0x86 || host_frame[13] != 0xdd) {
                        host_pending = 0; continue; /* OWL carries IPv6 only. */
                    }
                }
                /* Keep the frame until the scheduler has space, rather than dropping it. */
                if (awdl_sched_host_tx(&s, host_frame, host_pending) < 0) break;
                host_pending = 0; net.queued++;
            }
        }
        if (s.unicast_count > queue_peak) queue_peak = s.unicast_count;
        if (clock_time_us() >= metrics_at) {
            printf("metrics tick_ms=%llu loops=%u loop_us=%llu max_loop_us=%llu tx_due_late_us=%llu queue=%u queue_peak=%u tx_unicast=%u rx=%u\n",
                (unsigned long long)GetTickCount64(), loops, (unsigned long long)loop_us,
                (unsigned long long)max_loop_us, (unsigned long long)tx_late_us,
                s.unicast_count, queue_peak, s.awdl_state.stats.tx_data_unicast, received);
            if (s.ba.active || s.bar_seen || s.delba_seen)
                printf("ba active=%d in_order=%u reordered=%u duplicates=%u holes_skipped=%u held=%u bar=%u delba=%u\n",
                    s.ba.active, s.ba.in_order, s.ba.reordered, s.ba.duplicates, s.ba.holes_skipped,
                    s.ba.stored, s.bar_seen, s.delba_seen);
            fflush(stdout);
            metrics_at = clock_time_us() + 5000000;
            loops = queue_peak = 0; loop_us = max_loop_us = tx_late_us = 0;
        }
        if (failed) break;
        /* Sleep only when idle: a loop that moved frames goes straight back for
         * more, and so does one with a unicast due now. A unicast waiting for
         * the peer's next availability window is not work: spinning on it
         * would issue empty RX pipe requests until the window opens. */
        if (!drained && !(s.unicast_count && s.due_unicast <= clock_time_us())) Sleep(1);
    }
    printf("RX=%u OWL_OK=%u peers=%d peak_peers=%u ACK_to_local=%u action_attempts=%u directed_probes=%u bridge_failed=%d\n",
        received, accepted, awdl_peers_length(s.awdl_state.peers.peers), peak_peers, ack_to_self,
        s.awdl_state.stats.tx_action, probes, failed);
    if (net.nif) {
        struct awdl_peer *peer;
        awdl_peers_it_t it = awdl_peers_it_new(s.awdl_state.peers.peers);
        while (awdl_peers_it_next(it, &peer) == PEERS_OK) peer_removed(peer, &net);
        awdl_peers_it_free(it);
        printf("TAP host_to_awdl=%u awdl_to_host=%u dropped=%u write_errors=%u neighbor_errors=%u failed=%d\n",
            net.queued, net.delivered, net.dropped, net.write_errors, net.neighbor_errors, net.failed);
        if (net.failed) failed = 1;
    }
    timeEndPeriod(1);
    awdl_sched_free(&s);
    pipe_radio_backend.close(&r);
    awdl_netif_close(net.nif);
    return failed;
usage:
    fprintf(stderr, "usage: awdl_link <pipe> <6|36|44|149> <seconds 1..3600> [--probe|--listen] [--fixed [--match-channel]] [--data-rate 12|24] [--rx-aggregation] [--tap NAME] [--service-port PORT] [--stop-file PATH]\n");
    return 2;
}
