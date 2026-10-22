// SPDX-License-Identifier: MIT
// Standalone IPC definition; no OWL or devourer headers cross this boundary.
#pragma once
#include <stdint.h>
#define RADIO_PIPE_MAX 65535u
#define RADIO_PIPE_VERSION 1u
enum { RADIO_HELLO = 1, RADIO_RX, RADIO_TX, RADIO_CHANNEL, RADIO_CLOSE };
// One Windows pipe message: four little-endian uint32s followed by payload.
// Request: version, command, payload length, argument. Reply: same, argument=status.
// HELLO: argument=channel, payload=6-byte local unicast MAC; reply bit 0=ACK armed,
// bit 1=radio tuned to 80 MHz with the channel as primary (link then advertises VHT).
// RX: empty request, reply payload=radiotap+MPDU, empty when queue is empty.
// TX: radiotap+MPDU. CHANNEL: argument=channel. CLOSE: empty.
// Status 0xffffffff is an error. Frames are capped at RADIO_PIPE_MAX bytes.
struct radio_message { uint32_t version, command, length, argument; };
static inline int radio_channel_valid(unsigned c) { return c == 6 || c == 36 || c == 44 || c == 149; }
