# Architecture

`PulseDrop.exe` supervises the native link, standalone radio bridge, Python HTTPS receiver/sender and BLE helper. Helpers belong to a session and are drained or stopped when it ends. Readiness reports the failing startup stage and helper exit instead of a generic timeout.

`awdl_link.exe` links GPL-3.0-or-later OWL. It drives synchronization, peers, scheduled frames and a TAP-Windows6 interface. `devourer-bridge.exe` is a separate GPL-2.0-only executable owning USB, RX, injection, channel and hardware ACK identity. They exchange bounded messages over a local single-client named pipe. The shared protocol and frame filter are MIT licensed; neither build links the other project's objects.

The Python service handles discovery, approval and uploads over HTTPS on the AWDL link-local interface. It uses bounded disk-backed streaming, validates archives and approved metadata, and publishes a completed transfer folder only after validation. An OS-held folder lease prevents competing receivers sharing a destination. Only explicit matching approval accepts a transfer; silence declines.

Discovery distinguishes radio sightings from verified HTTPS endpoints. Sending revalidates stale endpoints without silently substituting another device with the same name. Approval and uploads are not automatically replayed after ambiguous failure.

Windows BLE advertising is shared and can be blocked. Ordinary retries do not reset Bluetooth. Explicit recovery may pause connected-device services and cycle Bluetooth, then attempts to restore services. Publisher status does not prove the iPhone heard an advertisement.

Logs contain UTC/monotonic timing, HTTP phases and radio/queue counters. Diagnostic export excludes private keys and received contents, but names, addresses, file names and paths can remain. Treat exports and captures as private.
