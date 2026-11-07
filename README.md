# resend

A desktop networking lab in **C++20 and Slint**, built for creating networks, changing their configuration and understanding why a packet arrives or stops. The project aims to be a free, extensible sandbox: experiments come from the network you build, not from a fixed sequence of lessons.

This first prototype simulates a LAN with hosts, switches, Ethernet, ARP, IPv4 and ICMP Echo. All traffic stays inside the local engine. The outcome of a ping follows from the state of the network and the modelled protocols; the resulting events drive the interface.

**Verification status (7 November 2026, Linux x86_64, GCC 16 and Clang):** the engine, the CLI and the Slint 1.18.1 GUI build without warnings. CTest runs the engine tests, which also check the structured trace of every step, the tests for the core types and the scheduler (both also under AddressSanitizer/UndefinedBehaviorSanitizer), and the GUI tests on Slint's testing backend, without a display. CI runs the same suites on Ubuntu and builds the engine with MSVC on Windows. The rendering was checked with snapshots from the real renderer; mouse, keyboard and HiDPI scaling have not yet been tested by hand systematically.

## Try the sandbox

At startup, `PC-A` (`192.168.1.10/24`) and `PC-B` (`192.168.1.20/24`) are connected to a switch.

1. Select `PC-A`, keep `192.168.1.20` as the destination and press **Send**.
2. Use **Step** to advance one event at a time, or **Run / Pause** to advance automatically.
3. Select events to read MAC/IP addresses, ARP requests and switch decisions. The inspector shows the hosts' ARP caches and the switch's MAC table.
4. Disable a cable and ping again: you will see the ARP retries and the resolution timing out.
5. Change a prefix or an address in the inspector and press **Apply configuration** to experiment with subnets.

You can add hosts and switches from the left panel (they appear in the middle of the view), drag them, pan the background and zoom with the mouse wheel. **Fit** frames the whole network; until you pan or zoom by hand, the network stays framed when the window is resized. Press **+** on two devices to connect their first free ports; press the first connector again to cancel. The control in the middle of a cable toggles it on or off, and the links panel can also remove it.

**Clear** creates an empty sandbox; **Sample network** reloads the initial LAN; **About** shows the version, the license and the Slint attribution. **Reset** keeps the topology and clears the simulation, the events and the learned tables. Changing cables or host configuration and adding or removing devices also clear that state: in this version, the network cannot be edited while traffic is in flight. Moving nodes or renaming a switch keeps the simulation.

## Building the engine and the CLI

You need a C++20 compiler and CMake 3.21 or later. The engine and its tests do not depend on Slint.

```sh
cmake -S . -B build-core -DRESEND_BUILD_GUI=OFF -DBUILD_TESTING=ON
cmake --build build-core --parallel
ctest --test-dir build-core --output-on-failure
./build-core/resend-cli
```

The CLI runs the sample LAN and prints the events with their simulated time:

```sh
./build-core/resend-cli 192.168.1.20
./build-core/resend-cli 192.168.1.20 --disconnect
./build-core/resend-cli 10.0.0.1
```

The exit code is `0` for a successful ping, `1` for a failed ping, `2` for invalid input and `3` if the CLI's event limit is exceeded.

## Building the Slint app

On Linux x86_64 you can use the launch script from a terminal with access to GitHub:

```sh
bash scripts/run.sh
```

The script needs a C++20 compiler, CMake, `curl`, `tar` and `sha256sum`. Through `scripts/fetch-slint.sh` it downloads the official Slint 1.18.1 SDK into `.deps/` and checks it against the SHA-256 digest published by GitHub; it then builds into `build/`, runs the tests and starts the app. Arguments are passed on to the executable, for example `bash scripts/run.sh --snapshot /tmp/resend.ppm`.

On its own, `scripts/fetch-slint.sh` prints the directory to pass as `Slint_DIR`:

```sh
cmake -S . -B build-gui -DSlint_DIR="$(bash scripts/fetch-slint.sh)"
```

With a Slint 1.18 C++ SDK already installed, point CMake to it:

```sh
cmake -S . -B build-gui \
  -DCMAKE_PREFIX_PATH=/path/to/Slint-cpp-1.18.1-Linux-x86_64
cmake --build build-gui --parallel
./build-gui/resend
```

Alternatively, CMake can download and build Slint from source. This needs network access to the dependencies, Rust 1.92 or later and the platform's build dependencies. The revision is pinned to Slint 1.18.1, commit `372cf0ee5577c3dfec309a45e7b778ba4e81b734`; the configuration uses the winit backend with the FemtoVG and software renderers, and disables the Qt backend.

```sh
cmake -S . -B build-gui -DRESEND_FETCH_SLINT=ON
cmake --build build-gui --parallel
./build-gui/resend
```

For platform requirements, see the [Slint C++ documentation](https://docs.slint.dev/latest/docs/cpp/).

### GUI tests

`resend-gui-tests` (run by CTest) uses Slint's testing backend: it opens no windows and needs no display. It finds elements by their accessible labels and drives them the way a user or a screen reader would: buttons, text fields, dragging with pointer events and arrow keys. Among other things, it checks that the initial network is fully visible, that new devices appear inside the visible area, that the timeline shows the latest event and that the About panel contains the Slint attribution.

For this to work, the code generated by Slint must include element debug information; CMake enables it when `BUILD_TESTING` is on. The `slint::testing` API is still experimental in Slint 1.18.

The testing backend does not render: appearance and `ListView` scrolling have to be checked with the real renderer. The `--snapshot` option opens the app, saves a PPM image of the window and exits; it needs a display:

```sh
./build-gui/resend --snapshot /tmp/resend.ppm
```

## Current model and limits

- At most 100 devices; one Ethernet interface per host, eight ports per switch. A cable occupies its ports even when disabled.
- The switch learns the source MAC on the ingress port, floods broadcasts and unknown destinations to the other ports, and uses the learned table for unicast.
- Hosts use static IPv4 addresses with prefixes from `/1` to `/30`. Network, broadcast, loopback and multicast addresses are excluded from the configuration in this prototype.
- ARP resolves local destinations; a destination outside the subnet produces a routing error. The return path is checked separately.
- One ping at a time, with caches kept between experiments until the network is reset or edited. ARP and MAC entries do not expire yet; resolving the sender via ARP before an ICMP reply is not modelled yet.
- Each cable adds 10 ms of simulated time. The prototype uses three ARP attempts one second apart and a one-second ICMP timeout: these are model parameters, not universal values set by the protocols.
- Topologies must be free of Ethernet loops, including cables that are temporarily disabled; the editor rejects cables that would create one. STP and broadcast storms are future work; no artificial TTL is added to Ethernet frames.
- Packets and frames are logical structures: checksums, binary encoding, MTU and fragmentation, link capacity, queues and losses are not simulated yet.
- Routers, gateways, TCP/UDP, IPv6, DHCP/DNS, VLANs and saving topologies are not available yet.

The prototype implements a teaching subset and claims neither full standards compliance nor compatibility with Cisco IOS. The references used to build and check the modelled behaviour are [RFC 826, ARP](https://www.rfc-editor.org/rfc/rfc826), [RFC 792, ICMP](https://www.rfc-editor.org/rfc/rfc792) and [RFC 1122, Requirements for Internet Hosts](https://www.rfc-editor.org/rfc/rfc1122), together with their applicable updates.

## Structure and development

| Path | Responsibility |
| --- | --- |
| `src/core/` | Core types: IPv4 and MAC addresses, typed identifiers, simulated time in nanoseconds |
| `src/sim/` | Deterministic scheduler with cancellable timers, frames and the structured trace |
| `src/network.hpp`, `src/network.cpp` | Topology, device state, protocols and event texts (the current engine, to be replaced step by step) |
| `src/app_controller.hpp`, `src/app_controller.cpp` | Glue between the C++ engine and the GUI models and callbacks |
| `src/main.cpp` | App startup and the `--snapshot` option |
| `ui/app.slint` | Canvas, device configuration, controls and event reading |
| `src/cli.cpp` | Running the engine without the GUI |
| `tests/network_tests.cpp` | Network behaviour, checked on the trace and on the events |
| `tests/core_tests.cpp` | Addresses, identifiers and scheduler |
| `tests/gui_tests.cpp` | GUI checks without a display |
| `scripts/fetch-slint.sh`, `scripts/run.sh` | Verified download of the Slint SDK; build, test and launch |
| `spikes/canvas/` | Canvas prototype used to evaluate Slint (`-DRESEND_BUILD_SPIKES=ON`); results in `docs/decisions/0002-ui-toolkit.md` |
| `docs/engine.md` | The engine today, the target architecture, the trace and the determinism rules |
| `docs/decisions/` | Design decisions and their reasons, including the license and the name |

CI (`.github/workflows/ci.yml`) builds and tests the GUI on Ubuntu 22.04 (the oldest glibc the Slint SDK supports), the engine under AddressSanitizer/UndefinedBehaviorSanitizer with GCC and Clang, and the engine alone with MSVC on Windows. CTest also checks the CLI's exit codes.

The event queue orders operations by simulated time (in nanoseconds) and then by insertion order. How often the GUI refreshes changes how fast you watch, never the result of the simulation. Each step also writes a structured trace with no text in it: frames sent and received, drops with their reason, switch learning, ARP cache updates, the ping's outcome. The tests check that trace instead of the event texts; details are in [`docs/engine.md`](docs/engine.md).

Next steps are saving networks, fuller handling of caches and failures, routers with static routing and IPv4 TTL, then transport protocols and services. For TCP the baseline reference will be [RFC 9293](https://www.rfc-editor.org/rfc/rfc9293); for IPv6, [RFC 8200](https://www.rfc-editor.org/rfc/rfc8200), with the complementary specifications they need. Every extension must add verifiable behaviour and state the simplifications that remain.

## License

resend's code is released under the [MIT License](LICENSE), © 2026 Zayd Soulmani: you may use, modify and redistribute it, including in closed-source projects, as long as you keep the copyright notice and the license text.

The graphical interface uses [Slint](https://slint.dev), which is not part of this repository: the SDK is downloaded separately, and resend uses it under Slint's Royalty-free License 2.0, which is free for desktop applications provided that the use of Slint is disclosed. resend discloses it with the `AboutSlint` widget in the **About** panel and with the badge below. Anyone distributing their own version of the app must in turn comply with one of Slint's licenses (GPLv3, Royalty-free or commercial). The engine and the CLI do not depend on Slint.

[![Made with Slint](https://raw.githubusercontent.com/slint-ui/slint/master/logo/MadeWithSlint-logo-whitebg.png)](https://slint.dev)
