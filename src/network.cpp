#include "network.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <utility>

namespace netsim {
namespace {
constexpr SimTime link_delay = std::chrono::milliseconds(10);
constexpr SimTime response_timeout = std::chrono::seconds(1);

bool unicast(Ipv4Address ip) {
    const auto first = ip.bits() >> 24;
    return first != 0 && first != 127 && first < 224;
}
bool subnet_contains(const Device& host, Ipv4Address target) {
    const auto mask = prefix_mask(host.prefix);
    return (host.ip.bits() & mask) == (target.bits() & mask);
}
bool host_address(Ipv4Address ip, int prefix) {
    const auto host_bits = ip.bits() & ~prefix_mask(prefix);
    return unicast(ip) && host_bits != 0 && host_bits != ~prefix_mask(prefix);
}
bool valid_name(const std::string& name) {
    return !name.empty() && name.size() <= 48 &&
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c > 32; }) &&
        std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
DeviceId device_id(int id) { return DeviceId{static_cast<std::uint32_t>(id)}; }
LinkId link_id(int id) { return LinkId{static_cast<std::uint32_t>(id)}; }
std::uint8_t port_number(int port) { return static_cast<std::uint8_t>(port); }
} // namespace

const Device* Engine::device(int id) const {
    auto it = std::find_if(devices_.begin(), devices_.end(), [id](const auto& item) { return item.id == id; });
    return it == devices_.end() ? nullptr : &*it;
}
Device* Engine::mutable_device(int id) {
    auto it = std::find_if(devices_.begin(), devices_.end(), [id](const auto& item) { return item.id == id; });
    return it == devices_.end() ? nullptr : &*it;
}
const Link* Engine::link(int id) const {
    auto it = std::find_if(links_.begin(), links_.end(), [id](const auto& item) { return item.id == id; });
    return it == links_.end() ? nullptr : &*it;
}
bool Engine::fail(const std::string& message) { last_error_ = message; return false; }
std::uint64_t Engine::time_ms() const {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(scheduler_.now()).count());
}

void Engine::reset_simulation() {
    scheduler_ = {};
    arp_timer_ = echo_timer_ = {};
    trace_.clear();
    arp_.clear();
    mac_.clear();
    events_.clear();
    ping_source_ = -1;
    ping_destination_ = {};
    ping_active_ = awaiting_arp_ = ping_succeeded_ = false;
    last_error_.clear();
}
void Engine::clear() {
    reset_simulation();
    devices_.clear();
    links_.clear();
    next_device_ = next_link_ = 1;
}
int Engine::add_device(DeviceKind kind, float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) { fail("Invalid position."); return -1; }
    if (devices_.size() >= 100) { fail("This prototype supports at most 100 devices."); return -1; }
    reset_simulation();
    const int id = next_device_++;
    Device item;
    item.id = id; item.kind = kind; item.x = x; item.y = y;
    item.name = (kind == DeviceKind::Host ? "PC-" : "Switch-") + std::to_string(id);
    item.mac = MacAddress::local(static_cast<std::uint32_t>(id));
    if (kind == DeviceKind::Host) {
        // 192.168.<third>.<fourth>, counting from 192.168.1.10.
        const auto third = static_cast<std::uint32_t>(1 + ((id - 1) / 240) % 254);
        const auto fourth = static_cast<std::uint32_t>(10 + (id - 1) % 240);
        item.ip = Ipv4Address(0xc0a80000U | third << 8 | fourth);
    }
    devices_.push_back(std::move(item));
    return id;
}
int Engine::free_port(int id) const {
    const auto* item = device(id);
    if (!item) return -1;
    const int count = item->kind == DeviceKind::Host ? 1 : 8;
    for (int port = 0; port < count; ++port) {
        const bool used = std::any_of(links_.begin(), links_.end(), [=](const auto& wire) {
            return (wire.a == id && wire.a_port == port) || (wire.b == id && wire.b_port == port);
        });
        if (!used) return port;
    }
    return -1;
}
bool Engine::connected(int a, int b) const {
    std::vector<int> visit{a};
    std::set<int> seen{a};
    for (std::size_t i = 0; i < visit.size(); ++i) {
        if (visit[i] == b) return true;
        for (const auto& wire : links_) {
            int neighbor = wire.a == visit[i] ? wire.b : wire.b == visit[i] ? wire.a : -1;
            if (neighbor >= 0 && seen.insert(neighbor).second) visit.push_back(neighbor);
        }
    }
    return false;
}
bool Engine::connect(int a, int b) {
    if (!device(a) || !device(b)) return fail("Device not found.");
    if (a == b) return fail("Connect two different devices.");
    if (connected(a, b)) return fail("Ethernet loop: this prototype only accepts loop-free networks; STP is not implemented yet.");
    const int pa = free_port(a), pb = free_port(b);
    if (pa < 0 || pb < 0) return fail("No free port: a PC has one port, a switch has eight.");
    reset_simulation();
    links_.push_back({next_link_++, a, pa, b, pb, true});
    return true;
}
bool Engine::remove_device(int id) {
    if (!device(id)) return fail("Device not found.");
    reset_simulation();
    std::erase_if(devices_, [id](const auto& item) { return item.id == id; });
    std::erase_if(links_, [id](const auto& wire) { return wire.a == id || wire.b == id; });
    return true;
}
bool Engine::remove_link(int id) {
    if (!link(id)) return fail("Link not found.");
    reset_simulation();
    std::erase_if(links_, [id](const auto& wire) { return wire.id == id; });
    return true;
}
bool Engine::configure_host(int id, const std::string& name, const std::string& ip, int prefix) {
    auto* item = mutable_device(id);
    if (!item || item->kind != DeviceKind::Host) return fail("Select a PC to configure IPv4.");
    if (!valid_name(name)) return fail("Use a name of 1 to 48 characters, without control characters.");
    if (prefix < 1 || prefix > 30) return fail("This LAN prototype supports IPv4 prefixes from /1 to /30.");
    const auto parsed = Ipv4Address::parse(ip);
    if (!parsed || !host_address(*parsed, prefix)) return fail("Invalid IPv4 host address: network, broadcast, loopback and multicast addresses are excluded.");
    reset_simulation();
    item->name = name; item->ip = *parsed; item->prefix = prefix;
    return true;
}
bool Engine::rename_device(int id, const std::string& name) {
    auto* item = mutable_device(id);
    if (!item) return fail("Device not found.");
    if (!valid_name(name)) return fail("Use a name of 1 to 48 characters, without control characters.");
    item->name = name;
    last_error_.clear();
    return true;
}
bool Engine::set_link_enabled(int id, bool enabled) {
    auto it = std::find_if(links_.begin(), links_.end(), [id](const auto& wire) { return wire.id == id; });
    if (it == links_.end()) return fail("Link not found.");
    reset_simulation();
    it->enabled = enabled;
    return true;
}
bool Engine::set_device_enabled(int id, bool enabled) {
    auto* item = mutable_device(id);
    if (!item) return fail("Device not found.");
    reset_simulation();
    item->enabled = enabled;
    return true;
}
bool Engine::move_device(int id, float x, float y) {
    auto* item = mutable_device(id);
    if (!item || !std::isfinite(x) || !std::isfinite(y)) return fail("Invalid position or device.");
    item->x = x; item->y = y;
    last_error_.clear();
    return true;
}
void Engine::seed_demo() {
    clear();
    const int left = add_device(DeviceKind::Host, 100, 130);
    const int right = add_device(DeviceKind::Host, 520, 130);
    const int sw = add_device(DeviceKind::Switch, 310, 280);
    configure_host(left, "PC-A", "192.168.1.10", 24);
    configure_host(right, "PC-B", "192.168.1.20", 24);
    rename_device(sw, "Switch-01");
    connect(left, sw);
    connect(right, sw);
}

std::vector<ArpEntry> Engine::arp_table(int id) const {
    std::vector<ArpEntry> result;
    auto table = arp_.find(id);
    if (table != arp_.end()) for (const auto& [ip, mac] : table->second) result.push_back({ip, mac});
    return result;
}
std::vector<MacEntry> Engine::mac_table(int id) const {
    std::vector<MacEntry> result;
    auto table = mac_.find(id);
    if (table != mac_.end()) for (const auto& [mac, port] : table->second) result.push_back({mac, port});
    return result;
}
void Engine::record(TraceRecord record) { trace_.add(scheduler_.now(), record); }
void Engine::emit(const std::string& protocol, const std::string& summary,
                  const std::string& detail, int from, int to, int link_id, bool success) {
    events_.push_back({time_ms(), protocol, summary, detail, from, to, link_id, success});
}
bool Engine::pending() const { return !scheduler_.empty(); }
bool Engine::transmit(int from, int port, PduId pdu) {
    const auto* source = device(from);
    if (!source || !source->enabled) return false;
    for (const auto& wire : links_) {
        if (!wire.enabled) continue;
        int target = -1, ingress = -1;
        if (wire.a == from && wire.a_port == port) { target = wire.b; ingress = wire.b_port; }
        if (wire.b == from && wire.b_port == port) { target = wire.a; ingress = wire.a_port; }
        if (target < 0) continue;
        scheduler_.schedule(scheduler_.now() + link_delay,
                            {.kind = ActionKind::Deliver, .from = from, .to = target, .link_id = wire.id, .port = ingress, .pdu = pdu});
        record({.kind = TraceKind::FrameSent, .device = device_id(from), .port = port_number(port),
                .link = link_id(wire.id), .pdu = pdu});
        return true;
    }
    return false;
}
bool Engine::start_ping(int source_id, const std::string& destination_ip) {
    const auto* source = device(source_id);
    if (!source || source->kind != DeviceKind::Host) return fail("Select a PC as the ping source.");
    if (!source->enabled) return fail("The source PC is powered off.");
    if (pending()) return fail("Finish the pending events or reset the simulation.");
    const auto target = Ipv4Address::parse(destination_ip);
    if (!target || !unicast(*target)) return fail("Enter a valid unicast IPv4 address.");
    if (subnet_contains(*source, *target) && !host_address(*target, source->prefix))
        return fail("Pinging network or broadcast addresses is not part of this prototype.");
    ping_source_ = source_id; ping_destination_ = *target;
    current_ping_ = ++next_ping_;
    ping_active_ = true; awaiting_arp_ = false; ping_succeeded_ = false;
    last_error_.clear();
    const auto now = scheduler_.now();
    if (source->ip == *target) scheduler_.schedule(now, {.kind = ActionKind::LocalPing});
    else if (!subnet_contains(*source, *target)) scheduler_.schedule(now, {.kind = ActionKind::NoRoute});
    else if (arp_[source_id].contains(*target)) send_echo_request();
    else {
        awaiting_arp_ = true;
        arp_timer_ = scheduler_.schedule(now, {.kind = ActionKind::ArpProbe, .attempt = 1});
    }
    return true;
}
void Engine::send_echo_request() {
    const auto* source = device(ping_source_);
    if (!source) return;
    awaiting_arp_ = false;
    scheduler_.cancel(arp_timer_);
    const auto pdu = trace_.archive({PduKind::EchoRequest, source->mac, arp_[source->id][ping_destination_],
                                     source->ip, ping_destination_, current_ping_});
    if (!transmit(source->id, 0, pdu))
        record({.kind = TraceKind::FrameDropped, .reason = Reason::NoCarrier, .device = device_id(source->id), .port = port_number(0), .pdu = pdu});
    echo_timer_ = scheduler_.schedule(scheduler_.now() + response_timeout, {.kind = ActionKind::EchoTimeout});
}
void Engine::deliver(const Action& action) {
    const auto* target = device(action.to);
    const auto* source = device(action.from);
    const auto* wire = link(action.link_id);
    if (!target || !source || !wire) return;
    // A copy: archiving a reply below may reallocate the archive.
    const Frame frame = trace_.pdu(action.pdu);
    const TraceRecord here{.device = device_id(target->id), .port = port_number(action.port),
                           .link = link_id(wire->id), .pdu = action.pdu};
    const auto note = [&](TraceKind kind, Reason reason = Reason::None) {
        auto entry = here;
        entry.kind = kind;
        entry.reason = reason;
        record(entry);
    };
    const auto reply = [&](const Frame& response) {
        const auto pdu = trace_.archive(response);
        if (!transmit(target->id, 0, pdu))
            record({.kind = TraceKind::FrameDropped, .reason = Reason::NoCarrier, .device = here.device, .port = port_number(0), .pdu = pdu});
    };
    const bool arp = is_arp(frame.kind);
    const std::string protocol = arp ? "ARP" : "ICMP";
    const std::string type = frame.kind == PduKind::ArpRequest ? "ARP Request" :
        frame.kind == PduKind::ArpReply ? "ARP Reply" :
        frame.kind == PduKind::EchoRequest ? "Echo Request" : "Echo Reply";
    const auto src_mac = frame.src_mac.to_string(), src_ip = frame.src_ip.to_string();
    std::string detail = "Ethernet: " + src_mac + " → " + frame.dst_mac.to_string() +
        "\n" + (arp ? "ARP: " : "IPv4: ") + src_ip + " → " + frame.dst_ip.to_string() +
        "\n" + type + "; ingress port " + std::to_string(action.port + 1) + ".";
    std::string summary = source->name + " → " + target->name + " · " + type;
    if (!wire->enabled || !target->enabled) {
        note(TraceKind::FrameDropped, wire->enabled ? Reason::DeviceDown : Reason::LinkDown);
        detail += "\nCannot be received: the link or the device is disabled.";
        emit(protocol, summary, detail, action.from, action.to, action.link_id, false);
        return;
    }
    note(TraceKind::FrameReceived);
    bool ok = true;
    if (target->kind == DeviceKind::Switch) {
        // Learning is always from the source address on the ingress port.
        auto& table = mac_[target->id];
        const bool learned = !table.contains(frame.src_mac) || table[frame.src_mac] != action.port;
        table[frame.src_mac] = action.port;
        if (learned) {
            note(TraceKind::MacLearned);
            detail += "\nLearned source MAC " + src_mac + " on port " + std::to_string(action.port + 1) + ".";
        }
        const auto destination = table.find(frame.dst_mac);
        if (frame.dst_mac.is_broadcast() || destination == table.end()) {
            note(TraceKind::FrameFlooded);
            int outputs = 0;
            for (int port = 0; port < 8; ++port)
                if (port != action.port && transmit(target->id, port, action.pdu)) ++outputs;
            detail += "\nFlooded to every active port except the ingress port (" + std::to_string(outputs) + ").";
        } else if (destination->second == action.port) {
            note(TraceKind::FrameFiltered);
            detail += "\nFrame filtered: the destination is on the ingress port.";
        } else {
            note(TraceKind::FrameForwarded);
            const bool sent = transmit(target->id, destination->second, action.pdu);
            if (!sent)
                record({.kind = TraceKind::FrameDropped, .reason = Reason::NoCarrier, .device = here.device,
                        .port = port_number(destination->second), .pdu = action.pdu});
            detail += "\nUnicast on port " + std::to_string(destination->second + 1) +
                (sent ? "." : ": the link is down.");
        }
    } else if (frame.dst_mac != target->mac && !frame.dst_mac.is_broadcast()) {
        note(TraceKind::FrameDropped, Reason::NotForUs);
        detail += "\nFrame ignored: the destination MAC is not the local MAC.";
    } else if (arp) {
        // RFC 826's merge step refreshes existing entries; a targeted request
        // or reply also creates the sender mapping.
        auto& cache = arp_[target->id];
        if (cache.contains(frame.src_ip) || frame.dst_ip == target->ip) {
            cache[frame.src_ip] = frame.src_mac;
            note(TraceKind::ArpCacheUpdated);
            detail += "\nARP cache updated: " + src_ip + " = " + src_mac + ".";
        }
        if (frame.dst_ip != target->ip) {
            note(TraceKind::FrameDropped, Reason::ArpForOtherHost);
            detail += "\nRequest for another address: no reply.";
        } else if (frame.kind == PduKind::ArpRequest) {
            reply({PduKind::ArpReply, target->mac, frame.src_mac, target->ip, frame.src_ip, 0});
            detail += "\nThis PC owns the requested IP: it sends a unicast ARP reply.";
        } else if (ping_active_ && awaiting_arp_ && target->id == ping_source_ && frame.src_ip == ping_destination_) {
            send_echo_request();
            detail += "\nMAC resolved: the IPv4 datagram carrying the ICMP Echo Request can be sent.";
        }
    } else if (frame.dst_ip != target->ip) {
        note(TraceKind::FrameDropped, Reason::Ipv4NotForUs);
        detail += "\nDatagram ignored: the destination IP is not the local IP.";
    } else if (frame.kind == PduKind::EchoRequest) {
        if (!subnet_contains(*target, frame.src_ip)) {
            note(TraceKind::FrameDropped, Reason::NoRoute);
            detail += "\nEcho received, but there is no return route: the source is outside the local subnet. Routers and gateways are not implemented yet.";
            ok = false;
        } else {
            // The sender was learned from the preceding ARP exchange. If
            // absent, do not invent an IPv4→MAC mapping from an IP packet.
            const auto entry = arp_[target->id].find(frame.src_ip);
            if (entry == arp_[target->id].end()) {
                note(TraceKind::FrameDropped, Reason::NotModelled);
                detail += "\nReply held back: there is no ARP entry for the sender. Resolving it via ARP before replying is not modelled in this prototype.";
                ok = false;
            } else {
                reply({PduKind::EchoReply, target->mac, entry->second, target->ip, frame.src_ip, frame.ping_id});
                detail += "\nICMP Echo Request received: an Echo Reply with the same identifier is generated.";
            }
        }
    } else if (ping_active_ && target->id == ping_source_ && frame.src_ip == ping_destination_ && frame.ping_id == current_ping_) {
        ping_active_ = false;
        ping_succeeded_ = true;
        scheduler_.cancel(echo_timer_);
        note(TraceKind::PingSucceeded);
        summary = "Ping succeeded · " + src_ip + " → " + target->name;
        detail += "\nEcho Reply matching the current ping. The destination is reachable.";
    }
    emit(protocol, summary, detail, action.from, action.to, action.link_id, ok);
}
bool Engine::step() {
    const auto due = scheduler_.pop();
    if (!due) return false;
    const auto& action = due->payload;
    const auto* source = device(ping_source_);
    const auto origin = device_id(ping_source_);
    const auto destination = ping_destination_.to_string();
    switch (action.kind) {
    case ActionKind::Deliver:
        deliver(action);
        break;
    case ActionKind::ArpProbe: {
        if (!source) return false;
        const auto pdu = trace_.archive({PduKind::ArpRequest, source->mac, MacAddress::broadcast(),
                                         source->ip, ping_destination_, 0});
        const bool sent = transmit(source->id, 0, pdu);
        if (!sent) record({.kind = TraceKind::FrameDropped, .reason = Reason::NoCarrier, .device = origin, .port = port_number(0), .pdu = pdu});
        emit("ARP", source->name + " · who has " + destination + "?",
             "ARP Request sent to broadcast (" + MacAddress::broadcast().to_string() + "). Attempt " + std::to_string(action.attempt) + "/3.\n" +
             (sent ? "The destination is in the local subnet. Looking for its MAC address." : "The port has no active link: no frame can leave."),
             source->id, -1, -1, sent);
        arp_timer_ = scheduler_.schedule(scheduler_.now() + response_timeout,
                                         {.kind = ActionKind::ArpTimeout, .attempt = action.attempt});
        break;
    }
    case ActionKind::ArpTimeout:
        record({.kind = TraceKind::ArpTimeout, .device = origin});
        if (action.attempt < 3) {
            emit("ARP", "No ARP reply · retrying",
                 "One simulated second has passed. The PC repeats the ARP request.", ping_source_);
            arp_timer_ = scheduler_.schedule(scheduler_.now(), {.kind = ActionKind::ArpProbe, .attempt = action.attempt + 1});
        } else {
            record({.kind = TraceKind::PingFailed, .reason = Reason::ArpUnresolved, .device = origin});
            emit("ARP", "Ping failed · ARP resolution timed out",
                 "No reply after three ARP requests. Check the address, the cables and that the devices are on. No ICMP was sent.", ping_source_, -1, -1, false);
            ping_active_ = awaiting_arp_ = false;
        }
        break;
    case ActionKind::EchoTimeout:
        record({.kind = TraceKind::PingFailed, .reason = Reason::EchoTimeout, .device = origin});
        emit("ICMP", "Ping failed · reply timed out",
             "The MAC is known but no Echo Reply arrived. Check the subnet and the return path too.", ping_source_, -1, -1, false);
        ping_active_ = false;
        break;
    case ActionKind::LocalPing:
        record({.kind = TraceKind::PingSucceeded, .device = origin});
        emit("ICMP", "Local ping succeeded", "The destination is the PC's own address: no frame crosses the cables.", ping_source_, ping_source_);
        ping_active_ = false; ping_succeeded_ = true;
        break;
    case ActionKind::NoRoute:
        record({.kind = TraceKind::PingFailed, .reason = Reason::NoRoute, .device = origin});
        emit("IPv4", "Ping failed · no route",
             "The destination " + destination + " is outside the local subnet. This prototype has no routers or gateways yet, so no ARP request is sent for the remote host.",
             ping_source_, -1, -1, false);
        ping_active_ = false;
        break;
    }
    return true;
}

} // namespace netsim
