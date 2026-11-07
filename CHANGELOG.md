# Changelog

Versions follow [Semantic Versioning](https://semver.org); before 1.0, minor versions may change the file formats and the interface.

## 0.1.3 — live faults (unreleased)

- The network can be changed while traffic flows. Every edit happens at the instant on screen and appears in the events: cables up, down, unplugged or moved, ports, power, addresses, devices added or removed. A frame on a cable that stops carrying, even for an instant, is lost when it reaches the end; a device powered off or removed forgets everything it learned and its pings end. Before the first ping an edit simply gives a new network.
- ARP entries expire a minute after their last update, so a cache can point to the old owner of an address until then. While an address is being resolved its entry is incomplete and holds up to three datagrams; a newer one pushes out the oldest. Any ARP packet from an address being resolved completes the resolution, even a request for another host.
- Switches age out MAC entries after 300 s without a frame from their address (IEEE 802.1Q's default), or the `ageing` time set in the scenario file; a frame for an aged-out entry is flooded. The inspector shows when each ARP entry expires and how old each MAC entry is.
- A host that must reply to an address it has no entry for, after a reboot for example, resolves it with ARP first.
- Ping is an application: 1 to 1,000 Echo Requests with 56 bytes of data, an interval and a per-request timeout; each reply or loss is an event with its round-trip time, a reply after its timeout is ignored, and the ping ends with statistics like `ping`'s. The app has a field for the number of requests.
- Every IPv4 datagram carries a TTL of 64.
- Loops of switches are allowed. A broadcast then goes round for ever, as without spanning tree; the simulator stops each frame copied more than four times per cable (at least 64), removes its copies from the cables and pauses the playback, with an event of its own in violet.
- Frames are encoded as bytes (Ethernet II, ARP, IPv4 and ICMP with their checksums), and `resend-cli --pcap FILE` writes a capture for Wireshark or tcpdump, `--pcap-link ID` one cable only. Wireshark's expert system finds no errors or warnings in them.
- `resend-cli` gains `--count`, `--interval`, `--timeout`, timed changes with `--at TIME` (`--link-down`, `--link-up`, `--unplug`, `--power-off`, `--power-on`, `--set-ip`), and output like `ping`'s.
- A new example, `switch-loop.json`.
- Scenario files: switches have an `ageing` member; loops are valid.
- Engine: `Simulation::apply(Edit)` with an edit log, cable serials, `PingOptions` and per-request status, `ArpEntryExpired`, `MacEntryAged`, `PingReply`, `PingLost`, `DatagramDropped`, `EditApplied` and `SimulatorGuard` records, `ArpQueueFull` and `UnmatchedReply` reasons; `NotModelled` is gone. The `wire` module encodes, decodes and writes pcap files. Nine regression scenarios and a randomised check of frame conservation over 300 edited networks join the tests.

## 0.1.2 — new core (unreleased)

- Networks are described by scenario files: JSON with a format version, explicit ports, per-cable delays from 1 µs to 10 s, per-port MAC and administrative state, layout and notes. `docs/scenario-format.md` describes every member; errors name the line and column or the member at fault.
- Five example scenarios in `examples/`, each with a lesson in its notes.
- Several pings can run at once, from the same host or from different ones; datagrams waiting for the same ARP reply share one resolution, and any update of the cache releases them.
- Hosts accept /31 and /32 prefixes (RFC 3021).
- `resend-cli --scenario FILE --ping NAME ADDRESS` runs scenarios, with several `--ping` options at once; `--trace` prints the structured records and `--check` validates a file.
- `resend --scenario FILE` opens a scenario in the app. **Send** stays available while a ping is running.
- Event times are shown in milliseconds, so sub-millisecond cables stay readable.
- Engine: the prototype's engine is replaced by a new core. `Simulation` runs a `Scenario` with a `Medium` for the cables and nodes built from modules (`Nic`, `Arp`, `PingApp`, `Bridge`); every explanation is generated from the trace by the `explain` module. The new core reproduced the prototype's records, frames and texts exactly in fourteen scenarios, which are now regression tests with golden files.
- Renaming a switch now clears the simulation, like every other edit except moving cards.

## 0.1.1 — animated playback (unreleased)

- Packets move along the cables in simulated time, coloured by protocol, with a ring on broadcast frames; the copies a switch floods travel together.
- Device decisions appear on the canvas: MAC learning with the port, flooding, forwarding, filtering, ARP cache updates, drops with their reason and the outcome of the ping.
- Cards show a light per port (connected, cable down, frame passing); cables end on the card borders with the switch port number.
- Clicking a packet or an event follows that frame: the cables it crossed light up and the frame is shown layer by layer.
- Playback speed from 0.25× to 4×, idle waits shortened to at most 0.8 s, a reduced-motion mode, and a simulated clock on the canvas.
- **Power off / Power on** in the inspector.
- `--ping` and `--at` render a snapshot of a given simulated instant.
- Engine: the trace records every transmission; `run_until()` and `next_event_time()` let the playback drive the engine without ever running ahead of the screen.
- Fixed: the label of a disabled primary button was unreadable.

## 0.1.0 — prototype

- Ethernet LAN with hosts and switches, ARP, IPv4 and ICMP Echo, a structured trace of every step, and a Slint interface with an event timeline.
