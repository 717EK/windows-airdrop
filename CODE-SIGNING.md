# Code signing policy

**Status: application submitted on October 18, 2026; review pending.** PulseDrop has no confirmed SignPath Foundation enrollment or Foundation signing certificate. Current installers are unsigned. Do not interpret the proposed policy below as an endorsement or approval.

## Maintainer and proposed roles

- Source maintainer, reviewer and proposed release approver: [gsl0001](https://github.com/gsl0001).
- Third-party changes must be reviewed by the maintainer before release.
- Signing requires an explicit release approval. Automated builds must not bypass it.
- Multi-factor authentication is required for repository access and SignPath accounts before signing is activated. Current enrollment has not been verified.

## Intended release process

Build from a reviewed public revision on GitHub-hosted runners. Preserve pinned dependencies, upstream signatures and corresponding source. Test the resulting artifact, upload it as a GitHub Actions artifact, and request signing only through SignPath's trusted build integration after approval of the project and configuration. A locally uploaded installer is not a substitute for this provenance.

Sign only project artifacts that SignPath explicitly approves. Do not replace third-party publisher signatures or sign arbitrary upstream binaries. The initial requested scope is the PulseDrop installer and its uninstaller; app/helper coverage requires review of ownership, metadata and dependency rules. Preserve the separate OWL/devourer process boundary.

Sign eligible payload files before generating their integrity manifest, then build/sign the installer and uninstaller. Verify certificate chain, timestamp and final hashes. Reject unexpected publisher/version/path changes and signing failures. Do not publish an artifact merely because a signing request was submitted.

If accepted, the release page will carry this acknowledgement:

> Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org).

The Foundation would appear as certificate publisher; this would not be a certificate issued personally to the maintainer. Keep this acknowledgement conditional until enrollment and an actual signed release exist.

## Privacy and release qualification

See [Privacy](PRIVACY.md). Signing does not establish device compatibility, successful transfer, driver restoration or application security. Release notes must retain the [hardware and reliability limits](docs/limitations.md).

Reference: [Foundation conditions](https://signpath.org/terms.html) and [GitHub build integration](https://docs.signpath.io/trusted-build-systems/github).
