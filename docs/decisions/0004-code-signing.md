# 0004 — Signing the packages

**Status:** proposed on 2026-11-10, for the maintainer to decide before 1.0 is published. The facts below about prices and eligibility were looked up on that day and change often: check them again before deciding.

**Proposal:** publish 1.0 unsigned, with `SHA256SUMS` beside the packages and the README saying what SmartScreen shows; apply to the SignPath Foundation for free signing of the Windows packages after 1.0, once the project meets its conditions; sign nothing on Linux for now.

## The problem

Windows SmartScreen warns about a program downloaded from the internet unless the program, or the certificate that signed it, has a reputation with Microsoft. An unsigned installer gets a blue "Windows protected your PC" window; **More info ▸ Run anyway** starts it. Every new version is a new file and starts over. For students on university or school computers this is a real obstacle: some managed machines do not offer **Run anyway** at all.

On Linux nothing warns: an AppImage or a tar.gz is trusted by whoever runs it. What can be offered there is a way to check that a file is the one CI built.

## The options

| Option | Cost | What the user sees | Notes |
|---|---|---|---|
| **Unsigned** (today) | none | SmartScreen's warning on every version | `SHA256SUMS` lets careful users check the download |
| **[SignPath Foundation](https://signpath.org)** | free for open-source projects that qualify | the publisher is "SignPath Foundation"; reputation is the certificate's, shared with many projects | Asks for an OSI license (MIT qualifies), a public repository, multi-factor authentication for everyone who can release, builds made in CI from the repository, and a code signing policy page naming them. Signing goes through their service from the release workflow. |
| **[Azure Artifact Signing](https://azure.microsoft.com/pricing/details/artifact-signing/)** (formerly Trusted Signing) | a monthly fee, billed from the moment the account exists | the publisher is the maintainer's verified name | Identity validation for individuals was reported to be limited to the USA and Canada; an organisation with its own legal existence is the alternative. |
| **A certificate from a certificate authority** (OV) | a few hundred euros a year | the publisher is the maintainer's name | Since 2023 the key must live in a hardware token or a cloud HSM, which complicates signing in CI. An EV certificate no longer skips SmartScreen's reputation either. |

On Linux, the candidates cost nothing and need no decision now: GitHub's artifact attestations (`actions/attest-build-provenance`, checked with `gh attestation verify`) prove that a file was built by this repository's workflow; signing `SHA256SUMS` with a key (minisign, GPG) needs that key to be kept safe for years.

## Why this proposal

- 1.0 is for a first course and the people around it, who can be told about the warning; a fee or a certificate is a cost before anyone has used the app.
- The SignPath Foundation is free and fits how resend is built: every package already comes from CI, from a tag, with nothing built by hand. The price is that the Windows publisher name is not resend's, and that the conditions must be kept (a signing policy page, multi-factor authentication, CI-only builds).
- Paid signing is worth reconsidering if resend is installed on managed machines where **Run anyway** is not offered.

## To decide

- [ ] Unsigned for 1.0, as proposed, or another option.
- [ ] Whether to apply to the SignPath Foundation after 1.0.
- [ ] Whether to add artifact attestations to the release workflow.
