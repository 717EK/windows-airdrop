/* The virtual NIC OWL calls awdl0, on Windows: a TAP-Windows6 adapter (Layer 2,
 * as OWL's Linux TAP device; Wintun is Layer 3 only), plus what OWL does with
 * netlink: a link-local address derived from our MAC, and a permanent IPv6
 * neighbour entry per AWDL peer, so the stack never needs neighbour discovery. */
#ifndef AWDL_NETIF_H
#define AWDL_NETIF_H

#include <stdint.h>
#include <net/ethernet.h>

#ifdef __cplusplus
extern "C" {
#endif

struct awdl_netif;

/* RFC 4291 modified EUI-64 link-local address, exactly as OWL's rfc4291_addr(). */
void awdl_rfc4291_addr(const struct ether_addr *mac, uint8_t out[16]);

/* Opens the TAP-Windows6 adapter with this friendly name, or the first one if
 * name is NULL. Returns 0, or -1 if there is none (the driver isn't installed). */
int awdl_netif_open(struct awdl_netif **out, const char *name);
void awdl_netif_close(struct awdl_netif *nif);

/* The adapter's MAC: use it as our AWDL address, so frames from the stack carry it. */
void awdl_netif_mac(const struct awdl_netif *nif, struct ether_addr *out);
unsigned long awdl_netif_ifindex(const struct awdl_netif *nif);

/* Media "connected", and our RFC 4291 link-local address added (this PC
 * randomizes link-local identifiers by default). Needs administrator rights. */
int awdl_netif_up(struct awdl_netif *nif);

/* One Ethernet frame the stack sent into awdl0. Non-blocking: returns its
 * length, 0 if none is waiting, -1 on error. Wait on awdl_netif_event() for more. */
int awdl_netif_read(struct awdl_netif *nif, uint8_t *buf, int cap);
void *awdl_netif_event(const struct awdl_netif *nif); /* HANDLE, signalled when a read completes */

/* One Ethernet frame from AWDL into the stack. Returns 0 or -1. */
int awdl_netif_write(struct awdl_netif *nif, const uint8_t *frame, int len);

/* Permanent neighbour entry fe80::<eui64(peer)> -> peer, as OWL's
 * neighbor_add_rfc4291(). Need administrator rights. Return 0 or -1. */
int awdl_netif_neighbor_add(struct awdl_netif *nif, const struct ether_addr *peer);
int awdl_netif_neighbor_remove(struct awdl_netif *nif, const struct ether_addr *peer);

#ifdef __cplusplus
}
#endif

#endif
