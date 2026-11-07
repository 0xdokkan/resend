# Scenario files

A scenario file describes a network: its devices, their ports and addresses, the cables, where the cards sit on the canvas, and notes for whoever opens it. The files are JSON, UTF-8, and meant to be read, diffed and edited by hand. The examples in [`examples/`](../examples) are written exactly as resend writes them.

```sh
resend --scenario examples/two-switches.json
resend-cli --scenario examples/two-switches.json --ping PC-A 192.168.1.20
resend-cli --check --scenario my-network.json
```

## A small example

Two PCs on one cable. Members that keep their default value can be left out (see below), so a minimal file can be this short:

```json
{
  "format": "resend-scenario",
  "version": 2,
  "title": "Two PCs",
  "devices": [
    {"id": 1, "kind": "host", "name": "A", "ports": [{"ipv4": "10.0.0.1/24"}]},
    {"id": 2, "kind": "host", "name": "B", "ports": [{"ipv4": "10.0.0.2/24"}]}
  ],
  "links": [
    {"id": 1, "a": {"device": 1, "port": 1}, "b": {"device": 2, "port": 1}}
  ]
}
```

When resend writes a file it writes every member except switch ports left at their defaults, with two-space indentation, members in the order of this page, and a final newline. Reading a file and writing it back gives the same bytes.

## Two subnets and a router

A router joins networks: each of its ports with an address is on one, and the hosts there use it as their default gateway. [`examples/two-lans-router.json`](../examples/two-lans-router.json) is the full version, with a switch on each side; directly cabled it is:

```json
{
  "format": "resend-scenario",
  "version": 2,
  "devices": [
    {"id": 1, "kind": "host", "name": "PC-A", "gateway": "10.1.0.1", "ports": [{"ipv4": "10.1.0.10/24"}]},
    {"id": 2, "kind": "router", "name": "Router", "ports": [{"ipv4": "10.1.0.1/24"}, {"ipv4": "10.2.0.1/24"}]},
    {"id": 3, "kind": "host", "name": "PC-B", "gateway": "10.2.0.1", "ports": [{"ipv4": "10.2.0.20/24"}]}
  ],
  "links": [
    {"id": 1, "a": {"device": 1, "port": 1}, "b": {"device": 2, "port": 1}},
    {"id": 2, "a": {"device": 2, "port": 2}, "b": {"device": 3, "port": 1}}
  ]
}
```

A network the router is not on needs a static route: `"routes": [{"destination": "10.3.0.0/24", "via": "10.2.0.254"}]`, where the next hop is another router on one of its networks. `0.0.0.0/0` is the default route.

## Top level

| Member | Type | Required | Meaning |
|---|---|---|---|
| `format` | string | yes | Always `"resend-scenario"` |
| `version` | number | yes | Format version, currently `2` |
| `title` | string | no | Shown when the file is opened |
| `notes` | string | no | What the scenario shows, or what to try |
| `seed` | number | no | Seed for random choices, 0 to 4294967295; default 1. Nothing in version 2 is random yet, but the seed is kept so a scenario stays reproducible when something is |
| `devices` | array | yes | The devices, in any order |
| `links` | array | yes | The cables, in any order |

## Devices

| Member | Type | Required | Meaning |
|---|---|---|---|
| `id` | number | yes | 1 to 4294967295, unique among devices. Cables refer to devices by id |
| `kind` | string | yes | `"host"` (a PC, one port), `"switch"` (eight ports) or `"router"` (two to eight ports) |
| `name` | string | yes | 1 to 48 characters, at least one visible, no control characters |
| `powered` | boolean | no | Default `true`. A powered-off device sends nothing and drops what reaches it |
| `x`, `y` | number | no | Top-left corner of the card on the canvas, in whole units from −1000000 to 1000000; default 0 |
| `ageing` | string | no | Switches only: how long a MAC table entry lasts without a frame from its address, a duration as for cable delays, from `"10s"` to `"1000000s"`; default `"300s"`, as IEEE 802.1Q recommends |
| `gateway` | string | no | Hosts only: the default gateway, an IPv4 address such as `"192.168.1.1"`, where datagrams for other subnets go. It should be on the host's subnet, usually a router's address there; a gateway outside it is accepted but unreachable, as on a real PC. Left out, the host reaches only its own subnet |
| `ports` | array | no | Exactly one entry for a host, eight for a switch, two to eight for a router, in port order. A router has as many ports as listed, so resend always writes its `ports`. Left out, every port takes its defaults, and a router has four |
| `routes` | array | no | Routers only: static routes, at most 32, in the order the router tries them when two prefixes are equally long. Each is `{"destination": "10.3.0.0/24", "via": "10.2.0.254"}`: a network with its prefix length and no host bits set, and the unicast address of the next hop. Default: none |

### Ports

| Member | Type | Required | Meaning |
|---|---|---|---|
| `mac` | string | no | Six pairs of hex digits separated by colons, e.g. `"02:00:00:00:00:01"`. Default: `02:00:` followed by the device id for a host, `02:1p:` followed by the device id for port p (0–7) of a switch, e.g. `02:13:00:00:00:05` for port 4 of switch 5, and `02:2p:` for a router, e.g. `02:21:00:00:00:03` for port 2 of router 3 |
| `up` | boolean | no | Administrative state, default `true`. A disabled port has no carrier, and neither has the port at the other end of its cable |
| `ipv4` | string | hosts | The interface address with its prefix length, e.g. `"192.168.1.10/24"`. Required on a host, optional on each router port: a router port without one takes part in no network |

An address must be usable on its prefix: not the network or broadcast address of its subnet (except on /31 and /32, which have neither, as in RFC 3021), not 0.0.0.0/8, loopback (127/8), multicast (224/4) or reserved (240/4). Switch ports have no address.

### Routes a router uses

Each router port with an address gives a connected route to its network. A static route is used only when its next hop is on one of those networks, as a router installs a route only through a neighbour it can reach directly, and is not one of the router's own addresses; a static route to the same network as a connected one is not used either. The inspector shows the routes that are not used, and why. A datagram follows the longest prefix that matches its destination (RFC 1812 §5.2.4.3); between equal prefixes, the first route in the table.

## Cables

| Member | Type | Required | Meaning |
|---|---|---|---|
| `id` | number | yes | 1 to 4294967295, unique among cables |
| `a`, `b` | object | yes | The two ends: `{"device": <id>, "port": <number>}`, ports counted from 1 as on the interface |
| `delay` | string | no | Time a frame takes from one end to the other: a number and a unit (`ns`, `us` or `µs`, `ms`, `s`) without spaces, e.g. `"10ms"`, `"250us"`, `"1.5s"`; from 1 µs to 10 s, default `"10ms"` |
| `up` | boolean | no | Default `true`. A cable that is down carries nothing |

## What is checked

Reading stops at the first problem with the file itself and says where it is:

- a syntax error, with its line and column: `line 3, column 8: expected a value`;
- a member of the wrong type or out of range, or an unknown member, with its path: `devices[2].ports[0].ipv4: expected an IPv4 address with its prefix, such as 192.168.1.10/24`;
- a member that does not belong to the device's kind: `devices[0].routes: only routers have routes`;
- a version newer than this resend understands: `version: this file needs a newer version of resend (format 3)`.

The JSON is strict: whole numbers only, no comments, no trailing commas, no duplicate members, valid UTF-8, at most 64 levels of nesting.

A file that reads correctly is then validated as a network, and every problem is reported with the device or cable it concerns: duplicate ids, a cable to a missing device or port, two cables on one port, a cable between two ports of the same device, a missing or unusable address, a gateway that is not a unicast address, a route with host bits set or a duplicate destination, more than 32 routes, a delay or ageing time out of range, or more than 100 devices. Questionable but valid networks, such as two hosts with the same address, masks that disagree, a gateway on another subnet, a missing return route or a loop of switches or of routes, are accepted: they are worth trying. In a loop of switches, resend's storm guard stops the frames that go round it; in a loop of routes, the TTL does.

## Versions

The version changes only when a file written by a newer resend could be misread by an older one. Older files keep opening: a newer resend reads every earlier version, and writes the current one.

- Version 1: hosts and switches.
- Version 2 (resend 0.3): routers, default gateways and static routes.
