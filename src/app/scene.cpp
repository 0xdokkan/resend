#include "app/scene.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace resend {
namespace {
using netsim::PduKind;
using netsim::Reason;
using netsim::TraceKind;
using netsim::TraceRecord;

float distance(Point a, Point b) { return std::hypot(b.x - a.x, b.y - a.y); }

// Where the ray from the centre of `box` towards `target` leaves the box.
Point exit_point(const Box& box, Point target) {
    const Point c = box.center();
    const float dx = target.x - c.x, dy = target.y - c.y;
    if (dx == 0 && dy == 0) return c;
    const float sx = dx == 0 ? INFINITY : box.width / 2 / std::abs(dx);
    const float sy = dy == 0 ? INFINITY : box.height / 2 / std::abs(dy);
    const float s = std::min(sx, sy);
    return {c.x + dx * s, c.y + dy * s};
}

std::string owner_name(const netsim::Engine& engine, const netsim::MacAddress& mac) {
    for (const auto& item : engine.devices())
        if (item.mac == mac) return item.name;
    return mac.to_string();
}

std::string port_name(int zero_based) { return "p" + std::to_string(zero_based + 1); }

std::string drop_text(Reason reason) {
    switch (reason) {
    case Reason::LinkDown: return "× link down";
    case Reason::DeviceDown: return "× device off";
    case Reason::NoCarrier: return "× no cable";
    case Reason::NotForUs: return "× not my MAC";
    case Reason::ArpForOtherHost: return "× not my IP";
    case Reason::Ipv4NotForUs: return "× not my IP";
    case Reason::NoRoute: return "× no route back";
    case Reason::NotModelled: return "× not modelled";
    default: return "× dropped";
    }
}

std::string ping_failure_text(Reason reason) {
    switch (reason) {
    case Reason::ArpUnresolved: return "× ping: no ARP reply";
    case Reason::EchoTimeout: return "× ping: timed out";
    case Reason::NoRoute: return "× ping: no route";
    default: return "× ping failed";
    }
}

// The marker a record leaves on its device, if any. `index` is the record's
// position, used to look at the records that follow it at the same instant.
bool marker_for(const netsim::Engine& engine, std::size_t index, Marker& marker) {
    const auto& records = engine.trace().records();
    const TraceRecord& record = records[index];
    marker.device = static_cast<int>(record.device.value);
    // Egress records a switch decision produces right after it.
    const auto following = [&](auto&& visit) {
        for (std::size_t next = index + 1; next < records.size(); ++next) {
            const auto& other = records[next];
            if (other.time != record.time || other.device != record.device || other.pdu != record.pdu) break;
            visit(other);
        }
    };
    switch (record.kind) {
    case TraceKind::MacLearned: {
        const auto& frame = engine.trace().pdu(*record.pdu);
        marker.text = "learn " + owner_name(engine, frame.src_mac) + " → " + port_name(*record.port);
        marker.tone = Tone::Learn;
        return true;
    }
    case TraceKind::FrameFlooded: {
        int copies = 0;
        following([&](const TraceRecord& other) { copies += other.kind == TraceKind::FrameSent; });
        marker.text = "flood ×" + std::to_string(copies);
        marker.tone = Tone::Forward;
        return true;
    }
    case TraceKind::FrameForwarded: {
        marker.text = "forward";
        following([&](const TraceRecord& other) {
            if (other.kind == TraceKind::FrameSent && other.port) marker.text = "forward → " + port_name(*other.port);
            if (other.kind == TraceKind::FrameDropped && other.port) marker.text = "forward → " + port_name(*other.port) + " ×";
        });
        marker.tone = Tone::Forward;
        return true;
    }
    case TraceKind::FrameFiltered:
        marker.text = "filtered";
        marker.tone = Tone::Muted;
        return true;
    case TraceKind::ArpCacheUpdated: {
        const auto& frame = engine.trace().pdu(*record.pdu);
        marker.text = "ARP + " + frame.src_ip.to_string();
        marker.tone = Tone::Learn;
        return true;
    }
    case TraceKind::FrameDropped:
        marker.text = drop_text(record.reason);
        marker.tone = Tone::Drop;
        return true;
    case TraceKind::ArpTimeout:
        marker.text = "ARP timeout";
        marker.tone = Tone::Wait;
        return true;
    case TraceKind::PingSucceeded:
        marker.text = "✓ ping reply";
        marker.tone = Tone::Success;
        return true;
    case TraceKind::PingFailed:
        marker.text = ping_failure_text(record.reason);
        marker.tone = Tone::Drop;
        return true;
    case TraceKind::FrameSent:
    case TraceKind::FrameReceived:
        return false;
    }
    return false;
}
} // namespace

Box card_box(const netsim::Device& device) { return {device.x, device.y, card_width, card_height}; }

int port_count(const netsim::Device& device) { return device.kind == netsim::DeviceKind::Host ? 1 : max_ports; }

Box port_light(const netsim::Device& device, int port) {
    if (device.kind == netsim::DeviceKind::Host) return {device.x + 112, device.y + 69, 11, 8};
    return {device.x + 12 + 14 * static_cast<float>(port), device.y + 69, 11, 8};
}

Segment cable_between(const Box& a, const Box& b, float gap) {
    Point from = exit_point(a, b.center()), to = exit_point(b, a.center());
    const float length = distance(a.center(), b.center());
    if (length > 0) {
        const float ux = (b.center().x - a.center().x) / length, uy = (b.center().y - a.center().y) / length;
        from = {from.x + ux * gap, from.y + uy * gap};
        to = {to.x - ux * gap, to.y - uy * gap};
    }
    return {from, to};
}

Point lerp(Point a, Point b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

std::string packet_label(PduKind kind) {
    switch (kind) {
    case PduKind::ArpRequest: return "ARP req";
    case PduKind::ArpReply: return "ARP rep";
    case PduKind::EchoRequest: return "Echo req";
    case PduKind::EchoReply: return "Echo rep";
    }
    return "Frame";
}

std::string frame_name(PduKind kind) {
    switch (kind) {
    case PduKind::ArpRequest: return "ARP Request";
    case PduKind::ArpReply: return "ARP Reply";
    case PduKind::EchoRequest: return "Echo Request";
    case PduKind::EchoReply: return "Echo Reply";
    }
    return "Frame";
}

std::string describe_layers(const netsim::Frame& frame) {
    const bool arp = netsim::is_arp(frame.kind);
    std::string text = "Ethernet II · dst " + frame.dst_mac.to_string() + " · src " + frame.src_mac.to_string() +
        (arp ? " · type 0x0806 (ARP)" : " · type 0x0800 (IPv4)");
    if (frame.kind == PduKind::ArpRequest) {
        text += "\nARP · request · sender " + frame.src_mac.to_string() + " " + frame.src_ip.to_string() +
            " · target " + frame.dst_ip.to_string();
    } else if (frame.kind == PduKind::ArpReply) {
        text += "\nARP · reply · sender " + frame.src_mac.to_string() + " " + frame.src_ip.to_string() +
            " · target " + frame.dst_mac.to_string() + " " + frame.dst_ip.to_string();
    } else {
        const bool request = frame.kind == PduKind::EchoRequest;
        text += "\nIPv4 · " + frame.src_ip.to_string() + " → " + frame.dst_ip.to_string() + " · protocol 1 (ICMP)";
        text += std::string("\nICMP · type ") + (request ? "8 Echo Request" : "0 Echo Reply") +
            " · code 0 · identifier " + std::to_string(frame.ping_id);
    }
    return text;
}

Scene build_scene(const netsim::Engine& engine, const PlaybackController& playback) {
    Scene scene;
    const auto now = playback.now();
    const auto& trace = engine.trace();

    for (const auto& flight : playback.in_flight()) {
        const auto& sent = trace.transmissions()[flight.transmission];
        const auto* from = engine.device(static_cast<int>(sent.from.value));
        const auto* to = engine.device(static_cast<int>(sent.to.value));
        if (!from || !to) continue;
        const auto cable = cable_between(card_box(*from), card_box(*to));
        const auto progress = static_cast<float>(flight.progress);
        const float length = distance(cable.a, cable.b);
        const float back = length > 0 ? std::min(progress, 28.0f / length) : 0.0f;
        const auto& frame = trace.pdu(sent.pdu);
        PacketSprite sprite;
        sprite.transmission = flight.transmission;
        sprite.pdu = static_cast<int>(sent.pdu.value);
        sprite.at = lerp(cable.a, cable.b, progress);
        sprite.tail = lerp(cable.a, cable.b, progress - back);
        sprite.kind = frame.kind;
        sprite.broadcast = frame.dst_mac.is_broadcast();
        sprite.label = packet_label(frame.kind);
        sprite.description = frame_name(frame.kind) + (sprite.broadcast ? ", broadcast, " : ", ") +
            from->name + " → " + to->name;
        scene.packets.push_back(std::move(sprite));
    }

    // Records are never ahead of the displayed time: scan back from the end.
    const auto& records = trace.records();
    std::vector<Marker> markers;
    std::map<int, unsigned> activity;
    std::size_t index = records.size();
    bool outcome_shown = false;
    while (index > 0 && records[index - 1].time > now - marker_life) {
        --index;
        const auto& record = records[index];
        const auto age = now - record.time;
        if (age < activity_life && record.port && record.link &&
            (record.kind == TraceKind::FrameSent || record.kind == TraceKind::FrameReceived ||
             record.kind == TraceKind::FrameDropped))
            activity[static_cast<int>(record.device.value)] |= 1U << *record.port;
        Marker marker;
        if (!marker_for(engine, index, marker)) continue;
        outcome_shown |= record.kind == TraceKind::PingSucceeded || record.kind == TraceKind::PingFailed;
        if (age > marker_fade) {
            const auto fading = static_cast<float>((age - marker_fade).count()) /
                static_cast<float>((marker_life - marker_fade).count());
            marker.opacity = 1 - 0.85f * fading;
        }
        markers.push_back(std::move(marker));
    }
    // The outcome of the last ping stays until new frames are sent.
    for (std::size_t back = index; back > 0 && !outcome_shown; --back) {
        const auto& record = records[back - 1];
        if (record.kind == TraceKind::FrameSent) break;
        if (record.kind == TraceKind::PingSucceeded || record.kind == TraceKind::PingFailed) {
            Marker marker;
            if (marker_for(engine, back - 1, marker)) markers.push_back(std::move(marker));
            break;
        }
    }
    std::reverse(markers.begin(), markers.end());  // oldest first

    // Chips stack above their card, the most recent closest to it.
    std::map<int, int> remaining;
    for (const auto& marker : markers) ++remaining[marker.device];
    for (auto& marker : markers) {
        const auto* device = engine.device(marker.device);
        if (!device) continue;
        marker.stack = --remaining[marker.device];
        marker.anchor = {device->x + card_width / 2, device->y};
        scene.markers.push_back(std::move(marker));
    }
    scene.port_activity.assign(activity.begin(), activity.end());
    return scene;
}

} // namespace resend
