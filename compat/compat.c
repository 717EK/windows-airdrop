/* POSIX function OWL's core calls that MinGW's CRT lacks. */
#include <stdio.h>
#include <net/ethernet.h>

char *ether_ntoa(const struct ether_addr *addr) {
	/* Same contract as glibc: static buffer, not thread-safe. */
	static char buf[18];
	const uint8_t *o = addr->ether_addr_octet;
	snprintf(buf, sizeof(buf), "%x:%x:%x:%x:%x:%x", o[0], o[1], o[2], o[3], o[4], o[5]);
	return buf;
}
