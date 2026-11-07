# Changelog

Versions follow [Semantic Versioning](https://semver.org); before 1.0, minor versions may change the file formats and the interface.

## 0.3.0 — routers (unreleased)

- Routers, with two to eight ports, each with an IPv4 address or none. A router forwards by the longest prefix match over its connected networks and up to 32 static routes, `0.0.0.0/0` being the default route, and lowers the TTL by one (RFC 1812). A static route whose next hop is on none of the router's networks, or is one of its own addresses, or that repeats a connected network, is kept but not used, and the inspector says why.
- Hosts have a default gateway: a datagram for another subnet goes to it, so ARP asks for the gateway, not for the destination. Without one, or with one outside the subnet, the ping fails with no route.
- ICMP errors: a router sends Time Exceeded when the TTL runs out, Destination Unreachable (network) without a route and (host) after three unanswered ARP requests for the next hop. Each quotes the whole datagram, as Linux does; none is sent about an ICMP error or a broadcast, and directed broadcasts are not forwarded (RFC 2644). A ping request that an error is about ends at once, with the error as its outcome.
- Traceroute over ICMP, one request per hop with TTL 1, 2, ..., as `traceroute -I`: in the run bar under the canvas (**Traceroute**, then **Trace**), and in `resend-cli --traceroute NAME ADDRESS`, whose output looks like `traceroute`'s.
- The router inspector edits the address of each port, adds and removes ports, adds and removes static routes, and shows the routing table, with the routes not used and why, and the ARP cache with the port of each entry. A host's inspector gains the default gateway. Routers have their own icon and colour, and the canvas shows the route each forwarded datagram takes and why one stops.
- The frame view and the pcap files show the datagram an ICMP error quotes, field by field, as Wireshark does.
- Five new examples: Two LANs and a router, Wrong gateway, Missing return route, Routing loop and Longest prefix match.
- `resend-cli` gains `--traceroute`, `--max-hops`, `--ttl` and `--set-gateway`, and prints ICMP errors as `ping` does (`From 10.12.0.2 icmp_seq=1 Time to live exceeded`); `resend` gains `--traceroute` and `--select` for snapshots.
- Scenario files are version 2: routers, a host's `gateway`, a router's `ports` and `routes`, and a router port's address is optional. Version 1 files still open; resend writes version 2.
- Engine: the `Router` node and the `routing` module (routing table, lookup, the reason a route is not used); the `SetGateway`, `SetRoutes` and `SetPorts` edits; `DatagramRouted` records with the route chosen; the `TtlExceeded`, `NetUnreachable`, `HostUnreachable`, `GatewayOffLink`, `NoAddress`, `UnmatchedError` and `DirectedBroadcast` reasons; `PingOptions` gains `ttl` and `traceroute`, and each request records its TTL and who answered. Seven regression scenarios, routing tests with 200 random routed networks, and `scripts/compare-linux.py`, which builds the router examples with Linux network namespaces and checks that every cable carries the same frames, join the tests; CI runs the comparison.

## 0.2.0 — usable sandbox (unreleased)

- A new interface around the canvas: a menu bar (File, Edit, View, Simulation, Help), the palette with the lesson of the network on the left, the inspector on the right, the timeline at the bottom. The three panels fold away, and the window has no fixed minimum size, so a 1080p screen at 2× scale holds it.
- The canvas: drag cards, pan the background, zoom with the wheel or the controls at the bottom right; when zoomed out, cards show only their icon and name. Plug a cable by dragging from a port light or from **+** to another device or one of its ports, or by pressing **+** on two devices; click a cable to select it. With the keyboard, Tab goes from card to card, the arrows move the selected one (Shift for small steps), Enter on one device and then on another plugs a cable between them, Delete removes the selection, Escape cancels, and +, − and 0 zoom and fit. New devices go to the free place nearest the middle of the view.
- The inspector edits a host's name, address and prefix, a switch's name and ageing time, power and the state of each port, and a cable's delay and state; a cable can be unplugged and a device deleted. It shows the ARP cache or the MAC table as tables, at the instant on the canvas.
- Undo and redo (Ctrl+Z, Ctrl+Shift+Z or Ctrl+Y, and buttons that say what they undo), up to 500 steps; a drag is one step. While the network runs, undoing makes the inverse change at the current instant.
- The timeline filters events by protocol, by the selected device, to problems only, and hides or shows changes and simulator events. Its slider goes back in time: the run is replayed up to the chosen instant, exactly, since the simulation is deterministic. Changes made later lie ahead, so the network cannot be changed until you return to the latest instant.
- The frame of the selected event or packet, field by field, as Wireshark names and prints them, next to its bytes: choosing a field highlights its bytes, choosing a byte the field that holds it.
- Files: **Open**, **Save** and **Save as** with the app's own file dialog, an unsaved-changes marker, a question before changes are lost (new network, open, quit, closing the window) and before a file is replaced. Unsaved changes are saved aside every few seconds, and after a crash the next start offers them back.
- The example scenarios are in File ▸ Open example.
- The interface and the explanations are translated: Italian is the first translation, chosen from the system's language or in View ▸ Language, and kept with the reduced-motion setting in `settings.json` in the state folder. `--lang CODE` chooses a language for one session.
- `resend` gains `--lang`; the CLI's output stays in English.
- Fixed: the first click on the canvas after the app started only gave it the keyboard; after a dialog closed, the keyboard shortcuts stopped working until something was clicked.
- Engine: `edit::inverse()` gives the exact inverse of an edit, with the new `RestoreLink` and `RestoreDevice` edits; `Simulation::resolve()` gives a new device or cable the id it will have; `wire::dissect()` lists the layers and fields of a frame with tshark's names, offsets and values. Every text for people goes through `text::tr()`, which reads the catalogs in `translations/`. New tests for the canvas controller, the workspace (undo, replay, filters, 50 random commands and 50 undos), files and recovery, the translations and the dissection compared with tshark's for every frame of the examples.

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
