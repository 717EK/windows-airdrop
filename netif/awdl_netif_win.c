#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winioctl.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "awdl_netif.h"

/* From TAP-Windows6's tap-windows.h. */
#define TAP_WIN_CONTROL_CODE(request) CTL_CODE(FILE_DEVICE_UNKNOWN, request, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define TAP_WIN_IOCTL_SET_MEDIA_STATUS TAP_WIN_CONTROL_CODE(6)
#define TAP_DESCRIPTION "TAP-Windows Adapter V9"

#define FRAME_MAX 1536

struct awdl_netif {
	HANDLE dev;
	NET_LUID luid;
	unsigned long ifindex;
	struct ether_addr mac;
	OVERLAPPED rx;
	uint8_t rx_buf[FRAME_MAX];
	int rx_pending;
};

void awdl_rfc4291_addr(const struct ether_addr *mac, uint8_t out[16]) {
	const uint8_t *m = mac->ether_addr_octet;
	memset(out, 0, 16);
	out[0] = 0xfe;
	out[1] = 0x80;
	out[8] = m[0] ^ 0x02;
	out[9] = m[1];
	out[10] = m[2];
	out[11] = 0xff;
	out[12] = 0xfe;
	out[13] = m[3];
	out[14] = m[4];
	out[15] = m[5];
}

static int wide_equals(const wchar_t *w, const char *s) {
	wchar_t buf[256];
	return MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, 256) > 0 && wcscmp(w, buf) == 0;
}

int awdl_netif_open(struct awdl_netif **out, const char *name) {
	ULONG size = 32 * 1024;
	IP_ADAPTER_ADDRESSES *list = NULL, *a;
	ULONG err;
	int found = 0;
	struct awdl_netif *nif = calloc(1, sizeof(*nif));
	char path[128];

	for (int tries = 0; tries < 3; tries++) {
		list = realloc(list, size);
		err = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_ALL_INTERFACES, NULL, list, &size);
		if (err != ERROR_BUFFER_OVERFLOW)
			break;
	}
	for (a = err == NO_ERROR ? list : NULL; a; a = a->Next) {
		if (wcsncmp(a->Description, L"" TAP_DESCRIPTION, wcslen(L"" TAP_DESCRIPTION)) != 0)
			continue;
		if (name && !wide_equals(a->FriendlyName, name))
			continue;
		snprintf(path, sizeof(path), "\\\\.\\Global\\%s.tap", a->AdapterName);
		nif->luid = a->Luid;
		nif->ifindex = a->Ipv6IfIndex ? a->Ipv6IfIndex : a->IfIndex;
		memcpy(nif->mac.ether_addr_octet, a->PhysicalAddress, ETHER_ADDR_LEN);
		found = 1;
		break;
	}
	free(list);
	if (!found) {
		free(nif);
		return -1;
	}

	nif->dev = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_SYSTEM | FILE_FLAG_OVERLAPPED, NULL);
	if (nif->dev == INVALID_HANDLE_VALUE) {
		free(nif);
		return -1;
	}
	nif->rx.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
	*out = nif;
	return 0;
}

void awdl_netif_close(struct awdl_netif *nif) {
	if (!nif)
		return;
	CancelIoEx(nif->dev, NULL);
	CloseHandle(nif->dev);
	CloseHandle(nif->rx.hEvent);
	free(nif);
}

void awdl_netif_mac(const struct awdl_netif *nif, struct ether_addr *out) {
	*out = nif->mac;
}

unsigned long awdl_netif_ifindex(const struct awdl_netif *nif) {
	return nif->ifindex;
}

void *awdl_netif_event(const struct awdl_netif *nif) {
	return nif->rx.hEvent;
}

static void ipv6_sockaddr(SOCKADDR_INET *sa, const struct ether_addr *mac) {
	memset(sa, 0, sizeof(*sa));
	sa->Ipv6.sin6_family = AF_INET6;
	awdl_rfc4291_addr(mac, sa->Ipv6.sin6_addr.s6_addr);
}

int awdl_netif_up(struct awdl_netif *nif) {
	ULONG connected = 1, len;
	MIB_UNICASTIPADDRESS_ROW row;

	if (!DeviceIoControl(nif->dev, TAP_WIN_IOCTL_SET_MEDIA_STATUS, &connected, sizeof(connected),
	                     &connected, sizeof(connected), &len, NULL))
		return -1;

	InitializeUnicastIpAddressEntry(&row);
	ipv6_sockaddr(&row.Address, &nif->mac);
	row.InterfaceLuid = nif->luid;
	row.OnLinkPrefixLength = 64;
	row.PrefixOrigin = IpPrefixOriginManual;
	row.SuffixOrigin = IpSuffixOriginManual;
	row.DadState = IpDadStatePreferred;
	ULONG err = CreateUnicastIpAddressEntry(&row);
	return (err == NO_ERROR || err == ERROR_OBJECT_ALREADY_EXISTS) ? 0 : -1;
}

int awdl_netif_read(struct awdl_netif *nif, uint8_t *buf, int cap) {
	DWORD got = 0;

	if (!nif->rx_pending) {
		ResetEvent(nif->rx.hEvent);
		if (ReadFile(nif->dev, nif->rx_buf, sizeof(nif->rx_buf), &got, &nif->rx)) {
			goto done; /* completed at once */
		}
		if (GetLastError() != ERROR_IO_PENDING)
			return -1;
		nif->rx_pending = 1;
	}
	if (!GetOverlappedResult(nif->dev, &nif->rx, &got, FALSE))
		return GetLastError() == ERROR_IO_INCOMPLETE ? 0 : (nif->rx_pending = 0, -1);
	nif->rx_pending = 0;
done:
	if ((int) got > cap)
		return -1;
	memcpy(buf, nif->rx_buf, got);
	return (int) got;
}

int awdl_netif_write(struct awdl_netif *nif, const uint8_t *frame, int len) {
	OVERLAPPED ov = {0};
	DWORD done = 0;
	BOOL ok;

	/* ponytail: waits for each write; TAP writes complete in microseconds. Queue them if that ever shows. */
	ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
	ok = WriteFile(nif->dev, frame, (DWORD) len, &done, &ov);
	if (!ok && GetLastError() == ERROR_IO_PENDING)
		ok = GetOverlappedResult(nif->dev, &ov, &done, TRUE);
	CloseHandle(ov.hEvent);
	return ok && (int) done == len ? 0 : -1;
}

static void neighbor_row(MIB_IPNET_ROW2 *row, const struct awdl_netif *nif, const struct ether_addr *peer) {
	memset(row, 0, sizeof(*row));
	ipv6_sockaddr(&row->Address, peer);
	row->InterfaceLuid = nif->luid;
	memcpy(row->PhysicalAddress, peer->ether_addr_octet, ETHER_ADDR_LEN);
	row->PhysicalAddressLength = ETHER_ADDR_LEN;
	row->State = NlnsPermanent;
}

int awdl_netif_neighbor_add(struct awdl_netif *nif, const struct ether_addr *peer) {
	MIB_IPNET_ROW2 row;
	neighbor_row(&row, nif, peer);
	ULONG err = CreateIpNetEntry2(&row);
	if (err == ERROR_OBJECT_ALREADY_EXISTS)
		err = SetIpNetEntry2(&row);
	return err == NO_ERROR ? 0 : -1;
}

int awdl_netif_neighbor_remove(struct awdl_netif *nif, const struct ether_addr *peer) {
	MIB_IPNET_ROW2 row;
	neighbor_row(&row, nif, peer);
	ULONG err = DeleteIpNetEntry2(&row);
	return (err == NO_ERROR || err == ERROR_NOT_FOUND) ? 0 : -1;
}
