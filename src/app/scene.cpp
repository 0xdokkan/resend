#include "app/scene.hpp"

#include "core/text.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <type_traits>
#include <variant>

namespace resend {
namespace {
using netsim::PduKind;
using netsim::Reason;
using netsim::TraceKind;
using netsim::TraceRecord;
using netsim::text::tr;

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

std::string owner_name(const netsim::Scenario& layout, const netsim::MacAddress& mac) {
    for (const auto& item : layout.devices)
        for (const auto& port : item.ports)
            if (port.mac == mac) return item.name;
    return mac.to_string();
}

std::string port_name(int zero_based) { return "p" + std::to_string(zero_based + 1); }

std::string drop_text(Reason reason) {
    switch (reason) {
    case Reason::LinkDown: return "× " + tr("link down");
    case Reason::DeviceDown: return "× " + tr("device off");
    case Reason::NoCarrier: return "× " + tr("no cable");
    case Reason::NotForUs: return "× " + tr("not my MAC");
    case Reason::ArpForOtherHost: return "× " + tr("not my IP");
    case Reason::Ipv4NotForUs: return "× " + tr("not my IP");
    case Reason::NoRoute: return "× " + tr("no route back");
    case Reason::ArpQueueFull: return "× " + tr("ARP queue full");
    case Reason::UnmatchedReply: return "× " + tr("reply too late");
    default: return "× " + tr("dropped");
    }
}

std::string ping_failure_text(Reason reason) {
    switch (reason) {
    case Reason::ArpUnresolved: return "× " + tr("ping: no ARP reply");
    case Reason::EchoTimeout: return "× " + tr("ping: timed out");
    case Reason::NoRoute: return "× " + tr("ping: no route");
    case Reason::DeviceDown: return "× " + tr("ping: PC off");
    case Reason::ArpQueueFull: return "× " + tr("ping: ARP queue full");
    default: return "× " + tr("ping failed");
    }
}

// The chip an edit leaves on its device; cables show their state themselves.
bool edit_marker(const netsim::EditEntry& entry, Marker& marker) {
    marker.tone = Tone::Edit;
    return std::visit(
        [&](const auto& edit) {
            using T = std::decay_t<decltype(edit)>;
            if constexpr (std::is_same_v<T, netsim::SetPowered>) marker.text = "⏻ " + (edit.powered ? tr("powered on") : tr("powered off"));
            else if constexpr (std::is_same_v<T, netsim::SetAddress>) marker.text = "IP " + edit.address.to_string();
            else if constexpr (std::is_same_v<T, netsim::SetPortUp>)
                marker.text = edit.up ? tr("{0} enabled", {port_name(edit.port.port)}) : tr("{0} disabled", {port_name(edit.port.port)});
            else if constexpr (std::is_same_v<T, netsim::SetAgeing>) marker.text = tr("ageing {0}", {netsim::format_duration(edit.ageing)});
            else if constexpr (std::is_same_v<T, netsim::AddDevice>) marker.text = tr("added");
            else if constexpr (std::is_same_v<T, netsim::RestoreDevice>) marker.text = tr("restored");
            else return false;
            return true;
        },
        entry.edit);
}

// The marker a record leaves on its device, if any. `index` is the record's
// position, used to look at the records that follow it at the same instant.
bool marker_for(const netsim::Scenario& layout, const netsim::Simulation& simulation, std::size_t index, Marker& marker) {
    const auto& trace = simulation.trace();
    const auto& records = trace.records();
    const TraceRecord& record = records[index];
    marker.device = static_cast<int>(record.device.value);
    // Egress records a switch decision produces right after it.
    const auto following = [&](auto&& visit) {
        for (std::size_t next = index + 1; next < records.size(); ++next) {
            const auto& other = records[next];
            if (other.episode != record.episode || other.device != record.device || other.pdu != record.pdu) break;
            visit(other);
        }
    };
    switch (record.kind) {
    case TraceKind::MacLearned: {
        const auto& frame = trace.pdu(*record.pdu);
        marker.text = tr("learn {0} → {1}", {owner_name(layout, frame.src), port_name(*record.port)});
        marker.tone = Tone::Learn;
        return true;
    }
    case TraceKind::FrameFlooded: {
        int copies = 0;
        following([&](const TraceRecord& other) { copies += other.kind == TraceKind::FrameSent; });
        marker.text = tr("flood ×{0}", {std::to_string(copies)});
        marker.tone = Tone::Forward;
        return true;
    }
    case TraceKind::FrameForwarded: {
        marker.text = tr("forward");
        following([&](const TraceRecord& other) {
            if (other.kind == TraceKind::FrameSent && other.port) marker.text = tr("forward → {0}", {port_name(*other.port)});
            if (other.kind == TraceKind::FrameDropped && other.port) marker.text = tr("forward → {0}", {port_name(*other.port)}) + " ×";
        });
        marker.tone = Tone::Forward;
        return true;
    }
    case TraceKind::FrameFiltered:
        marker.text = tr("filtered");
        marker.tone = Tone::Muted;
        return true;
    case TraceKind::ArpCacheUpdated: {
        const auto& frame = trace.pdu(*record.pdu);
        marker.text = "ARP + " + (frame.arp() ? frame.arp()->sender_ip.to_string() : std::string("?"));
        marker.tone = Tone::Learn;
        return true;
    }
    case TraceKind::FrameDropped:
        marker.text = drop_text(record.reason);
        marker.tone = Tone::Drop;
        return true;
    case TraceKind::ArpTimeout:
        marker.text = tr("ARP timeout");
        marker.tone = Tone::Wait;
        return true;
    case TraceKind::PingSucceeded: {
        const auto* ping = simulation.ping(netsim::PingId{record.value});
        marker.text = ping && ping->options.count > 1
            ? "✓ " + tr("ping {0}/{1}", {std::to_string(ping->replies()), std::to_string(ping->echoes.size())}) : "✓ " + tr("ping reply");
        marker.tone = Tone::Success;
        return true;
    }
    case TraceKind::PingFailed:
        marker.text = ping_failure_text(record.reason);
        marker.tone = Tone::Drop;
        return true;
    case TraceKind::PingReply:
    case TraceKind::PingLost: {
        // A single request has the ping's own chip.
        const auto* ping = simulation.ping(netsim::PingId{record.value});
        if (!ping || ping->options.count == 1) return false;
        const auto echo = std::find_if(ping->echoes.begin(), ping->echoes.end(), [&](const auto& item) { return item.record == record.seq; });
        const auto number = echo == ping->echoes.end() ? std::string("?") : std::to_string(echo->sequence);
        const bool reply = record.kind == TraceKind::PingReply;
        marker.text = reply ? "✓ " + tr("reply {0}", {number}) : "× " + tr("request {0} lost", {number});
        marker.tone = reply ? Tone::Success : Tone::Drop;
        return true;
    }
    case TraceKind::ArpEntryExpired:
        marker.text = tr("ARP entry expired");
        marker.tone = Tone::Wait;
        return true;
    case TraceKind::MacEntryAged:
        marker.text = tr("entry aged out");
        marker.tone = Tone::Wait;
        return true;
    case TraceKind::DatagramDropped:
        marker.text = "× " + tr("reply discarded");
        marker.tone = Tone::Drop;
        return true;
    case TraceKind::EditApplied:
        return record.device.value != 0 && edit_marker(simulation.edits().at(record.value), marker);
    case TraceKind::SimulatorGuard:
        marker.text = "⚠ " + tr("storm stopped");
        marker.tone = Tone::Guard;
        return true;
    case TraceKind::FrameSent:
    case TraceKind::FrameReceived:
    case TraceKind::PingStarted:
        return false;
    }
    return false;
}
} // namespace

Box card_box(const netsim::DeviceConfig& device) {
    return {static_cast<float>(device.x), static_cast<float>(device.y), card_width, card_height};
}

Box port_light(const netsim::DeviceConfig& device, int port) {
    const auto card = card_box(device);
    if (device.kind == netsim::DeviceKind::Host) return {card.x + 112, card.y + 69, 11, 8};
    return {card.x + 12 + 14 * static_cast<float>(port), card.y + 69, 11, 8};
}

Box connector_box(const netsim::DeviceConfig& device) {
    const auto card = card_box(device);
    return {card.x + 128, card.y + 62, 23, 23};
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
    case PduKind::ArpRequest: return tr("ARP req");
    case PduKind::ArpReply: return tr("ARP rep");
    case PduKind::EchoRequest: return tr("Echo req");
    case PduKind::EchoReply: return tr("Echo rep");
    case PduKind::OtherIcmp: return "ICMP";
    }
    return tr("Frame");
}

std::string frame_name(PduKind kind) {
    switch (kind) {
    case PduKind::ArpRequest: return tr("ARP Request");
    case PduKind::ArpReply: return tr("ARP Reply");
    case PduKind::EchoRequest: return tr("Echo Request");
    case PduKind::EchoReply: return tr("Echo Reply");
    case PduKind::OtherIcmp: return tr("ICMP message");
    }
    return tr("Frame");
}

Scene build_scene(const netsim::Scenario& layout, const netsim::Simulation& simulation, const PlaybackController& playback) {
    Scene scene;
    const auto now = playback.now();
    const auto& trace = simulation.trace();

    for (const auto& flight : playback.in_flight()) {
        const auto& sent = trace.transmissions()[flight.transmission];
        const auto* from = layout.device(sent.from);
        const auto* to = layout.device(sent.to);
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
        sprite.kind = netsim::kind_of(frame);
        sprite.broadcast = frame.dst.is_broadcast();
        sprite.label = packet_label(sprite.kind);
        sprite.description = sprite.broadcast ? tr("{0}, broadcast, {1} → {2}", {frame_name(sprite.kind), from->name, to->name})
                                              : tr("{0}, {1} → {2}", {frame_name(sprite.kind), from->name, to->name});
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
        if (!marker_for(layout, simulation, index, marker)) continue;
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
            if (marker_for(layout, simulation, back - 1, marker)) markers.push_back(std::move(marker));
            break;
        }
    }
    std::reverse(markers.begin(), markers.end());  // oldest first

    // Chips stack above their card, the most recent closest to it.
    std::map<int, int> remaining;
    for (const auto& marker : markers) ++remaining[marker.device];
    for (auto& marker : markers) {
        const auto* device = layout.device(netsim::DeviceId{static_cast<std::uint32_t>(marker.device)});
        if (!device) continue;
        marker.stack = --remaining[marker.device];
        if (marker.stack >= max_chips) continue;
        const auto card = card_box(*device);
        marker.anchor = {card.x + card_width / 2, card.y};
        scene.markers.push_back(std::move(marker));
    }
    scene.port_activity.assign(activity.begin(), activity.end());
    return scene;
}

} // namespace resend
