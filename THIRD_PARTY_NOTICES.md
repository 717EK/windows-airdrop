# Third-party components

Preserve upstream copyright, notices and license texts. This inventory describes source builds, not a completed binary-distribution compliance review.

| Component | Source/revision | License and use |
| --- | --- | --- |
| OWL | [jedbillyb/owl](https://github.com/jedbillyb/owl), `832d70f815c3d4a06a02117bf0fc5e868daa1ff0` | GPL-3.0-or-later, linked only into OWL targets |
| omdrop-owl patches | [t4t5/omdrop-owl](https://github.com/t4t5/omdrop-owl), `ca3de11584370325647b63f8d2ecf571bb10e9d2` | OWL-derived changes, upstream notices retained |
| devourer | [OpenIPC/devourer](https://github.com/OpenIPC/devourer), `74c6b37ce1c7ae198873d3a720466def691b86fb` | GPL-2.0-only, separate process |
| radiotap | OWL pinned submodule | Upstream permissive notices retained in fetched source |
| GoogleTest | v1.15.2, hash in CMake | BSD-3-Clause, tests |
| libusb | v1.0.30 | LGPL-2.1-or-later, radio process |
| libwdi | v1.5.1 | LGPL-3.0-or-later; USB helper distribution needs rebuild/relink inputs |
| Media metadata | omdrop-plugin `37b40ac1d03153f60e954789cfd58a398375566a` | MIT; full [notice](airdrop/THIRD_PARTY_NOTICES.md) |

Python dependencies are pinned in the requirements files and carry notices in wheel distributions. Installer builds additionally use Python, .NET, GCC runtimes, TAP-Windows6, Inno Setup and Microsoft WDK redistributables. Preserve their respective terms, exceptions and redistribution restrictions; this repository does not relicense them.

GPL-3.0-or-later OWL and GPL-2.0-only devourer must remain separate builds/processes. Do not combine objects or imply one permissive license covers the bundle. Binary release requires exact source/notices, relinking-input, signing and redistribution review.
