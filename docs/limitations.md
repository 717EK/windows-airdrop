# Status and limitations

Recorded October 18, 2026; speed notes updated October 21, 2026. This is a developer preview.

- Hardware qualification is limited to Archer T3U `2357:012d` and iPhone 14 Pro Max. Other hardware and OS combinations are unverified.
- Earlier builds completed bidirectional transfers, including a 135.6 MB outgoing video. The 0.2.7 startup fix passed ten adapter start/stop cycles and stopping during discovery. Current-build iPhone-to-PC transfers of an 80 MB file completed repeatedly; current-build PC-to-iPhone transfers and live BLE qualification remain pending.
- Discovery can expire with the phone's Everyone window or blocked Windows BLE advertising. Retained UI rows require endpoint verification before sending.
- Bluetooth recovery may interrupt accessories. Intel Bluetooth controller timeouts have occurred during investigation; attribution to PulseIt is unestablished. Controller failure may require a Windows restart. PnP/service status alone does not prove working Bluetooth.
- Fixed channel 149 is the baseline. Receiving uses 80 MHz and our unicast frames go out at 54 Mbit/s; **Compatible sending** falls back to 12 Mbit/s. An 80 MB iPhone-to-PC transfer took about 14–20 seconds (4–6 MB/s) when the phone gave AirDrop most of its time slots, and much longer when the phone stayed on a Wi-Fi network on another channel. These are samples, not a speed guarantee. Occasional USB send stalls of 100–230 ms remain unexplained. Prior PC-to-iPhone samples were about 0.33 MB/s and have not been re-measured. Channel matching showed no verified gain.
- The application enforces a 1 GiB transfer limit. Large transfers may pause. Repeated cancellation, unplug/replug and sustained use need further physical qualification.
- Contacts Only and Apple-account identity verification are unsupported. TLS does not authenticate a peer as an Apple contact. Confirm the intended device yourself.
- Administrator access, WinUSB binding, TAP and scoped firewall rules are required. WinUSB takes the USB adapter away from ordinary Wi-Fi. Clean-PC installation and driver restoration remain release gates.
- Cold radio HELLO allows 45 seconds after an observed 22-second initialization; ordinary requests still use a 3-second deadline.
- Exports and captures require privacy review. Hard-crash staging scavenging is not enabled; age alone does not prove a temporary folder is inactive.

Acceptance requires traced current-build large transfers both ways, matching files and user opening/playback, cancellation/recovery checks, BLE coexistence and clean-PC setup/restoration. Fixtures do not clear these gates.
