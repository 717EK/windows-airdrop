# Privacy

PulseIt transfers files directly between nearby devices at the user's request. The application has no built-in analytics, advertising, automatic cloud file storage or automatic diagnostic-upload feature identified in the current source.

## Nearby discovery and transfers

Starting receiving or device discovery sends discovery traffic to nearby devices. This can disclose the configured PC name/model, service availability and network identifiers. Bluetooth and AWDL/mDNS discovery are not private contact authentication. Contacts Only and Apple-account verification are unsupported.

Selecting a recipient and sending a file discloses transfer metadata, including file name/size, to that recipient and transmits the contents as part of the requested transfer. Incoming files require explicit approval. Confirm the intended recipient yourself. No third-party cloud relay is used by the file-transfer implementation.

## Local data

Settings, session diagnostics, received files and driver backups remain on the PC unless the user shares them. Logs may contain device names, addresses, file names/paths, hashes, timings and errors. Received files and driver backups are retained on uninstall. The app shows local received-file history; public render mode excludes it.

Diagnostic ZIP export is a local operation. It excludes received file contents and private keys, but can retain personal metadata. Inspect and redact exports before submitting a bug report. Do not post raw packet captures, private keys or personal files publicly.

## Other services

Developer builds download tools/dependencies from their upstream providers. Windows networking, Bluetooth, certificate validation and administrator prompts are handled by Windows. The project does not control the operating system's separate diagnostics or account settings.

The repository and any future release downloads are hosted by GitHub under [GitHub's privacy statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement). If SignPath is adopted, its service handles maintainer application/account information and build artifacts under [its privacy policy](https://signpath.io/privacy-policy); the PulseIt app does not send user files to SignPath. Applying to the signing program is separate from running the app.

Report a privacy/security concern through the process in [SECURITY.md](SECURITY.md), without posting sensitive data publicly.
