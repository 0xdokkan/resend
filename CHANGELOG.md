# Changelog

Versions follow [Semantic Versioning](https://semver.org); before 1.0, minor versions may change the file formats and the interface.

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
