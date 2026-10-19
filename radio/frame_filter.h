// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <string.h>
/* Only AWDL traffic and ACKs to our identity enter the IPC queue. Ambient
 * infrastructure traffic can otherwise bury time-sensitive sync frames. */
static inline int radio_frame_wanted(const unsigned char *d, size_t n, const unsigned char *self) {
    if (n >= 10 && d[0] == 0xd4) return !memcmp(d + 4, self, 6);
    if (n < 24) return 0;
    if (d[0] == 0xd0 && n >= 32 && d[24] == 127 &&
        !memcmp(d + 25, "\x00\x17\xf2", 3) && d[28] == 8) return 1;
    return (d[0] & 0x0c) == 8 && !memcmp(d + 16, "\x00\x25\x00\xff\x94\x73", 6);
}
