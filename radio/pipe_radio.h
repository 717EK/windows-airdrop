#pragma once
#include "radio.h"
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
struct pipe_radio {
    const char *name;
    unsigned int channel;
    unsigned char mac[6];
    HANDLE pipe;
    HANDLE io_event; /* single-caller transport; reused after each completed/reaped operation */
    unsigned char *message;
    bool fixed_channel;
    bool ack_armed; /* register verification only, not proof of on-air ACKs */
    bool wide80;    /* bridge reported an 80 MHz tune (HELLO reply bit 1) */
};
extern const struct RadioBackend pipe_radio_backend;
#ifdef __cplusplus
}
#endif
