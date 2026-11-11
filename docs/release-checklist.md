# Release checklist

How a version of resend is released: what the machines do on their own, and what a person checks by hand on clean machines before the draft becomes a release. The packages are described in the README ([Install](../README.md#install), [Packages](../README.md#packages)).

## 1. Prepare

- [ ] `project(resend VERSION X.Y.Z ...)` in `CMakeLists.txt` names the new version.
- [ ] `packaging/linux/io.github._0xdokkan.resend.metainfo.xml` has a `<release version="X.Y.Z" date="...">` at the top of `<releases>`, with a sentence on what changed.
- [ ] The README's verification status and its limits say what this version does and does not do; `docs/roadmap.md` has its milestone ticked.
- [ ] CI is green on `main` for the commit to be tagged.

## 2. Tag

```sh
git tag -a vX.Y.Z -m "resend X.Y.Z"
git push origin vX.Y.Z
```

The **Release** workflow (`.github/workflows/release.yml`) checks that the tag names the version in `CMakeLists.txt`, runs the whole of CI on the tag (tests on Linux and Windows, the packages, the smoke tests on clean Ubuntu 22.04, Ubuntu 24.04 and Fedora 42 containers) and attaches to a **draft** release:

- `resend-X.Y.Z-x86_64.AppImage`
- `resend-X.Y.Z-linux-x86_64.tar.gz`
- `resend-X.Y.Z-windows-x64.exe` (installer) and `resend-X.Y.Z-windows-x64.zip`
- `SHA256SUMS`

Nothing is published until a person publishes the draft.

## 3. Check by hand

On clean machines: a fresh virtual machine or a user account that never ran resend. At least one of them has no GPU (a virtual machine without 3D acceleration). For each, note the system, the package, and what failed.

Download the packages from the draft and check them against `SHA256SUMS` (`sha256sum -c SHA256SUMS`, or `Get-FileHash` on Windows).

### Every package

- [ ] **Install.** Windows installer: SmartScreen warns (the packages are not signed, [decision 0004](decisions/0004-code-signing.md)); **More info ▸ Run anyway** installs it; the Start menu entry and the desktop shortcut have the icon. Zip: unpacked, `bin\resend.exe` starts. AppImage: `chmod +x`, it starts by double click and from a terminal. tar.gz: with the libraries the README lists, `bin/resend` starts.
- [ ] **The window.** The icon is in the task bar or the dock; the title says resend; the text is sharp at 100 %, 150 % and 200 % scaling; on Linux, under Wayland and under X11.
- [ ] **Open an example.** File ▸ Open example ▸ Two LANs and a router: the network appears, framed in the canvas.
- [ ] **Ping.** From PC-A to `10.2.0.20`: the frames play, the ping succeeds, the timeline lists the events, a frame opens field by field.
- [ ] **Sound.** During the ping, the hops climb a scale and the reply resolves; M or the button beside the clock silences it at once; with the sound unplugged or disabled, resend starts silent and Help ▸ About says no device could be opened.
- [ ] **Fault.** Disable the router's cable while the ping runs, or power off PC-B: the ping fails and the inspector says why.
- [ ] **Save and reopen.** File ▸ Save as into the home folder; quit; start again; File ▸ Open: the network is the one saved, and undo still works on it.
- [ ] **A lab.** File ▸ Labs… ▸ The missing gateway: the panel shows the brief and the goals; repair the network, press Check: every goal is met and the library marks the lab solved, after a restart too.
- [ ] **No GPU.** On the machine without one, resend starts and draws; if it does not, `SLINT_BACKEND=winit-software` makes it start (and the failure is noted).
- [ ] **About.** Help ▸ About shows the version being released, the MIT license, the Slint attribution, miniaudio and the sound output in use.
- [ ] **Uninstall.** Windows: the uninstaller removes the program and its Start menu entry. Settings and autosave stay in `%LOCALAPPDATA%\resend` (on Linux in `~/.local/state/resend`) and can be deleted by hand.

### Notes

- A package that fails here is not published: fix, tag `vX.Y.Z+1` (the draft of the failed tag is deleted), check again.
- The manual checks are the ones no CI runs: real displays, real GPUs and drivers, scaling, the desktop's menus, SmartScreen.

## 4. Publish

- [ ] The draft's notes (generated from the commits) are edited into a short list of what changed, with a link to the README's limits.
- [ ] The draft is published. The Slint badge in the README, on the page the packages are downloaded from, stays (Slint's Royalty-free License, [decision 0001](decisions/0001-license-and-name.md)).
