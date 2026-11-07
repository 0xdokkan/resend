# Roadmap

This is the plan from the current prototype to resend 1.0. Every milestone lists what it delivers, how it is broken down and the checks that close it. Boxes are ticked as the work lands on `main`; anything that cannot be checked automatically says so and stays open until it is checked by hand.

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
| M4 | Usable sandbox → **0.2** | 50–80 | Final canvas, save and undo, layered inspector, timeline filters, Italian translation |
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
- [x] **A8. Snapshots at a given instant.** `resend --snapshot out.ppm --ping 192.168.1.20 --at 15ms` renders the canvas at a simulated time, for documentation and visual checks.

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
- [x] **M4.9 Translation.** Interface strings in `@tr()` with bundled translations, explanation texts in a C++ catalog read from the same `.po` files, Italian as the first translation, language from the system with an override in View ▸ Language. `scripts/update-translations.py` brings the catalogs up to date with the source.
- [x] **M4.10 Layout:** collapsible palette, inspector and timeline, so the canvas keeps room at 2× scale: the window has no fixed minimum, and a 1080p screen at 2× (960 × 540 logical pixels) holds the whole interface with the side panels folded.

**Exit checks**

- [x] Saving and reopening gives an identical file. (`saving_and_opening` in `tests/files_tests.cpp` for every example; the GUI tests save, reopen and save again through the interface.)
- [x] 50 random commands followed by 50 undos give back the initial scenario. (`random_commands_and_undos` in `tests/workspace_tests.cpp`, running and not, then 50 redos give back the last one.)
- [x] The inspector's fields match tshark's for every frame of the example scenarios. (CTest `dissection-tshark`: position, size and value of every field, 405 frames, skipped when tshark is missing.)
- [x] Every interface string is translatable and the Italian translation is complete. (CTest `translations`: every `@tr()` and `tr()` text and every example's title and notes has an Italian entry with the same placeholders, and no entry is unused.)
- [ ] A 10-device network can be built in under 5 minutes (checked with 2–3 people, by hand).

## M5 — Routers and static routing (40–60 h)

**Outcome:** several subnets, and the classic routing mistakes become visible.

- [ ] **R1.** Router with 2–8 interfaces; hosts with a default gateway ARP for the gateway when the destination is off-link (RFC 1122 §3.3.1); the router decrements the TTL (RFC 1812 §5.3.1).
- [ ] **R2.** Connected, static and default routes; longest prefix match with overlapping /8, /16 and /24 (RFC 1812 §5.2.4.3, RFC 4632).
- [ ] **R3.** ICMP Time Exceeded (11/0) and Destination Unreachable for network (3/0) and host (3/1); no ICMP error about an ICMP error or a broadcast (RFC 792, RFC 1812 §4.3, RFC 1122 §3.2.2).
- [ ] **R4.** Traceroute over ICMP; a routing loop ends when the TTL runs out.
- [ ] Routing table and router configuration in the interface; router scenarios among the examples.

**Exit checks**

- [ ] Scenarios: two LANs and a router, wrong gateway, missing return route, routing loop, longest prefix.
- [ ] Comparison with Linux: a script builds the same topologies with network namespaces and captures with tcpdump; the sequence of packet types (ARP operation, ICMP type and code, TTL) matches the simulator's. Timing may differ.

## M6 — Distribution → 1.0 (35–55 h)

**Outcome:** people can install resend without building it.

- [ ] Install layout with the Slint library next to the executable (`$ORIGIN/../lib`), tar.gz and AppImage built on Ubuntu 22.04 (glibc 2.35, the Slint SDK's minimum), desktop file, icon and AppStream metadata.
- [ ] Windows: MSVC build of the whole app in CI, portable zip and an installer.
- [ ] Third-party licenses shipped with the binaries.
- [ ] Release workflow: tagged builds attached to a draft GitHub release.
- [ ] Container smoke tests: the AppImage starts and renders a snapshot on Ubuntu 22.04, 24.04 and Fedora with the software renderer.
- [ ] The same scenarios give the same trace hash on Linux and Windows (CI).
- [ ] Manual checklist on clean machines, including one without a GPU: install, open an example, ping, fault, save, reopen (by hand).
- [ ] Decided at this point: unsigned Windows installers trigger SmartScreen warnings; code signing has a cost.

### What 1.0 includes

- Devices: hosts with a gateway, switches with 8 ports, routers with 2–8 interfaces.
- Protocols: Ethernet II, ARP with a cache that follows RFC 1122, static IPv4 from /1 to /32, ICMP Echo, Destination Unreachable (network and host), Time Exceeded, static and default routes.
- Applications: ping and traceroute.
- Faults while running: cables, ports, power, moved cables, configuration changes, L2 loops with declared protection.
- Observation: animated playback, timeline with filters, layered and hex inspector, ARP, MAC and routing tables, pcap export.
- Documents: JSON files, undo and redo, example scenarios.
- Platforms: Linux x86_64 and Windows x64. Interface in English and Italian.

**Not in 1.0, and said so in the README:** UDP, TCP, DHCP, DNS, VLANs, STP, IPv6, NAT and ACLs, bandwidth, queues, MTU, dynamic routing, guided labs, plugins, macOS.

## After 1.0

The order follows the dependencies between protocols:

```
Ethernet/ARP/IPv4/ICMP
   ├──► routers ──► UDP ──► DHCP (+relay) ──► DNS
   │       │         └──► medium v2 (bandwidth, queues, loss) ──► TCP
   │       └──► IPv6 (NDP, SLAAC, ICMPv6)
   └──► storm protection ──► VLANs ──► STP/RSTP
```

| Release | Contents | Hours |
|---|---|---|
| 1.1 | UDP (RFC 768) and DHCP with relay (RFC 2131, 2132, 1542) | 50–80 |
| 1.2 | DNS: stub resolver, authoritative server, then recursion and cache (RFC 1034, 1035) | 30–50 |
| 1.3 | Medium v2: bandwidth, queues, loss | 30–45 |
| 2.0 | TCP: handshake, data, RTO, FIN, then window and congestion control (RFC 9293, 6298, 5681) | 120–190 |
| — | VLANs (access, trunk, router on a stick), then STP/RSTP (IEEE 802.1Q) | 80–130 |
| — | IPv6: addresses, NDP, SLAAC and DAD, static routing (RFC 4291, 4861, 4862, 8200) | 100–160 |
| — | Guided labs: a scenario plus goals checked on the trace | — |

Deferred on purpose: dynamically loaded plugins (fragile C++ ABI), an IOS-style CLI, connecting to real networks, wireless, OSPF and BGP, network collaboration, a scripting language.

## Risks

1. **Explaining without inventing.** Every explanation must come from a record and the state it consulted. The `explain` module exists for this, and a case the model does not cover is said to be outside it, never presented as network behaviour; the storm guard's `SimulatorGuard` records are the example.
2. **Scope.** Each protocol gets a model contract: what is modelled, which RFC sections it follows, which simplifications it declares.
3. **Edits while running** (M3) are the hardest part to keep consistent, and the most instructive.
4. **Software rendering.** Without a GPU, Slint's software renderer drew 15–19 fps with 150 devices; semantic zoom must bring it above 30 fps or the toolkit choice is reopened (ADR 0002).
5. **Distribution** of a C++ application with a Rust runtime: glibc versions, library paths, MSVC, installers.
