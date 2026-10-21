// SPDX-License-Identifier: GPL-2.0-only
// Standalone radio service. Never link this executable to OWL.
#include <windows.h>
#include <libusb.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include "../pipe_protocol.h"
#include "../frame_filter.h"
#include <stdexcept>
#include "DeviceSession.h"
#include "RxPacket.h"
#include "WiFiDriver.h"
#include "IRtlRadio.h"
#include "SignalStop.h"

// Realtek RX descriptor rate code (hal_com.h DESC_RATE*) to a short name.
static const char *rate_name(unsigned code, char *buf, size_t len) {
    static const char *legacy[] = {"CCK1", "CCK2", "CCK5.5", "CCK11", "OFDM6", "OFDM9", "OFDM12", "OFDM18", "OFDM24", "OFDM36", "OFDM48", "OFDM54"};
    if (code < 12) return legacy[code];
    if (code < 0x2c) snprintf(buf, len, "MCS%u", code - 0x0c);
    else snprintf(buf, len, "VHT%uSS-MCS%u", (code - 0x2c) / 10 + 1, (code - 0x2c) % 10);
    return buf;
}

int main(int argc, char **argv) {
    bool trace = false, no_ack = false;
    if (argc < 2 || argc > 4) { fprintf(stderr, "usage: devourer-bridge <local-pipe-name> [--no-ack] [--trace]\n"); return 2; }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) trace = true;
        else if (!strcmp(argv[i], "--no-ack")) no_ack = true;
        else return 2;
    }
    if (strncmp(argv[1], "\\\\.\\pipe\\", 9)) return 2;
    HANDLE pipe = CreateNamedPipeA(argv[1], PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, RADIO_PIPE_MAX + 16, RADIO_PIPE_MAX + 16, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 1;
    fprintf(stderr, "radio bridge waiting on %s\n", argv[1]);
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(pipe); return 1; }
    auto logger = std::make_shared<Logger>();
    // Per-packet informational logging can block the USB event thread on a full stderr pipe.
    // Logger left at its defaults: quieting it (set_level Warn + events().disable())
    // coincided with the radio receiving nothing on 2026-10-19; keep verbose until explained.
    devourer::DeviceSession session{logger};
    std::thread rx;
    std::atomic<bool> rx_failed{false};
    std::mutex mu;
    std::deque<std::vector<uint8_t>> queue;
    unsigned long received = 0, dropped = 0, tx_ok = 0, tx_failed = 0;
    unsigned char self[6]{};
    std::atomic<unsigned> normal_received{0};
    std::atomic<unsigned> reports{0}, unicast_ok{0}, unicast_drop{0};
    std::atomic<unsigned long long> unicast_retries{0};
    unsigned unicast_submitted = 0;
    // Per-interval RX link stats for frames addressed to us: what rate the
    // peer actually chooses toward this adapter, whether it aggregates, and
    // how strong it is. Rate codes are Realtek DESC_RATE values (see rate_name).
    struct RxLink { unsigned frames = 0, ampdu = 0, sgi = 0, bw40 = 0, action_ba = 0, action_other = 0;
                    unsigned bar = 0, retry = 0, amsdu = 0, seq_jumps = 0, last_seq = 0; bool have_seq = false;
                    unsigned long long rssi = 0; unsigned rates[0x54]{}; } rx_link;
    auto metrics_at = GetTickCount64() + 5000;
    unsigned long long tx_us = 0, max_tx_us = 0;
    bool ready = false;
    int exit_code = 0;
    std::vector<uint8_t> buffer(RADIO_PIPE_MAX + sizeof(radio_message));
    try {
        for (;;) {
            DWORD n;
            if (!ReadFile(pipe, buffer.data(), buffer.size(), &n, nullptr)) break;
            if (n < sizeof(radio_message)) break;
            radio_message h;
            memcpy(&h, buffer.data(), sizeof h);
            if (h.version != RADIO_PIPE_VERSION || h.length > RADIO_PIPE_MAX || n != sizeof h + h.length) break;
            if (ready && rx_failed) throw std::runtime_error("USB receive loop ended; reconnect the adapter and start a new session");
            const uint8_t *data = buffer.data() + sizeof h;
            radio_message reply{RADIO_PIPE_VERSION, h.command, 0, UINT32_MAX};
            std::vector<uint8_t> frame;
            if (h.command == RADIO_HELLO && !ready && h.length == 6 && !(data[0] & 1) && radio_channel_valid(h.argument)) {
                fprintf(stderr, "startup step=usb-open tick_ms=%llu\n", (unsigned long long)GetTickCount64());
                libusb_context *ctx = nullptr;
                if (libusb_init(&ctx) < 0) throw std::runtime_error("libusb init failed");
                session.adopt_context(ctx);
                auto handle = libusb_open_device_with_vid_pid(ctx, 0x2357, 0x012d);
                if (!handle) throw std::runtime_error("no T3U 2357:012d on WinUSB");
                std::shared_ptr<devourer::UsbDeviceLock> lock;
                fprintf(stderr, "startup step=usb-claim tick_ms=%llu\n", (unsigned long long)GetTickCount64());
                int result = devourer::claim_interface_reset_reopen(ctx, handle, logger, true, lock);
                session.adopt_handle(handle);
                if (result) throw std::runtime_error("USB claim failed");
                session.adopt_lock(lock);
                WiFiDriver driver(logger);
                devourer::DeviceConfig cfg;
                cfg.tx.report = 1;
                // Hardware retries per unacknowledged unicast (8812BU honours this per frame).
                // 2026-10-19: raising this to 10 tripled retries, left the ~9% drop rate
                // unchanged and slowed a 53 MB receive from 81 s to 111 s; the drops are
                // the phone being off-channel, not transient loss.
                cfg.tx.retry_limit = 3;
                fprintf(stderr, "startup step=driver-init tick_ms=%llu\n", (unsigned long long)GetTickCount64());
                auto device = driver.CreateRadio(handle, ctx, lock, cfg);
                if (!device) throw std::runtime_error("unsupported radio");
                session.adopt_device(std::move(device));
                session.device()->SetTxReportSink([&](const devourer::TxReport &r) {
                    if (!r.valid) return;
                    reports++;
                    if (!r.bmc) {
                        if (r.state == 0) unicast_ok++; else unicast_drop++;
                        unicast_retries += r.data_retries;
                        unsigned count = unicast_ok + unicast_drop;
                        if (trace || count <= 8 || count % 1024 == 0)
                        fprintf(stderr, "unicast TX report: state=%u retries=%u rate=%u tag=%u\n",
                            r.state, r.data_retries, r.final_rate, r.sw_define);
                    }
                });
                fprintf(stderr, "startup step=radio-init tick_ms=%llu\n", (unsigned long long)GetTickCount64());
                session.device()->InitWrite(SelectedChannel{.Channel = (uint8_t)h.argument, .ChannelOffset = 0, .ChannelWidth = CHANNEL_WIDTH_20});
                fprintf(stderr, "startup step=radio-initialized tick_ms=%llu\n", (unsigned long long)GetTickCount64());
                devourer::MacAddr mac;
                memcpy(mac.bytes.data(), data, 6);
                memcpy(self, data, 6);
                bool armed = !no_ack && session.device()->SetAckResponder(mac);
                fprintf(stderr, "ACK responder register verification: %s (on-air ACKs unverified)\n", armed ? "armed" : no_ack ? "disabled for diagnosis" : "FAILED");
                rx = std::thread([&] {
                    try {
                        session.device()->StartRxLoop([&](const Packet &p) {
                            if (p.RxAtrib.pkt_rpt_type != RX_PACKET_TYPE::NORMAL_RX) return;
                            normal_received++;
                            if (p.RxAtrib.crc_err) return;
                            size_t len = p.Data.size();
                            if (p.RxAtrib.fcs_present) { if (len < 4) return; len -= 4; }
                            if (len < 10 || len > RADIO_PIPE_MAX - 8) return;
                            std::lock_guard<std::mutex> g(mu);
                            // Counted before the filter, so frames the link never sees still show.
                            if (len >= 16 && !(p.Data[4] & 1) && !memcmp(p.Data.data() + 4, self, 6)) {
                                if (p.Data[0] == 0x84) rx_link.bar++;
                                if (p.Data[0] == 0x88 && len >= 26) {
                                    unsigned seq = (p.Data[22] | p.Data[23] << 8) >> 4;
                                    if (p.Data[1] & 0x08) rx_link.retry++;
                                    if (p.Data[24] & 0x80) rx_link.amsdu++;
                                    if (rx_link.have_seq && seq != ((rx_link.last_seq + 1) & 0xfff)) rx_link.seq_jumps++;
                                    rx_link.last_seq = seq; rx_link.have_seq = true;
                                }
                            }
                            if (!radio_frame_wanted(p.Data.data(), len, self)) return;
                            std::vector<uint8_t> f(8 + len, 0);
                            f[2] = 8; // empty radiotap; OWL currently uses its monotonic clock, not chip TSF
                            memcpy(f.data() + 8, p.Data.data(), len);
                            received++;
                            if (!(p.Data[4] & 1) && !memcmp(p.Data.data() + 4, self, 6)) {
                                rx_link.frames++;
                                if (p.RxAtrib.paggr) rx_link.ampdu++;
                                if (p.RxAtrib.sgi) rx_link.sgi++;
                                if (p.RxAtrib.bw) rx_link.bw40++;
                                if (p.RxAtrib.physt) rx_link.rssi += p.RxAtrib.rssi[0];
                                if (p.RxAtrib.data_rate < 0x54) rx_link.rates[p.RxAtrib.data_rate]++;
                                // Management action frames to us: category 3 is Block Ack (ADDBA/DELBA).
                                if (p.Data[0] == 0xd0 && len >= 26) { if (p.Data[24] == 3) rx_link.action_ba++; else rx_link.action_other++; }
                            }
                            // 256 overflowed 201 times during a 1.4 MB/s receive (2026-10-19); each
                            // dropped frame is a TCP loss. 4096 x ~1.5 KB bounds memory at ~6 MB.
                            if (queue.size() == 4096) { queue.pop_front(); dropped++; }
                            queue.push_back(std::move(f));
                        });
                    } catch (const std::exception &e) { fprintf(stderr, "RX: %s\n", e.what()); }
                    rx_failed = true;
                });
                // Give the asynchronous RX ring time to arm before the scheduler's first TX.
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                fprintf(stderr, "RX warmup normal_frames=%u\n", normal_received.load());
                fprintf(stderr, "startup step=ready tick_ms=%llu\n", (unsigned long long)GetTickCount64());
                ready = true;
                reply.argument = armed ? 1 : 0;
            } else if (ready && !rx_failed) {
                if (h.command == RADIO_RX && !h.length) {
                    std::lock_guard<std::mutex> g(mu);
                    if (!queue.empty()) { frame = std::move(queue.front()); queue.pop_front(); }
                    reply.argument = 0;
                } else if (h.command == RADIO_TX && h.length >= 18) {
                    unsigned rt = data[2] | data[3] << 8;
                    if (data[0] == 0 && rt >= 8 && rt <= h.length - 10) {
                        if (!(data[rt + 4] & 1)) {
                            unicast_submitted++;
                            if (trace || unicast_submitted <= 8 || unicast_submitted % 1024 == 0) {
                            auto rtl = dynamic_cast<IRtlRadio *>(session.device());
                            auto tag = rtl ? rtl->NextTxReportTag() : std::nullopt;
                            fprintf(stderr, "TX submit tag=%d dst=%02x:%02x:%02x:%02x:%02x:%02x type=0x%02x\n",
                                tag ? int(*tag) : -1, data[rt+4], data[rt+5], data[rt+6],
                                data[rt+7], data[rt+8], data[rt+9], data[rt]);
                            }
                        }
                        auto tx_start = std::chrono::steady_clock::now();
                        bool ok = session.device()->send_packet(data, h.length);
                        auto elapsed = (unsigned long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - tx_start).count();
                        tx_us += elapsed; if (elapsed > max_tx_us) max_tx_us = elapsed;
                        if (ok) tx_ok++; else tx_failed++;
                        reply.argument = ok ? 0 : UINT32_MAX;
                    }
                } else if (h.command == RADIO_CHANNEL && !h.length && radio_channel_valid(h.argument)) {
                    session.device()->FastRetune((uint8_t)h.argument);
                    reply.argument = session.device()->GetSelectedChannel().Channel == h.argument ? 0 : UINT32_MAX;
                } else if (h.command == RADIO_CLOSE && !h.length) break;
            }
            if (ready && GetTickCount64() >= metrics_at) {
                unsigned long rx_count, lost; size_t depth; RxLink link;
                { std::lock_guard<std::mutex> guard(mu); rx_count = received; lost = dropped; depth = queue.size(); link = rx_link; rx_link = RxLink{}; rx_link.last_seq = link.last_seq; rx_link.have_seq = link.have_seq; }
                fprintf(stderr, "metrics tick_ms=%llu RX=%lu queue=%zu queue_dropped=%lu TX_submitted=%lu TX_failed=%lu unicast_ok=%u unicast_drop=%u retries=%llu tx_usb_us=%llu max_tx_usb_us=%llu\n",
                    (unsigned long long)GetTickCount64(), rx_count, depth, lost, tx_ok, tx_failed,
                    unicast_ok.load(), unicast_drop.load(), unicast_retries.load(), tx_us, max_tx_us);
                if (link.frames) {
                    fprintf(stderr, "rx link: frames=%u ampdu=%u sgi=%u bw40=%u action_ba=%u action_other=%u bar=%u retry=%u amsdu=%u seq_jumps=%u rssi_avg=%llu rates=",
                        link.frames, link.ampdu, link.sgi, link.bw40, link.action_ba, link.action_other,
                        link.bar, link.retry, link.amsdu, link.seq_jumps, link.rssi / link.frames);
                    const char *sep = "";
                    char name[24];
                    for (unsigned r = 0; r < 0x54; r++) if (link.rates[r]) {
                        fprintf(stderr, "%s%s:%u", sep, rate_name(r, name, sizeof name), link.rates[r]); sep = ",";
                    }
                    fputc('\n', stderr);
                }
                metrics_at = GetTickCount64() + 5000; tx_us = max_tx_us = 0;
            }
            reply.length = frame.size();
            memcpy(buffer.data(), &reply, sizeof reply);
            if (!frame.empty()) memcpy(buffer.data() + sizeof reply, frame.data(), frame.size());
            if (!WriteFile(pipe, buffer.data(), sizeof reply + frame.size(), &n, nullptr)) break;
        }
    } catch (const std::exception &e) { fprintf(stderr, "bridge: %s\n", e.what()); exit_code = 1; }
    if (session.device()) {
        g_devourer_should_stop = true;
        session.device()->StopRxLoop();
        if (rx.joinable()) rx.join();
        session.device()->SetTxReportSink({});
        session.device()->ClearAckResponder();
        session.device()->Stop();
    }
    CloseHandle(pipe);
    fprintf(stderr, "radio: RX=%lu queue_dropped=%lu TX_submitted=%lu TX_failed=%lu; submission is not on-air proof\n",
            received, dropped, tx_ok, tx_failed);
    fprintf(stderr, "normal_RX=%u TX_reports=%u unicast_completed=%u unicast_dropped=%u\n",
            normal_received.load(), reports.load(), unicast_ok.load(), unicast_drop.load());
    fprintf(stderr, "unicast_submitted=%u unicast_retries=%llu trace=%d\n", unicast_submitted, unicast_retries.load(), trace);
    return exit_code;
}
