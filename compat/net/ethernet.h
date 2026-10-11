/* Windows stand-in for <net/ethernet.h>: only what OWL's core uses. */
#ifndef COMPAT_NET_ETHERNET_H
#define COMPAT_NET_ETHERNET_H

#include <stdint.h>

#define ETHER_ADDR_LEN 6
#define ETHER_MAX_LEN 1518

struct ether_addr {
	uint8_t ether_addr_octet[ETHER_ADDR_LEN];
} __attribute__((__packed__));

#ifdef __cplusplus
extern "C" {
#endif

char *ether_ntoa(const struct ether_addr *addr);

#ifdef __cplusplus
}
#endif

#endif
