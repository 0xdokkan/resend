# 0001 — License and project name

**Status:** decided by the author on 7 November 2026: the project is called resend and is released under the MIT License.

## License

Slint (runtime and compiler) is available under the GPLv3, under the *Royalty-free License 2.0* or under a commercial license, at the user's choice (see `.deps/slint-1.18.1/.../licenses/LICENSE.md` after running `scripts/fetch-slint.sh`).

| Option | Consequences |
| --- | --- |
| GPL-3.0-or-later for the whole project | Consistent with Slint under the GPLv3; every distributed derivative stays open. The engine cannot be reused in closed-source software. |
| MIT or Apache-2.0 for the project's code, Slint under the GPLv3 | The sources stay permissive, but distributed binaries combine GPLv3 code and must be distributed under the GPLv3's terms. |
| **MIT or Apache-2.0, Slint under the Royalty-free License 2.0** | Binaries can be distributed without the GPL, provided the app discloses that it uses Slint (for example with the `AboutSlint` widget in an *About* screen). |

**Decision:** the MIT License for all of the project's code (`LICENSE`, © 2026 Zayd Soulmani), with Slint used under its Royalty-free License 2.0 (third row of the table). The engine and the CLI do not depend on Slint and can be reused anywhere.

What Slint's Royalty-free License 2.0 ([text](https://github.com/slint-ui/slint/blob/master/LICENSES/LicenseRef-Slint-Royalty-free-2.0.md)) asks for, and how resend complies:

- disclose the use of Slint, either with the `AboutSlint` widget in an "About" screen reachable from the main interface, or with the Slint badge on a public web page, preferably the one where the binaries are downloaded: resend does both (the **About** button in the header, the badge in the README);
- no embedded systems: resend is a desktop application;
- do not distribute applications that expose Slint's APIs: resend does not;
- do not remove Slint's license notices.

Slint is not included in this repository. Anyone distributing a derived version of the app picks one of Slint's licenses in turn. Both the GPLv3 and the Royalty-free License also allow modifying Slint itself; the Royalty-free License only as part of a desktop, mobile or web application.

## Name

**Decision:** the project is called **resend**, written in lowercase everywhere, including at the start of a sentence and in the interface.

Names considered:

- "netlab" was the working name, but it is the name of a well-known open-source tool for network labs (ipSpace, netlab.tools), in the same field as this project.
- "Resend" is also the name of an email delivery service for developers. The author chose the name knowing this: the field is different (email, not network simulation). Package identifiers and search-facing descriptions should use a qualified form, for example "resend network sandbox" and `io.github.<user>.resend`.

Conventions that follow from the name:

- `project(resend)` in CMake and targets `resend_*`; CMake options stay uppercase by convention (`RESEND_*`);
- executables `resend`, `resend-cli`, `resend-tests`, `resend-core-tests` and `resend-gui-tests`;
- C++ namespace `resend` for the app; the engine lives in the `netsim` namespace, which names the module rather than the product.
