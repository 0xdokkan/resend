# Simulation core

This document describes how resend simulates a network: the scenario that describes it, the simulation that runs it, the trace that records every step and the explanations derived from the trace. It also lists the rules that keep the results deterministic. The plan for what comes next is in [`roadmap.md`](roadmap.md); the scenario file format is in [`scenario-format.md`](scenario-format.md).

## Layers

| Path | Contents |
|---|---|
| `src/core/` | Values with no behaviour: `Ipv4Address`, `Ipv4Prefix`, `MacAddress`, typed ids (`DeviceId`, `LinkId`, `PduId`), `SimTime` and durations, a seeded `Rng`, a strict JSON reader and writer, and the `Problem` codes |
| `src/scenario/` | What the user builds, saves and shares: `Scenario`, `validate()`, editing operations (`edit::`) and the JSON format |
| `src/sim/` | The simulation: scheduler, layered frames, trace, the cables (`Medium`), the nodes and their modules |
| `src/explain/` | English text: events built from the records, and descriptions of problems and reasons |
| `src/app/` | Playback and what the canvas shows at an instant; no Slint dependency |
| `src/app_controller.*`, `ui/` | The Slint interface |
| `src/cli.cpp` | `resend-cli`, which runs a scenario and prints the explanations or the trace |

Dependencies point downwards only: `core` depends on nothing, `scenario` on `core`, `sim` on both, `explain` on `sim`, and the app on everything below it. `src/sim/` contains no user-facing text.

```
┌──────────────────────────── app (Slint + C++) ───────────────────────────┐
│ ui/app.slint ◄── models / callbacks ──► AppController                    │
│                                         PlaybackController · build_scene │
└──────────┬──────────────────────────────────────┬────────────────────────┘
           │ edit::…, then rebuild                │ step / run_until / query
┌──────────▼──────────────┐  Simulation(scenario)  ┌─▼─────────────────────────────┐
│ scenario                ├───────────────────────►│ sim                           │
│ devices, ports, cables, │                        │ Scheduler: Delivery, Timer    │
│ layout, notes, seed     │                        │ Medium: cables and carrier    │
│ validate() → issues     │                        │ Host: Nic · Arp · IPv4 · ICMP │
│ JSON, version 1         │                        │       · PingApp               │
└─────────────────────────┘                        │ Bridge (switch)               │
                                                   │ Trace: records, frames,       │
                                                   │        transmissions          │
                                                   └──────────────┬────────────────┘
                                                   ┌──────────────▼────────────────┐
                                                   │ explain: records → events     │
                                                   └───────────────────────────────┘
 Planned: Router and routing (M5), edits while running and pcap export (M3).
```

## Scenario

A `Scenario` holds a title, notes, a seed, the devices and the cables (`src/scenario/scenario.hpp`).

- A **device** has an id, a kind (host or switch), a name, a power state, a position on the canvas and its **ports**: one for a host, eight for a switch. A port has a MAC address, an administrative state and, on hosts, an IPv4 address with its prefix (`Ipv4Prefix`, from /1 to /32; /31 and /32 follow RFC 3021).
- A **cable** joins two ports (`PortRef`: a device and a zero-based port), has a delay from 1 µs to 10 s (10 ms by default) and can be down.

`validate()` returns every problem that makes a scenario impossible to simulate, each with the device or cable it concerns: duplicate or zero ids, invalid names, a wrong number of ports, hosts without an address or with a network, broadcast, loopback or multicast address, addresses on switch ports, cables to missing devices or ports, two cables on one port, a cable between two ports of one device, delays out of range, more than 100 devices, and Ethernet loops. Loops are refused until storm protection exists (M3). Valid but questionable networks pass: two hosts with the same address, or masks that disagree, are lessons, not errors.

The `edit::` functions apply the interface's rules: `add_device` names a host `PC-<id>` and gives it 192.168.1.10/24 onwards, `connect` uses the first free port of each device and refuses loops, and every refused edit leaves the scenario unchanged and returns a `Problem`. Problems are codes; `explain::describe()` turns them into text.

## Simulation

`Simulation` (`src/sim/simulation.hpp`) is built from a scenario that passes `validate()` and never changes it: an edit in the interface builds a new simulation. Moving a card is the exception, since positions do not take part in the simulation. Edits applied while the simulation runs come with M3.

### Events and episodes

The scheduler holds two kinds of entries: a **delivery** (a frame reaching the end of a cable) and a **timer** of a device (a ping starting, an ARP retry, an Echo timeout). `step()` runs the next entry; `run_until(t)` runs every entry due up to `t`, then moves the clock to `t`.

Everything a single entry causes is an **episode**, and its records carry the episode number. A frame arriving at a switch, for instance, gives one episode: the arrival, the learning, the decision and every copy sent. Explanations are built an episode at a time.

### The cables

`Medium` knows which port each cable joins, its delay, and whether a port has **carrier**: the cable is up, the ports at both ends are enabled and the sending device is powered. A frame sent from a port without carrier is dropped at the sender with `NoCarrier`. A frame that reaches a powered-off device is dropped there with `DeviceDown`.

### Nodes

Each device is a node that receives frames and timers through a `Context`, which lets it read its configuration, archive frames, transmit them, schedule and cancel timers, and append records, always at the current time and at its own device.

- **Bridge** (the switch, IEEE 802.1D without spanning tree). It learns the source MAC on the ingress port (`MacLearned` when the entry is new or moves), then floods broadcasts and unknown destinations out of every other port (`FrameFlooded`), sends known destinations out of the learned port (`FrameForwarded`), or drops the frame when that is the ingress port (`FrameFiltered`). Entries do not age yet (M3).
- **Host**, built from modules:
  - `Nic`: the port, its MAC and address. It accepts frames for its MAC or broadcast and drops the others with `NotForUs`.
  - `Arp` (RFC 826): the cache and the resolutions in progress. A known sender is refreshed by any ARP packet; a packet for the host's own address also teaches it the sender, and a request for that address gets a reply. A datagram whose next hop is unknown waits in a queue for that address while the host sends a broadcast request, repeated after a second up to three attempts (RFC 1122 §2.3.2.1). Any cache update for that address releases the queue at once; after the third timeout the waiting pings fail with `ArpUnresolved`.
  - IPv4, limited to the local subnet: a destination outside it fails with `NoRoute` until routers exist (M5). Datagrams are numbered per host as they leave, from 1.
  - ICMP echo responder (RFC 792): an Echo Request gets an Echo Reply with the same identifier and sequence. If the sender is not in the ARP cache the request is dropped with `NotModelled`; resolving it first comes with M3. If the sender is outside the subnet there is no route back (`NoRoute`).
  - `PingApp`: each ping sends one Echo Request whose identifier is the ping's id and whose sequence is 1. It succeeds when the matching Echo Reply arrives within a second of the request leaving (`EchoTimeout` otherwise). A ping to the host's own address succeeds at once, without a frame.

### Pings

`start_ping(source, destination, problem)` starts a ping at the current time and returns its id, or refuses it with a `Problem`: the source is missing, is not a host, is powered off, the destination is not unicast, or it is the network or broadcast address of the source's subnet. Several pings may run at once, from the same host or from different ones: two pings from one host to the same address share one ARP resolution. `pings()` gives the state of each ping (running, succeeded or failed with a reason) with its start and end times.

`arp_cache(device)` and `mac_table(device)` return the tables in address order.

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

Every `FrameSent` also adds a **transmission**: the frame, the cable, the sending and receiving device and port, the instant it leaves and the instant it arrives, and the position of its `FrameSent` record. Transmissions are kept in the order they start. They are what the animation interpolates; what happens on arrival is in the records at that instant.

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
| `value` | The attempt for ARP requests sent or dropped and for `ArpTimeout`; the ping's id for `PingStarted`, `PingSucceeded` and `PingFailed`; 0 otherwise |

### Record kinds

| `TraceKind` | When | Reference |
|---|---|---|
| `PingStarted` | A host starts a ping | |
| `FrameSent` | A frame leaves a port onto a cable with carrier | |
| `FrameReceived` | A frame arrives at a port of a powered device | |
| `FrameDropped` | The frame stops here; the reason says why | |
| `MacLearned` | The switch binds the source MAC to the ingress port (new or moved) | IEEE 802.1Q, learning |
| `FrameFlooded` | Broadcast or unknown destination: the switch sends the frame out of every other port | IEEE 802.1Q, forwarding |
| `FrameForwarded` | Known destination: the switch sends the frame only out of the learned port | IEEE 802.1Q, forwarding |
| `FrameFiltered` | The learned port is the ingress port: the switch sends nothing | IEEE 802.1Q, filtering |
| `ArpCacheUpdated` | The receiver maps the ARP sender's IP address to its MAC | RFC 826, "merge" step |
| `ArpTimeout` | The retransmission interval passes without a reply; `port` is the interface | RFC 1122 §2.3.2.1, at most one request per second |
| `PingSucceeded` | The Echo Reply with the ping's identifier arrives, or the ping targets the host's own address (record without `pdu`) | RFC 792 |
| `PingFailed` | The ping ends without a reply; the reason says in which phase | |

### Reasons

| `Reason` | Meaning | Used with |
|---|---|---|
| `LinkDown` | The cable went down while the frame was on it. It cannot happen until edits apply while running (M3) | `FrameDropped` |
| `DeviceDown` | The receiving device is powered off | `FrameDropped` |
| `NoCarrier` | The egress port has no cable with carrier | `FrameDropped` |
| `NotForUs` | The destination MAC is neither the local MAC nor broadcast | `FrameDropped` |
| `ArpForOtherHost` | The ARP request asks for an address the receiver does not own | `FrameDropped` |
| `Ipv4NotForUs` | The destination IPv4 address is not the local address | `FrameDropped` |
| `NoRoute` | The destination is outside the subnet and there are no routers yet. With `PingFailed` it concerns the request, with `FrameDropped` the reply | `PingFailed`, `FrameDropped` |
| `NotModelled` | The case is outside what this version simulates (here: resolving via ARP the sender of an Echo Request with no cache entry) | `FrameDropped` |
| `ArpUnresolved` | Three ARP requests without a reply | `PingFailed` |
| `EchoTimeout` | No matching Echo Reply within a second | `PingFailed` |

`NotModelled` must be shown as a limit of the simulator, never as network behaviour.

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
| 80 | 9 | `FrameReceived`, `PingSucceeded` | PC-A, 0 | 3 |

A second ping finds both caches filled: ICMP only, no `MacLearned` or `ArpCacheUpdated`, 40 ms in all.

## Explanations

`explain::Explainer` (`src/explain/explain.hpp`) turns the records into the event list of the interface and the CLI: a time, a protocol, a title, a detail, the devices and the cable involved, the frame, and whether it went well. It works an episode at a time, on the records added since its last call:

- an episode that starts with a frame at a port is one event: the frame, the ingress port, and everything the device did about it;
- an episode that starts with a timer gives an event per fact: an ARP request sent ("who has…?"), a retry, a ping that fails or succeeds without a frame. An Echo Request sent from a filled cache has no event of its own; the arrivals that follow tell its story.

`describe(Problem)` and `describe(Reason)` explain refusals and failures; `name()` gives the identifiers printed by `resend-cli --trace`. The short labels on the canvas ("flood ×2", "× not my MAC") are part of the app (`src/app/scene.cpp`).

Until milestone M2 the prototype's engine wrote its own texts. The new core reproduced them exactly, record by record, frame by frame and text by text, in fourteen scenarios before that engine was removed; the same scenarios are now the regression tests.

## Determinism rules

Given the same scenario and the same commands at the same times, the trace must be identical on every platform and compiler.

- The order of events depends only on time and scheduling order, never on memory addresses or real time.
- No floating point in the simulation: scenario positions are whole canvas units, and the canvas's own coordinates stay in the app.
- Unordered containers are used only for lookup, never iterated in an order that could affect the result. The scheduler, for example, uses an `unordered_map` only to find the time of an entry being cancelled.
- Random numbers: `Rng` uses `std::mt19937_64`, whose output the standard fixes, and derives ranges itself, because the `std::uniform_*_distribution` classes give different values on libstdc++, libc++ and MSVC.
- Playback speed and animations read the simulation; they never drive it. `playback_never_changes_the_result` in `tests/app_tests.cpp` plays the same ping at 0.25× and 4×, with small and large ticks, and compares the traces.
- `random_topologies` in `tests/sim_tests.cpp` builds 1,000 random networks with random faults and pings, and hashes everything they record into one number, which must match a constant on every CI platform. The constant changes only when the model does, and then on purpose.

## Tests

| Test | What it checks |
|---|---|
| `tests/core_tests.cpp` | Addresses, prefixes, ids, the scheduler, durations |
| `tests/scenario_tests.cpp` | The JSON reader and writer, `Rng`, every validation rule, the edits, the example files byte for byte, errors in files |
| `tests/sim_tests.cpp` | Concurrent pings, shared ARP resolutions, refused pings, carrier and delays, episodes, /31 and /32, the lesson of each example file, the 1,000-network hash |
| `tests/regression_tests.cpp` | The fourteen scenarios of the prototype against the golden files in `tests/golden/`; `RESEND_UPDATE_GOLDEN=1` rewrites them after an intended change, to be reviewed in the diff |
| `tests/app_tests.cpp` | Playback and what the canvas shows |
| `tests/gui_tests.cpp` | The interface on Slint's testing backend |
| CTest `cli-*` | `resend-cli` exit codes, scenario files and concurrent pings |
