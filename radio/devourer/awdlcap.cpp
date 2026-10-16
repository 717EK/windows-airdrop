// awdlcap: monitor one channel through devourer (user-space Realtek driver),
// write every frame to a pcap Wireshark opens, and count what was heard,
// picking out AWDL action frames (category 127, Apple OUI 00:17:f2, type 8).
//
//   awdlcap <channel> <seconds> <out.pcap>      e.g.  awdlcap 6 30 ch6.pcap
//
// Links devourer, so this program is GPL-2.0 (see docs/native-radio-workaround.md).
#include <libusb.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "DeviceSession.h"
#include "RxPacket.h"
#include "SignalStop.h"
#include "UsbOpen.h"
#include "WiFiDriver.h"
#include "logger.h"

static std::mutex mu;
static FILE *pcap;
static std::map<std::string, int> kinds;
static std::set<std::string> awdl_peers;
static int frames, awdl;

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

static std::string mac(const uint8_t *p) {
	char s[18];
	snprintf(s, sizeof s, "%02x:%02x:%02x:%02x:%02x:%02x", p[0], p[1], p[2], p[3], p[4], p[5]);
	return s;
}

static void on_frame(const Packet &p) {
	size_t len = p.Data.size();
	if (p.RxAtrib.fcs_present && len >= 4)
		len -= 4; // pcap DLT 105 carries no FCS
	if (len < 10 || p.RxAtrib.crc_err)
		return;
	const uint8_t *d = p.Data.data();
	auto now = std::chrono::system_clock::now().time_since_epoch();
	auto us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();

	std::string kind;
	const int type = (d[0] >> 2) & 3, subtype = d[0] >> 4;
	if (type == 0 && subtype == 13 && len >= 24 + 8 && d[24] == 127 && d[25] == 0x00 && d[26] == 0x17 &&
	    d[27] == 0xf2 && d[28] == 8)
		kind = d[30] == 0 ? "AWDL PSF" : d[30] == 3 ? "AWDL MIF" : "AWDL other";
	else
		kind = (type == 0 ? "mgmt/" : type == 1 ? "ctrl/" : type == 2 ? "data/" : "ext/") + std::to_string(subtype);

	std::lock_guard<std::mutex> g(mu);
	frames++;
	kinds[kind]++;
	if (kind.rfind("AWDL", 0) == 0) {
		awdl++;
		awdl_peers.insert(mac(d + 10));
	}
	put32(pcap, (uint32_t) (us / 1000000));
	put32(pcap, (uint32_t) (us % 1000000));
	put32(pcap, (uint32_t) len);
	put32(pcap, (uint32_t) len);
	fwrite(d, 1, len, pcap);
}

int main(int argc, char **argv) {
	if (argc != 4) {
		fprintf(stderr, "usage: awdlcap <channel> <seconds> <out.pcap>\n");
		return 2;
	}
	const int channel = atoi(argv[1]), seconds = atoi(argv[2]);
	pcap = fopen(argv[3], "wb");
	if (!pcap)
		return 1;
	put32(pcap, 0xa1b2c3d4); // pcap global header, DLT_IEEE802_11
	fwrite("\x02\x00\x04\x00", 1, 4, pcap);
	put32(pcap, 0);
	put32(pcap, 0);
	put32(pcap, 65535);
	put32(pcap, 105);

	auto logger = std::make_shared<Logger>();
	devourer::DeviceSession session{logger};
	libusb_context *ctx = nullptr;
	if (libusb_init(&ctx) < 0)
		return 1;
	session.adopt_context(ctx);
	libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_ERROR);

	const uint16_t vid = getenv("DEVOURER_VID") ? strtol(getenv("DEVOURER_VID"), nullptr, 0) : 0x2357;
	const uint16_t pid = getenv("DEVOURER_PID") ? strtol(getenv("DEVOURER_PID"), nullptr, 0) : 0x012d;
	libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, vid, pid);
	if (!h) {
		fprintf(stderr, "no adapter %04x:%04x (is it bound to WinUSB?)\n", vid, pid);
		return 1;
	}
	std::shared_ptr<devourer::UsbDeviceLock> lock;
	if (devourer::claim_interface_reset_reopen(ctx, h, logger, true, lock) != 0) {
		session.adopt_handle(h);
		return 1;
	}
	session.adopt_handle(h);
	session.adopt_lock(lock);

	WiFiDriver driver(logger);
	auto radio = driver.CreateRadio(h, ctx, lock);
	if (!radio)
		return 1;
	session.adopt_device(std::move(radio));
	IRadio *dev = session.device();

	std::thread stopper([&] {
		std::this_thread::sleep_for(std::chrono::seconds(seconds));
		g_devourer_should_stop = true; // what devourer's RX loop polls
		dev->StopRxLoop();
	});
	dev->Init(on_frame, SelectedChannel{.Channel = (uint8_t) channel, .ChannelOffset = 0,
	                                     .ChannelWidth = CHANNEL_WIDTH_20});
	stopper.join();
	dev->Stop(); // power the chip down before the session releases USB

	std::lock_guard<std::mutex> g(mu);
	fclose(pcap);
	printf("channel %d, %d s: %d frames, %d AWDL from %zu address(es)\n", channel, seconds, frames, awdl,
	       awdl_peers.size());
	for (auto &[k, n] : kinds)
		printf("  %-12s %d\n", k.c_str(), n);
	for (auto &m : awdl_peers)
		printf("  AWDL peer %s\n", m.c_str());
	return 0;
}
