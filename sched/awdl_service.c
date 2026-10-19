#include "awdl_service.h"
#include <stdio.h>
#include <string.h>

static void le16(uint8_t *p, unsigned value) { p[0] = value; p[1] = value >> 8; }
static unsigned label(uint8_t *p, const char *s) {
    unsigned n = strlen(s); p[0] = n; memcpy(p + 1, s, n); return n + 1;
}
static unsigned record(uint8_t *p, const uint8_t *name, unsigned nn, uint8_t kind,
                       const uint8_t *data, unsigned dn) {
    p[0] = 2; /* Service Response TLV */
    le16(p + 1, 2 + nn + 1 + 2 + 2 + dn);
    le16(p + 3, nn + 1);
    memcpy(p + 5, name, nn);
    p[5 + nn] = kind;
    le16(p + 6 + nn, dn);
    le16(p + 8 + nn, 0);
    memcpy(p + 10 + nn, data, dn);
    return 10 + nn + dn;
}

int awdl_service_records(uint8_t *out, unsigned cap, const uint8_t mac[6], const char *host, unsigned port) {
    unsigned hn = strlen(host);
    if (!port || port > 65535 || !hn || hn > 63) return -1;
    for (unsigned i = 0; i < hn; i++) {
        char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-')) return -1;
    }
    char sid[13];
    snprintf(sid, sizeof sid, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    uint8_t tmp[512], name[80], data[128];
    unsigned offset = 0, nn, dn;
    /* AWDL's fixed DNS dictionary: 7=_airdrop._tcp.local, 12=local, 0=end. */
    name[0] = 0xc0; name[1] = 7;
    dn = label(data, sid); data[dn++] = 0xc0; data[dn++] = 0;
    offset += record(tmp + offset, name, 2, 12, data, dn);
    nn = label(name, sid); name[nn++] = 0xc0; name[nn++] = 7;
    memset(data, 0, 4); data[4] = port >> 8; data[5] = port;
    dn = 6 + label(data + 6, host); data[dn++] = 0xc0; data[dn++] = 12;
    offset += record(tmp + offset, name, nn, 33, data, dn);
    dn = label(data, "flags=136");
    offset += record(tmp + offset, name, nn, 16, data, dn);
    if (offset > cap) return -1;
    memcpy(out, tmp, offset);
    return offset;
}

int awdl_service_filter(uint8_t *out, unsigned cap) {
    /* AirDrop browsing marks observed in Apple's service bloom filter: byte
     * 13 bit 2 and byte 19 bit 6. SUI stays stable across advertisements. */
    static const uint8_t filter[] = {6, 11, 0, 0, 0, 0, 1, 0, 0, 0x20, 8, 0, 4, 0x40};
    if (cap < sizeof filter) return -1;
    memcpy(out, filter, sizeof filter);
    return sizeof filter;
}
