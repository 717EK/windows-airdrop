/* Target-specific libwdi frontend. Build and licenses: installer/README.md. */
#include <stdio.h>
#include <string.h>
#include "libwdi.h"
int main(int argc, char **argv) {
    struct wdi_device_info *list = NULL, *target = NULL;
    struct wdi_options_create_list scan = {0};
    struct wdi_options_prepare_driver prepare = {0};
    struct wdi_options_install_driver install = {0};
    int count = 0, result;
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("PulseDrop USB: only TP-Link 2357:012d. --inspect, --prepare DIR, --install DIR"); return 0;
    }
    if (argc < 2 || (strcmp(argv[1], "--inspect") && strcmp(argv[1], "--prepare") && strcmp(argv[1], "--install"))) return 2;
    prepare.driver_type = WDI_WINUSB; prepare.vendor_name = "TP-Link";
    if (!strcmp(argv[1], "--prepare")) {
        struct wdi_device_info device = {0};
        if (argc != 3) return 2;
        device.vid = 0x2357; device.pid = 0x012d; device.desc = "PulseDrop Archer T3U";
        prepare.disable_cat = TRUE; prepare.disable_signing = TRUE;
        result = wdi_prepare_driver(&device, argv[2], "pulsedrop-usb.inf", &prepare);
        printf("Prepare: %s\n", wdi_strerror(result)); return result ? 1 : 0;
    }
    scan.list_all = TRUE;
    result = wdi_create_list(&list, &scan);
    if (result) { printf("USB enumeration: %s\n", wdi_strerror(result)); return 1; }
    for (struct wdi_device_info *d = list; d; d = d->next)
        if (d->vid == 0x2357 && d->pid == 0x012d && !d->is_composite) { count++; target = d; }
    if (count != 1) { fprintf(stderr, "Connect exactly one Archer T3U (2357:012d). Found %d.\n", count); wdi_destroy_list(list); return 1; }
    if (!strcmp(argv[1], "--inspect")) { printf("Supported USB 2357:012d driver=%s\n", target->driver ? target->driver : "none"); result = 0; }
    else if (argc != 3) result = 2;
    else if (target->driver && !_stricmp(target->driver, "WinUSB")) { puts("Already using WinUSB; unchanged."); result = 0; }
    else {
        install.pending_install_timeout = 120000;
        result = wdi_prepare_driver(target, argv[2], "pulsedrop-usb.inf", &prepare);
        if (!result) result = wdi_install_driver(target, argv[2], "pulsedrop-usb.inf", &install);
        printf("USB driver: %s\n", wdi_strerror(result));
    }
    wdi_destroy_list(list); return result ? 1 : 0;
}
