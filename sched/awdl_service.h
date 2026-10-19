#pragma once
#include <stdint.h>
/* Encode AWDL-native Bonjour PTR/SRV/TXT service-response TLVs. */
int awdl_service_records(uint8_t *out, unsigned cap, const uint8_t mac[6], const char *host, unsigned port);

int awdl_service_filter(uint8_t *out, unsigned cap);
