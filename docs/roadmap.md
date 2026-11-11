# Roadmap

This is the plan from the current prototype to resend 1.0, and from there to a lab environment that can take Packet Tracer's place for learning ([After 1.0](#after-10-from-sandbox-to-lab)). Every milestone lists what it delivers, how it is broken down and the checks that close it. Boxes are ticked as the work lands on `main`; anything that cannot be checked automatically says so and stays open until it is checked by hand.

Estimates are in hours of focused work and assume one developer; they include tests and documentation. In total 240–375 hours: at 10 hours a week, with a 25% margin, 7–11 months.

## Where we are

Done before this roadmap was written (7 November 2026):

- **M0, foundations.** Git, MIT license, `run.sh` with a checksummed Slint SDK, CI on Linux (GCC and Clang with sanitizers) and Windows (MSVC, engine only), GUI tests without a display.
- **M1, Slint evaluation.** Hands-on spike in `spikes/canvas/`, decision recorded in [`decisions/0002-ui-toolkit.md`](decisions/0002-ui-toolkit.md): Slint stays.
- **The first part of M2.** Typed addresses and ids, the nanosecond scheduler with cancellable timers and the structured trace (`src/core/`, `src/sim/`, described in [`engine.md`](engine.md)).
- The interface and every text in the repository are in English; name and license are decided ([`decisions/0001-license-and-name.md`](decisions/0001-license-and-name.md)).

| # | Milestone | Hours | Delivers |
|---|---|---|---|
| A | Animated playback | 35–55 | Packets that move in simulated time, decisions shown on the canvas |
| M2 | New core | 35–55 | Scenario files, nodes built from modules, explanations generated from the trace, concurrent pings |
| M3 | Faithful L2/L3 and live faults | 45–70 | Edits while running, ARP and MAC ageing, TTL, loops with storm protection, pcap export |
| M4 | Usable sandbox → **0.2** | 50–80 | Final canvas, save and undo, layered inspector, timeline filters, Italian translation (since removed) |
| M5 | Routers and static routing | 40–60 | Gateways, longest prefix match, ICMP errors, traceroute |
| M6 | Distribution → **1.0** | 35–55 | AppImage, Windows build, clean-machine checks |

Animation comes first because it only needs the trace, which already exists, and every later milestone benefits from it: in M3 you watch a frame die on an unplugged cable, in M5 you watch the TTL go down hop by hop.

## A — Animated playback (35–55 h)

**Outcome:** the canvas shows what the engine computed, when it computed it. Several packets can be in flight at once, each protocol has its own look and every decision of a device appears where it happens.

**Design.** The engine is never ahead of the screen: the `PlaybackController` turns real time into simulated time and asks the engine to run up to that instant, so the tables in the inspector always describe the moment shown. Animation reads the engine; it never drives the result.

- [x] **A1. Transmissions in the trace.** Every `FrameSent` also records `{pdu, link, from device/port, to device/port, start, arrival}`. The engine gains `run_until(t)` and `next_event_time()`; the scheduler gains a non-destructive `next_time()`.
  - Tests: the sample ping produces 8 transmissions with the expected ports and times; a flood creates simultaneous transmissions; `run_until` stops exactly at `t` and processes every entry due at `t`.
- [x] **A2. `PlaybackController`** (plain C++, no Slint): play, pause, step, speed (0.25×–4×), reduced motion, end of simulation. At 1× a 10 ms hop takes 600 ms; an idle gap with nothing on the wires lasts at most 0.8 s of real time whatever its simulated length, so a 1 s ARP timeout does not stall the screen. Step plays up to the next event and stops there.
  - Tests with a fake clock: positions at given real times, speed changes, idle compression, step boundaries, no event skipped or processed twice.
- [x] **A3. Packets on the canvas.** A packet model computed in C++ each frame: position on the cable, protocol colour (ARP amber, ICMP mint), a short label (`ARP req`, `ARP rep`, `Echo req`, `Echo rep`) and a ring for broadcast frames. Flooded copies travel together.
- [x] **A4. Decisions on the devices.** Short-lived markers derived from the records: `+MAC` with the port when a switch learns, the egress port highlighted on a forward, a fan-out on a flood, `filtered`, `+ARP` on a cache update, a red ✕ with the reason where a frame stops, and the ping result on the source host.
- [x] **A5. Ports on the cards.** Each card has a front panel with one light per port: connected, cable down, or lit while a frame passes. Cables end on the card borders with the switch port number, so the egress port of every frame is visible. Dragging cables from port to port comes with the final canvas (M4.1).
- [x] **A6. Follow a frame.** Selecting a packet (or an event) highlights every cable the same frame crossed, flooded copies included, and the inspector shows the frame by layer: Ethernet, then ARP or IPv4 and ICMP.
- [x] **A7. Controls.** Speed selector, reduced-motion switch, simulated clock in the toolbar.
- [x] **A8. Snapshots at a given instant.** `resend --snapshot out.ppm --ping 192.168.1.20 --at 15ms` renders the canvas at a simulated time, for documentation and visual checks. *With `--offscreen` it needs no display and draws at exactly `--size`.*

**Exit checks**

- [x] At 15 ms into the sample ping exactly one packet is on cable 2, halfway (controller test and snapshot).
- [x] With a third host on the switch, at 15 ms two copies of the ARP request are in flight (controller and GUI tests).
- [x] A disabled destination shows a drop marker at the arrival time with the reason (GUI test).
- [x] Changing speed or pausing never changes the trace (test: same records at 0.25× and 4×).
- [ ] Rendering stays at 60 fps with 20 packets in flight on the GPU renderers (measured by hand with `SLINT_DEBUG_PERFORMANCE`).

After A, a public **0.1.1 preview** can collect early feedback.

## M2 — New core (35–55 h)

**Outcome:** for the user the interface looks the same, but the network is described by a scenario file, several pings can run at once and every explanation is generated from the trace.

- [x] **M2.1 Core types.** `Ipv4Prefix`, an address with its prefix length for both interfaces and networks (/31 and /32 accepted as in RFC 3021), a seeded `Rng` with its own range mapping, and a small strict JSON reader and writer with stable key order.
- [x] **M2.2 Scenario.** Devices with explicit ports, cables between `PortRef`s with a delay, kinds (host, switch), layout and notes; `validate()` reports impossible input with a precise message; versioned JSON (`"format": "resend-scenario", "version": 1`), described in [`scenario-format.md`](scenario-format.md).
  - Tests: round trip of every example file byte for byte; each validation rule has a failing case.
- [x] **M2.3 Simulation.** Immutable layered frames (`EthernetFrame` carrying `ArpPacket` or `Ipv4Packet` with `IcmpMessage`), a `Medium` for cables and carrier, nodes built from modules (`Nic`, `Arp`, `PingApp` in the host, which also holds its IPv4 layer and echo responder; `Bridge` for the switch) behind a `Context` with `transmit`, `schedule`, `cancel` and `record`. Each ping is an application with its own timers; every scheduled entry is an episode in the trace.
- [x] **M2.4 Explanations.** The `explain` module turns records into the event list: a title, a detail and a protocol per group of records, in English. During the transition its output is compared with the old engine's texts.
- [x] **M2.5 App and CLI on the new core.** The GUI keeps its behaviour; `resend-cli --scenario file.json --ping "PC-A" 192.168.1.20` runs scenarios and prints the trace or the explanations.
- [x] **M2.6 Old engine removed** once every scenario passes on the new one.

**Exit checks**

- [x] The seven scenarios of the prototype produce the same sequence of frames, records and texts on both engines (parity test, then kept as regression tests). Fourteen scenarios in the end, now compared with golden files in `tests/golden/`.
- [x] No user-facing text in `src/sim/`.
- [ ] 1,000 random topologies give the same trace hash on every run, and the hash matches a constant checked on Linux and Windows CI. Checked on Linux with GCC and Clang; the Windows job runs it on the next CI run.
- [x] Pings A→B and C→D started together both succeed.

## M3 — Faithful L2/L3 and live faults (45–70 h)

**Outcome:** the instructive failures become possible: stale caches, cables moved while traffic flows, loops.

- [x] **M3.1 Edits while running.** `Simulation::apply(Edit)` at the current time, recorded in the trace: cable up/down, plug/unplug/move, port admin state, power, IPv4 change, add/remove devices. A frame on a cable that goes down is lost on arrival with `LinkDown`; powering off clears volatile state.
- [x] **M3.2 ARP cache with states** (RFC 1122 §2.3.2): incomplete entries with a queue (the latest packet is kept), at most one request per second per address, three attempts, entries that expire after 60 s.
- [x] **M3.3 MAC ageing** in the switch, 300 s by default (IEEE 802.1Q), with the time of the last update.
- [x] **M3.4 Replies resolve the sender.** A host that must answer an address it has no entry for resolves it with ARP first; `Reason::NotModelled` disappears.
- [x] **M3.5 Ping as an application:** identifier, sequence numbers, count, interval and timeout; per-reply round-trip time and a summary like `ping`'s.
- [x] **M3.6 IPv4 TTL** in every packet (64 from hosts).
- [x] **M3.7 Explicit ports** in the engine API and in the scenario (done with M2).
- [x] **M3.8 Loops allowed, storm protection.** Ethernet has no TTL: when a frame is copied beyond a limit the simulator pauses with a record of its own category (`SimulatorGuard`), shown in a distinct colour and never as network behaviour.
- [x] **M3.9 Bytes and pcap.** The `wire` module encodes frames (Ethernet II, ARP, IPv4 with checksum, ICMP with checksum), decodes them for round trips, and writes pcap files with nanosecond timestamps, for the whole network or one cable.

**Exit checks** (automated scenarios)

- [x] After an IP change the caches of the other hosts stay stale until the entry expires, then ARP finds the new owner. (`stale_entry_after_address_change` in `tests/live_tests.cpp`, regression case 17.)
- [x] After a cable is moved to another switch port, frames go to the old port until the switch relearns or the entry ages out. (`moved_cable` and `mac_ageing`, regression case 18.)
- [x] A loop of three switches trips the storm protection, which is clearly separate from network events. (`storm_guard`, regression case 19, the GUI and app tests, `examples/switch-loop.json`.)
- [x] `tshark -z expert` reports no errors or warnings on the exported pcap files (test skipped when tshark is missing). (CTest `pcap-wireshark-expert`, checked with TShark 4.7.4 on Linux.)
- [x] At every instant, frames sent = frames delivered + frames dropped + frames in flight (randomised test). (`random_live_networks`: 300 networks, loops included, edited at random; frames the storm guard stops count as dropped.)

## M4 — Usable sandbox → 0.2 (50–80 h)

**Outcome:** a desktop sandbox you can build, save, undo and study in.

- [x] **M4.1 Final canvas.** The spike's `CanvasController` moves into the app (`src/app/canvas.hpp`): port-to-port cables, cable selection with a pixel tolerance, keyboard, fit view, semantic zoom (simplified cards when zoomed out), cables in one `Path` per state. One touch area over the whole canvas sends the pointer to C++, which decides what lies under it; new cards go to the free place nearest the middle of the view.
- [x] **M4.2 Properties.** Per device: name, power, per-port admin state and IPv4; per cable: delay and state; switch ageing time. A refused value says why and changes nothing.
- [x] **M4.3 Tables** with `StandardTableView`: ARP cache (address, MAC, state, expiry) and MAC table (MAC, port, age), at the instant shown.
- [x] **M4.4 Timeline.** Filters (protocol, device, problems only, changes, simulator), a time scrubber that moves back by replaying the deterministic simulation, and the playback speeds of A. While changes recorded later lie ahead, the network cannot be changed and the timeline says so.
- [x] **M4.5 Layered inspector with hex view:** every field of Ethernet, ARP, IPv4 and ICMP with its value and meaning (`wire::dissect()`), and the bytes of the selected field highlighted; a byte selects the smallest field that holds it.
- [x] **M4.6 Files.** Open and save scenarios, autosave every 5 s while there are unsaved changes, recovery after a crash, unsaved-changes marker, questions before changes are lost. Slint has no file dialog: an in-app dialog over `std::filesystem` first, native dialogs later.
- [x] **M4.7 Undo and redo.** Each step keeps its edits and their exact inverses (`edit::inverse()`, with `RestoreLink` and `RestoreDevice` for what was removed), so undoing gives back the same scenario; a drag is one step; while the simulation runs, undo applies the inverse edit at the current time.
- [x] **M4.8 Example scenarios** in File ▸ Open example, built into the app.
- [x] **M4.9 Translation.** Interface strings in `@tr()` with bundled translations, explanation texts in a C++ catalog read from the same `.po` files, Italian as the first translation, language from the system with an override in View ▸ Language. `scripts/update-translations.py` brings the catalogs up to date with the source. *Removed on 10 November 2026, at the maintainer's request: resend is in English only. The texts still go through `@tr()` and `text::tr()`, so a translation can come back without touching them; the Italian catalog is in the history.*
- [x] **M4.10 Layout:** collapsible palette, inspector and timeline, so the canvas keeps room at 2× scale: the window has no fixed minimum, and a 1080p screen at 2× (960 × 540 logical pixels) holds the whole interface with the side panels folded.

**Exit checks**

- [x] Saving and reopening gives an identical file. (`saving_and_opening` in `tests/files_tests.cpp` for every example; the GUI tests save, reopen and save again through the interface.)
- [x] 50 random commands followed by 50 undos give back the initial scenario. (`random_commands_and_undos` in `tests/workspace_tests.cpp`, running and not, then 50 redos give back the last one.)
- [x] The inspector's fields match tshark's for every frame of the example scenarios. (CTest `dissection-tshark`: position, size and value of every field, 405 frames, skipped when tshark is missing.)
- [x] Every interface string is translatable and the Italian translation is complete. (CTest `translations`: every `@tr()` and `tr()` text and every example's title and notes has an Italian entry with the same placeholders, and no entry is unused.) *Withdrawn with the translation, see M4.9.*
- [ ] A 10-device network can be built in under 5 minutes (checked with 2–3 people, by hand).

## M5 — Routers and static routing (40–60 h)

**Outcome:** several subnets, and the classic routing mistakes become visible.

- [x] **R1.** Router with 2–8 interfaces; hosts with a default gateway ARP for the gateway when the destination is off-link (RFC 1122 §3.3.1); the router decrements the TTL (RFC 1812 §5.3.1). Scenario format version 2 (`gateway`, router `ports` and `routes`), with edits for the gateway, the port count and the routes.
- [x] **R2.** Connected, static and default routes; longest prefix match with overlapping /8, /16 and /24 (RFC 1812 §5.2.4.3, RFC 4632). A static route whose next hop is on none of the router's networks, or is one of its addresses, or that repeats a connected network, is kept but not used, and says why.
- [x] **R3.** ICMP Time Exceeded (11/0) and Destination Unreachable for network (3/0) and host (3/1); no ICMP error about an ICMP error or a broadcast (RFC 792, RFC 1812 §4.3, RFC 1122 §3.2.2). Errors quote the whole datagram, as Linux does; the ping that caused one ends with it, and the frame view and pcap files show the quoted datagram.
- [x] **R4.** Traceroute over ICMP; a routing loop ends when the TTL runs out. One probe per hop, as `traceroute -I -q 1`, in the engine, the CLI (`--traceroute`, `--max-hops`) and the app.
- [x] Routing table and router configuration in the interface; router scenarios among the examples. The router inspector edits the address of each port, the number of ports and the static routes, and shows the routing table, with the routes not used and why, and the ARP cache by port; hosts gain the default gateway; the run bar has a traceroute switch.

**Exit checks**

- [x] Scenarios: two LANs and a router, wrong gateway, missing return route, routing loop, longest prefix. (Examples and regression cases 24–30; `tests/routing_tests.cpp`, with 200 random routed networks checked for frame conservation and against a constant trace hash.)
- [x] Comparison with Linux: a script builds the same topologies with network namespaces and captures with tcpdump; the sequence of packet types (ARP operation, ICMP type and code, TTL) matches the simulator's. Timing may differ. (`scripts/compare-linux.py`: 16 cases over the five examples, cable by cable, also comparing MAC and IPv4 addresses and the datagram each error quotes; run in the Linux CI job.)

## M6 — Distribution → 1.0 (35–55 h)

**Outcome:** people can install resend without building it.

- [x] Install layout with the Slint library next to the executable (`$ORIGIN/../lib`), tar.gz and AppImage built on Ubuntu 22.04 (glibc 2.35, the Slint SDK's minimum), desktop file, icon and AppStream metadata. *`cmake --install` lays out `bin/`, `lib/` and `share/` (launcher, icons from 16 to 256 px painted from the sprites, AppStream metadata, fonts, licenses); `cpack` makes the tar.gz, `scripts/make-appimage.sh` the AppImage with appimagetool 1.9.1 and the type 2 runtime pinned by digest, carrying libinput and what it needs and nothing on the AppImage project's exclude list. The window sets its app id. Checked on Arch Linux: the installed program and the AppDir find the Slint library and the fonts with no font on the system, the desktop file and the metadata pass `desktop-file-validate` and `appstreamcli validate --pedantic`.*
- [x] Windows: MSVC build of the whole app in CI, portable zip and an installer. *The `windows-gui` job installs the official Slint SDK (`scripts/fetch-slint.ps1`, digest checked), builds and tests everything, renders a snapshot, makes the zip and the NSIS installer with `cpack` and starts the unpacked zip. `resend.exe` is a windowed program with its icon and version resource that still writes to the console it was started from; the packages carry Slint's DLL and the Visual C++ runtime. Written, not yet run: its first run on GitHub is the check.*
- [x] Third-party licenses shipped with the binaries. *`THIRD-PARTY-NOTICES.md`, Slint's `LICENSE.md` and `THIRDPARTY.md`, the fonts' OFL texts, and in the AppImage the copyright file and Ubuntu version of every library it carries.*
- [x] Release workflow: tagged builds attached to a draft GitHub release. *`release.yml` checks that the tag names the version in `CMakeLists.txt`, runs the whole of `ci.yml` on it and drafts the release with the four packages and `SHA256SUMS`.*
- [x] Container smoke tests: the AppImage starts and renders a snapshot on Ubuntu 22.04, 24.04 and Fedora with the software renderer. *`linux-smoke`: in `ubuntu:22.04`, `ubuntu:24.04` and `fedora:42`, with only a desktop's libraries and Xvfb, the AppImage and the tar.gz render a snapshot, `resend-cli` pings, and the metadata validates. Written, not yet run.*
- [x] The same scenarios give the same trace hash on Linux and Windows (CI). *The regression tests compare thirty scenarios, 1,000 random networks and the random routed networks with the same golden hashes on Linux, in `windows-core` and now in `windows-gui`.*
- [ ] Manual checklist on clean machines, including one without a GPU: install, open an example, ping, fault, save, reopen (by hand). *Written as [`docs/release-checklist.md`](release-checklist.md); to be done by a person on the first draft release.*
- [ ] Decided at this point: unsigned Windows installers trigger SmartScreen warnings; code signing has a cost. *Proposed in [decision 0004](decisions/0004-code-signing.md): 1.0 unsigned, then the SignPath Foundation's free signing; the maintainer decides.*

### What 1.0 includes

- Devices: hosts with a gateway, switches with 8 ports, routers with 2–8 interfaces.
- Protocols: Ethernet II, ARP with a cache that follows RFC 1122, static IPv4 from /1 to /32, ICMP Echo, Destination Unreachable (network and host), Time Exceeded, static and default routes.
- Applications: ping and traceroute.
- Faults while running: cables, ports, power, moved cables, configuration changes, L2 loops with declared protection.
- Observation: animated playback, timeline with filters, layered and hex inspector, ARP, MAC and routing tables, pcap export.
- Documents: JSON files, undo and redo, example scenarios.
- Platforms: Linux x86_64 and Windows x64. Interface in English.

**Not in 1.0, and said so in the README:** UDP, TCP, DHCP, DNS, VLANs, STP, IPv6, NAT and ACLs, bandwidth, queues, MTU, dynamic routing, guided labs, plugins, macOS.

## After 1.0: from sandbox to lab

**Goal.** resend becomes a free and open alternative to Cisco Packet Tracer for learning networks. The sandbox stays as it is; guided labs are built on top of it, and the protocols of a first networking course arrive one at a time, each with its labs.

**The yardstick.** The plan is measured against one public outline: the video ["POV: You Learn Networking For Fun"](https://www.youtube.com/watch?v=sFck88rcl4s) (dzuma, October 2026), which teaches by building networks in Packet Tracer, and the two lab collections it links. When the three checkpoints below are reached, every networking exercise it proposes can be done in resend.

Estimates use the same unit as above, hours of focused work by one developer: 640–1,015 for the milestones M7–M18, and 255–405 more for the look, the feel and the play around them ([G1–G7](#look-feel-and-play-g1g7)).

### What the video asks for

| In the video | resend at 1.0 | Milestone |
|---|---|---|
| Building Your First Network (5:14): a router, two switches and two PCs on two subnets; cables, addresses by hand, router ports enabled, gateways, ping | Possible in the sandbox, with the inspector in place of commands | M7 makes it the first lab, M8 adds the terminal |
| Its two deliberate mistakes: both PCs in one subnet across the router, and no default gateway | Possible in the sandbox; the `wrong-gateway` example is close to the second | M7, as troubleshooting labs |
| Basic Networking Explained (20:03): ping, ARP, the MAC table, the routing table | Possible: the **Why** tab, the tables, the frame view | M7, labs whose answers are read from the trace |
| DHCP: discover, offer, request, acknowledge; pool, lease, gateway and DNS options | Missing | M9 |
| DNS: local cache, resolver, root, top-level domain, authoritative server | Missing | M10 |
| Ports and sockets; UDP; TCP's handshake, retransmission and ordering | Missing | M9, M11 |
| HTTP: request line, methods, headers, port 80, ephemeral source ports | Missing; the frame view and the captures are ready for it | M12 |
| NAT and PAT with the translation table; port forwarding | Missing | M13 |
| Firewalls: allow, drop or reject by address, protocol and port | Missing | M13 |
| Ten-day roadmap, day 1: interface, MAC, address, prefix, gateway, DNS server | Partly, in the inspector | M8, M10 |
| Day 2: observe ARP and a ping | Possible | M7 |
| Day 3: a lab from the linked collection | Missing | M7, then the next table |
| Day 4: Wireshark on captures | Possible: every frame exports to pcap | Each protocol adds to the captures |
| Day 5: traceroute, explaining every hop | Possible | M7 |
| Day 6: host a web page on the local network | Missing | M12 |

The first collection it links, [PPechmann/Packet-Tracer-Practice-Labs](https://github.com/PPechmann/Packet-Tracer-Practice-Labs) (beginner to intermediate, MIT):

| Lab | Needs | Milestone |
|---|---|---|
| Basic IPv4 configuration | What 1.0 has | M7 |
| IPv4 static routes | What 1.0 has | M7 |
| VLSM subnetting | What 1.0 has (prefixes from /1 to /32) | M7 |
| NAT: static, dynamic, overload | NAT | M13 |
| Router on a stick | VLANs, trunks, router sub-interfaces | M14 |
| Branch network: VLANs, trunks, router on a stick, static routes | The same | M14 |
| VTP configuration | VTP is Cisco's own protocol and is not modelled: the lab becomes "the same VLANs on three switches, by hand" | M14 |
| Analyzing STP | Spanning tree | M15 |
| IPv6 configuration | IPv6 | M16 |
| DHCP and ACLs, named in its README | DHCP, packet filters | M9, M13 |

The second, the [Witty Networks troubleshooting labs](https://wittynetworks.net/courses/ccna-packet-tracer-troubleshooting-labs/) (intermediate to advanced), is about twenty broken networks with a goal and no instructions. It needs an account, and its page names VLANs, STP, OSPF "and more": M14, M15 and M17 cover what it names, and M18 starts by finding out what the rest is.

**Outside the plan**, as the README will say from M7.9 on:

- **Opening `.pkt` files.** The format is Packet Tracer's own and undocumented. Labs on the same topics are written again as resend labs, with their own networks and texts.
- **Cisco's command line and Cisco's own protocols** (VTP, DTP, PVST+, HSRP, EIGRP, CDP). resend models standards and checks itself against Linux; where a standard does the same job (RSTP, VRRP, LLDP) it is a candidate. The terminal of M8 speaks the Linux tools' language, and the first lab shows each of the video's commands next to what does the same in resend.
- **The second half of the video**, about security. It uses real tools on real and virtual machines and on Wi-Fi hardware, not Packet Tracer. All of resend's traffic stays inside the simulation, wireless is not modelled, and resend ships no attack tools. What belongs to configuring a network, the packet filters of M13, is in the plan.
- **HTTPS and TLS.** resend models no cryptography; the HTTP labs say what TLS would change.
- **Real software.** Hosts run resend's own model services (a DHCP, DNS or web server), not programs from outside.

### Principles

1. **A lab never scripts an outcome.** A goal is a fact about the simulation, checked by the engine: a lab cannot say "well done" for something the network did not do.
2. **Every lab is proved in CI:** unsolved as given, solved by its solution.
3. **A contract before the code.** Each protocol gets a page under `docs/models/`: what is modelled, the RFC sections followed, the simplifications declared.
4. **A real implementation checks the model.** Each protocol extends `scripts/compare-linux.py` with the Linux tool that speaks it, and the frame view is compared with tshark field by field, as today.
5. **No marks, no timers.** The video's method is a goal, room for mistakes, and looking things up; a lab shows which goals are met and why the others are not. Progress (G6) records what was seen and solved, never points, levels or streaks.

### Order

```
labs (M7) ──► terminal (M8)               every later milestone ships its labs and commands
routers ──► UDP ──► DHCP (M9) ──► DNS (M10)
   │         └──► loss on cables ──► TCP (M11) ──► HTTP (M12)
   │                   UDP + TCP ──► NAT, port forwarding, filters (M13)
   ├──► IPv6 (M16)
   └──► multicast on a link ──► OSPF (M17)
switches ──► VLANs (M14) ──► spanning tree (M15)
```

| # | Milestone | Hours | Delivers |
|---|---|---|---|
| M7 | Guided labs → **1.1** | 50–80 | Lab files, goals checked on the simulation, the lab panel, the first ten labs |
| M8 | Device terminal → **1.2** | 35–55 | `ip`, `ping`, `traceroute` on every device, as edits like any other |
| M9 | UDP and DHCP → **1.3** | 50–80 | Ports, sockets, DHCP client, server and relay |
| M10 | DNS → **1.4** | 30–50 | Stub resolver, authoritative and recursive servers, names everywhere |
| M11 | TCP → **2.0** | 90–140 | Connections, retransmission over lossy cables |
| M12 | HTTP and services → **2.0** | 30–45 | A web server and a client: DNS, TCP and HTTP in one run |
| M13 | NAT, port forwarding, packet filters → **2.1** | 60–95 | Private networks behind one address, published services, rules |
| M14 | VLANs → **2.2** | 45–70 | Access and trunk ports, 802.1Q tags, router on a stick |
| M15 | Spanning tree → **2.3** | 45–70 | Loops that settle into a tree |
| M16 | IPv6 → **2.4** | 100–160 | NDP, SLAAC, static routing |
| M17 | OSPF → **2.5** | 80–130 | Routers that learn routes from each other |
| M18 | Troubleshooting collection → **2.6** | 25–40 | Twenty broken networks, lab paths, the review of what is still missing |

Three checkpoints:

- **2.1, the video** (M7–M13, 345–545 h): everything in the first table.
- **2.4, the lab collection** (M14–M16, 190–300 h): everything in the second.
- **2.6, troubleshooting** (M17–M18, 105–170 h).

Labs come first because they need only what 1.0 has, as the animation did before them, and every later milestone is finished only when its labs exist. M14 and M15 depend on nothing after M8 and can be brought forward whenever switching matters more than services.

The look, feel and play track runs between them: G1 → G2 → G3 → M6 → M7 → G4 → G5 (released together as 1.1) → M8 → G6 → G7 (1.2) → M9 and on. G1, G2 and G3 come before M6 so that 1.0 ships in the new interface, and before M7 so that the lab panel, the terminal and every panel after them are built once, in it.

## M7 — Guided labs → 1.1 (50–80 h)

**Outcome:** a lab is a network with a task. resend tells which goals are met by running the simulation, and CI proves that every lab it ships can be solved.

**Design.** A lab file (`"format": "resend-lab"`, version 1, in `labs/`, built into the app as the examples are) holds a title, a brief, the network to start from (the members of a scenario file), what the learner may not change, the goals, optional steps and hints, and a solution: the solved network, the commands to run on it and the answers. There are three kinds, after the way the video teaches: *guided* (steps to follow), *challenge* (a goal and an empty or half-built network) and *troubleshooting* (a broken network and a goal, nothing else).

Goals are checked by a new module, `src/lab/`, with no text and no Slint in it:

| Goal | Met when |
|---|---|
| `reach` | A ping from a host to an address succeeds, or fails for a given reason |
| `path` | A traceroute from a host is answered by the listed hops |
| `config` | The network satisfies a predicate: an address inside a prefix, a gateway, a route, a port's state, a cable between two devices |
| `observed` | The learner's own run holds a given record: an ARP request sent by a device, a datagram routed by a router |
| `answer` | The learner's answer equals a value read from the trace: a destination MAC, a TTL on arrival, a number of hops |
| `kept` | The named devices or settings are as the lab gave them |

`reach`, `path` and `answer` run on a **probe**: a new `Simulation` of the learner's current network, from time zero and with empty tables. A probe never touches the learner's run, and the same network always gets the same verdict, in the app and in `resend-cli`.

- [x] **M7.1 Lab format.** Reader, writer and validation as strict as the scenario's. Goals name devices, so a lab that starts from an empty canvas says which names to use. Described in `docs/lab-format.md`. *`src/lab/lab.cpp`: errors name the member's path as the scenario's do, writing gives back the bytes read, and `validate()` catches unknown devices, duplicate ids, answers without a question and commands from non-hosts. The documentation's example is checked by `lab_tests`.*
- [x] **M7.2 Goal checker.** Each verdict carries its evidence, the records that decided it, and `explain` turns it into text: "PC-A's request has no route: PC-A has no default gateway". *`src/lab/check.cpp`, worded by `src/explain/lab_explain.cpp`. A probe runs until its ping ends and then as long as a router could still be asking with ARP, at most 200,000 events; a traceroute's own Time Exceeded drops are not evidence.*
- [x] **M7.3 Hosts without an address.** A host port may have none (scenario format version 3): the host sends no IPv4 and says why (`NoAddress`). A lab can ask that new hosts start without an address and new router ports disabled, as the devices in the video do. DHCP needs the first in M9.
- [x] **M7.4 Locks.** The workspace refuses an edit to a locked device or setting with a `Problem`, as it refuses any other. *`Problem::Locked`, with the lock in the outcome so that the refusal names the device and the settings; the inspector shows a lock beside a locked device's settings. Moving a card is never locked.*
- [x] **M7.5 Lab panel**, on the HUD of G2. The brief, the steps, each goal with its state and the reason it is not met, hints shown one at a time on request, **Check**, **Restart** and **Compare with the solution**; File ▸ Open lab, and a library by topic that remembers what was solved in the state folder. *`ui/lab.slint` and `src/app_lab.cpp`. **Solution** puts the learner's network aside, untouched, until **Back to my network**; a change after a check marks the verdicts as old. File ▸ Open takes lab files as it takes scenarios, File ▸ Labs… (G) is the library, and the welcome offers the first lab. Solved labs are kept in `settings.json`.*
- [x] **M7.6 CLI.** `resend-cli --lab FILE --check NETWORK` prints each goal and exits 0 when all are met; `--verify` checks the lab against its own solution. *Also `--answer ID VALUE` for the questions.*
- [x] **M7.7 The first ten labs**, all on what 1.0 simulates: `first-network` (the video's exercise, guided), `one-subnet-two-sides` and `missing-gateway` (its two mistakes), `ipv4-basics`, `static-routes`, `vlsm`, `missing-return-route`, `routing-loop`, and `watch-arp-and-ping` and `explain-traceroute`, whose answers are read from the trace. *In four topics: First networks, Addresses, Routing, Observing.*
- [x] **M7.8 Texts.** ~~Lab texts go through the catalogs as the examples' notes do, with Italian complete.~~ `first-network` shows, step by step, what the video types in Packet Tracer and what does the same in resend. *The translation was removed before M7 closed (see M4.9): lab texts are English, written in the files. Steps name the interface as it is.*
- [x] **M7.9 Positioning.** The README and the About panel present the sandbox and the labs together.

**Exit checks**

- [x] Every lab in `labs/` has at least one goal unmet as given, and all met with its solution (CTest `labs-verify`). *One test per lab, `labs-verify-<id>`, which also checks that the solution keeps the locks.*
- [x] `first-network` from an empty canvas, through the interface: with the addresses set and the router ports enabled but no gateways the goal is unmet with `NoRoute`; with the gateways it is met (GUI test). *`first_lab_through_the_interface` in `tests/gui_tests.cpp`: five devices from the build bar, renamed, addressed and cabled with the pointer and the inspector.*
- [x] A verdict depends on the network alone: after a ping that succeeded, an edit that breaks the network turns the goal unmet although the caches are still warm (test). *`verdicts_follow_the_network` in `tests/lab_tests.cpp`.*
- [x] A locked edit is refused and changes nothing, the trace included. *`locks_refuse_edits`: the network, the history and the trace hash are unchanged, before the run and while it runs.*
- [x] ~~Every lab text is translated (CTest `translations`).~~ *Withdrawn with the translation.*
- [ ] Someone who has never used resend completes `first-network` in under 20 minutes (by hand).

## M8 — Device terminal → 1.2 (35–55 h)

**Outcome:** every device has a terminal, so a lab can ask for commands, and a new protocol needs commands rather than a new panel.

**Design.** The commands are a subset of the Linux tools resend is already compared with, with resend's port names (`p1`, `p2`, …): what a learner types here works on a Linux machine. A command that changes the network makes the same `Edit` as the inspector, so it is undone, replayed and shown in the timeline in the same way. A command or an option outside the subset is refused with a message, never ignored.

- [ ] **M8.1 Terminal tab** in the inspector: history, completion, `help` and `help COMMAND`, and a menu for each family of commands (`ip`, `ping`, `bridge`) that lists what can be run on this device with a line on what each does. Choosing one writes it on the command line, and runs it when nothing is left to fill in. Output is plain text built from the simulation at the instant on screen. *As in the mock-up, the tab is a row of small buttons, one for each family and `clear`, over one screen: what was printed, each command after its prompt and errors in the warm tone, and at its foot the line typed on. `help` fits the screen's 42 columns.*
- [ ] **M8.2 Queries:** `ip addr`, `ip link`, `ip route` and `ip neigh` on hosts and routers, `bridge fdb` on switches.
- [ ] **M8.3 Changes:** `ip addr add|del`, `ip link set PORT up|down`, `ip route add|del`, with `default via` on hosts.
- [ ] **M8.4 Applications:** `ping` (`-c`, `-i`, `-W`, `-t`) and `traceroute` (`-m`), printing line by line as the simulation plays.
- [ ] **M8.5 Labs** can ask for a command, and `first-network` gains a variant solved from the terminal.

**Exit checks**

- [ ] Each command that changes the network records the same edit as its action in the inspector (test over every command).
- [ ] The commands of every lab's solution run unchanged in the namespaces of `scripts/compare-linux.py`, whose interfaces are named as resend's ports, and leave the same addresses and routes (CI).
- [ ] 50 random commands typed in terminals, then 50 undos, give back the initial scenario.
- [ ] `first-network` is solved with the keyboard alone (GUI test).

**Decided here:** no Cisco IOS command line. Its modes and abbreviations are a product's behaviour, not a standard, and the labs are written for resend anyway. An IOS-flavoured dialect could come later as a second parser over the same edits.

## M9 — UDP and DHCP → 1.3 (50–80 h)

**Outcome:** applications have ports, and a host can get its address, mask, gateway and DNS server by itself.

- [ ] **M9.1 Contract** (`docs/models/udp-dhcp.md`).
- [ ] **M9.2 UDP** (RFC 768). `Ipv4Packet` carries ICMP or UDP; hosts have sockets bound to ports; the checksum covers the pseudo-header. A datagram for a port nobody listens on gets an ICMP Port Unreachable (3/3, RFC 1122 §4.1.3.1), and ICMP errors quote UDP datagrams. The limited broadcast 255.255.255.255 is delivered on the link and never forwarded (RFC 1812 §5.3.5.1).
- [ ] **M9.3 Bytes.** UDP and DHCP encoded, decoded and dissected with tshark's field names.
- [ ] **M9.4 DHCP client** (RFC 2131 §4.4): DISCOVER, OFFER, REQUEST and ACK from 0.0.0.0; the states from INIT to BOUND, RENEWING and REBINDING; renewal at half the lease and rebinding at seven eighths (§4.4.5); NAK; retransmission with the randomized backoff of §4.1, drawn from the scenario's seed, which is used here for the first time.
- [ ] **M9.5 DHCP server** (RFC 2131 §4.3), a service on a host or on a router port: pool, excluded addresses, lease time, and the options for the mask, the routers, the DNS servers, the lease and the server identifier (1, 3, 6, 51, 54 of RFC 2132). The leases appear in the inspector.
- [ ] **M9.6 Relay agent** on routers (RFC 1542 §4): one server for several subnets.
- [ ] **M9.7 Interface and terminal.** A host's address is static or from DHCP; the lease and its timers are shown.
- [ ] **M9.8 Labs**, with a new goal, `lease` (a host holds an address in a prefix, from a given server): `dhcp-first-lease` (guided: the four messages one by one), `dhcp-wrong-gateway`, `dhcp-pool-exhausted`, `dhcp-relay`.

**Exit checks**

- [ ] Comparison with Linux: a `dnsmasq` server and a `udhcpc` client in namespaces exchange the same message types in the same order, with the same addresses and the options above (CI). Timing may differ.
- [ ] A lease is renewed at T1, rebinds at T2, and the address is given up when it expires with the server gone (tests at exact simulated times).
- [ ] With two servers on one link the client takes the first offer, and both offers are in the trace.
- [ ] The frame view matches tshark on every frame of the new examples, and `tshark -z expert` stays clean.
- [ ] Frame conservation and the trace hashes hold on random networks with DHCP.

## M10 — DNS → 1.4 (30–50 h)

**Outcome:** names in place of addresses, and a query that can be followed from the root down.

- [ ] **M10.1 Contract** (`docs/models/dns.md`). Left out and said so: DNS over TCP, EDNS, DNSSEC.
- [ ] **M10.2 Messages** (RFC 1035 §4) over UDP port 53: header, question and resource records of type A, NS, CNAME and SOA, with name compression; encoded, decoded, dissected.
- [ ] **M10.3 Stub resolver** on hosts (RFC 1034 §5.3.1): servers from the configuration or from DHCP, a local table of names consulted first, retries. `ping` and `traceroute` take names.
- [ ] **M10.4 Authoritative server**, a service with zones edited in the inspector: answers, referrals with glue, name errors and empty answers (RFC 2308).
- [ ] **M10.5 Recursive resolver:** iteration from root hints (RFC 1034 §5.3.3), a cache that honours TTLs, negative caching (RFC 2308). Its cache appears in the inspector.
- [ ] **M10.6 Terminal:** `dig NAME [@SERVER]` and `+trace`.
- [ ] **M10.7 Example and labs.** `small-internet`: a root, a top-level domain, an authoritative server, a resolver and a LAN. Goal `resolve`. Labs: `dns-follow-a-query`, `dns-wrong-server`, `dns-missing-delegation`, `dns-stale-cache`.

**Exit checks**

- [ ] A second lookup within the TTL sends no query beyond the resolver; after the TTL it does (test at exact times).
- [ ] The exchange between a stub and its resolver matches `dig` against `dnsmasq` in namespaces: questions, answers, flags (CI).
- [ ] The frame view matches tshark on every DNS frame of `small-internet`.

## M11 — TCP → 2.0 (90–140 h)

**Outcome:** connections: the handshake, data that arrives whole and in order over a cable that loses frames, and an orderly close.

- [ ] **M11.1 Contract** (`docs/models/tcp.md`), on RFC 9293 and RFC 6298. Left out and said so: window scaling, selective acknowledgements, timestamps, delayed acknowledgements, congestion control (see [Later](#later)).
- [ ] **M11.2 Loss on cables.** A cable can drop the n-th frame, for lessons and tests, or each frame with a probability drawn from the scenario's seed; a lost frame is a drop with a reason of its own.
- [ ] **M11.3 Segments as bytes:** header, flags, the MSS option, the checksum with its pseudo-header.
- [ ] **M11.4 Connections** (RFC 9293 §3.3.2, §3.5, §3.6): the three-way handshake, a reset from a port nobody listens on, closing from either side, TIME-WAIT.
- [ ] **M11.5 Data:** sequence numbers and cumulative acknowledgements, segments no larger than the MSS, segments out of order put back in order, retransmission on the timer of RFC 6298 (RTO from the measured round trip, doubled at each timeout, Karn's rule), a fixed window.
- [ ] **M11.6 Sockets:** listen, connect, send, receive, close. Ephemeral ports come from the dynamic range 49152–65535 (RFC 6335 §6) in order, and initial sequence numbers from the seed, so a run repeats.
- [ ] **M11.7 Observation.** Each host's connections with their state, ports, sequence numbers and timer, in the inspector and with `ss`; `SYN`, `SYN-ACK`, `ACK`, `FIN` and `RST` on the canvas, retransmissions marked; a connection can be followed as a frame can.
- [ ] **M11.8 A test application and labs.** `nc` to listen, connect and send. Goal `connect`. Labs: `tcp-handshake`, `tcp-closed-port` (a reset, where UDP gets an ICMP error), `tcp-lost-segment`, `udp-and-tcp-under-loss`.

**Exit checks**

- [ ] 10,000 bytes arrive byte for byte over a cable that loses every fifth frame, and over random losses with 200 seeds.
- [ ] The retransmission timer takes the values computed by hand from RFC 6298 for a given series of round trips, and doubles at each timeout (test).
- [ ] Comparison with Linux on a transfer without losses: the same flags, relative sequence and acknowledgement numbers and lengths for the handshake, three segments of data and the close (CI). Linux's timers are not compared.
- [ ] `tshark -z expert` reports nothing but the retransmissions that were injected.
- [ ] Frames sent = delivered + dropped + lost + in flight, at every instant of random runs.

## M12 — HTTP and services → 2.0 (30–45 h)

**Outcome:** one host serves pages and another fetches them by name: DNS, TCP and HTTP in a single run, readable frame by frame.

- [ ] **M12.1 Contract** (`docs/models/http.md`): HTTP/1.1 (RFC 9110, RFC 9112) with GET, HEAD and POST, the statuses 200, 301 and 404, and one request per connection. Left out and said so: TLS, persistent connections, HTTP/2.
- [ ] **M12.2 Web server**, a service on a host: pages edited in the inspector (a path and its text), the port, redirects.
- [ ] **M12.3 Client:** `curl URL` in the terminal (`-I`, `-L`, `-d`), and a small page view in the inspector with an address bar. Names go through the host's resolver.
- [ ] **M12.4 Frame view:** the HTTP layer with tshark's field names; a connection can be read as text.
- [ ] **M12.5 Services:** what listens on which port of a host (`ss -ltn`).
- [ ] **M12.6 Labs**, with the goal `fetch` (a URL from a host gives a status and a text): `serve-a-page`, `page-by-name`, `wrong-port`, `follow-a-redirect`.

**Exit checks**

- [ ] `page-by-name` on a cold network shows, in order, ARP, the DNS query, the handshake, the request, the response and the close (regression case).
- [ ] The frame view matches tshark on every HTTP frame of the labs.
- [ ] The exported captures open in Wireshark with the conversation followed as a stream (by hand, once).

## M13 — NAT, port forwarding and packet filters → 2.1 (60–95 h)

**Outcome:** a private LAN reaches a public network through one address, a server behind the router is published on a port, and a router decides what may pass.

- [ ] **M13.1 Contract** (`docs/models/nat-filter.md`): RFC 3022 with the behaviours of RFC 4787 (UDP), RFC 5382 (TCP) and RFC 5508 (ICMP); Linux's netfilter where they leave a choice.
- [ ] **M13.2 Connection tracking** on routers: flows by addresses, protocol and ports, or the identifier for ICMP queries, with timeouts in simulated time. The table appears in the inspector.
- [ ] **M13.3 Source NAT** in the three forms of the linked lab: static one to one, dynamic from a pool (a pool that runs out is a failure one can see), and overload, where many hosts share one address and the ports tell them apart; the source port is kept when free and the next free one taken otherwise, as Linux does. Checksums are recomputed; ICMP errors are translated with the datagram they quote.
- [ ] **M13.4 Port forwarding:** a public port to a private address and port.
- [ ] **M13.5 Packet filter** on router ports, inbound and outbound: ordered rules on source and destination prefix, protocol and port; `accept`, `drop`, or `reject` with an ICMP Destination Unreachable (communication administratively prohibited, code 13, RFC 1812 §5.3.9); a match on established flows from M13.2; a default policy; a counter per rule.
- [ ] **M13.6 On the canvas** a translated packet shows its addresses before and after at the router, a filtered one stops with the rule that stopped it, and **Why** names the rule or the translation.
- [ ] **M13.7 Example and labs.** `home-and-provider`: two homes, both 192.168.1.0/24 (RFC 1918), behind two public addresses, and a provider with no route to private networks. Labs: `nat-static`, `nat-pool`, `nat-overload`, `port-forwarding` (publishing the web server of M12), `filter-web-only`, `filter-return-traffic`.

**Exit checks**

- [ ] Comparison with Linux: nftables source NAT, masquerade, destination NAT and filter rules in the router's namespace put the same addresses, ports and ICMP messages on every cable (CI).
- [ ] Two hosts using the same source port at the same time get two mappings, and each reply reaches its host (test).
- [ ] A mapping expires, and a later packet from outside is dropped (test at exact times).
- [ ] On random networks every reply to a translated flow is mapped back to the original addresses and ports.
- [ ] `tshark -z expert` stays clean: no checksum is wrong after a translation.

## M14 — VLANs → 2.2 (45–70 h)

**Outcome:** one switch carries several LANs, a trunk carries them between switches, and a router serves all of them on one cable.

- [ ] **M14.1 Contract** (`docs/models/vlan.md`): IEEE 802.1Q's tag and its rules on the way in and out of a port; Linux's bridge with VLAN filtering where the standard leaves a choice. Not modelled: VTP and DTP, double tags, switches that route.
- [ ] **M14.2 Switch ports:** access (one VLAN) or trunk (the VLANs allowed, the native VLAN untagged); learning and flooding per VLAN; VLAN ids from 1 to 4094 with names; switches with 8, 16 or 24 ports.
- [ ] **M14.3 Tagged frames** in the engine, in the bytes (TPID 0x8100) and in the frame view; a VLAN's frames share a colour on the canvas; a frame for a VLAN its port does not carry is a drop with a reason of its own.
- [ ] **M14.4 Router sub-interfaces:** one for each VLAN on a port (`p1.10`), each with its address.
- [ ] **M14.5 Inspector and terminal:** the port table of a switch; `bridge vlan`, and `ip link add link p1 name p1.10 type vlan id 10`.
- [ ] **M14.6 Labs:** `two-lans-one-switch`, `trunk-between-switches`, `native-vlan-mismatch`, `trunk-allowed-list`, `router-on-a-stick`, `branch-network` (VLANs, trunks, a router on a stick and static routes together), `same-vlans-three-switches`.

**Exit checks**

- [ ] Comparison with Linux: bridges with VLAN filtering and VLAN sub-interfaces put the same frames, with the same tags, on every cable (CI).
- [ ] Two hosts in different VLANs of one switch never receive each other's frames, broadcasts included (property over random networks).
- [ ] The frame view matches tshark on tagged frames; frame conservation and the trace hashes hold.

## M15 — Spanning tree → 2.3 (45–70 h)

**Outcome:** a loop of switches settles into a tree by itself. The storm guard stays for the switches where spanning tree is off.

- [ ] **M15.1 Contract** (`docs/models/stp.md`): the spanning tree protocol of IEEE 802.1D-1998, as Linux's bridge implements it, with one tree for every VLAN. Not modelled: RSTP at first (see [Later](#later)), and Cisco's tree per VLAN.
- [ ] **M15.2 BPDUs:** the first frames in resend that are not Ethernet II: 802.3 with an LLC header, sent to 01:80:C2:00:00:00; configuration and topology change BPDUs as bytes and in the frame view.
- [ ] **M15.3 The protocol:** bridge identifiers from a priority and a MAC, the election of the root, path costs, root and designated ports, the states blocking, listening, learning and forwarding, the timers (hello 2 s, max age 20 s, forward delay 15 s), and the shorter ageing after a topology change.
- [ ] **M15.4 Configuration:** spanning tree per switch, off unless turned on, with its priority, and a cost per port. Off by default because a BPDU every two seconds and thirty seconds before a port forwards would stand in front of every first lesson; the storm guard's message says what would have prevented the storm.
- [ ] **M15.5 Canvas and inspector:** the role and state of each port on its light, the root marked, a table per switch.
- [ ] **M15.6 Playback:** a stretch with nothing but periodic control frames counts as idle, so thirty seconds of listening and learning do not take thirty seconds to watch; the timeline can fold those frames.
- [ ] **M15.7 Labs:** `stp-who-is-root`, `stp-move-the-root`, `stp-which-port-blocks`, `stp-cut-a-cable`, `loop-with-and-without-stp`.

**Exit checks**

- [ ] Comparison with Linux: bridges with `stp_state 1` in namespaces elect the same root and give every port the same role and state, on five topologies, with the same BPDU fields (CI). Timing may differ.
- [ ] On 300 random networks with loops and spanning tree on everywhere, the storm guard never steps in once the tree has settled, and every pair of hosts can ping.
- [ ] After any one cable of a redundant network is cut, the tree settles again and every pair of hosts can ping (property).

## M16 — IPv6 → 2.4 (100–160 h)

**Outcome:** the same lessons with IPv6: link-local addresses, neighbours found with NDP, addresses from router advertisements, static routes.

- [ ] **M16.1 Contract** (`docs/models/ipv6.md`): RFC 8200, RFC 4291, RFC 4861, RFC 4862, RFC 4443, RFC 2464. Not modelled: DHCPv6, MLD, extension headers, fragmentation, temporary addresses (RFC 8981).
- [ ] **M16.2 Addresses and prefixes,** read and written as RFC 5952 asks; several per interface; the link-local address from the MAC (modified EUI-64, RFC 4291 appendix A), so a run repeats.
- [ ] **M16.3 Bytes:** the IPv6 header, ICMPv6 with its pseudo-header checksum, multicast on Ethernet (33:33, RFC 2464 §7). Switches flood multicast.
- [ ] **M16.4 NDP** (RFC 4861): neighbour solicitations and advertisements, the neighbour cache with its states (§7.3), router solicitations and advertisements, the default router list; duplicate address detection (RFC 4862 §5.4).
- [ ] **M16.5 Addresses on hosts:** static, or autoconfigured from advertisements (RFC 4862); advertisements configured on each router port.
- [ ] **M16.6 Forwarding:** the hop limit, static routes with the longest prefix match, ICMPv6 Time Exceeded and Destination Unreachable.
- [ ] **M16.7 Tools:** ping and traceroute over IPv6, the neighbour cache and the IPv6 routing table in the inspector, `ip -6`, AAAA records in DNS.
- [ ] **M16.8 Labs:** `ipv6-link-local`, `ipv6-static-addresses`, `ipv6-autoconfiguration`, `ipv6-static-routes`, `ipv6-duplicate-address`.

**Exit checks**

- [ ] Comparison with Linux: namespaces with `radvd` on the routers put the same NDP and ICMPv6 messages on every cable (CI); Linux's MLD reports are left out of the comparison, and the contract says so.
- [ ] The five router examples of M5 give the same outcomes when renumbered to IPv6 (regression cases).
- [ ] The frame view matches tshark; frame conservation and the trace hashes hold.

## M17 — OSPF → 2.5 (80–130 h)

**Outcome:** routers learn their routes from each other, and a cut link heals.

- [ ] **M17.1 Contract** (`docs/models/ospf.md`): OSPF version 2 (RFC 2328) in a single area. Not modelled: several areas, summaries, external routes, authentication, virtual links, OSPFv3.
- [ ] **M17.2 Multicast on a link:** 224.0.0.5 and 224.0.0.6 with their Ethernet addresses (RFC 1112 §6.4), IP protocol 89, the five OSPF packets as bytes and in the frame view.
- [ ] **M17.3 Neighbours:** hellos (§9.5), the checks that stop an adjacency (§10.5: mask, hello and dead intervals, area), the neighbour states, the election of the designated router and its backup (§9.4).
- [ ] **M17.4 The database:** description, request, update and acknowledgement packets, flooding (§13), router and network LSAs, ageing.
- [ ] **M17.5 Routes:** the shortest-path tree (§16.1) and OSPF routes in the routing table beside connected and static ones, in a fixed, declared order of preference.
- [ ] **M17.6 Configuration and observation:** router id, OSPF per port with its cost and timers, passive ports; the neighbours and the database in the inspector; the shortest-path tree of a router drawn on the canvas.
- [ ] **M17.7 Labs:** `ospf-first-adjacency`, `ospf-timers-mismatch`, `ospf-area-mismatch`, `ospf-passive-port`, `ospf-cost-and-path`, `ospf-cut-a-link`.

**Exit checks**

- [ ] On 200 random routed networks every router's OSPF routes equal the shortest paths computed by an independent Dijkstra in the tests; after a cable is cut they do again.
- [ ] Comparison with FRRouting's `ospfd` in namespaces, on five topologies: the same neighbours reach Full, the same designated routers are elected, the routing tables are equal (CI). Packets are not compared one by one: the timers have jitter.
- [ ] The frame view matches tshark on every OSPF packet of the labs.

## M18 — Troubleshooting collection → 2.6 (25–40 h)

**Outcome:** twenty broken networks of resend's own, each with one sentence of goal, across everything above, and a list of what the linked collections still need.

- [ ] **M18.1 Review.** Go through the linked troubleshooting labs and write down what each relies on. Whatever resend does not model becomes a candidate with an estimate: link aggregation (IEEE 802.1AX), a redundant first hop (VRRP), port security, DHCP snooping.
- [ ] **M18.2 Twenty labs** with one to three faults each, on different layers at once: a trunk that drops a VLAN and a missing route, a filter that blocks DNS, a pool that hands out the wrong gateway.
- [ ] **M18.3 Paths:** ordered sets of labs (first steps, addressing, switching, routing, services, troubleshooting), with what each one needs done first.

**Exit checks**

- [ ] `labs-verify` passes on every lab, and every row of the two tables at the top of this section has its lab.
- [ ] Three of the twenty are solved by someone other than their author, without help (by hand).

## Look, feel and play (G1–G7)

**Goal.** resend should feel like a game about networks: a pixel-art world, a HUD around it, movement and sound for everything that happens. And it stays the same instrument: nothing on the screen or in the speakers that the engine did not compute.

**Direction: a pixel world, crisp data.** The canvas, the frames of the HUD, the icons and the short labels are pixel art in three themes painted from one set of drawings: Blueberry, the default, Bubblegum, and Picnic, the light one. Each theme gives the same roles their colours, so a role keeps its meaning in all three (a warm tone for ARP, an aqua one for ICMP, violet for the simulator stepping in). Whatever is read at length or compared character by character, tables, the hex view, the terminal, the explanations, stays in a smooth font at 12 px or more: pixel fonts are sharp only at whole multiples of their size and tire the eye in long text.

**Where the interface is today** (0.3.0, measured): the canvas takes about a third of the window, 45 of the 75 font sizes in `ui/` are below 12 px, the ARP table in the inspector cuts its own cells, and the font is named (Inter) but not shipped, so the look depends on the machine. Nothing in `ui/` is animated by Slint: all motion comes from the C++ scene.

**What "gamified" means here.** Three layers:

1. **Feedback:** every record of the trace has a look and a sound (G3–G5).
2. **Progress that remembers:** a field guide of what you have seen happen, stamps for feats, the labs you solved (G6).
3. **Play:** a gremlin that breaks a network that works, for you to find out how (G7).

Left out on purpose: points, levels, streaks, timers, leaderboards, lives. They reward attendance and speed, and a learner who is afraid of losing a streak stops breaking things, which is how this is learned.

**Rules**

1. **Derived, never decorative.** Effects and sounds are a function of the trace and of the instant on screen, as the packets are today: dragging the timeline back shows and plays the same things. None of them can change a result.
2. **Nothing in sound alone, nothing in motion alone.** Each cue and each effect has a text beside it, a marker or an event. Effects can be full, reduced or off; sound has a volume and a mute key.
3. **Still when nothing happens.** Paused and untouched, the window is not redrawn.
4. **Assets are sources.** Sprites are indexed images on the palette, checked by a script; sounds are rows of parameters for a built-in synthesizer, with no audio files. Everything is resend's own under MIT, except the fonts (OFL).
5. **Each protocol brings its own.** From G6 on, a protocol milestone is finished when it also has its sprites, its cues and its field guide entries.

| # | Milestone | Hours | Delivers |
|---|---|---|---|
| G1 | Identity and foundations | 35–50 | Mock-ups, themes, type, sprite pipeline, square pixels at every scale |
| G2 | The HUD | 50–80 | The canvas as the stage, drawers, tabs, action bar, menus, first run |
| G3 | The pixel world | 40–65 | Devices, cables and packets as sprites with states |
| G4 | Game feel | 30–50 | Effects from the trace, interface motion, celebrations, intensity |
| G5 | Sound | 35–55 | A synthesizer, cues from the trace, a score one can read by ear |
| G6 | Progress | 40–65 | Field guide, stamps, home screen |
| G7 | The gremlin | 25–40 | Random faults by seed in any working network |

## G1 — Identity and foundations (35–50 h)

**Outcome:** one look in three themes, written down and enforced: palettes, type, sprites, and the rule that keeps pixels square.

- [x] **G1.1 Mock-ups** of the screens: the network in the middle of a ping, a lab with its goals, the inspector with its tables and the terminal. The direction was approved on them on 2026-11-10, before any code, and is recorded in [`docs/decisions/0003-visual-direction.md`](decisions/0003-visual-direction.md). The prototype of motion, sound, themes and terminal is in `design/motion-prototype/`.
- [x] **G1.2 Palettes:** three themes, Blueberry (the default), Bubblegum and Picnic (light), of 48 colours at most each, in one file (`assets/palettes.json`) built into the program and read by the look tests and, through `src/app_look.cpp`, by the `Theme` global of `ui/theme.slint`. Tokens are named by role (thirteen surfaces, four inks, and eight families of four tones: metal, accent, ok, link, ask, route, drop, guard), not by hue, and sprites are drawn by role, so one drawing serves every theme. The theme is a setting, remembered, in View ▸ Theme and the command palette; `--theme` chooses one for a session. Blueberry is based on Sweetie 16 by GrafxKid, credited in the README and in the About panel. *The limit was 32: eight families of four tones and the surfaces need more, so it is 48; Blueberry adds ten tones to Sweetie's sixteen, for the families Sweetie has no ramp for and for the grid and the shadow.*
- [x] **G1.3 Type:** Pixelify Sans for titles, HUD numbers and buttons; Inter for text and JetBrains Mono for data, all three built into the program (`assets/fonts/`, with their OFL texts) and in the `Type` global. Sizes: 12, 14, 16, 20, 28. *Pixelify Sans draws on a grid of 1/11 em, but its side bearings and its semibold strokes are off that grid, so no size is sharp, and at every size its C reads as an O, its 7 as a 1 and its 2 as an 8. Since 2026-11-11 it is for titles only, at 22 px; buttons, tabs, chips and captions are in Inter semibold, and names, port numbers and events in Inter or JetBrains Mono ([decision 0003](decisions/0003-visual-direction.md)). Every Italian letter, accented capitals included, is in all three fonts (checked on their character maps). Inter has no ⏻, which the engine's texts use for power: G3's marker plates draw it as a sprite.*
- [x] **G1.4 Sprite pipeline:** sheets in `assets/sprites/` as text, one character per art pixel naming its role (`legend.txt`), built into the program and painted per theme into the images of the `UiSprites` global, drawn with `image-rendering: pixelated`; icons drawn in ink are tinted with `colorize`. The look tests check sizes (multiples of 8, up to 64), rows, roles, duplicates and that the legend matches the code. *Changed from the plan: no image files and no `source-clip`, since one image per sprite painted at start needs neither; and panels and buttons are `PixelFrame`, rectangles with notched corners and a shade, instead of nine-slice images, so they take every theme's colours without a sheet per theme.*
- [x] **G1.5 Square pixels.** A sprite's scale is a whole number of physical pixels, chosen from the zoom and the window's scale factor (`look::art_pixel`, `look::canvas_art`); positions stay continuous. Below one physical pixel per art pixel the canvas draws the 16 px small set.
- [x] **G1.6 Widgets** restyled once in `ui/widgets.slint`: `Action`, `Chip`, `TextBox` and `Field`, `PanelHeader`, `Tabs`, `Scrubber` and `Table`, each with its hover, pressed, focused and disabled states.
- [x] **G1.7 The app icon** in the same style: a letter over an arrow, on the accent (at first two hosts and a frame between them), 32 px (`app-icon`) with a 16 px version that is also the HUD's logo. It is the window's icon, and `resend-icons` paints the PNGs of every size (16 to 256 px, enlarged by whole numbers) and the ICO at build time, for M6 to install.

**Exit checks**

- [x] No text below 12 px and no colour outside the tokens anywhere in `ui/` (look tests, in CTest). *Text on the canvas is drawn at a token's size times the zoom; G3 keeps it readable when zoomed out.*
- [x] Every pair of text and surface tokens in use has a contrast of at least 4.5:1 in each theme (WCAG 2, level AA; look tests over the palette file).
- [x] Changing the theme changes no size and no position (GUI test on the position and size of every labelled element).
- [x] At scale factors 1 to 3 and at every zoom, each sprite's edges fall on whole physical pixels (look tests on `art_pixel`, `canvas_art` and `snap`).
- [x] On a machine with no fonts installed the snapshot is the same: checked on 2026-11-10 by hiding every system and user font with `bwrap`, pixel for pixel. *Found on the way: fontique 0.11.1, which Slint 1.18.1 uses, stops the program when fontconfig knows no font at all, so on Linux `resend` points fontconfig at a configuration that adds the directory of its installed fonts (`src/main.cpp`).*
- [x] The GUI tests pass. *Not quite without a change: every label and role is kept, but the timeline starts closed now, so the tests open it first, and the test of folding panels measures their area instead of the canvas's width.*

## G2 — The HUD (50–80 h)

**Outcome:** the canvas is the stage and fills the window; everything else is a HUD around it that opens when it is needed.

- [x] **G2.1 Layout.** The canvas lies under everything. On the left a build bar of sprites with tooltips and keys, the zoom under it; on the right the inspector, a drawer that folds (I) and, with nothing selected, shows the lesson and the network; at the bottom the timeline, one row high, that opens into the events, **Why** and **Frame** (L); the document and undo, the clock, the command palette and the keys in the top corners. One key (H) hides it all. Fit frames the network in what the panels leave free, and new cards appear there. *The inspector stays where the learner puts it rather than opening on a selection, which made it jump; sound and effects join the corners with G4 and G5. Every button that shows only an icon has a tip with its key, drawn over the whole HUD. A window too narrow for the lab and the inspector side by side shows the one opened last; in a low one the events open beside the lab and the build bar; the zoom is two rows high so it fits under the build bar.*
- [x] **G2.2 Inspector in tabs:** Settings, Tables and, from M8, Terminal, each across the full width. Table columns take the width of the longest value the model allows (counted in characters of the monospace) and the table scrolls sideways.
- [x] **G2.3 Action bar.** Ping and traceroute are tools: the selected host is the source, and the destination is typed or picked on the canvas (right click ▸ Ping this device); P goes to the destination, T switches to traceroute, Space runs, `.` steps, `]` changes the speed and R resets. With little room the buttons keep their icons only. *The bar names the host that sends beside the destination, and Ping and Traceroute are two chips of one switch. The playback (Run, Step, the speed, Reset) is in the timeline's bar, beside the scrubber, and Run lights up when something waits to be played.*
- [x] **G2.4 Menus where the pointer is:** a right click on a device (ping from here, ping this device, connect a cable, power, delete), a cable (disable, unplug) or the background (add a device, fit); a command palette (Ctrl+K) that lists every action with its key and runs the one typed. *A packet's menu (follow) waits for G3's packet sprites, and Open the terminal for M8.*
- [x] **G2.5 Messages where they belong:** a refused value is explained under its field; undo, redo and saving answer with a toast; a dialog only when something would be lost. *Since 2026-11-11 every answer to an action is a toast, a ping that cannot start or an edit that is refused included: G2 had left them in a status the HUD did not show. What the screen shows by itself (an edit applied, a file opened, a theme) raises none.*
- [x] **G2.6 First run:** a welcome that offers a first ping on the sample network, an example or an empty network, once; empty states that say what to do next. *The first lab joins it with M7.*
- [x] **G2.7 Keys:** a map of every key on `?`, and every action reachable from the keyboard, as today.

**Exit checks**

- [x] At 1360 × 900 with a device selected, at least 55% of the window is canvas no panel covers (63%); with the HUD hidden, all of it (GUI test on the layout's numbers).
- [x] The whole interface still fits 960 × 540 (the GUI test of M4.10, extended).
- [x] Every action in the menus is in the command palette under the same accessible name (GUI test, reading the menus from `ui/app.slint`).
- [x] No table cell is cut at the default width with the longest values the model allows, such as 255.255.255.255/32 and ff:ff:ff:ff:ff:ff (GUI test).
- [ ] Three people build the sample network and send a ping without being told how. Each place where one hesitates for more than ten seconds is written down, then fixed or justified (by hand).

## G3 — The pixel world (40–65 h)

**Outcome:** devices, cables and packets are sprites with states, and can be told apart at a glance.

- [x] **G3.1 Devices:** host, switch and router at 32 px, each off, on, selected, sending, receiving and in error, painted from one drawing by turning roles into others (`look::in_state`); the scene says what each device does (`Scene::device_activity`). The port lights are sprites with four states and keep their meaning.
- [x] **G3.2 Packets:** a sprite for each kind of frame (ARP request and reply, Echo request and reply, Time Exceeded, Unreachable, and any other ICMP message), different in shape as well as in colour, at about one and a half times the zoom. Broadcast copies keep their ring; the selected packet shows its label.
- [x] **G3.3 Cables:** thick lines on a dark edge with their states (up, down, highlighted, selected), dashes that move with simulated time towards the receiver while a frame is on them (`resend::dashes`), port numbers on plates.
- [x] **G3.4 Markers:** the devices' decisions on small plates above them, with the texts they have today; power and the storm guard draw their symbol as a sprite, since the fonts have no ⏻.
- [x] **G3.5 The small set** (16 px) for the view from far away, and the grid as one opaque tile repeated. The texts of a card stay at 12 px or more: when the addresses do not fit, the card shows them without their prefix lengths.
- [x] **G3.6 [`docs/sprites.md`](sprites.md):** how sprites are written and painted, and what a new protocol must draw, a packet for each kind of message and a state for each role of a device.

**Exit checks**

- [x] Every kind of frame and every device state has a sprite (look tests over `pdu_kinds`, `device_states` and `port_states`: each paints a different image in every theme).
- [x] No two packet sprites are the same shape once reduced to one colour (look tests on the sheets).
- [x] 60 fps with 20 packets in flight on the GPU renderers, and 30 fps on the software renderer with 50 devices. *Measured on 2026-11-10 with `resend --bench`, which times whole frames drawn for a snapshot, read-back included, on a 948 × 1020 window: 50 devices with an ARP flood in flight take 28 ms with the software renderer (36 fps), 11.6 ms with FemtoVG and 17.6 ms with Skia. `SLINT_DEBUG_PERFORMANCE` gave no usable number under the tiling Wayland compositor, which throttles a window it does not show. The opaque grid tile took 5 ms off every software frame.*
- [x] The README's description of the canvas and the HUD is redone, with a snapshot (`docs/images/sandbox.png`).

## G4 — Game feel (30–50 h)

**Outcome:** everything that happens in the network gets a reaction, and everything the learner does gets an answer. Motion is fluid and lively, never stiff: eased, drawn at the display's rate, and overlapping, so that an arrival, the device's decision and the next departure read as one movement. `design/motion-prototype/` shows the feel to match.

- [x] **G4.1 Effects in the scene.** `build_scene()` gains an effects model computed from the records and the instant shown: a pop on arrival, a spark on the port that learns, a fan-out on a flood, a puff with its reason on a drop, a burst at the source when a reply comes back, a flash for the storm guard. An effect is a kind, a place, an age and a tone; Slint only draws it. *`src/app/effects.cpp`: seven kinds, each from the records of `effect_sources`, a failed ping shaking its host besides. C++ also computes the shapes, square rings, particles and flashes, and the nudges that make a device hop when a frame arrives; `ui/canvas.slint` draws them. The reason of a drop is on its marker, beside the puff. Lives are in simulated time, 3 to 10 ms; the playback now lets the last 12 ms after a record play at the normal pace (`PlaybackController::afterglow`) before it compresses a wait or stops, so that markers and effects end.*
- [x] **G4.2 Interface motion** with Slint's `animate` and states: drawers slide, buttons go down, goals tick, toasts come and go, in 120 to 200 ms. *The `Motion` global (`ui/theme.slint`) gives the durations, 120 and 180 ms, and none with effects off; drawers slide 24 px and fade in as they open, reduced effects keep only the fade.*
- [x] **G4.3 Moments:** a goal met, a lab solved. The celebration is short, then gets out of the way and shows what the network did: the frames that decided it, to play again. *A goal the last check met, and the one before had not, glows for 1.2 s. "Lab solved!" drops in over the free canvas with confetti for 2.2 s, driven by a timer only while it plays; then **Watch it again** in the panel pings again the first goal's destination from its host, on a fresh run as the check did, or, for a lab of observed goals, plays the learner's run from its start.*
- [x] **G4.4 Intensity:** full; reduced, without particles or shaking (today's Reduce motion); off, with markers and texts only. *View ▸ Effects, the button beside the clock and E; kept as `"effects"` in `settings.json`, which reads the old `"reduced-motion"` too.*
- [x] **G4.5 Budget:** at most 64 effects alive, the oldest going first, and none while paused. *And at most 320 shapes, particles going first. Paused, effects hold still with the instant shown, so nothing is redrawn.*

**Exit checks**

- [x] Dragging the timeline to an instant gives the same effects as playing up to it (test, as `travel_in_time` does for the trace). *`dragging_gives_what_playing_gave` in `tests/effects_tests.cpp`, on five networks: effects, shapes and nudges are equal.*
- [x] Every kind of effect comes from a kind of record listed in one table, and each of those records also has a marker or an event: with effects off, nothing is lost (test over the table). *`every_effect_comes_from_the_table` sees all seven kinds; `three_intensities` checks that the markers and packets are the same with effects off.*
- [x] The traces of the regression scenarios do not change by a byte. *Effects live in `src/app/`; the regression and random-network hashes are unchanged.*
- [ ] Paused and untouched for a second, the window is not redrawn (by hand, with a rendering notifier).

## G5 — Sound (35–55 h)

**Outcome:** you can hear a ping succeed, and hear where one dies. The cues of `design/motion-prototype/` are the starting point.

- [x] **G5.1 Audio output,** decided and recorded in `docs/decisions/`: Slint has none, so a small library for playback only (miniaudio, one file in the public domain, chosen as long as it serves the work), pinned and checked as the Slint SDK is, and a null output for tests and for machines without sound. *[Decision 0005](decisions/0005-audio-output.md): miniaudio 0.11.25, downloaded at the first configure into `.deps/` and checked against its SHA-256; built silent when offline or with `-DRESEND_AUDIO=OFF`. `src/audio/output.cpp` opens the default device, or gives the null output, which the tests, the snapshots and `--silent` use.*
- [x] **G5.2 Synthesizer:** square, triangle and noise voices with an envelope and a slide, eight voices mixed. A sound is a row of parameters in a table; there are no audio files. *`src/audio/synth.cpp`: 48 kHz mono, an 8 ms attack and an exponential decay, the oldest of eight voices giving way, and a soft limiter; the sounds are in `src/audio/sounds.cpp`.*
- [x] **G5.3 Cue engine** in `src/app/`, with no audio in it: records and the playback clock in, cues with their times out. *`src/app/cues.cpp`: `cue_sources` lists the records heard, `CueEngine` reads those the playback has just shown, `CueMixer` merges them in real time.*
- [x] **G5.4 The score.** Each hop of a frame plays the next note of a pentatonic scale, so pings that run together stay in tune. ARP asks with a rising pair of notes and is answered with a falling one; a drop is a low thud; the reply that reaches its source resolves the phrase; a goal and a solved lab have their jingles; the storm guard has an alarm of its own; the interface clicks softly. *A failed ping sinks a fourth. Hops count up the scale across every ping of a run, from C5.*
- [x] **G5.5 Limits:** a flood is one chord, not seven notes; cues of one kind less than 30 ms apart merge; nothing plays while the timeline is dragged; at 4× the notes shorten. *A chord has at most three notes; only records added by a playback command are heard, so a jump, a replay or an edit is silent; notes are 1/√speed as long above 1×.*
- [x] **G5.6 Settings:** a volume, a mute key and a button on the HUD, remembered; the first run shows where they are. No music. *View ▸ Sound (on, quiet, medium, loud), M, and the button beside the clock; `"sound"` and `"volume"` in `settings.json`. The welcome says the buttons beside the clock turn the effects and the sound down or off.*

**Exit checks**

- [x] The cues of five regression scenarios match golden files; a ping that succeeds ends on the resolving cue, and one that fails never plays it (test). *`tests/sound_tests.cpp` against `tests/golden/cues/`: the sample ping, two LANs and a router, the wrong gateway, the routing loop and the switch loop.*
- [x] The trace is the same with sound on, off, and with no audio device (test with the null output). *`sound_leaves_the_trace_alone`, and `sound_on_and_off` in the GUI tests, which runs a ping heard and unheard.*
- [x] Every kind of cue has its counterpart on screen (test over the table of G4). *`every_cue_is_seen`: each cue's record is an effect in the frame it plays, or, for ARP, the packet leaving its sender.*
- [x] Eight voices at full volume never clip (test on the mixer's samples).
- [x] resend starts and runs on a machine without an audio device (container check). *The smoke job starts the installed program in each container, where no sound library is installed, and checks it is still running after 5 s.*

## G6 — Progress: field guide, stamps, home (40–65 h)

**Outcome:** resend remembers what you have seen and solved, and shows it as a collection to complete, never as a score.

- [ ] **G6.1 Field guide.** An entry for every kind of frame, every decision of a device and every reason a frame stops, about 45 at 1.0. An entry opens the first time the thing happens in one of your runs, with its sprite, the explanation `explain` already gives, the RFC section, and where you first saw it; the events link to it. Each protocol shows how much of it you have seen.
- [ ] **G6.2 Stamps:** about twenty feats read from the trace or from a lab's verdict, most of them about exploring and breaking: the first ping across a router, a TTL that ran out, a broadcast storm, a lab solved from the terminal, a lab solved without hints.
- [ ] **G6.3 Home:** at start, the labs by topic with what is solved, the field guide, the last files, and the sandbox one click away.
- [ ] **G6.4 Storage:** one versioned JSON file in the state folder, with export, import and reset.
- [ ] **G6.5 The contract:** a later protocol adds its entries, and a test fails until it does.

**Exit checks**

- [ ] Every `TraceKind`, every `Reason` and every kind of frame has an entry (test over the enums).
- [ ] Each regression scenario opens exactly the entries listed for it (golden file).
- [ ] Each stamp is earned by a scripted run in the tests, and none by an empty one.
- [ ] The progress file holds no time, no count of attempts and no failure (test on its schema).

## G7 — The gremlin (25–40 h)

**Outcome:** troubleshooting without end: resend breaks one thing in a network that works, and you find it.

- [ ] **G7.1 Fault catalogue:** edits that break without removing anything: a cable down, a port disabled, a wrong mask, an address in another subnet, a gateway wrong or missing, a route removed or pointing the wrong way, a device powered off. Each protocol adds its own.
- [ ] **G7.2 The draw.** From a seed, `Rng` picks a fault that makes at least one pair of hosts that could ping unable to (checked by a probe) and that the learner can undo: its inverse exists (`edit::inverse()`).
- [ ] **G7.3 The round.** The goal is that every pair that could ping can again. The fault is kept out of the timeline and of undo until **Give up** shows it.
- [ ] **G7.4 Any network:** a lab's solution, an example, or your own file. The seed is shown, and typing it gives the same fault.
- [ ] **G7.5 The gremlin itself,** a small character on the HUD, the only one resend has.

**Exit checks**

- [ ] For 1,000 seeds on the example networks the fault breaks at least one pair, its inverse mends them all, and the same seed gives the same fault on every platform (test, with a constant hash).
- [ ] Every fault in the catalogue can be seen and undone through the interface (test over the catalogue).

## Later

Not needed by the checklist, in rough order: medium v2 (bandwidth, queues) and then TCP flow and congestion control (RFC 5681); RSTP; switches that route between VLANs; persistent HTTP connections; an IOS-flavoured dialect for the terminal; native file dialogs; macOS.

Deferred on purpose: dynamically loaded plugins (fragile C++ ABI), connecting to real networks, wireless, BGP, network collaboration, a scripting language, opening Packet Tracer files.

## Risks

1. **Explaining without inventing.** Every explanation must come from a record and the state it consulted. The `explain` module exists for this, and a case the model does not cover is said to be outside it, never presented as network behaviour; the storm guard's `SimulatorGuard` records are the example.
2. **Scope.** Each protocol gets a model contract: what is modelled, which RFC sections it follows, which simplifications it declares.
3. **Edits while running** (M3) are the hardest part to keep consistent, and the most instructive.
4. **Software rendering.** Without a GPU, Slint's software renderer drew 15–19 fps with 150 devices; semantic zoom must bring it above 30 fps or the toolkit choice is reopened (ADR 0002).
5. **Distribution** of a C++ application with a Rust runtime: glibc versions, library paths, MSVC, installers.
6. **Breadth after 1.0.** Twelve milestones and nine protocols. What protects them is the rule of M7 onwards: no protocol without its contract, its comparison with a real implementation and its labs. A milestone is cut down rather than faked.
7. **TCP** is the largest model and the easiest to get subtly wrong. Connections land before retransmission, and congestion control is left out of the checklist.
8. **Labs that mislead.** A goal met for the wrong reason teaches the wrong thing: hence probes from an empty state, locks, `labs-verify`, and trials with people who did not write the lab.
9. **What Packet Tracer users expect.** No IOS commands and no `.pkt` files. The README says so before anything else, and the first lab puts the video's commands next to resend's.
10. **Long, busy runs.** Spanning tree, OSPF and DHCP send frames for as long as the network is on, and going back in time replays a run from zero. Replaying may need checkpoints, built so that a replay from a checkpoint still equals the run, and tested as `travel_in_time` is today.
11. **Pixel art on fractional scaling.** At 1.25× or 1.5× not every pixel can be square. G1.5 picks the nearest whole scale; what it looks like on such a display is checked by hand.
12. **Who draws.** For now the sprites are generated by a script, by role, so they will be simple and consistent rather than beautiful. The pipeline takes better ones without a change to the code, and resend has one small character, not a cast.
13. **Too much.** Sounds and effects that delight for ten minutes can tire in an hour. The defaults are short and quiet, with no loops and no music; reduced and off are one key away; G4 and G5 are tried in sessions of an hour.
14. **The look before the substance.** G1–G7 are 255–405 hours that add no protocol. They are placed where they save work, before the lab panel, and wait their turn elsewhere.
