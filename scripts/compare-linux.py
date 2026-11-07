#!/usr/bin/env python3
"""Compares resend with the Linux network stack: the exit check of M5.

Builds example networks with network namespaces, one per device: a veth
pair for each cable, a bridge for each switch, forwarding and static routes
for each router. Runs the same ping or traceroute there and in resend-cli,
captures every cable with tcpdump and with resend-cli --pcap-link, and
compares what crossed each cable, frame by frame: Ethernet addresses, ARP
operation and addresses, IPv4 addresses and TTL, ICMP type and code, and
for an ICMP error the datagram it quotes. Timing may differ; the order on a
cable may not.

Linux is set up as resend models it: no IPv6, no multicast on bridges, no
reverse path filter, no ICMP redirects or rate limits. Each run is short enough that no ARP entry
needs confirming, so Linux sends no unicast probes.

Needs iproute2, tcpdump and iputils ping. Without root it runs in a user
namespace (unprivileged user namespaces must be allowed); as root it still
works in namespaces of its own and leaves the system untouched.

Usage: scripts/compare-linux.py RESEND_CLI [--keep DIR] [--only NAME]
  --keep DIR   keep both captures of every cable in DIR
  --only NAME  run only the cases whose name contains NAME
Exit status: 0 every cable matches, 1 a difference, 2 the setup failed.
"""
import argparse
import difflib
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXAMPLES = os.path.join(ROOT, "examples")
INSIDE = "RESEND_COMPARE_INSIDE"


@dataclass
class Case:
    name: str
    scenario: str
    source: str
    destination: str
    traceroute: bool = False
    count: int = 1
    ttl: int = 64
    timeout: float = 1.0
    max_hops: int = 30
    settle: float = 1.0  # seconds to keep capturing after the last request
    until: float = 0.0   # and at least this long after the first


CASES = [
    Case("sample-lan", "sample-lan.json", "PC-A", "192.168.1.20", count=2),
    Case("two-lans", "two-lans-router.json", "PC-A", "10.2.0.20", count=2),
    Case("two-lans-router-address", "two-lans-router.json", "PC-A", "10.2.0.1"),
    Case("two-lans-traceroute", "two-lans-router.json", "PC-A", "10.2.0.20", traceroute=True),
    Case("two-lans-ttl-1", "two-lans-router.json", "PC-A", "10.2.0.20", ttl=1),
    Case("net-unreachable", "two-lans-router.json", "PC-A", "203.0.113.1", count=2),
    # Three ARP requests a second apart, then the error: after 3 s. Before
    # 5 s, when Linux would start confirming its ARP entries.
    Case("host-unreachable", "two-lans-router.json", "PC-A", "10.2.0.99", timeout=4, until=4),
    Case("wrong-gateway-nobody", "wrong-gateway.json", "PC-A", "10.2.0.20", timeout=4, until=4),
    Case("wrong-gateway-off-subnet", "wrong-gateway.json", "PC-C", "10.2.0.20"),
    Case("missing-return-route", "missing-return-route.json", "PC-A", "10.2.0.20"),
    Case("routing-loop", "routing-loop.json", "PC-A", "10.9.9.9"),
    Case("routing-loop-traceroute", "routing-loop.json", "PC-A", "10.9.9.9", traceroute=True, max_hops=6),
    Case("longest-prefix-8", "longest-prefix.json", "PC-A", "10.9.0.10"),
    Case("longest-prefix-16", "longest-prefix.json", "PC-A", "10.20.9.10"),
    Case("longest-prefix-24", "longest-prefix.json", "PC-A", "10.20.30.10"),
    Case("longest-prefix-traceroute", "longest-prefix.json", "PC-A", "10.20.30.10", traceroute=True),
]


class SetupError(Exception):
    pass


def run(*command, check=True, capture=False):
    result = subprocess.run(command, text=True, capture_output=True)
    if check and result.returncode != 0:
        raise SetupError(f"{' '.join(command)}: {result.stderr.strip() or result.returncode}")
    return result.stdout if capture else result


# --- Reading captures ----------------------------------------------------

def frames(path):
    """The frames of a pcap file, in order."""
    with open(path, "rb") as file:
        data = file.read()
    magic = data[:4]
    order = {b"\xd4\xc3\xb2\xa1": "little", b"\x4d\x3c\xb2\xa1": "little",
             b"\xa1\xb2\xc3\xd4": "big", b"\xa1\xb2\x3c\x4d": "big"}.get(magic)
    if not order or int.from_bytes(data[20:24], order) != 1:
        raise SetupError(f"{path}: not an Ethernet pcap file")
    result, at = [], 24
    while at + 16 <= len(data):
        length = int.from_bytes(data[at + 8:at + 12], order)
        result.append(data[at + 16:at + 16 + length])
        at += 16 + length
    return result


def address(raw):
    return ".".join(str(byte) for byte in raw)


def mac(raw):
    return ":".join(f"{byte:02x}" for byte in raw)


ICMP = {(0, 0): "Echo Reply", (8, 0): "Echo Request", (11, 0): "Time Exceeded",
        (3, 0): "Destination Unreachable (network)", (3, 1): "Destination Unreachable (host)"}


def ipv4(packet, quoted=False):
    header = (packet[0] & 0xF) * 4
    text = f"{address(packet[12:16])} → {address(packet[16:20])} TTL {packet[8]}"
    if packet[9] != 1:
        return f"IPv4 protocol {packet[9]} {text}"
    icmp = packet[header:]
    name = ICMP.get((icmp[0], icmp[1]), f"ICMP {icmp[0]}/{icmp[1]}")
    if icmp[0] in (3, 11) and not quoted:
        return f"{name} {text}, about {ipv4(icmp[8:], True)}"
    return f"{name} {text}"


def describe(frame):
    """One line for a frame, with everything the comparison looks at."""
    ethernet = f"{mac(frame[6:12])} → {mac(frame[0:6])}"
    ethertype = int.from_bytes(frame[12:14], "big")
    if ethertype == 0x0806:
        operation = {1: "request", 2: "reply"}.get(int.from_bytes(frame[20:22], "big"), "operation ?")
        return f"{ethernet} ARP {operation} {address(frame[28:32])} → {address(frame[38:42])}"
    if ethertype == 0x0800:
        return f"{ethernet} {ipv4(frame[14:])}"
    return f"{ethernet} ethertype {ethertype:#06x}"


# --- Building the network in Linux -----------------------------------------

class Network:
    """A scenario built with network namespaces, one per device."""

    def __init__(self, scenario):
        self.scenario = scenario
        self.devices = {device["id"]: device for device in scenario["devices"]}
        self.names = {}  # device id -> namespace

    def namespace(self, device_id):
        return self.names[device_id]

    def build(self):
        for device in self.scenario["devices"]:
            name = f"d{device['id']}"
            self.names[device["id"]] = name
            run("ip", "netns", "add", name)
            # Before any interface exists, so new ones inherit the defaults.
            run("ip", "netns", "exec", name, "sysctl", "-qw",
                "net.ipv6.conf.all.disable_ipv6=1", "net.ipv6.conf.default.disable_ipv6=1",
                "net.ipv4.conf.all.rp_filter=0", "net.ipv4.conf.default.rp_filter=0",
                "net.ipv4.conf.all.send_redirects=0", "net.ipv4.conf.default.send_redirects=0",
                "net.ipv4.icmp_ratelimit=0",
                f"net.ipv4.ip_forward={1 if device['kind'] == 'router' else 0}")
            run("ip", "-n", name, "link", "set", "lo", "up")
            if device["kind"] == "switch":
                ageing = int(float(device.get("ageing", "300s").rstrip("s")) * 100)
                # Without multicast the bridge sends no IGMP reports of its own.
                run("ip", "-n", name, "link", "add", "br0", "type", "bridge", "stp_state", "0", "ageing_time", str(ageing),
                    "mcast_snooping", "0")
                run("ip", "-n", name, "link", "set", "br0", "multicast", "off", "up")
        for link in self.scenario["links"]:
            a, b = link["a"], link["b"]
            run("ip", "link", "add", f"p{a['port']}", "netns", self.names[a["device"]], "type", "veth",
                "peer", "name", f"p{b['port']}", "netns", self.names[b["device"]])
            for end in (a, b):
                self.configure_port(end["device"], end["port"], link.get("up", True))
        for device in self.scenario["devices"]:
            name = self.names[device["id"]]
            if device["kind"] == "host" and device.get("gateway"):
                # Linux refuses a gateway outside the subnet, as resend sends
                # nothing to one; a failure here is part of the comparison.
                run("ip", "-n", name, "route", "add", "default", "via", device["gateway"], check=False)
            for route in device.get("routes", []):
                run("ip", "-n", name, "route", "add", route["destination"], "via", route["via"], check=False)

    def configure_port(self, device_id, port, cable_up):
        device = self.devices[device_id]
        name, interface = self.names[device_id], f"p{port}"
        if device["kind"] == "switch":
            run("ip", "-n", name, "link", "set", interface, "master", "br0")
        else:
            config = device["ports"][port - 1]
            run("ip", "-n", name, "link", "set", interface, "address", config["mac"])
            if config.get("ipv4"):
                run("ip", "-n", name, "addr", "add", config["ipv4"], "dev", interface)
        up = cable_up and device.get("powered", True)
        if device["kind"] != "switch":
            up = up and device["ports"][port - 1].get("up", True)
        if up:
            run("ip", "-n", name, "link", "set", interface, "up")

    def host(self, name):
        for device in self.scenario["devices"]:
            if device["name"] == name:
                return self.names[device["id"]]
        raise SetupError(f"no device named {name}")

    def destroy(self):
        for name in self.names.values():
            run("ip", "netns", "delete", name, check=False)


def ping(namespace, destination, count, ttl, timeout):
    """Runs iputils ping; its output, for traceroute to read."""
    result = subprocess.run(["ip", "netns", "exec", namespace, "ping", "-n", "-c", str(count), "-i", "1",
                             "-t", str(ttl), "-W", str(timeout), destination],
                            text=True, capture_output=True, env={**os.environ, "LC_ALL": "C"})
    return result.stdout + result.stderr


def linux_run(case, scenario, folder):
    network = Network(scenario)
    captures = {}
    try:
        network.build()
        dumps = []
        for link in scenario["links"]:
            end = link["a"]
            path = os.path.join(folder, f"linux-{link['id']}.pcap")
            dump = subprocess.Popen(["ip", "netns", "exec", network.namespace(end["device"]), "tcpdump", "-i",
                                     f"p{end['port']}", "-w", path, "-U", "-n", "-s", "0", "-Z", "root"],
                                    stderr=subprocess.PIPE, text=True)
            # tcpdump says when it is listening.
            said = []
            while not said or ("listening" not in said[-1] and said[-1]):
                said.append(dump.stderr.readline())
            if "listening" not in said[-1]:
                raise SetupError(f"tcpdump: {''.join(said).strip()}")
            dumps.append(dump)
            captures[link["id"]] = path
        source = network.host(case.source)
        start = time.monotonic()
        if case.traceroute:
            # As resend's traceroute: one request at a time, TTL 1, 2, ...,
            # until the destination or an unreachable answers.
            for hop in range(1, case.max_hops + 1):
                output = ping(source, case.destination, 1, hop, case.timeout)
                if "bytes from" in output or "Unreachable" in output:
                    break
        else:
            ping(source, case.destination, case.count, case.ttl, case.timeout)
        time.sleep(max(case.settle, start + case.until - time.monotonic()))
        for dump in dumps:
            dump.send_signal(signal.SIGINT)
        for dump in dumps:
            dump.wait(timeout=5)
    finally:
        network.destroy()
    return captures


def resend_run(case, cli, scenario_path, links, folder):
    captures = {}
    command = [cli, "--scenario", scenario_path, "--timeout", f"{case.timeout}s"]
    if case.traceroute:
        command += ["--max-hops", str(case.max_hops), "--traceroute", case.source, case.destination]
    else:
        command += ["--count", str(case.count), "--ttl", str(case.ttl), "--ping", case.source, case.destination]
    for link in links:
        path = os.path.join(folder, f"resend-{link['id']}.pcap")
        result = subprocess.run(command + ["--pcap", path, "--pcap-link", str(link["id"])], capture_output=True, text=True)
        if result.returncode not in (0, 1):
            raise SetupError(f"resend-cli failed ({result.returncode}): {result.stderr.strip()}")
        captures[link["id"]] = path
    return captures


def compare(case, cli, keep):
    scenario_path = os.path.join(EXAMPLES, case.scenario)
    with open(scenario_path, encoding="utf-8") as file:
        scenario = json.load(file)
    folder = tempfile.mkdtemp(prefix=f"resend-{case.name}-")
    try:
        ours = resend_run(case, cli, scenario_path, scenario["links"], folder)
        theirs = linux_run(case, scenario, folder)
        same, total, report = True, 0, []
        for link in scenario["links"]:
            a = [describe(frame) for frame in frames(ours[link["id"]])]
            b = [describe(frame) for frame in frames(theirs[link["id"]])]
            total += len(b)
            if a != b:
                same = False
                report.append(f"  cable {link['id']}:")
                report += ["    " + line for line in difflib.unified_diff(a, b, "resend", "linux", lineterm="", n=2)]
        if keep:
            target = os.path.join(keep, case.name)
            os.makedirs(target, exist_ok=True)
            for path in list(ours.values()) + list(theirs.values()):
                shutil.copy(path, target)
        verdict = "same" if same else "DIFFERENT"
        print(f"{case.name}: {len(scenario['links'])} cables, {total} frames, {verdict}")
        for line in report:
            print(line)
        return same
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def enter_namespaces():
    """Re-runs this script in namespaces of its own: a user namespace
    without root, a network namespace and a mount namespace, where a
    private /run holds the device namespaces."""
    command = ["unshare", "--net", "--mount"]
    if os.geteuid() != 0:
        command += ["--user", "--map-root-user"]
    os.environ[INSIDE] = "1"
    os.execvp(command[0], command + [sys.executable, os.path.abspath(__file__)] + sys.argv[1:])


def main():
    parser = argparse.ArgumentParser(description="Compare resend with the Linux network stack.")
    parser.add_argument("cli", help="the resend-cli executable")
    parser.add_argument("--keep", help="keep the captures in this directory")
    parser.add_argument("--only", help="run only the cases whose name contains this")
    arguments = parser.parse_args()
    for tool in ("ip", "tcpdump", "ping", "unshare"):
        if not shutil.which(tool):
            print(f"{tool} not found", file=sys.stderr)
            return 2
    cli = os.path.abspath(arguments.cli)
    if not os.environ.get(INSIDE):
        enter_namespaces()
    try:
        run("mount", "-t", "tmpfs", "tmpfs", "/run")
        os.makedirs("/run/netns")
        keep = os.path.abspath(arguments.keep) if arguments.keep else None
        cases = [case for case in CASES if not arguments.only or arguments.only in case.name]
        results = [compare(case, cli, keep) for case in cases]
    except SetupError as error:
        print(f"setup failed: {error}", file=sys.stderr)
        return 2
    print(f"{sum(results)}/{len(results)} cases match Linux.")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
