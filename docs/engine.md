# Simulation engine

This document describes the engine as it is today, the architecture it is moving towards and the rules both must follow in the meantime. The transition is gradual: the new parts grow next to the existing engine, the same test scenarios act as a safety net, and old code is removed only once new code has replaced it.

## Current state

| Path | Contents | Status |
|---|---|---|
| `src/core/address.*` | `Ipv4Address` (strict parser, numeric ordering), `MacAddress`, `prefix_mask` | New, used by the engine |
| `src/core/ids.hpp` | `Id<Tag>`: `DeviceId`, `LinkId` and `PduId`, which cannot be mixed up | New, used by the trace |
| `src/core/sim_time.hpp` | `SimTime`, a 64-bit duration in nanoseconds | New, used by the engine |
| `src/sim/scheduler.hpp` | Deterministic event queue with cancellable timers | New, used by the engine |
| `src/sim/pdu.hpp` | `Frame`: the frame as modelled today, with ARP or IPv4/ICMP flattened into the address fields | Temporary |
| `src/sim/trace.hpp` | `TraceRecord`, `TraceKind`, `Reason` and the PDU archive | New, used by the engine |
| `src/network.*` | `Engine`: topology, hosts, switches, ARP, ICMP and the readable event texts (`Event`) | To be replaced in M2 |

`Engine` keeps its public interface, but it uses the typed addresses, the scheduler instead of a queue filtered by validity flags, and at every step it writes the structured trace next to the textual `Event`s. When the trace was introduced, the CLI output was compared line by line with the previous engine in seven scenarios and was identical.

The structural limits described in the plan remain: the engine is built around a single ping, every edit resets the simulation and loops are rejected by the editor. These limits go away with the new core (M2), not with further changes to `Engine`.

## Target architecture

```
┌──────────────────────────── app (Slint + C++) ─────────────────────────────┐
│ ui/*.slint ◄── models/callbacks ──► CanvasController · PlaybackController  │
│                                     ViewModels · UndoStack · file dialogs  │
└──────────────┬───────────────────────────────────┬─────────────────────────┘
               │ edits                              │ step / run_until / query
┌──────────────▼────────────┐  apply()  ┌───────────▼────────────────────────────┐
│ scenario                  ├──────────►│ sim                                    │
│ devices, ports, cables,   │  while    │ Scheduler (time in ns, seq)            │
│ config, layout, notes,    │  running  │ Medium (cables, frames in flight)      │
│ seed; validate() → issues │           │ Nodes: Host · Switch · Router          │
│ versioned JSON            │           │  └ modules: Nic, Bridge, Arp, Ipv4,    │
└───────────────────────────┘           │    Icmp, PingApp (later Udp, Dhcp…)    │
                                        │ Trace: TraceRecord + PDU archive       │
                                        └───────┬────────────────────┬───────────┘
                                   ┌────────────▼─────────┐  ┌───────▼─────────┐
                                   │ explain: record →    │  │ wire: bytes,    │
                                   │ text (en, then more) │  │ checksums, pcap │
                                   └──────────────────────┘  └─────────────────┘
 core: Ipv4Address, Ipv4Prefix, MacAddress, typed ids, SimTime, seeded Rng
 cli: scenario + script → trace/pcap (used by the scenario tests)
```

A single library, with the directories `core/ scenario/ sim/ wire/ explain/` as boundaries and dependencies pointing only downwards: `core` depends on nothing, `sim` on `core`, `explain` and `wire` on `sim`. Today `core/` and `sim/` exist.

The main types we are moving towards (those already written are in `src/core/` and `src/sim/`):

```cpp
using SimTime = std::chrono::duration<std::int64_t, std::nano>;
class Ipv4Address { std::uint32_t bits_; };            // parse(string_view), to_string()
class MacAddress  { std::array<std::uint8_t, 6> bytes_; };
struct PortRef    { DeviceId device; std::uint8_t port; };

// Scenario: what the user edits, saves and shares
struct PortConfig   { std::string name; MacAddress mac; bool admin_up = true;
                      std::optional<Ipv4Interface> ipv4; };
struct DeviceConfig { DeviceId id; std::string name; bool powered = true;
                      std::vector<PortConfig> ports;
                      std::variant<HostRole, SwitchRole, RouterRole> role; };  // gateway / ageing / routes
struct LinkConfig   { LinkId id; PortRef a, b; SimTime delay; bool up = true; };

// Sim: immutable layered PDUs, shared between flooding and the trace
struct Ipv4Packet    { Ipv4Address src, dst; std::uint8_t ttl, protocol; std::uint16_t ident;
                       std::variant<IcmpMessage /*, UdpDatagram, TcpSegment */> payload; };
struct EthernetFrame { MacAddress dst, src; std::variant<ArpPacket, Ipv4Packet> payload; };
using FramePtr = std::shared_ptr<const EthernetFrame>;  // a router decrementing the TTL makes a copy

// Trace: structured facts, never text
struct TraceRecord { SimTime time; std::uint64_t seq; TraceKind kind; Reason reason;
                     DeviceId device; std::optional<std::uint8_t> port;
                     std::optional<LinkId> link; std::optional<PduId> pdu; };
```

- **Nodes and modules.** Each node exposes `receive(port, FramePtr, Context&)` and `on_timer(TimerId, Context&)`. The `Context` offers `send`, `schedule`, `cancel`, `trace` and `rng`. It is an internal interface, not a plugin system.
- **Tables.** Each table belongs to the module that manages it: `ArpCache` with the state and expiry of every entry, `MacTable` with port and last update, `RoutingTable` with longest-prefix lookup. They reach the UI as typed, read-only snapshots.
- **Timers.** `schedule()` returns a cancellable handle. Each ping is an application with its own timers, so several pings at the same time come naturally.
- **Edits while running.** `Simulation::apply(Edit)` applies the edit at the current time and records it in the trace. A frame in flight on a cable that gets unplugged is lost on delivery with the reason `LinkDown`; after an IP change the other hosts' caches stay stale, as they would in reality. "Reset" means rebuilding the simulation from the scenario.
- **Time and animation.** The `PlaybackController` turns real time into simulated time with a speed factor. The engine exposes transmissions in flight as `{cable, direction, t_start, t_arrival}`; the animation only interpolates those and never affects the result.
- **Byte encoding kept separate.** The `wire` module handles checksums, the hex view and pcap export. The engine works on structures; the encoding is checked with round trips and with `tshark`.

## Time and scheduler

`SimTime` counts nanoseconds in 64 bits: enough for about 292 simulated years, and exact for serialization delays (1500 bytes at 1 Gb/s take 12 µs). The current model uses 10 ms per cable and 1 s for the ARP and Echo timeouts.

`Scheduler<Payload>` (`src/sim/scheduler.hpp`):

- runs entries in time order; entries due at the same time run in the order they were scheduled. An entry scheduled while running, for the current time, runs after the entries already queued for that time;
- `schedule(at, payload)` returns a `Handle`; `cancel(handle)` cancels the entry if it is still pending, clears the handle and reports whether it cancelled anything;
- `pop()` skips cancelled entries, moves the clock to the entry's time and returns it. A cancelled entry never moves the clock;
- `empty()` and `size()` count only pending entries.

In `Engine`, cancellation replaces the flags that used to decide after the fact whether a timer was still valid: the ARP reply cancels the ARP timer, the Echo Reply cancels the ICMP timeout. As a result, `pending()` no longer copies and scans the queue.

## Trace

The trace is an append-only sequence of records, plus the archive of the frames the records refer to. A frame is archived once, when it is created, and keeps its `PduId` along its whole path: the copies a switch forwards or floods are the same frame, so a frame can be followed across the network by its id.

Records contain no text. The readable explanation is a separate layer: today it is still the `Event` texts written by `Engine`; with the new core it becomes the `explain` module, which produces the text from the records. Tests check the records, so rewording or translating the explanations does not break them.

### Fields

| Field | Meaning |
|---|---|
| `time` | Simulated time of the fact |
| `seq` | Position in the trace, from 0 |
| `kind` | What happened |
| `reason` | Why; `None` when the fact needs no reason |
| `device` | The device where it happened |
| `port` | Port of `device`, from 0. For switch decisions it is the ingress port; the egress ports appear in the `FrameSent` records that follow |
| `link` | The cable crossed, if any |
| `pdu` | The frame involved, if any |

### Record kinds

| `TraceKind` | When | Reference |
|---|---|---|
| `FrameSent` | A frame leaves a port onto an active cable | |
| `FrameReceived` | A frame arrives at a port of a powered device | |
| `FrameDropped` | The frame stops here; the reason says why | |
| `MacLearned` | The switch binds the source MAC to the ingress port (new or moved) | IEEE 802.1Q, learning |
| `FrameFlooded` | Broadcast or unknown destination: the switch sends the frame out of every other port | IEEE 802.1Q, forwarding |
| `FrameForwarded` | Known destination: the switch sends the frame only out of the learned port | IEEE 802.1Q, forwarding |
| `FrameFiltered` | The learned port is the ingress port: the switch sends nothing | IEEE 802.1Q, filtering |
| `ArpCacheUpdated` | The receiver maps the ARP sender's IP address to its MAC | RFC 826, "merge" step |
| `ArpTimeout` | The retransmission interval passes without a reply | RFC 1122 §2.3.2.1, at most one request per second |
| `PingSucceeded` | The Echo Reply with the request's identifier arrives, or the ping targets the host's own address (record without `pdu`) | RFC 792 |
| `PingFailed` | The ping ends without a reply; the reason says in which phase | |

### Reasons

| `Reason` | Meaning | Used with |
|---|---|---|
| `LinkDown` | The cable was disabled while the frame was on it. It cannot happen today, because every edit resets the simulation; it will be needed for edits while running | `FrameDropped` |
| `DeviceDown` | The receiving device is powered off | `FrameDropped` |
| `NoCarrier` | The egress port has no active cable | `FrameDropped` |
| `NotForUs` | The destination MAC is neither the local MAC nor broadcast | `FrameDropped` |
| `ArpForOtherHost` | The ARP request asks for an address the receiver does not own | `FrameDropped` |
| `Ipv4NotForUs` | The destination IPv4 address is not the local address | `FrameDropped` |
| `NoRoute` | The destination is outside the subnet and there are no routers yet. With `PingFailed` it concerns the request, with `FrameDropped` the reply | `PingFailed`, `FrameDropped` |
| `NotModelled` | The case is outside what this version simulates (here: resolving via ARP the sender of an Echo Request with no cache entry) | `FrameDropped` |
| `ArpUnresolved` | Three ARP requests without a reply | `PingFailed` |
| `EchoTimeout` | No matching Echo Reply within the timeout | `PingFailed` |

`NotModelled` corresponds to the "not modelled yet" category of the plan: it must be shown as a limit of the simulator, never as network behaviour.

### Example: the first ping of the sample network

PC-A (1) port 1 ─ cable 1 ─ port 1 Switch-01 (3) port 2 ─ cable 2 ─ port 1 PC-B (2). In the table, ports and PDUs are numbered from 0, as in the trace. `basic_ping_and_cache` in `tests/network_tests.cpp` checks this sequence record by record.

| ms | Records | Device, port | PDU |
|---|---|---|---|
| 0 | `FrameSent` | PC-A, 0 | 0: broadcast ARP Request |
| 10 | `FrameReceived`, `MacLearned`, `FrameFlooded` | Switch, 0 | 0 |
| 10 | `FrameSent` | Switch, 1 | 0 |
| 20 | `FrameReceived`, `ArpCacheUpdated` | PC-B, 0 | 0 |
| 20 | `FrameSent` | PC-B, 0 | 1: unicast ARP Reply |
| 30 | `FrameReceived`, `MacLearned`, `FrameForwarded` | Switch, 1 | 1 |
| 30 | `FrameSent` | Switch, 0 | 1 |
| 40 | `FrameReceived`, `ArpCacheUpdated` | PC-A, 0 | 1 |
| 40 | `FrameSent` | PC-A, 0 | 2: Echo Request |
| 50 | `FrameReceived`, `FrameForwarded` | Switch, 0 | 2 |
| 50 | `FrameSent` | Switch, 1 | 2 |
| 60 | `FrameReceived` | PC-B, 0 | 2 |
| 60 | `FrameSent` | PC-B, 0 | 3: Echo Reply |
| 70 | `FrameReceived`, `FrameForwarded` | Switch, 1 | 3 |
| 70 | `FrameSent` | Switch, 0 | 3 |
| 80 | `FrameReceived`, `PingSucceeded` | PC-A, 0 | 3 |

The second ping finds both caches filled: 11 records, ICMP only, no `MacLearned` or `ArpCacheUpdated`, ending at 120 ms.

## Determinism rules

Given the same scenario, seed and sequence of commands, the trace must be identical on every platform and compiler. The `deterministic_reset` test compares two runs record by record.

- The order of events depends only on time and scheduling order, never on memory addresses or real time.
- No `float` in simulation logic: canvas coordinates stay outside the engine.
- Unordered containers (`unordered_map`, `unordered_set`) are used only for membership and lookup, never iterated in an order that could affect the result. The scheduler, for example, uses an `unordered_set` only to know whether an entry is still pending.
- Random numbers: `std::mt19937_64` seeded by the scenario, but without the `std::uniform_*_distribution` classes, which produce different values on libstdc++, libc++ and MSVC. Ranges are derived with a function of our own.
- Playback speed and animations read the engine; they never drive it.

## Next steps (M2)

1. `Ipv4Prefix` and `Ipv4Interface` in `core/`, then the scenario with explicit ports and the versioned JSON format.
2. `Simulation` with a `Medium` and nodes built from modules (`Nic`, `Bridge`, `Arp`, `Ipv4`, `Icmp`, `PingApp`) on top of the same `Scheduler` and `Trace`, with layered PDUs instead of `Frame`.
3. The `explain` module, producing from the records the same text as today's `Event`s; comparing the two texts serves as a parity test during the transition.
4. The scenarios of `tests/network_tests.cpp` run on both engines until `Engine` can be removed.
