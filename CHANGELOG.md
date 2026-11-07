# Changelog

Versions follow [Semantic Versioning](https://semver.org); before 1.0, minor versions may change the file formats and the interface.

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
