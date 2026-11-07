# resend

A desktop networking lab in **C++20 and Slint**, built for creating networks, changing their configuration and understanding why a packet arrives or stops. The project aims to be a free, extensible sandbox: experiments come from the network you build, not from a fixed sequence of lessons.

resend currently simulates a LAN with hosts, switches, Ethernet, ARP, IPv4 and ICMP Echo. All traffic stays inside the local simulation. The outcome of a ping follows from the state of the network and the modelled protocols; several pings can run at once, and networks can be loaded from scenario files.

**Verification status (7 November 2026, Linux x86_64, GCC 16 and Clang):** the core, the CLI and the Slint 1.18.1 GUI build without warnings. CTest runs the tests of the core types, the scenario files, the simulation and the playback, also under AddressSanitizer/UndefinedBehaviorSanitizer; regression tests compare fourteen scenarios with golden files, and 1,000 random networks must give the same trace hash everywhere. The GUI tests run on Slint's testing backend, without a display. CI runs the same suites on Ubuntu and builds the core with MSVC on Windows. The rendering was checked with snapshots from the real renderer; mouse, keyboard and HiDPI scaling have not yet been tested by hand systematically.

## Try the sandbox

At startup, `PC-A` (`192.168.1.10/24`) and `PC-B` (`192.168.1.20/24`) are connected to a switch.

1. Select `PC-A`, keep `192.168.1.20` as the destination and press **Send**.
2. Press **Run** to watch the packets travel, or **Step** to play one event at a time: the frame moves along the cable, arrives, and the device's decision appears above it.
3. Click a packet or an event to follow that frame: the cables it crossed light up and the panel at the bottom shows it layer by layer (Ethernet, then ARP or IPv4 and ICMP). The inspector shows the hosts' ARP caches and the switch's MAC table at the instant on the canvas.
4. Disable a cable, or power off `PC-B` from the inspector, and ping again: you will see the frame stop where it is lost, the ARP retries and the resolution timing out.
5. Change a prefix or an address in the inspector and press **Apply configuration** to experiment with subnets.
6. Pings can overlap: while one is on its way, select another host, change the destination and press **Send** again. Two pings that ask ARP for each other at the same time finish sooner: each host learns the other from its request.

**What the canvas shows.** Packets are coloured by protocol (ARP amber, ICMP green) and broadcast frames carry a ring; the copies a switch floods travel together. Above a device, short labels show its decisions: `learn PC-A → p1` when a switch binds a MAC to a port, `flood ×2`, `forward → p2`, `filtered`, `ARP + 192.168.1.20` when a cache is updated, a red `×` with the reason when a frame stops, and the outcome of the ping on its source. The lights on each card are its ports: blue when connected, amber when the cable is down, lit while a frame passes. Switch port numbers appear where the cables meet the card.

**Playback.** The clock at the top right of the canvas shows the simulated time on screen. At 1× a 10 ms hop lasts 0.6 s; the speed button cycles from 0.25× to 4×. When nothing is on the cables, waiting for a timeout lasts at most 0.8 s, however long it is in simulated time. **Reduce motion** shows one instant at a time without moving packets. Playback only decides when the simulation runs: the result of an experiment never depends on the speed.

You can add hosts and switches from the left panel (they appear in the middle of the view), drag them, pan the background and zoom with the mouse wheel. **Fit** frames the whole network; until you pan or zoom by hand, the network stays framed when the window is resized. Press **+** on two devices to connect their first free ports; press the first connector again to cancel. The control in the middle of a cable toggles it on or off, and the links panel can also remove it.

**Clear** creates an empty sandbox; **Sample network** reloads the initial LAN; **About** shows the version, the license and the Slint attribution. **Reset** keeps the topology and clears the simulation, the events and the learned tables. Every other edit (cables, power, names, addresses, adding or removing devices) also clears that state: in this version, the network cannot be edited while traffic is in flight. Moving cards keeps the simulation.

To open one of the [example scenarios](#example-scenarios), or a file of your own, start the app with `--scenario`:

```sh
./build-gui/resend --scenario examples/office-floor.json
```

## Example scenarios

The files in [`examples/`](examples) are small networks, each with a lesson in its notes. Their format is described in [`docs/scenario-format.md`](docs/scenario-format.md).

| File | What it shows |
| --- | --- |
| `sample-lan.json` | The network the app starts with: two PCs on a switch |
| `two-switches.json` | Learning across two switches; the reply is forwarded, never flooded |
| `mismatched-masks.json` | Masks that disagree: ARP works, the reply has no route back |
| `unplugged.json` | A cable down: three ARP requests, then the ping gives up |
| `office-floor.json` | Six PCs on two switches, for several pings at once; one port disabled |

## Building the core and the CLI

You need a C++20 compiler and CMake 3.21 or later. The core, the CLI and their tests do not depend on Slint.

```sh
cmake -S . -B build-core -DRESEND_BUILD_GUI=OFF -DBUILD_TESTING=ON
cmake --build build-core --parallel
ctest --test-dir build-core --output-on-failure
./build-core/resend-cli
```

The CLI runs the sample LAN, or a scenario file, and prints the events with their simulated time, then the outcome of each ping:

```sh
./build-core/resend-cli 192.168.1.20
./build-core/resend-cli 192.168.1.20 --disconnect
./build-core/resend-cli 10.0.0.1
./build-core/resend-cli --scenario examples/two-switches.json --ping PC-C 192.168.1.10
./build-core/resend-cli --scenario examples/office-floor.json --ping Desk-1 10.0.0.22 --ping Desk-5 10.0.0.12
./build-core/resend-cli --scenario examples/unplugged.json --trace
./build-core/resend-cli --check --scenario my-network.json
```

A bare address is pinged from the first host. `--ping NAME ADDRESS` names the source and can be repeated: the pings start together. `--trace` prints the structured records instead of the explanations, and `--check` only reads and validates the file. The exit code is `0` when every ping succeeded, `1` when one failed, `2` for invalid input and `3` if the simulation does not finish within the CLI's event limit.

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

The testing backend does not render: appearance and `ListView` scrolling have to be checked with the real renderer. The `--snapshot` option opens the app, saves a PPM image of the window and exits; it needs a display. With `--ping` and `--at` it shows a given instant of a ping from the first host, paused, and `--scenario` chooses the network:

```sh
./build-gui/resend --snapshot /tmp/resend.ppm
./build-gui/resend --snapshot /tmp/at-15ms.ppm --ping 192.168.1.20 --at 15ms
./build-gui/resend --snapshot /tmp/office.ppm --scenario examples/office-floor.json --ping 10.0.0.22 --at 6ms
```

## Current model and limits

- At most 100 devices; one Ethernet interface per host, eight ports per switch. A cable occupies its ports even when disabled.
- The switch learns the source MAC on the ingress port, floods broadcasts and unknown destinations to the other ports, and uses the learned table for unicast.
- Hosts use static IPv4 addresses with prefixes from `/1` to `/32`. Network, broadcast, loopback and multicast addresses are excluded, except on `/31` and `/32`, which have no network or broadcast address (RFC 3021).
- ARP resolves local destinations; a destination outside the subnet produces a routing error. The return path is checked separately.
- Several pings can run at once, each with one Echo Request; datagrams waiting for the same ARP reply share one resolution. Caches are kept between experiments until the network is reset or edited. ARP and MAC entries do not expire yet; resolving the sender via ARP before an ICMP reply is not modelled yet.
- Each cable adds 10 ms of simulated time by default; scenario files can set any delay from 1 µs to 10 s. Hosts use three ARP attempts one second apart and a one-second ICMP timeout: these are model parameters, not universal values set by the protocols.
- Topologies must be free of Ethernet loops, including cables that are temporarily disabled; the editor rejects cables that would create one. STP and broadcast storms are future work; no artificial TTL is added to Ethernet frames.
- Packets and frames are logical structures: checksums, binary encoding, MTU and fragmentation, link capacity, queues and losses are not simulated yet.
- Routers, gateways, TCP/UDP, IPv6, DHCP/DNS and VLANs are not available yet. Scenario files can be opened from the command line; opening and saving from the interface come with milestone M4.

resend implements a teaching subset and claims neither full standards compliance nor compatibility with Cisco IOS. The references used to build and check the modelled behaviour are [RFC 826, ARP](https://www.rfc-editor.org/rfc/rfc826), [RFC 792, ICMP](https://www.rfc-editor.org/rfc/rfc792) and [RFC 1122, Requirements for Internet Hosts](https://www.rfc-editor.org/rfc/rfc1122), together with their applicable updates.

## Structure and development

| Path | Responsibility |
| --- | --- |
| `src/core/` | Core types: IPv4 addresses and prefixes, MAC addresses, typed identifiers, simulated time, a seeded random generator, a strict JSON reader and writer |
| `src/scenario/` | The network as the user builds it: devices, ports, cables and layout; validation, edits and the JSON file format |
| `src/sim/` | The simulation: deterministic scheduler, layered frames, cables, switches and hosts (ARP, IPv4, ICMP, ping), and the structured trace |
| `src/explain/` | The English explanations, built from the trace |
| `src/app/` | App logic without Slint: playback in simulated time and what the canvas shows at an instant |
| `src/app_controller.hpp`, `src/app_controller.cpp` | Glue between the scenario, the simulation and the GUI models and callbacks |
| `src/main.cpp` | App startup and the `--scenario`, `--snapshot`, `--ping` and `--at` options |
| `ui/app.slint` | Canvas, device configuration, controls and event reading |
| `src/cli.cpp` | Running scenarios without the GUI |
| `examples/` | Example scenario files |
| `tests/core_tests.cpp` | Addresses, identifiers, durations and scheduler |
| `tests/scenario_tests.cpp` | JSON, validation rules, edits and the example files |
| `tests/sim_tests.cpp` | Network behaviour: concurrent pings, ARP, carrier, delays, and the hash of 1,000 random networks |
| `tests/regression_tests.cpp`, `tests/golden/` | Fourteen scenarios compared with their recorded output |
| `tests/app_tests.cpp` | Playback, cable geometry and the canvas scene |
| `tests/gui_tests.cpp` | GUI checks without a display |
| `scripts/fetch-slint.sh`, `scripts/run.sh` | Verified download of the Slint SDK; build, test and launch |
| `spikes/canvas/` | Canvas prototype used to evaluate Slint (`-DRESEND_BUILD_SPIKES=ON`); results in `docs/decisions/0002-ui-toolkit.md` |
| `docs/engine.md` | How the simulation works: scenario, nodes, trace, explanations and the determinism rules |
| `docs/scenario-format.md` | The scenario file format |
| `docs/roadmap.md` | Milestones up to 1.0, with their tasks and exit checks |
| `docs/decisions/` | Design decisions and their reasons, including the license and the name |

CI (`.github/workflows/ci.yml`) builds and tests the GUI on Ubuntu 22.04 (the oldest glibc the Slint SDK supports), the core under AddressSanitizer/UndefinedBehaviorSanitizer with GCC and Clang, and the core alone with MSVC on Windows. CTest also checks the CLI's exit codes.

The event queue orders operations by simulated time (in nanoseconds) and then by insertion order. The animation runs the simulation up to the instant on screen and never beyond, so how fast you watch never changes the result. Each step writes a structured trace with no text in it: frames sent and received, drops with their reason, switch learning, ARP cache updates, the outcome of each ping. The explanations are built from that trace, and the tests check the trace itself; details are in [`docs/engine.md`](docs/engine.md).

The plan up to 1.0, with the checks that close each milestone, is in [`docs/roadmap.md`](docs/roadmap.md): animated playback, the new core, live faults, a usable sandbox, routers, then distribution. For TCP the baseline reference will be [RFC 9293](https://www.rfc-editor.org/rfc/rfc9293); for IPv6, [RFC 8200](https://www.rfc-editor.org/rfc/rfc8200), with the complementary specifications they need. Every extension must add verifiable behaviour and state the simplifications that remain.

## License

resend's code is released under the [MIT License](LICENSE), © 2026 Zayd Soulmani: you may use, modify and redistribute it, including in closed-source projects, as long as you keep the copyright notice and the license text.

The graphical interface uses [Slint](https://slint.dev), which is not part of this repository: the SDK is downloaded separately, and resend uses it under Slint's Royalty-free License 2.0, which is free for desktop applications provided that the use of Slint is disclosed. resend discloses it with the `AboutSlint` widget in the **About** panel and with the badge below. Anyone distributing their own version of the app must in turn comply with one of Slint's licenses (GPLv3, Royalty-free or commercial). The core and the CLI do not depend on Slint.

[![Made with Slint](https://raw.githubusercontent.com/slint-ui/slint/master/logo/MadeWithSlint-logo-whitebg.png)](https://slint.dev)
