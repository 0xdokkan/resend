# resend

A desktop networking lab in **C++20 and Slint**, built for creating networks, changing their configuration and understanding why a packet arrives or stops. The project aims to be a free, extensible sandbox: experiments come from the network you build, not from a fixed sequence of lessons.

resend currently simulates a LAN with hosts, switches, Ethernet, ARP, IPv4 and ICMP Echo. All traffic stays inside the local simulation. The outcome of a ping follows from the state of the network and the modelled protocols. The network can be changed while traffic flows: unplug a cable under a frame, move a PC to another switch port, change an address and watch stale caches mislead the others until they expire, or close a loop of switches. Several pings can run at once, networks can be loaded from scenario files, and every frame can be exported as a capture for Wireshark.

**Verification status (7 November 2026, Linux x86_64, GCC 16 and Clang):** the core, the CLI and the Slint 1.18.1 GUI build without warnings. CTest runs the tests of the core types, the scenario files, the simulation, the live network, the byte encoding and the playback, also under AddressSanitizer/UndefinedBehaviorSanitizer; regression tests compare twenty-three scenarios with golden files, 1,000 random networks and 300 networks edited at random while they run must give the same trace hashes everywhere, and Wireshark's `tshark` finds no errors or warnings in the exported captures. The GUI tests run on Slint's testing backend, without a display. CI runs the same suites on Ubuntu and builds the core with MSVC on Windows. The rendering was checked with snapshots from the real renderer; mouse, keyboard and HiDPI scaling have not yet been tested by hand systematically.

## Try the sandbox

At startup, `PC-A` (`192.168.1.10/24`) and `PC-B` (`192.168.1.20/24`) are connected to a switch.

1. Select `PC-A`, keep `192.168.1.20` as the destination and press **Send**. The field next to it sets how many Echo Requests the ping sends, one a second.
2. Press **Run** to watch the packets travel, or **Step** to play one event at a time: the frame moves along the cable, arrives, and the device's decision appears above it.
3. Click a packet or an event to follow that frame: the cables it crossed light up and the panel at the bottom shows it layer by layer (Ethernet, then ARP or IPv4 and ICMP). The inspector shows the hosts' ARP caches, with when each entry expires, and the switch's MAC table with the age of each entry, at the instant on the canvas.
4. Change the network while it runs: every edit happens at the instant on screen and appears in the events. Disable the cable to `PC-B` while a frame is on it and the frame is lost on arrival; power `PC-B` off and it forgets everything it learned. `PC-A` still has `PC-B`'s MAC in its cache, so the next ping times out instead of failing at ARP.
5. Change a prefix or an address in the inspector and press **Apply configuration** to experiment with subnets. After a ping, add a host, give `PC-B` another address and the new host `192.168.1.20`: until `PC-A`'s entry expires, a minute after it was learned, its pings still go to `PC-B`, which ignores them.
6. Pings can overlap: while one is on its way, select another host, change the destination and press **Send** again. Two pings that ask ARP for each other at the same time finish sooner: each host learns the other from its request.
7. Connect the switches of a network in a ring: a broadcast goes round for ever, and resend's storm guard stops it and pauses the playback, in a colour of its own, since that is the simulator stepping in, not the network.

**What the canvas shows.** Packets are coloured by protocol (ARP amber, ICMP green) and broadcast frames carry a ring; the copies a switch floods travel together. Above a device, short labels show its decisions: `learn PC-A → p1` when a switch binds a MAC to a port, `flood ×2`, `forward → p2`, `filtered`, `entry aged out`, `ARP + 192.168.1.20` when a cache is updated, `ARP entry expired`, a red `×` with the reason when a frame stops, each reply or lost request of a ping and its outcome on its source, edits such as `⏻ powered off` in blue, and `⚠ storm stopped` in violet. The lights on each card are its ports: blue when connected, amber when the cable is down, lit while a frame passes. Switch port numbers appear where the cables meet the card.

**Playback.** The clock at the top right of the canvas shows the simulated time on screen. At 1× a 10 ms hop lasts 0.6 s; the speed button cycles from 0.25× to 4×. When nothing is on the cables, waiting for a timeout lasts at most 0.8 s, however long it is in simulated time. **Reduce motion** shows one instant at a time without moving packets. Playback only decides when the simulation runs: the result of an experiment never depends on the speed.

You can add hosts and switches from the left panel (they appear in the middle of the view), drag them, pan the background and zoom with the mouse wheel. **Fit** frames the whole network; until you pan or zoom by hand, the network stays framed when the window is resized. Press **+** on two devices to connect their first free ports; press the first connector again to cancel. The control in the middle of a cable toggles it on or off, and the links panel can also remove it.

**Clear** creates an empty sandbox; **Sample network** reloads the initial LAN; **About** shows the version, the license and the Slint attribution. **Reset** keeps the topology and clears the simulation, the events and the learned tables. Every other edit (cables, power, addresses, adding or removing devices) happens at the instant on screen and keeps what the network has learned; before the first ping it simply gives a new network. Names and positions change nothing in the simulation.

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
| `switch-loop.json` | Three switches in a ring: a broadcast storm, stopped by the simulator's guard |

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
./build-core/resend-cli --count 4 --at 1.5s --link-down 2 --at 2.5s --link-up 2
./build-core/resend-cli --count 2 --at 500ms --set-ip PC-B 192.168.1.21/24
./build-core/resend-cli --scenario examples/switch-loop.json --ping PC-A 192.168.1.20 --pcap loop.pcap
```

A bare address is pinged from the first host. `--ping NAME ADDRESS` names the source and can be repeated: the pings start together. `--count`, `--interval` and `--timeout` shape every ping, and the output ends like `ping`'s, with a line per request and the statistics. Changes happen while the simulation runs: `--link-down`, `--link-up` and `--unplug` take a cable id, `--power-off` and `--power-on` a device name, `--set-ip` a host name and an address; `--at TIME` puts the next ping or change at that simulated time. `--pcap FILE` writes every frame to a capture for Wireshark or tcpdump, and `--pcap-link ID` only those on one cable. `--trace` prints the structured records instead of the explanations, and `--check` only reads and validates the file. The exit code is `0` when every ping succeeded, `1` when one failed, `2` for invalid input and `3` if the simulation does not finish within the CLI's event limit.

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
- The switch learns the source MAC on the ingress port, floods broadcasts and unknown destinations to the other ports, and uses the learned table for unicast. Entries age out after 300 s without a frame from their address (IEEE 802.1Q's default), or the time set in the scenario file.
- Hosts use static IPv4 addresses with prefixes from `/1` to `/32`. Network, broadcast, loopback and multicast addresses are excluded, except on `/31` and `/32`, which have no network or broadcast address (RFC 3021).
- ARP resolves local destinations; a destination outside the subnet produces a routing error. The return path is checked separately, and a host that must reply to an address it has no entry for resolves it first. Entries expire a minute after their last update. While an address is being resolved, up to three datagrams wait for it and a newer one pushes out the oldest; requests are repeated once a second, three times in all. These are the values of common stacks, chosen as model parameters, not universal values set by the protocols.
- A ping sends 1 to 1,000 Echo Requests with 56 bytes of data and TTL 64, one per interval; each waits for its reply up to its timeout (both one second by default), and a later reply is ignored. Several pings can run at once; datagrams waiting for the same ARP reply share one resolution.
- Each cable adds 10 ms of simulated time by default; scenario files can set any delay from 1 µs to 10 s. A frame on a cable that stops carrying, even for an instant, is lost.
- Loops of switches are allowed. Without spanning tree (STP, future work) a broadcast goes round them for ever, as in a real network; the simulator stops each frame copied more than four times per cable, and at least 64 times, and says so as a simulator event. No artificial TTL is added to Ethernet frames.
- Frames are logical structures, encoded as bytes for captures: MTU and fragmentation, link capacity, queues and random losses are not simulated yet.
- Routers, gateways, TCP/UDP, IPv6, DHCP/DNS and VLANs are not available yet. Scenario files can be opened from the command line; opening and saving from the interface come with milestone M4.

resend implements a teaching subset and claims neither full standards compliance nor compatibility with Cisco IOS. The references used to build and check the modelled behaviour are [RFC 826, ARP](https://www.rfc-editor.org/rfc/rfc826), [RFC 792, ICMP](https://www.rfc-editor.org/rfc/rfc792) and [RFC 1122, Requirements for Internet Hosts](https://www.rfc-editor.org/rfc/rfc1122), together with their applicable updates.

## Structure and development

| Path | Responsibility |
| --- | --- |
| `src/core/` | Core types: IPv4 addresses and prefixes, MAC addresses, typed identifiers, simulated time, a seeded random generator, a strict JSON reader and writer |
| `src/scenario/` | The network as the user builds it: devices, ports, cables and layout; validation, edits and the JSON file format |
| `src/sim/` | The simulation: deterministic scheduler, layered frames, cables, switches and hosts (ARP, IPv4, ICMP, ping), edits while running, the storm guard and the structured trace |
| `src/wire/` | Frames as bytes, and pcap captures |
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
| `tests/live_tests.cpp` | The network while it runs: edits, power, expiry and ageing, the ping application, the storm guard, and frame conservation in 300 randomly edited networks |
| `tests/wire_tests.cpp`, `tests/pcap_expert.cmake` | Bytes on the wire and captures, checked by hand-computed frames and by Wireshark's `tshark` |
| `tests/regression_tests.cpp`, `tests/golden/` | Twenty-three scenarios compared with their recorded output |
| `tests/app_tests.cpp` | Playback, cable geometry and the canvas scene |
| `tests/gui_tests.cpp` | GUI checks without a display |
| `scripts/fetch-slint.sh`, `scripts/run.sh` | Verified download of the Slint SDK; build, test and launch |
| `spikes/canvas/` | Canvas prototype used to evaluate Slint (`-DRESEND_BUILD_SPIKES=ON`); results in `docs/decisions/0002-ui-toolkit.md` |
| `docs/engine.md` | How the simulation works: scenario, nodes, trace, explanations and the determinism rules |
| `docs/scenario-format.md` | The scenario file format |
| `docs/roadmap.md` | Milestones up to 1.0, with their tasks and exit checks |
| `docs/decisions/` | Design decisions and their reasons, including the license and the name |

CI (`.github/workflows/ci.yml`) builds and tests the GUI on Ubuntu 22.04 (the oldest glibc the Slint SDK supports), the core under AddressSanitizer/UndefinedBehaviorSanitizer with GCC and Clang, and the core alone with MSVC on Windows. CTest also checks the CLI's exit codes.

The event queue orders operations by simulated time (in nanoseconds) and then by insertion order. The animation runs the simulation up to the instant on screen and never beyond, so how fast you watch never changes the result. Each step writes a structured trace with no text in it: frames sent and received, drops with their reason, switch learning and ageing, ARP cache updates and expiry, each request of a ping and its outcome, edits, and the storm guard. The explanations are built from that trace, and the tests check the trace itself; details are in [`docs/engine.md`](docs/engine.md).

The plan up to 1.0, with the checks that close each milestone, is in [`docs/roadmap.md`](docs/roadmap.md): animated playback, the new core, live faults, a usable sandbox, routers, then distribution. For TCP the baseline reference will be [RFC 9293](https://www.rfc-editor.org/rfc/rfc9293); for IPv6, [RFC 8200](https://www.rfc-editor.org/rfc/rfc8200), with the complementary specifications they need. Every extension must add verifiable behaviour and state the simplifications that remain.

## License

resend's code is released under the [MIT License](LICENSE), © 2026 Zayd Soulmani: you may use, modify and redistribute it, including in closed-source projects, as long as you keep the copyright notice and the license text.

The graphical interface uses [Slint](https://slint.dev), which is not part of this repository: the SDK is downloaded separately, and resend uses it under Slint's Royalty-free License 2.0, which is free for desktop applications provided that the use of Slint is disclosed. resend discloses it with the `AboutSlint` widget in the **About** panel and with the badge below. Anyone distributing their own version of the app must in turn comply with one of Slint's licenses (GPLv3, Royalty-free or commercial). The core and the CLI do not depend on Slint.

[![Made with Slint](https://raw.githubusercontent.com/slint-ui/slint/master/logo/MadeWithSlint-logo-whitebg.png)](https://slint.dev)
