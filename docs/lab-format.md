# Lab files

A lab is an exercise: a network to start from, possibly empty or broken, the goals the learner has to meet, and the steps, hints and solution that go with them. resend checks the goals on the simulation, not on what the learner clicked: a goal such as "PC-A reaches PC-B" is met when a ping on a new simulation of the learner's network gets its reply. The files are JSON, UTF-8, and meant to be written by hand. The labs built into resend are in [`labs/`](../labs).

```sh
resend labs/missing-gateway.json                                   # do it in the app
resend-cli --lab labs/missing-gateway.json --verify                # does the lab prove itself?
resend-cli --lab labs/missing-gateway.json --check my-network.json # which goals does a network meet?
resend-cli --lab labs/watch-arp-and-ping.json --check --answer reply-ttl 64
```

`--check` prints each goal with a sentence saying why it is met or not, and exits with 0 when every goal is met; without a network it checks the one the lab starts from. Goals of kind `observed` read the learner's own run, which the command line does not have: there they are never met. `--verify` is described [below](#how-a-lab-proves-itself).

In the app, **File ▸ Open** opens a lab file as it opens a scenario file, and **File ▸ Labs…** (G) shows the labs built in.

## A small example

Two PCs on one cable; PC-B has no address. The learner has to give it one, and may not touch PC-A.

```json
{
  "format": "resend-lab",
  "version": 1,
  "kind": "troubleshooting",
  "topic": "First networks",
  "title": "The silent neighbour",
  "brief": "PC-A pings PC-B on the same cable, and nothing comes back. Find out why and repair it, without touching PC-A.",
  "network": {
    "devices": [
      {"id": 1, "kind": "host", "name": "PC-A", "x": 80, "y": 90, "ports": [{"ipv4": "10.0.0.1/24"}]},
      {"id": 2, "kind": "host", "name": "PC-B", "x": 330, "y": 90, "ports": [{}]}
    ],
    "links": [
      {"id": 1, "a": {"device": 1, "port": 1}, "b": {"device": 2, "port": 1}}
    ]
  },
  "locks": [{"device": "PC-A"}],
  "goals": [
    {"id": "reach", "kind": "reach", "text": "PC-A reaches PC-B at 10.0.0.2.", "from": "PC-A", "to": "10.0.0.2"},
    {"id": "address", "kind": "config", "text": "PC-B has an address in 10.0.0.0/24.", "device": "PC-B", "address_in": "10.0.0.0/24"}
  ],
  "hints": [
    "Select PC-B and look at its Settings tab."
  ],
  "solution": {
    "network": {
      "devices": [
        {"id": 1, "kind": "host", "name": "PC-A", "x": 80, "y": 90, "ports": [{"ipv4": "10.0.0.1/24"}]},
        {"id": 2, "kind": "host", "name": "PC-B", "x": 330, "y": 90, "ports": [{"ipv4": "10.0.0.2/24"}]}
      ],
      "links": [
        {"id": 1, "a": {"device": 1, "port": 1}, "b": {"device": 2, "port": 1}}
      ]
    }
  }
}
```

When resend writes a lab it writes every member in the order of this page, with two-space indentation and a final newline. Reading a file and writing it back gives the same bytes.

## The members

| Member | Required | Value |
|---|---|---|
| `format` | yes | `"resend-lab"` |
| `version` | yes | `1`. A newer version than resend knows is refused with "this file needs a newer version of resend". |
| `kind` | yes | `"guided"`, `"challenge"` or `"troubleshooting"`, see [kinds](#kinds) |
| `topic` | yes | the section of the library the lab is in, e.g. `"Routing"` |
| `title` | yes | a short name, not empty |
| `brief` | yes | what the lab is about, in a few sentences, not empty |
| `network` | yes | the network the learner starts from, see [networks](#networks) |
| `names` | no | the names goals use for devices the learner has to add, see [names](#names) |
| `new_devices` | no | how devices the learner adds start, see [new devices](#new-devices) |
| `locks` | no | what the learner may not change, see [locks](#locks) |
| `goals` | yes | at least one goal, see [goals](#goals) |
| `steps` | no | an array of strings: what to do, in order, for a guided lab |
| `hints` | no | an array of strings, shown one at a time when asked for, from the gentlest |
| `solution` | yes | a network that meets every goal, see [the solution](#the-solution) |

Any other member is an error. A lab's id is its file name without `.json`: resend remembers the labs solved by id.

Texts are what the learner reads, in English. Steps name the interface as it is: "press Send, then Run", "open the Frame tab of the event". The panel shows the brief, the steps numbered, then the goals; the hints come below, as many as were asked for.

## Kinds

After the way a first networks course teaches:

- **guided**: steps to follow, one at a time, and goals that confirm each part was done. The learner reads, does, then looks: `observed` and `answer` goals fit here.
- **challenge**: a goal and an empty or half-built network, no steps. The learner designs, for instance an addressing plan.
- **troubleshooting**: a broken network and a goal, nothing else. The learner finds the fault with pings and the timeline's Why, and repairs it; locks keep the repair where the fault is.

The kind is shown beside the title; it changes nothing in how goals are checked.

## Networks

`network` and `solution.network` are networks as [scenario files](scenario-format.md) write them, without `format` and `version`: an object with `title`, `notes`, `seed`, `devices` and `links`, the members of the current scenario format, each with its default when left out. `network` may have no devices at all, for a lab where the learner builds everything.

The solution's network is a separate network. Devices are matched between the two by id, so a device the lab gives keeps its id in the solution; devices the learner adds can have any ids there, as long as their names are those the goals use.

## Goals

Every goal is an object with these members, then those of its kind:

| Member | Value |
|---|---|
| `id` | a short name, unique in the lab: the solution's answers and the command line's `--answer` refer to it |
| `kind` | `"reach"`, `"path"`, `"config"`, `"observed"`, `"answer"` or `"kept"` |
| `text` | the goal in one sentence, as the learner reads it: "PC-A reaches PC-B at 192.168.2.20." |

Goals are checked when the learner presses **Check**, all at once, on the network as it is then. Each gets a verdict: met, or not met with a sentence saying why, worded from the simulation's own records ("PC-A does not reach 192.168.2.20: the request arrives, but PC-B cannot send its reply back: PC-B has no default gateway."). A change to the network after a check marks the verdicts as old until the next one.

Devices are named, never numbered: a goal names a device by its `name`. A goal that names a device the network does not have is not met, with "There is no device named PC-A yet."

### Probes

`reach`, `path` and `answer` goals are checked with a **probe**: one ping, or one traceroute, on a new simulation of the learner's network, from time zero with empty ARP caches and MAC tables. The same network therefore always gets the same verdict, whatever the learner ran before and whatever is in their tables. The probe runs until the ping ends, then for as long as a router could still be asking with ARP, so that the records say where the requests stopped; it gives up after 200,000 events, which only a network that never stops sending (a loop of switches) reaches.

The source must be a host. Its pings use the defaults of the app: one Echo Request, a timeout of 1 second, TTL 64; a traceroute tries up to 30 hops.

### `reach`

The host `from` pings `to`.

| Member | Value |
|---|---|
| `from` | the name of a host |
| `to` | an IPv4 address, e.g. `"192.168.2.20"` |
| `expect` | optional: the name of a reason. Without it the goal is met when a reply comes. With it, the goal is met when the ping fails, and fails for that reason. |

`expect` makes goals such as "PC-A cannot reach the server any more: there is no route". The reasons a ping fails for:

| Name | The ping failed because |
|---|---|
| `EchoTimeout` | no reply came within the timeout |
| `NoRoute` | the source has no route to the address: outside its subnet without a gateway |
| `GatewayOffLink` | the source's default gateway is outside its subnet |
| `NoAddress` | the source has no IPv4 address |
| `ArpUnresolved` | the source's ARP requests went unanswered |
| `ArpQueueFull` | newer datagrams for the same unresolved address took the request's place |
| `NetUnreachable` | a router sent back a Destination Unreachable (network): it had no route |
| `HostUnreachable` | a router sent back a Destination Unreachable (host): it could not resolve the next hop |
| `TtlExceeded` | a router sent back a Time Exceeded: the TTL ran out, as in a routing loop |
| `DeviceDown` | the source is powered off |
| `NoCarrier` | the source's port has no active cable |

### `path`

A traceroute from `from` to `to` meets the goal when it reaches the destination and the addresses that answer, hop by hop, are `hops`.

| Member | Value |
|---|---|
| `from` | the name of a host |
| `to` | an IPv4 address |
| `hops` | the addresses that answer, in order, the destination last: `["10.1.0.1", "10.2.0.2", "10.3.0.10"]` |

A router answers from the port its answer leaves by, the one towards the source (RFC 1812 §4.3.2.4): with routes the same both ways, the port the request arrived on. When the hops differ the verdict lists those that answered, with `*` for a hop that did not.

### `config`

A setting of the device `device`. Exactly one of the predicates below is given.

| Member | Met when |
|---|---|
| `address_in` | a network with its prefix, e.g. `"10.1.0.0/26"`: a port's address is inside it, with that prefix length. With `port`, that port only; without, any port. |
| `gateway` | an IPv4 address: the host's default gateway is that address |
| `route` | a network with its prefix: the router has a static route to that network. With `via`, an IPv4 address, its next hop must be that address. |
| `up` | `true` or `false`, with `port`: that port is enabled, or disabled |
| `cable_to` | the name of another device: a cable joins the two, on any ports |

`port` is 1-based, as the interface numbers ports. `via` is allowed with `route` only.

Config goals check the design, not what it does: pair them with a `reach` goal so that the learner sees the setting work.

### `observed`

A record in the learner's own run: something they saw happen, not something a probe can do for them. The run is the one on the canvas, from the last reset; the goal is not met until the learner has run it.

| Member | Value |
|---|---|
| `device` | the name of a device |
| `record` | what it did, below |
| `for` | optional, with `arp-request` only: the address asked for |

| `record` | Met when |
|---|---|
| `arp-request` | `device` sent an ARP request, for `for` if given |
| `arp-reply` | `device` sent an ARP reply |
| `routed` | the router `device` routed a datagram |
| `echo-reply` | a ping from `device` got an Echo Reply |
| `mac-learned` | the switch `device` learned a MAC address |

### `answer`

A question the learner answers in the panel, and a probe reads the answer from the simulation. The answer field is under the goal's text; the text is the question.

| Member | Value |
|---|---|
| `from` | the name of a host |
| `to` | an IPv4 address |
| `quantity` | what is asked, below |

| `quantity` | The answer is |
|---|---|
| `reply-ttl` | the TTL of the Echo Reply when it reaches `from`, after a ping to `to` |
| `hops` | how many hops a traceroute from `from` to `to` counts, the destination included |
| `first-hop-mac` | the destination MAC address of the Echo Request as it leaves `from`: the gateway's, or the destination's on the same subnet |

Answers compare as what they mean: case and surrounding spaces do not matter, a MAC address may be written with dashes, a number with leading zeros. An empty answer is not met, with "Write your answer."; one the probe cannot read (the ping failing) is not met either, with the reason.

### `kept`

Settings of devices of the starting network stay as the lab gave them. It makes a lab's rule into a goal: "R2's default route stays as it is" is a `kept` goal on the routes of R2, next to the `reach` goal the loop breaks.

| Member | Value |
|---|---|
| `devices` | an array of at least one object: `device`, a name in the starting network, and `settings`, optional, the settings that stay (see [locks](#locks)); without `settings`, all of them |

A `kept` goal is not met when a device is removed, or when one of its settings differs, and the verdict names the first that does. Unlike a lock, it lets the learner change the setting on the way, and only asks that it be as given at the end.

## Locks

```json
"locks": [
  {"device": "Router"},
  {"device": "PC-A", "settings": ["addresses", "gateway"]}
]
```

A lock names a device of the starting network and, optionally, what of it the learner may not change; without `settings`, everything. While the lab is open, an edit that would change a locked setting is refused before it is made, with a sentence such as "The lab keeps Router as it is: it cannot be changed." The inspector shows a lock beside a locked device's settings. Moving a device on the canvas is never locked.

The settings:

| Name | What it covers |
|---|---|
| `name` | the device's name |
| `power` | whether it is powered |
| `addresses` | the IPv4 address of each port |
| `gateway` | the host's default gateway |
| `routes` | the router's static routes |
| `ports` | how many ports, whether each is enabled, and a switch's MAC ageing time |
| `cables` | the cables plugged into it, and their state and delay |

Removing a device changes everything about it and the cables of the devices it was cabled to: a lock on either refuses it. Plugging a cable into a device changes its `cables`.

## Names

```json
"names": ["PC-A", "PC-B", "Switch-1", "Router"]
```

For a lab where the learner adds devices: the names goals use that the starting network does not have. A goal may name only devices of the starting network or of `names`, so that a misspelt name is an error of the lab, not a goal nobody can meet. The panel's steps tell the learner which names to give.

## New devices

```json
"new_devices": {"host_address": false, "router_ports_up": false}
```

How the devices the learner adds start. In the sandbox a new host comes with a free address and a new router with its ports enabled, so that a network works at once; a lab about addressing wants them as they come out of the box. With `host_address` false, a host the learner adds has no address; with `router_ports_up` false, a router the learner adds has its ports disabled. Both are true when left out. The change is part of the same step as the addition, and undone with it.

## The solution

```json
"solution": {
  "network": { "devices": [...], "links": [...] },
  "commands": [{"from": "PC-A", "ping": "192.168.1.20"}, {"from": "PC-A", "traceroute": "10.3.0.10"}],
  "answers": {"reply-ttl": "64"}
}
```

| Member | Value |
|---|---|
| `network` | required: a network that meets every goal and keeps the locks |
| `commands` | optional: pings and traceroutes run on the solution's network, in order, each to its end, so that `observed` goals see their records. Each is an object with `from`, the name of a host, and exactly one of `ping` or `traceroute`, an IPv4 address. |
| `answers` | optional: the answer to each `answer` goal, by goal id, as a string |

**Solution** in the lab panel shows the solution's network in place of the learner's, which is kept aside untouched until **Back to my network**. Showing it does not mark the lab solved.

## How a lab proves itself

`resend-cli --lab FILE --verify` checks that a lab can be done and is worth doing, and exits with 0 when it can:

- the lab is valid: its networks are valid networks; every goal and lock names a device the starting network or `names` has; a `kept` goal or a lock names a device of the starting network; goal ids are unique and every goal has a text; every `answer` goal has an answer in the solution, and every answer is for an `answer` goal; every command starts from a host of the solution; the title and the brief are not empty;
- at least one goal is not met on the starting network;
- every goal is met on the solution's network, after its commands, with its answers;
- the solution keeps the locks: it removes no locked device and changes none of their locked settings.

Each problem is one sentence, e.g. `the solution does not meet goal "reach"`. The labs built into resend are listed in `CMakeLists.txt` (`resend_labs`), built into the program in that order, and each is verified by a test of its own (`labs-verify-<id>`); `tests/lab_tests.cpp` checks that the files on disk are those built in, that they write back to the same bytes, and that each topic's labs are next to each other.

A file that cannot be read is explained with the path of the member, as scenario files are: `goals[2].kind: expected one of "reach", "path", "config", "observed", "answer" or "kept"`.

## Writing a lab

- Start from the solution: build it in the sandbox, save it, then break or empty a copy for `network`. Keep the devices' ids.
- One idea a lab. A troubleshooting lab has one fault, locks around it, and a `reach` goal that the fault breaks.
- Goals that check what the network does (`reach`, `path`) say more than those that check a setting (`config`): use `config` for what a probe cannot see, such as an addressing plan, next to a `reach` goal.
- Write the goal's `text` as the learner will tick it, and the hints from the gentlest ("Which device's reply does not come back?") to the most direct.
- Run `--verify`, then do the lab in the app once as a learner would.
