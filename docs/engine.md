# Simulation core

This document describes how resend simulates a network: the scenario that describes it, the simulation that runs it, the trace that records every step and the explanations derived from the trace, and the workspace through which the app changes, replays and undoes a run. It also lists the rules that keep the results deterministic. The plan for what comes next is in [`roadmap.md`](roadmap.md); the scenario file format is in [`scenario-format.md`](scenario-format.md).

## Layers

| Path | Contents |
|---|---|
| `src/core/` | Values with no behaviour: `Ipv4Address`, `Ipv4Prefix`, `MacAddress`, typed ids (`DeviceId`, `LinkId`, `PduId`), `SimTime` and durations, a seeded `Rng`, a strict JSON reader and writer, the `Problem` codes, and the translation catalogs (`text::`) |
| `src/scenario/` | What the user builds, saves and shares: `Scenario`, `validate()`, editing operations and their inverses (`edit::`) and the JSON format |
| `src/sim/` | The simulation: scheduler, layered frames, trace, the cables (`Medium`), the nodes and their modules |
| `src/wire/` | Frames as bytes (Ethernet II, ARP, IPv4, ICMP), their fields as Wireshark shows them, and pcap captures |
| `src/explain/` | Text for people: events built from the records, and descriptions of problems and reasons |
| `src/app/` | The workspace (commands, replay, undo), the canvas controller, playback, what the canvas shows at an instant, event filters and files; no Slint dependency |
| `src/app_controller.*`, `src/app_inspector.cpp`, `src/app_documents.cpp`, `ui/` | The Slint interface |
| `src/cli.cpp` | `resend-cli`, which runs a scenario, applies changes at given times, and prints the explanations or the trace or writes a capture |

Dependencies point downwards only: `core` depends on nothing, `scenario` on `core`, `sim` on both, `explain` and `wire` on `sim`, and the app on everything below it. `src/sim/` contains no user-facing text; everything else writes its texts in English and passes them through `text::tr()` (see [Explanations](#explanations)).

```
┌──────────────────────────── app (Slint + C++) ───────────────────────────┐
│ ui/*.slint ◄── models / callbacks ──► AppController                      │
│                                       CanvasController · build_scene     │
│                                       PlaybackController ─► Workspace    │
│                                       (commands · replay · History)      │
└──────────┬──────────────────────────────────────┬────────────────────────┘
           │ Edit values                          │ step / run_until / apply / query
┌──────────▼──────────────┐  Simulation(scenario)  ┌─▼─────────────────────────────┐
│ scenario                ├───────────────────────►│ sim                           │
│ devices, ports, cables, │                        │ Scheduler: Delivery, Timer    │
│ layout, notes, seed     │                        │ Medium: cables and carrier    │
│ validate() → issues     │                        │ Host: Nic · Arp · IPv4 · ICMP │
│ JSON, version 1         │                        │       · PingApp               │
└─────────────────────────┘                        │ Bridge (switch)               │
                                                   │ Trace: records, frames,       │
                                                   │        transmissions          │
                                                   └───────┬──────────────┬────────┘
                                                   ┌───────▼──────┐ ┌─────▼────────┐
                                                   │ explain:     │ │ wire: bytes, │
                                                   │ records →    │ │ pcap files   │
                                                   │ events       │ │              │
                                                   └──────────────┘ └──────────────┘
 Planned: Router and routing (M5).
```

## Scenario

A `Scenario` holds a title, notes, a seed, the devices and the cables (`src/scenario/scenario.hpp`).

- A **device** has an id, a kind (host or switch), a name, a power state, a position on the canvas and its **ports**: one for a host, eight for a switch. A port has a MAC address, an administrative state and, on hosts, an IPv4 address with its prefix (`Ipv4Prefix`, from /1 to /32; /31 and /32 follow RFC 3021). A switch also has its MAC **ageing time**, from 10 s to 1,000,000 s (300 s by default, as IEEE 802.1Q recommends).
- A **cable** joins two ports (`PortRef`: a device and a zero-based port), has a delay from 1 µs to 10 s (10 ms by default) and can be down.

`validate()` returns every problem that makes a scenario impossible to simulate, each with the device or cable it concerns: duplicate or zero ids, invalid names, a wrong number of ports, hosts without an address or with a network, broadcast, loopback or multicast address, addresses on switch ports, cables to missing devices or ports, two cables on one port, a cable between two ports of one device, delays or ageing times out of range, and more than 100 devices. Valid but questionable networks pass: two hosts with the same address, masks that disagree and Ethernet loops are lessons, not errors.

### Edits

A change to a network is a value, `Edit` (`src/scenario/edit.hpp`): a cable up or down (`SetLinkUp`), its delay (`SetDelay`), a port enabled or disabled (`SetPortUp`), power (`SetPowered`), a host's address (`SetAddress`), a switch's ageing time (`SetAgeing`), a name (`Rename`), a position (`MoveDevice`), a new cable (`Connect`), a cable removed (`Disconnect`) or moved to another port (`Replug`), a device added (`AddDevice`) or removed with its cables (`RemoveDevice`).

Two more edits exist for undo: `RestoreLink` puts back a cable as it was, at its place in the list, and `RestoreDevice` a device with the cables that were plugged into it. `edit::inverse(before, edit)` gives the edit that takes the scenario back to `before` once `edit` is made on it, exactly, down to the order of devices and cables: `MoveDevice` gives the old position, `Disconnect` a `RestoreLink`, `RemoveDevice` a `RestoreDevice`, `AddDevice` and `Connect` their removal. New devices and cables must carry their ids for that, and `Simulation::resolve()` gives an edit the ids `apply()` would choose, never used before in that simulation.

`edit::apply(scenario, edit)` makes the change, or leaves the scenario unchanged and returns a `Problem`. The other `edit::` functions are the shorthands the interface uses: `add_device` names a host `PC-<id>` and gives it 192.168.1.10/24 onwards, `connect` uses the first free port of each device, `configure_host` checks a name and an address together. Problems are codes; `explain::describe()` turns them into text.

## Simulation

`Simulation` (`src/sim/simulation.hpp`) is built from a scenario that passes `validate()`. From then on the network changes only through `apply(edit)`, at the current time.

### Events and episodes

The scheduler holds two kinds of entries: a **delivery** (a frame reaching the end of a cable) and a **timer** of a device (a ping starting, an ARP retry, an Echo timeout). `step()` runs the next entry; `run_until(t)` runs every entry due up to `t`, then moves the clock to `t`.

Everything a single entry causes is an **episode**, and its records carry the episode number. A frame arriving at a switch, for instance, gives one episode: the arrival, the learning, the decision and every copy sent. Explanations are built an episode at a time.

### The cables

`Medium` knows which port each cable joins, its delay, and whether a port has **carrier**: the cable is up, the ports at both ends are enabled and the sending device is powered. A frame sent from a port without carrier is dropped at the sender with `NoCarrier`. A frame that reaches a powered-off device is dropped there with `DeviceDown`.

Each cable has a **serial**, carried by every frame put on it. When the cable stops carrying (it goes down, a port at either end is disabled, an end is moved or unplugged, a device at an end is removed) it gets a new serial, and it keeps it when it carries again. A frame whose serial no longer matches when it reaches the end was on the cable when the connection broke: it is lost there with `LinkDown`, even if the cable carries again by then, as a real frame would be. A new delay applies to the frames sent after it.

### Edits while running

`apply(edit)` changes the network at the current time:

- Names and positions do not change how the network behaves: they are taken silently.
- Any other edit is an episode of its own, recorded with `EditApplied`, whose `value` is the edit's index in `edits()`. The log keeps each edit with its time and the device and cables it changed as they were before, so a removed device can still be named: `known_device(id)` finds it.
- A device that is powered off or removed stops at once: its timers are cancelled, its pings end (their waiting requests are lost with `DeviceDown`), and it forgets everything it learned. Powered on again, it starts with empty tables, as after a reboot.
- New devices and cables get ids never used before in this simulation, so the records about a removed one keep their meaning.
- A refused edit returns its `Problem` and changes nothing, not even the trace.

### Nodes

Each device is a node that receives frames and timers through a `Context`, which lets it read its configuration, archive frames, transmit them, schedule and cancel timers, and append records, always at the current time and at its own device.

- **Bridge** (the switch, IEEE 802.1D without spanning tree). It learns the source MAC on the ingress port (`MacLearned` when the entry is new, moves or had aged out), then floods broadcasts and unknown destinations out of every other port (`FrameFlooded`), sends known destinations out of the learned port (`FrameForwarded`, followed by `FrameDropped` with `NoCarrier` if that port has no active cable), or drops the frame when that is the ingress port (`FrameFiltered`). An entry not refreshed for the ageing time is gone: a frame for it finds it aged out (`MacEntryAged`) and is flooded. A switch keeps sending a moved host's frames to the old port until a frame from the host arrives on the new one or the entry ages out.
- **Host**, built from modules:
  - `Nic`: the port, its MAC and address. It accepts frames for its MAC or broadcast and drops the others with `NotForUs`.
  - `Arp` (RFC 826, RFC 1122 §2.3.2): the cache and the resolutions in progress, described below.
  - IPv4, limited to the local subnet: a destination outside it fails with `NoRoute` until routers exist (M5). Every datagram leaves with TTL 64, and datagrams are numbered per host as they leave, from 1.
  - ICMP echo responder (RFC 792): an Echo Request gets an Echo Reply with the same identifier, sequence and data. The reply goes through ARP like any datagram: a host that has no entry for the sender, after a reboot for example, asks for it first, and the reply waits. If that fails the reply is discarded (`DatagramDropped`). If the sender is outside the subnet there is no route back (`NoRoute`).
  - `PingApp`, described below.

### ARP

The cache maps addresses to MAC addresses; an entry expires a minute after its last update, the classic BSD timeout. Expiry is checked when the entry is needed: a datagram that finds its entry expired records `ArpEntryExpired` and resolves the address again.

- **Merge** (RFC 826): an ARP packet refreshes the entry of its sender when the receiver has one, or is resolving that address; a packet for the receiver's own address also creates the entry, since a conversation is about to start. A request for the receiver's address gets a unicast reply. Overheard requests for other addresses teach nothing.
- **Resolution** (RFC 1122 §2.3.2.1): a datagram whose next hop has no entry waits in an **incomplete** entry while the host broadcasts a request, repeated after a second without a reply, three attempts in all: at most one request per second per address. Any packet that merges the address releases the waiting datagrams at once, in order; after the third timeout they are lost with `ArpUnresolved`.
- **Queue** (RFC 1122 §2.3.2.2): at most three datagrams wait for one address. When a fourth comes, the oldest is discarded (`ArpQueueFull`), so the latest is always kept.

`arp_cache(device)` lists the reachable entries with their expiry and the incomplete ones with the number of datagrams waiting.

### Ping

`PingApp` works as the `ping` command. A ping sends `count` Echo Requests (1 to 1,000), one every `interval` (10 ms to 60 s, 1 s by default), with the ping's id as identifier, sequence numbers from 1 and 56 bytes of data. Each request waits `timeout` (10 ms to 60 s, 1 s by default) for its reply, counted from the moment it leaves the host, so the wait for ARP is not counted; its round trip is counted from the moment the ping sent it, so the first one includes ARP, as with `ping`.

Each request ends with `PingReply`, or with `PingLost` and the reason: `EchoTimeout`, `ArpUnresolved`, `ArpQueueFull`, `NoRoute` or `DeviceDown`. When every request has ended the ping ends with `PingSucceeded` if at least one was answered, as `ping`'s exit status, and with `PingFailed` otherwise, whose reason is the first loss. A reply that arrives after its request timed out is ignored (`FrameDropped` with `UnmatchedReply`), as a ping with a per-request timeout does. A ping to the host's own address is answered at once, without a frame.

### The storm guard

Ethernet frames have no TTL: in a loop of switches a broadcast is copied round for ever, both ways, and the switches keep learning its source on the wrong ports. Loops are allowed, so these lessons are possible, and the simulator protects itself. Without a loop a frame crosses each cable at most once, so a frame sent more than four times per cable, and at least 64 times (`storm_limit()`), is going round a loop. The guard then records `SimulatorGuard`, whose `value` is the number of copies sent, removes every copy of that frame still on the cables and refuses to send it again. The cut transmissions are marked `stopped`.

`SimulatorGuard` is the simulator stepping in, not network behaviour: explanations show it apart, in a colour of its own, and the interface pauses on it. Real networks prevent loops with spanning tree (STP), not implemented yet.

### Pings and tables

`start_ping(source, destination, problem, options)` starts a ping at the current time and returns its id, or refuses it with a `Problem`: the source is missing, is not a host, is powered off, the destination is not unicast, it is the network or broadcast address of the source's subnet, or the options are out of range. Several pings may run at once, from the same host or from different ones: pings from one host to the same address share one ARP resolution. `pings()` gives the state of each ping (running, succeeded or failed with a reason), its start and end times, and the fate of each request with its times.

`arp_cache(device)` and `mac_table(device)` return the tables at the current time, in address order; a MAC table entry has the time a frame from its address last arrived. `frames_in_flight()` counts the frames on the cables.

## Time and scheduler

`SimTime` counts nanoseconds in 64 bits: enough for about 292 simulated years, and exact for serialization delays (1500 bytes at 1 Gb/s take 12 µs). `parse_duration` reads values such as `15ms`, `250us` or `1.5s` exactly, without floating point, and `format_duration` writes them back.

`Scheduler<Payload>` (`src/sim/scheduler.hpp`) keeps its entries in a map ordered by time and then by scheduling order:

- entries due at the same time run in the order they were scheduled; an entry scheduled while running, for the current time, runs after the entries already queued for that time;
- `schedule(at, payload)` returns a handle; `cancel(handle)` removes the entry at once if it is still pending, so a cancelled timer never moves the clock;
- `next_time()` tells when the next entry is due; `pop()` removes it and moves the clock to its time;
- `advance_to(t)` moves the clock forward when nothing is due before `t`, so commands issued afterwards start from there.

## Trace

The trace is an append-only sequence of records, plus the archive of the frames the records refer to and the transmissions on the cables. A frame is archived once, when it is created, and keeps its `PduId` along its whole path: the copies a switch forwards or floods are the same frame, so a frame can be followed across the network by its id.

Frames are layered values (`src/sim/pdu.hpp`): an `EthernetFrame` carries an `ArpPacket` or an `Ipv4Packet`, which carries an `IcmpMessage`.

Every `FrameSent` also adds a **transmission**: the frame, the cable, the sending and receiving device and port, the instant it leaves and the instant it arrives, and the position of its `FrameSent` record. Transmissions are kept in the order they start. They are what the animation interpolates; what happens on arrival is in the records at that instant. The storm guard is the only thing that changes one afterwards: it marks it `stopped` and moves its arrival to the instant it was removed.

At every instant, frames sent = frames received or dropped on arrival (`LinkDown`, `DeviceDown`) + frames the guard stopped + `frames_in_flight()`.

Records contain no text, so the tests check facts, and rewording or translating the explanations does not break them.

### Fields

| Field | Meaning |
|---|---|
| `time` | Simulated time of the fact |
| `seq` | Position in the trace, from 0 |
| `episode` | The scheduled entry that caused the record, from 1 |
| `kind` | What happened |
| `reason` | Why; `None` when the fact needs no reason |
| `device` | The device where it happened |
| `port` | Port of `device`, from 0. For switch decisions it is the ingress port; the egress ports appear in the `FrameSent` records that follow |
| `link` | The cable crossed, if any |
| `pdu` | The frame involved, if any |
| `value` | The attempt for ARP requests sent or dropped and for `ArpTimeout`; the ping's id for every `Ping*` record; an IPv4 address, as 32 bits, for `ArpEntryExpired` (the expired entry) and `DatagramDropped` (the datagram's destination); the edit's index in `edits()` for `EditApplied`; the copies sent for `SimulatorGuard`; 0 otherwise |

### Record kinds

| `TraceKind` | When | Reference |
|---|---|---|
| `PingStarted` | A host starts a ping | |
| `FrameSent` | A frame leaves a port onto a cable with carrier | |
| `FrameReceived` | A frame arrives at a port of a powered device | |
| `FrameDropped` | The frame stops here; the reason says why | |
| `MacLearned` | The switch binds the source MAC to the ingress port (new, moved or aged out) | IEEE 802.1Q, learning |
| `MacEntryAged` | The destination's entry was not refreshed for the ageing time: the switch floods | IEEE 802.1Q, ageing |
| `FrameFlooded` | Broadcast or unknown destination: the switch sends the frame out of every other port | IEEE 802.1Q, forwarding |
| `FrameForwarded` | Known destination: the switch sends the frame only out of the learned port | IEEE 802.1Q, forwarding |
| `FrameFiltered` | The learned port is the ingress port: the switch sends nothing | IEEE 802.1Q, filtering |
| `ArpCacheUpdated` | The receiver maps the ARP sender's IP address to its MAC | RFC 826, "merge" step |
| `ArpTimeout` | The retransmission interval passes without a reply; `port` is the interface | RFC 1122 §2.3.2.1, at most one request per second |
| `ArpEntryExpired` | A host needs an entry and finds it expired | RFC 1122 §2.3.2.1, cache timeout |
| `DatagramDropped` | A datagram that is not a ping request cannot leave: an Echo Reply whose destination ARP did not resolve | |
| `PingReply` | An Echo Reply answers one request of a ping | RFC 792 |
| `PingLost` | One request of a ping gets no reply; the reason says why; `pdu` is the request when it was sent | |
| `PingSucceeded` | The ping ends with at least one reply | |
| `PingFailed` | The ping ends without any reply; the reason is its first loss | |
| `EditApplied` | The network was edited while running; `device`, `port` and `link` name what changed | |
| `SimulatorGuard` | The storm guard stopped a frame: not network behaviour | |

### Reasons

| `Reason` | Meaning | Used with |
|---|---|---|
| `LinkDown` | The cable stopped carrying while the frame was on it: disabled, unplugged, moved, or a port at either end disabled | `FrameDropped` |
| `DeviceDown` | The receiving device is powered off; for a ping, its source was powered off or removed | `FrameDropped`, `PingLost`, `PingFailed` |
| `NoCarrier` | The egress port has no cable with carrier | `FrameDropped` |
| `NotForUs` | The destination MAC is neither the local MAC nor broadcast | `FrameDropped` |
| `ArpForOtherHost` | The ARP request asks for an address the receiver does not own | `FrameDropped` |
| `Ipv4NotForUs` | The destination IPv4 address is not the local address | `FrameDropped` |
| `NoRoute` | The destination is outside the subnet and there are no routers yet. With `PingLost` it concerns the request, with `FrameDropped` the reply | `PingLost`, `PingFailed`, `FrameDropped` |
| `ArpUnresolved` | Three ARP requests without a reply | `PingLost`, `PingFailed`, `DatagramDropped` |
| `ArpQueueFull` | Newer datagrams for the same unresolved address took its place | `PingLost`, `PingFailed`, `DatagramDropped` |
| `EchoTimeout` | No matching Echo Reply within the timeout | `PingLost`, `PingFailed` |
| `UnmatchedReply` | An Echo Reply that no request is waiting for: it timed out, or its ping ended | `FrameDropped` |

### Example: the first ping of the sample network

PC-A (1) port 1 ─ cable 1 ─ port 1 Switch-01 (3) port 2 ─ cable 2 ─ port 1 PC-B (2). In the table, ports and frames are numbered from 0, as in the trace. [`tests/golden/01-warm-caches.txt`](../tests/golden/01-warm-caches.txt) holds this run in full, with the following pings.

| ms | Episode | Records | Device, port | Frame |
|---|---|---|---|---|
| 0 | 1 | `PingStarted`, `FrameSent` | PC-A, 0 | 0: broadcast ARP Request |
| 10 | 2 | `FrameReceived`, `MacLearned`, `FrameFlooded` | Switch, 0 | 0 |
| 10 | 2 | `FrameSent` | Switch, 1 | 0 |
| 20 | 3 | `FrameReceived`, `ArpCacheUpdated` | PC-B, 0 | 0 |
| 20 | 3 | `FrameSent` | PC-B, 0 | 1: unicast ARP Reply |
| 30 | 4 | `FrameReceived`, `MacLearned`, `FrameForwarded` | Switch, 1 | 1 |
| 30 | 4 | `FrameSent` | Switch, 0 | 1 |
| 40 | 5 | `FrameReceived`, `ArpCacheUpdated` | PC-A, 0 | 1 |
| 40 | 5 | `FrameSent` | PC-A, 0 | 2: Echo Request |
| 50 | 6 | `FrameReceived`, `FrameForwarded` | Switch, 0 | 2 |
| 50 | 6 | `FrameSent` | Switch, 1 | 2 |
| 60 | 7 | `FrameReceived` | PC-B, 0 | 2 |
| 60 | 7 | `FrameSent` | PC-B, 0 | 3: Echo Reply |
| 70 | 8 | `FrameReceived`, `FrameForwarded` | Switch, 1 | 3 |
| 70 | 8 | `FrameSent` | Switch, 0 | 3 |
| 80 | 9 | `FrameReceived`, `PingReply`, `PingSucceeded` | PC-A, 0 | 3 |

A second ping finds both caches filled: ICMP only, no `MacLearned` or `ArpCacheUpdated`, 40 ms in all.

## The workspace

The app does not change the simulation directly: a `Workspace` (`src/app/workspace.hpp`) holds the latest network (its *design*, the one saved to a file), the simulation running it, the commands made on it and the undo history. Tests drive it as the interface does.

- **Before anything has happened** an edit simply gives a new network: the simulation is rebuilt from the edited scenario and nothing is recorded.
- **Once the simulation has run**, an edit or a ping is a *command* made at the current instant: the simulation applies it (`Simulation::apply()`) and the workspace records it with its time.
- **Going back in time** (`travel()`) builds a new simulation of the network as it was at time zero and replays the commands made up to the chosen instant. The simulation is deterministic, so the replay is the same run: `travel_in_time` in `tests/workspace_tests.cpp` checks that the trace up to every instant matches a run made directly up to it. Going forward runs the simulation on, and the playback makes the recorded commands again as it passes their instant.
- **While commands lie ahead** of the instant shown (`in_past()`), changes, pings, undo and redo are refused: the past cannot be edited without discarding the future, and the interface says so. Back at the latest instant, a change discards only what the simulation had computed after it.
- **Undo and redo** (`History`, `src/app/history.hpp`) keep each step's edits and their inverses from `edit::inverse()`, so undoing gives back the very same scenario; steps made during a gesture, such as a drag, join into one. Undo is itself a change: before anything happened it rebuilds the network, while running it makes the inverse edit at the current instant. Each step has a state number, and the network is modified when that number differs from the one saved.
- `generation()` changes whenever a new simulation replaces the old one, so the app knows to start its events and playback over.

`random_commands_and_undos` makes 50 random commands, running and not, then 50 undos that must give back the initial scenario and 50 redos that must give back the last one.

## Bytes and captures

`wire::encode()` (`src/wire/wire.hpp`) gives the bytes of a frame in network order, as they would cross the cable without preamble and FCS: Ethernet II; ARP for IPv4 over Ethernet (RFC 826); IPv4 with a 20-byte header, no options, no flags and its checksum (RFC 791); ICMP with its checksum (RFC 792), and echo data 0x00, 0x01, ..., the pattern `ping` uses after its timestamp. Frames shorter than 60 bytes are padded with zeros. `wire::decode()` reads them back, refusing anything the model cannot represent, and `wire::checksum()` is the Internet checksum (RFC 1071).

`wire::dissect()` lists the layers and fields of those bytes as Wireshark shows them: for each field its label, its display-filter name (`eth.dst`, `arp.opcode`, `ip.checksum`, `icmp.seq`), its offset and length, its value printed as tshark prints it and what the value means ("Broadcast", "request"). The frame view of the app shows exactly these rows. CTest `dissection-tshark` (`tests/dissect_tshark.cpp`) compares them with tshark's own dissection, position, size and value, for every frame of every example scenario; it is skipped when tshark is missing.

`wire::write_pcap()` writes a capture in the pcap format with nanosecond timestamps (link type Ethernet): every transmission in the order it started, for the whole network or one cable, at the simulated time it left. Simulated time 0 is the Unix epoch. A frame crossing three cables appears three times, as with a capture on every cable; `resend-cli --pcap FILE [--pcap-link ID]` writes one. Wireshark's expert system reports no errors or warnings on these files, checksums included (CTest `pcap-wireshark-expert`).

## Explanations

`explain::Explainer` (`src/explain/explain.hpp`) turns the records into the event list of the interface and the CLI: a time, a protocol, a title, a detail, the devices and the cable involved, the frame, and whether it went well. It works an episode at a time, on the records added since its last call:

- an episode that starts with a frame at a port is one event: the frame, the ingress port, and everything the device did about it;
- an episode that starts with a timer gives an event per fact: an ARP request sent ("who has…?"), a retry, a request lost, a ping that ends without a frame. An Echo Request sent from a filled cache has no event of its own; the arrivals that follow tell its story;
- an edit is one event, under the protocol "EDIT", that says what changed and what follows from it;
- a `SimulatorGuard` record is an event of its own under the protocol "SIMULATOR", marked `simulator`, never mixed with the network's events.

`describe(Problem)` and `describe(Reason)` explain refusals and failures; `name()` gives the identifiers printed by `resend-cli --trace`, which are never translated. The short labels on the canvas ("flood ×2", "× not my MAC") are part of the app (`src/app/scene.cpp`).

Every text is written in English and passed through `text::tr()` (`src/core/text.hpp`), with `{0}`, `{1}`... for the values: `tr("{0} powered off", {name})`. `tr()` looks the English text up in the catalog of the current language, the same gettext `.po` files in `translations/` that Slint bundles into the interface, built into the program; a text without a translation stays in English. Plurals are chosen in the code, with a text for each form. The CLI never changes the language, so its output, which the tests compare with golden files, is always English. CTest `translations` checks that every text, in the C++ code, in the `.slint` files and in the titles and notes of the examples, has a translation in every built-in language with the same placeholders.

Until milestone M2 the prototype's engine wrote its own texts. The new core reproduced them exactly, record by record, frame by frame and text by text, in fourteen scenarios before that engine was removed; the same scenarios are now the regression tests.

## Determinism rules

Given the same scenario and the same commands at the same times, the trace must be identical on every platform and compiler.

- The order of events depends only on time and scheduling order, never on memory addresses or real time.
- No floating point in the simulation: scenario positions are whole canvas units, and the canvas's own coordinates stay in the app.
- Unordered containers are used only for lookup, never iterated in an order that could affect the result. The scheduler, for example, uses an `unordered_map` only to find the time of an entry being cancelled.
- Random numbers: `Rng` uses `std::mt19937_64`, whose output the standard fixes, and derives ranges itself, because the `std::uniform_*_distribution` classes give different values on libstdc++, libc++ and MSVC.
- Going back in time replays the run on a new simulation; it never restores a copy, so a replay that differed from the run would show up in the tests.
- Playback speed and animations read the simulation; they never drive it. `playback_never_changes_the_result` in `tests/app_tests.cpp` plays the same ping at 0.25× and 4×, with small and large ticks, and compares the traces.
- `random_topologies` in `tests/sim_tests.cpp` builds 1,000 random loop-free networks with random faults and pings, and `random_live_networks` in `tests/live_tests.cpp` 300 networks, loops included, edited at random while they run. Each hashes everything recorded into one number, which must match a constant on every CI platform and compiler. The constants change only when the model does, and then on purpose. Tests draw random numbers one per statement, since compilers evaluate function arguments in different orders.

## Tests

| Test | What it checks |
|---|---|
| `tests/core_tests.cpp` | Addresses, prefixes, ids, the scheduler, durations, `.po` catalogs and `tr()` |
| `tests/scenario_tests.cpp` | The JSON reader and writer, `Rng`, every validation rule, the edits and their exact inverses, the example files byte for byte, errors in files |
| `tests/sim_tests.cpp` | Concurrent pings, shared ARP resolutions, refused pings, carrier and delays, episodes, /31 and /32, the lesson of each example file, the 1,000-network hash |
| `tests/live_tests.cpp` | Edits while running (cables, delays, ports, power, removal, ids), ARP states, expiry, the merge rule and the queue, MAC ageing, replies that resolve the sender, the ping application, late replies, the storm guard, and conservation of frames at every step of 300 random edited networks |
| `tests/wire_tests.cpp` | Checksums, known frames byte for byte, round trips, refused bytes, pcap files, the fields of known frames |
| `tests/regression_tests.cpp` | Twenty-three scenarios, the prototype's fourteen and nine for live faults, against the golden files in `tests/golden/`; `RESEND_UPDATE_GOLDEN=1` rewrites them after an intended change, to be reviewed in the diff |
| `tests/app_tests.cpp` | Playback and what the canvas shows, edits and storms included |
| `tests/canvas_tests.cpp` | The canvas controller: view, hit testing, dragging, cables, removal, refusals in the past |
| `tests/workspace_tests.cpp` | Edits before and after the first event, gestures, commands, travel in time, event filters, 50 random commands and their undos |
| `tests/files_tests.cpp` | Saving and opening byte for byte, the built-in examples, autosave and recovery, the state folder, the file dialog's listing |
| `tests/translation_tests.cpp` | Every text translated in every built-in language, with the same placeholders, and no unused entries |
| `tests/gui_tests.cpp` | The interface on Slint's testing backend |
| CTest `cli-*` | `resend-cli` exit codes and output, scenario files, concurrent pings, timed changes, loops |
| CTest `pcap-wireshark-expert` | Captures checked by `tshark -z expert`; skipped when tshark is missing |
| CTest `dissection-tshark` | Every field of every frame of the examples compared with tshark's dissection; skipped when tshark is missing |
