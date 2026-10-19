# SignPath application readiness

Recorded October 18, 2026. Application submitted by the user; browser confirmation verified. Review is pending, with no approval or certificate issued. The gap table below records the preparation-time observations; public release/build state and MFA have not been reverified after submission. These checks do not certify compliance.

## Gaps before submission

| Requirement | Observed state | Next step |
| --- | --- | --- |
| Public source/license/documentation | GitHub main is the older research harness; GitHub reports no detected license. The complete licensed UI/installer source is prepared locally. | Publish the reviewed source without private history/records. |
| Released software | No GitHub release was returned by the release API. The unsigned diagnostic installer exists locally. | Establish a public, clearly labelled development release after appropriate qualification and approval. |
| Verifiable build | A manual unsigned installer workflow is prepared, with pinned actions/compiler/downloads, native and Python checks, full packaging and artifact provenance. It has not been published or run on GitHub. | Publish reviewed source and run the workflow on a GitHub-hosted runner; retain its artifact. Local checks do not establish trusted CI origin. |
| Project reputation | Repository created October 11; queried counts were zero stars/forks. No independent adoption or coverage was verified. | Disclose the project's early stage. Supply genuine public evidence if available; acceptance is discretionary. |
| Maintainer contact and MFA | Contact details and MFA confirmation are pending; GitHub did not return a usable MFA status. | Maintainer supplies contact and confirms MFA before activation. |
| Signing/privacy policies | Local CODE-SIGNING.md and PRIVACY.md prepared, with a conditional provider acknowledgement. | Publish them with the source; keep current unsigned status visible. |
| Dependency eligibility | OWL/devourer/libwdi and vendor runtimes/driver redistributables are bundled separately or embedded in the setup helper. | Ask SignPath to review ownership, upstream-binary rules and the System Libraries exception, especially WDK coinstallers. |
| Hardware behaviour | Diagnostic build remains unsigned and incompletely qualified on current phone/Bluetooth/clean-PC scenarios. | Retain the developer-preview limits; do not equate signing with release readiness. |

## Proposed build and signing sequence

1. Publish the reviewed source/license/policies and pin the release revision.
2. On GitHub-hosted Windows runners, provision the documented compiler/toolchain and hash/signature-checked dependencies; build native OWL and the separate devourer helper, libwdi frontend, desktop UI, BLE helper and installer runtime.
3. Run native, Python and managed checks; publish explicit skip/hardware limits. Archive source and provenance without secrets or local device records.
4. Upload the candidate as a GitHub Actions artifact. Once SignPath accepts the project and approves its artifact configuration, integrate the official signing action using the assigned organization/project/policy identifiers and repository secret. Keep release approval manual. No placeholder signing workflow is enabled now.
5. Agree on installer/uninstaller and any additional project-owned signature scope. Regenerate payload hashes after inner-file signing, then package/sign the outer installer; validate publisher, timestamp and hashes before release.

The initial scope requested in the application is the installer/uninstaller. No upstream program is presumed eligible for re-signing. Raw AWDL radio operation and driver rebinding are disclosed as interoperability/setup features, not hidden from review.

Sources: [Foundation terms](https://signpath.org/terms.html), [application](https://signpath.org/apply), [GitHub integration](https://docs.signpath.io/trusted-build-systems/github).
