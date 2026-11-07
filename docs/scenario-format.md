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
  "version": 1,
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

## Top level

| Member | Type | Required | Meaning |
|---|---|---|---|
| `format` | string | yes | Always `"resend-scenario"` |
| `version` | number | yes | Format version, currently `1` |
| `title` | string | no | Shown when the file is opened |
| `notes` | string | no | What the scenario shows, or what to try |
| `seed` | number | no | Seed for random choices, 0 to 4294967295; default 1. Nothing in version 1 is random yet, but the seed is kept so a scenario stays reproducible when something is |
| `devices` | array | yes | The devices, in any order |
| `links` | array | yes | The cables, in any order |

## Devices

| Member | Type | Required | Meaning |
|---|---|---|---|
| `id` | number | yes | 1 to 4294967295, unique among devices. Cables refer to devices by id |
| `kind` | string | yes | `"host"` (a PC, one port) or `"switch"` (eight ports) |
| `name` | string | yes | 1 to 48 characters, at least one visible, no control characters |
| `powered` | boolean | no | Default `true`. A powered-off device sends nothing and drops what reaches it |
| `x`, `y` | number | no | Top-left corner of the card on the canvas, in whole units from −1000000 to 1000000; default 0 |
| `ports` | array | no | Exactly one entry for a host, eight for a switch, in port order. Left out, every port takes its defaults |

### Ports

| Member | Type | Required | Meaning |
|---|---|---|---|
| `mac` | string | no | Six pairs of hex digits separated by colons, e.g. `"02:00:00:00:00:01"`. Default: `02:00:` followed by the device id for a host, and `02:1p:` followed by the device id for port p (0–7) of a switch, e.g. `02:13:00:00:00:05` for port 4 of switch 5 |
| `up` | boolean | no | Administrative state, default `true`. A disabled port has no carrier, and neither has the port at the other end of its cable |
| `ipv4` | string | hosts | The interface address with its prefix length, e.g. `"192.168.1.10/24"` |

A host's address must be usable on its prefix: not the network or broadcast address of its subnet (except on /31 and /32, which have neither, as in RFC 3021), not 0.0.0.0/8, loopback (127/8), multicast (224/4) or reserved (240/4). Switch ports have no address.

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
- a version newer than this resend understands: `version: this file needs a newer version of resend (format 2)`.

The JSON is strict: whole numbers only, no comments, no trailing commas, no duplicate members, valid UTF-8, at most 64 levels of nesting.

A file that reads correctly is then validated as a network, and every problem is reported with the device or cable it concerns: duplicate ids, a cable to a missing device or port, two cables on one port, a cable between two ports of the same device, a missing or unusable host address, a delay out of range, more than 100 devices, or an Ethernet loop. Loops are refused until resend can protect against broadcast storms. Questionable but valid networks, such as two hosts with the same address or masks that disagree, are accepted: they are worth trying.

## Versions

The version changes only when a file written by a newer resend could be misread by an older one. Older files keep opening: a newer resend reads every earlier version. Version 1 is the first.
