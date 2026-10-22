#include "gtest/gtest.h"
#include "pipe_radio.h"
#include "pipe_protocol.h"
#include <thread>
#include <vector>
#include <cstring>
#include <functional>
#include <chrono>

// Real named-pipe transport with a synthetic service; no USB or radio access.
class PipeServer {
public:
    const char *name = "\\\\.\\pipe\\awdl-ipc-unit-test";
    HANDLE handle;
    std::thread worker;
    explicit PipeServer(std::function<void(radio_message &, std::vector<unsigned char> &)> reply) {
        handle = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_REJECT_REMOTE_CLIENTS, 1, 70000, 70000, 0, nullptr);
        EXPECT_NE(handle, INVALID_HANDLE_VALUE);
        worker = std::thread([=, this] {
            if (!ConnectNamedPipe(handle, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return;
            unsigned char buf[70000];
            DWORD n;
            while (ReadFile(handle, buf, sizeof buf, &n, nullptr)) {
                if (n < sizeof(radio_message)) break;
                radio_message h;
                memcpy(&h, buf, sizeof h);
                EXPECT_EQ(n, sizeof h + h.length);
                std::vector<unsigned char> data(buf + sizeof h, buf + n);
                reply(h, data);
                h.length = data.size();
                memcpy(buf, &h, sizeof h);
                if (!data.empty()) memcpy(buf + sizeof h, data.data(), data.size());
                if (!WriteFile(handle, buf, sizeof h + data.size(), &n, nullptr)) break;
            }
        });
    }
    ~PipeServer() { if (worker.joinable()) worker.join(); CloseHandle(handle); }
    pipe_radio client() { pipe_radio r{}; r.name = name; r.channel = 6; r.mac[0] = 2; return r; }
};

TEST(pipe_radio, handshake_capture_transmit_channel_and_disconnect) {
    int tx = 0;
    PipeServer server([&](radio_message &h, std::vector<unsigned char> &data) {
        if (h.command == RADIO_HELLO) { EXPECT_EQ(data.size(), 6u); EXPECT_EQ(h.argument, 6u); data.clear(); h.argument = 1 | 2; }
        else if (h.command == RADIO_RX) { data = {0, 0, 8, 0, 0, 0, 0, 0, 0xd4, 0}; h.argument = 0; }
        else { if (h.command == RADIO_TX) tx++; data.clear(); h.argument = 0; }
    });
    auto r = server.client();
    ASSERT_EQ(pipe_radio_backend.open(&r), 0);
    EXPECT_TRUE(r.ack_armed);
    EXPECT_TRUE(r.wide80);
    unsigned char frame[64]{};
    EXPECT_EQ(pipe_radio_backend.capture(&r, frame, sizeof frame), 10);
    EXPECT_EQ(frame[8], 0xd4);
    EXPECT_EQ(pipe_radio_backend.transmit(&r, frame, 24), 0);
    EXPECT_EQ(pipe_radio_backend.set_channel(&r, 149), 0);
    EXPECT_EQ(r.channel, 149u);
    r.fixed_channel = true;
    EXPECT_EQ(pipe_radio_backend.set_channel(&r, 6), -1);
    EXPECT_EQ(r.channel, 149u);
    EXPECT_EQ(pipe_radio_backend.set_channel(&r, 999), -1);
    RadioCapabilities caps{};
    pipe_radio_backend.capabilities(&r, &caps);
    EXPECT_TRUE(caps.monitor_capture);
    EXPECT_FALSE(caps.raw_80211_injection);
    EXPECT_FALSE(caps.reliable_unicast_ack);
    pipe_radio_backend.close(&r);
    server.worker.join();
    EXPECT_EQ(tx, 1);
}

TEST(pipe_radio, rejects_wrong_protocol_version) {
    PipeServer server([](radio_message &h, std::vector<unsigned char> &data) { h.version++; data.clear(); h.argument = 0; });
    auto r = server.client();
    EXPECT_EQ(pipe_radio_backend.open(&r), -1);
    EXPECT_EQ(r.pipe, INVALID_HANDLE_VALUE);
    EXPECT_EQ(r.io_event, nullptr);
    EXPECT_EQ(r.message, nullptr);
}

TEST(pipe_radio, refused_handshake_releases_transport_resources) {
    PipeServer server([](radio_message &h, std::vector<unsigned char> &data) {
        data.clear(); h.argument = UINT32_MAX;
    });
    auto r = server.client();
    EXPECT_EQ(pipe_radio_backend.open(&r), -1);
    EXPECT_EQ(r.pipe, INVALID_HANDLE_VALUE);
    EXPECT_EQ(r.io_event, nullptr);
    EXPECT_EQ(r.message, nullptr);
    pipe_radio_backend.close(&r);
}

TEST(pipe_radio, rejects_capture_larger_than_callers_buffer) {
    PipeServer server([](radio_message &h, std::vector<unsigned char> &data) {
        if (h.command == RADIO_HELLO) data.clear(); else data.assign(100, 0);
        h.argument = 0;
    });
    auto r = server.client();
    ASSERT_EQ(pipe_radio_backend.open(&r), 0);
    unsigned char small[8];
    EXPECT_EQ(pipe_radio_backend.capture(&r, small, sizeof small), -1);
    EXPECT_EQ(r.pipe, INVALID_HANDLE_VALUE);
}

TEST(pipe_radio, driver_error_status_fails_request_but_keeps_connection) {
    PipeServer server([](radio_message &h, std::vector<unsigned char> &data) {
        data.clear(); h.argument = h.command == RADIO_TX ? UINT32_MAX : 0;
    });
    auto r = server.client();
    ASSERT_EQ(pipe_radio_backend.open(&r), 0);
    unsigned char frame[24]{};
    EXPECT_EQ(pipe_radio_backend.transmit(&r, frame, sizeof frame), -1);
    EXPECT_NE(r.pipe, INVALID_HANDLE_VALUE);
    EXPECT_EQ(pipe_radio_backend.capture(&r, frame, sizeof frame), 0);
    pipe_radio_backend.close(&r);
}

TEST(pipe_radio, times_out_unresponsive_service) {
    PipeServer server([](radio_message &h, std::vector<unsigned char> &data) {
        if (h.command != RADIO_HELLO) Sleep(3200);
        data.clear(); h.argument = 0;
    });
    auto r = server.client();
    ASSERT_EQ(pipe_radio_backend.open(&r), 0);
    unsigned char frame[64]{};
    auto start = GetTickCount64();
    EXPECT_EQ(pipe_radio_backend.capture(&r, frame, sizeof frame), -1);
    EXPECT_LT(GetTickCount64() - start, 4000u);
    EXPECT_EQ(r.pipe, INVALID_HANDLE_VALUE);
}

TEST(pipe_radio, allows_slow_firmware_initialization_without_relaxing_io_timeout) {
    PipeServer server([](radio_message &h, std::vector<unsigned char> &data) {
        if (h.command == RADIO_HELLO) Sleep(3200);
        data.clear(); h.argument = 0;
    });
    auto r = server.client();
    ASSERT_EQ(pipe_radio_backend.open(&r), 0);
    unsigned char frame[64]{};
    EXPECT_EQ(pipe_radio_backend.capture(&r, frame, sizeof frame), 0);
    pipe_radio_backend.close(&r);
}

// Opt-in transport measurement: no USB device and no wall-clock speed assertion.
TEST(pipe_radio, DISABLED_request_round_trip_benchmark) {
    PipeServer server([](radio_message &h, std::vector<unsigned char> &data) {
        data.clear(); h.argument = 0;
    });
    auto r = server.client();
    ASSERT_EQ(pipe_radio_backend.open(&r), 0);
    unsigned char frame[1500]{};
    const int requests = 10000;
    auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < requests; ++i)
        ASSERT_EQ(pipe_radio_backend.transmit(&r, frame, sizeof frame), 0);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    printf("PIPE_BENCH requests=%d seconds=%.6f requests_per_second=%.0f\n", requests, seconds, requests / seconds);
    pipe_radio_backend.close(&r);
}

#include "frame_filter.h"

TEST(pipe_radio, filters_ambient_traffic_without_losing_awdl_or_local_acks) {
    unsigned char self[6] = {2, 1, 2, 3, 4, 5}, frame[64]{};
    EXPECT_FALSE(radio_frame_wanted(frame, 0, self));
    frame[0] = 0x80;
    EXPECT_FALSE(radio_frame_wanted(frame, sizeof frame, self));
    frame[0] = 0xd4; memcpy(frame + 4, self, 6);
    EXPECT_TRUE(radio_frame_wanted(frame, 10, self));
    frame[4] ^= 2;
    EXPECT_FALSE(radio_frame_wanted(frame, 10, self));
    frame[0] = 0xd0; frame[24] = 127; memcpy(frame + 25, "\x00\x17\xf2", 3); frame[28] = 8;
    EXPECT_TRUE(radio_frame_wanted(frame, 32, self));
    EXPECT_FALSE(radio_frame_wanted(frame, 28, self));
    frame[0] = 0x88; memcpy(frame + 16, "\x00\x25\x00\xff\x94\x73", 6);
    EXPECT_TRUE(radio_frame_wanted(frame, sizeof frame, self));
    frame[16] = 1;
    EXPECT_FALSE(radio_frame_wanted(frame, sizeof frame, self));
}

TEST(pipe_radio, delivers_local_awdl_addba_to_responder_but_rejects_ambient_requests) {
    unsigned char self[6] = {2, 1, 2, 3, 4, 5}, frame[33]{};
    frame[0] = 0xd0;
    memcpy(frame + 4, self, 6);
    memcpy(frame + 16, "\x00\x25\x00\xff\x94\x73", 6);
    frame[24] = 3; // Block Ack category
    frame[25] = 0; // ADDBA Request
    EXPECT_TRUE(radio_frame_wanted(frame, sizeof frame, self));
    EXPECT_FALSE(radio_frame_wanted(frame, sizeof frame - 1, self));
    frame[4] ^= 2;
    EXPECT_FALSE(radio_frame_wanted(frame, sizeof frame, self));
    frame[4] ^= 2;
    frame[16] ^= 1;
    EXPECT_FALSE(radio_frame_wanted(frame, sizeof frame, self));
    frame[16] ^= 1;
    frame[24] = 4;
    EXPECT_FALSE(radio_frame_wanted(frame, sizeof frame, self));
}

TEST(pipe_radio, truncated_block_ack_actions_are_rejected_without_reading_past_the_frame) {
    unsigned char self[6] = {2, 1, 2, 3, 4, 5};
    for (size_t n = 24; n < 30; n++) {
        // Exact-size heap copy, so a read past n is out of bounds for sanitizer builds.
        std::vector<unsigned char> f(n, 0);
        f[0] = 0xd0;
        memcpy(f.data() + 4, self, 6);
        memcpy(f.data() + 16, "\x00\x25\x00\xff\x94\x73", 6);
        EXPECT_FALSE(radio_frame_wanted(f.data(), f.size(), self)) << n;
    }
    std::vector<unsigned char> delba(30, 0);
    delba[0] = 0xd0;
    memcpy(delba.data() + 4, self, 6);
    memcpy(delba.data() + 16, "\x00\x25\x00\xff\x94\x73", 6);
    delba[24] = 3;
    delba[25] = 2;
    EXPECT_TRUE(radio_frame_wanted(delba.data(), delba.size(), self));
}
