// The canvas's reactions to the records (G4.1): computed from the trace and
// the instant shown, like the packets, so that dragging the timeline back
// shows the same effects as playing up to the instant.
#include "app/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>

namespace resend {
namespace {
using netsim::TraceKind;
using netsim::TraceRecord;

constexpr float pi = std::numbers::pi_v<float>;

std::optional<EffectKind> effect_of(TraceKind kind) {
    for (const auto& [record, effect] : effect_sources)
        if (record == kind) return effect;
    return std::nullopt;
}

// Where an effect at a device shows: the middle of its sprite, as
// ui/canvas.slint places it at the card's top left.
Point sprite_center(const netsim::DeviceConfig& device) {
    const auto card = card_box(device);
    return {card.x + 30, card.y + 24};
}

std::string protocol_tone(netsim::PduKind kind) { return netsim::is_arp(kind) ? "ask" : "ok"; }

float ease_out(float a) { return 1 - (1 - a) * (1 - a); }

// A number from 0 to 1 that depends on the record and the particle only.
float noise(std::size_t record, std::uint32_t particle) {
    std::uint64_t x = (static_cast<std::uint64_t>(record) << 8) ^ particle;
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return static_cast<float>(x >> 40) / static_cast<float>(1ULL << 24);
}

// The effects a record makes, if it makes any: a flood makes one for each copy.
void effects_for(const netsim::Scenario& layout, const netsim::Simulation& simulation, std::size_t index, netsim::SimTime now,
                 std::vector<Effect>& out) {
    const auto& trace = simulation.trace();
    const auto& records = trace.records();
    const TraceRecord& record = records[index];
    const auto kind = effect_of(record.kind);
    if (!kind) return;
    Effect effect;
    effect.kind = *kind;
    effect.record = index;
    effect.device = static_cast<int>(record.device.value);
    const auto* device = record.device.value != 0 ? layout.device(record.device) : nullptr;
    const auto aged = [&](Effect item) {
        const auto life = effect_life(item.kind, item.big);
        const auto age = now - record.time;
        if (age < netsim::SimTime{} || age >= life) return;
        item.age = static_cast<float>(age.count()) / static_cast<float>(life.count());
        out.push_back(std::move(item));
    };
    // The other end of a cable of `device`, as the packets travel it.
    const auto other_end = [&](std::optional<netsim::LinkId> link) -> const netsim::DeviceConfig* {
        const auto* cable = link ? layout.link(*link) : nullptr;
        if (!cable || !device) return nullptr;
        const auto other = cable->a.device == device->id ? cable->b.device : cable->a.device;
        return layout.device(other);
    };
    switch (*kind) {
    case EffectKind::Arrival: {
        const auto* from = other_end(record.link);
        if (!from || !record.pdu) return;
        effect.at = cable_between(card_box(*from), card_box(*device)).b;
        effect.tone = protocol_tone(netsim::kind_of(trace.pdu(*record.pdu)));
        aged(effect);
        return;
    }
    case EffectKind::Learn:
        if (!device || !record.port) return;
        effect.at = port_light(*device, *record.port).center();
        effect.tone = "link";
        aged(effect);
        return;
    case EffectKind::Flood: {
        if (!device || !record.pdu) return;
        effect.tone = protocol_tone(netsim::kind_of(trace.pdu(*record.pdu)));
        for (std::size_t next = index + 1; next < records.size(); ++next) {
            const auto& copy = records[next];
            if (copy.episode != record.episode || copy.device != record.device || copy.pdu != record.pdu) break;
            if (copy.kind != TraceKind::FrameSent) continue;
            const auto* to = other_end(copy.link);
            if (!to) continue;
            effect.at = cable_between(card_box(*device), card_box(*to)).a;
            aged(effect);
        }
        return;
    }
    case EffectKind::Drop:
        if (!device || !is_problem(record.reason)) return;
        effect.at = sprite_center(*device);
        effect.tone = "drop";
        aged(effect);
        return;
    case EffectKind::Reply: {
        // A single request has the ping's own burst, as it has its own chip.
        const auto* ping = simulation.ping(netsim::PingId{record.value});
        if (!device || !ping) return;
        if (record.kind == TraceKind::PingReply && ping->options.count == 1) return;
        effect.big = record.kind == TraceKind::PingSucceeded;
        effect.at = sprite_center(*device);
        effect.tone = "ok";
        aged(effect);
        return;
    }
    case EffectKind::Failure:
        if (!device) return;
        effect.at = sprite_center(*device);
        effect.tone = "drop";
        aged(effect);
        return;
    case EffectKind::Guard:
        if (device) {
            effect.at = sprite_center(*device);
        } else if (!layout.devices.empty()) {
            // The middle of the network.
            float x = 0, y = 0;
            for (const auto& item : layout.devices) {
                x += card_box(item).center().x;
                y += card_box(item).center().y;
            }
            effect.at = {x / static_cast<float>(layout.devices.size()), y / static_cast<float>(layout.devices.size())};
        }
        effect.tone = "guard";
        aged(effect);
        return;
    }
}

void ring(Scene& scene, Point at, float size, const std::string& tone, float opacity) {
    if (opacity > 0.01f) scene.shapes.push_back({Shape::Form::Ring, at, size, tone, opacity});
}

// The particles of a full effect; `room` is how many shapes may still be added.
void particles(Scene& scene, const Effect& effect, std::size_t room) {
    const float a = effect.age, e = ease_out(a);
    const auto add = [&](Point at, float size, std::string tone, float opacity) {
        if (room == 0 || opacity <= 0.01f) return;
        --room;
        scene.shapes.push_back({Shape::Form::Dot, at, size, std::move(tone), opacity});
    };
    const auto spray = [&](int count, float reach, float lift, float gravity, float size) {
        for (int i = 0; i < count; ++i) {
            const float angle = 2 * pi * (static_cast<float>(i) + noise(effect.record, static_cast<std::uint32_t>(i))) / static_cast<float>(count);
            const float speed = reach * (0.6f + 0.4f * noise(effect.record, static_cast<std::uint32_t>(i + 64)));
            const Point at{effect.at.x + std::cos(angle) * speed * e, effect.at.y + std::sin(angle) * speed * e - lift * e + gravity * a * a};
            add(at, std::max(1.0f, size * (1 - a)), effect.tone, 1 - a);
        }
    };
    switch (effect.kind) {
    case EffectKind::Arrival: spray(3, 14, 0, 0, 2.5f); break;
    case EffectKind::Learn: spray(4, 10, 6, 0, 2); break;
    case EffectKind::Flood: break;
    case EffectKind::Drop: spray(6, 10, 12, 0, 3.5f); break;
    case EffectKind::Failure: spray(4, 8, 0, 18, 3); break;
    case EffectKind::Guard: break;
    case EffectKind::Reply:
        if (!effect.big) {
            spray(6, 20, 0, 0, 3);
            break;
        }
        // Confetti: thrown up, falling back, in every family.
        static const char* const tones[] = {"ok", "ask", "link", "route", "accent"};
        for (int i = 0; i < 12; ++i) {
            const auto n = static_cast<std::uint32_t>(i);
            const float angle = -pi * (0.1f + 0.8f * noise(effect.record, n));
            const float speed = 26 + 22 * noise(effect.record, n + 32);
            const Point at{effect.at.x + std::cos(angle) * speed * e, effect.at.y + std::sin(angle) * speed * e - 26 * e + 46 * a * a};
            add(at, 2.5f, tones[i % 5], std::min(1.0f, (1 - a) * 3));
        }
        break;
    }
}

void draw(Scene& scene, const Effect& effect, Intensity intensity) {
    const float a = effect.age;
    // Reduced: still rings, as if halfway through their life.
    const float e = intensity == Intensity::Full ? ease_out(a) : 0.6f;
    const float fade = 1 - a;
    switch (effect.kind) {
    case EffectKind::Arrival: ring(scene, effect.at, 4 + 10 * e, effect.tone, fade); break;
    case EffectKind::Learn:
        ring(scene, effect.at, 6 + 6 * e, effect.tone, fade);
        scene.shapes.push_back({Shape::Form::Dot, effect.at, 5, effect.tone, 0.6f * fade});
        break;
    case EffectKind::Flood: ring(scene, effect.at, 4 + 8 * e, effect.tone, fade); break;
    case EffectKind::Drop: ring(scene, effect.at, 8 + 14 * e, effect.tone, fade); break;
    case EffectKind::Failure: ring(scene, effect.at, 24 - 10 * e, effect.tone, fade); break;
    case EffectKind::Reply:
        if (!effect.big) {
            ring(scene, effect.at, 12 + 16 * e, effect.tone, fade);
            break;
        }
        // Two rings, the second a quarter of the life behind.
        for (const float lag : {0.0f, 0.25f}) {
            const float k = (a - lag) / (1 - lag);
            if (k < 0) continue;
            ring(scene, effect.at, 22 + 24 * (intensity == Intensity::Full ? ease_out(k) : 0.6f), effect.tone, 1 - k);
        }
        break;
    case EffectKind::Guard:
        scene.shapes.push_back({Shape::Form::Flash, {}, 0, effect.tone, (intensity == Intensity::Full ? 0.3f : 0.18f) * fade});
        ring(scene, effect.at, 10 + 30 * e, effect.tone, fade);
        break;
    }
}

} // namespace

std::string_view name(Intensity intensity) {
    switch (intensity) {
    case Intensity::Full: return "full";
    case Intensity::Reduced: return "reduced";
    case Intensity::Off: return "off";
    }
    return "full";
}

std::optional<Intensity> intensity_named(std::string_view text) {
    for (const auto intensity : {Intensity::Full, Intensity::Reduced, Intensity::Off})
        if (name(intensity) == text) return intensity;
    return std::nullopt;
}

std::string_view name(EffectKind kind) {
    switch (kind) {
    case EffectKind::Arrival: return "arrival";
    case EffectKind::Learn: return "learn";
    case EffectKind::Flood: return "flood";
    case EffectKind::Drop: return "drop";
    case EffectKind::Reply: return "reply";
    case EffectKind::Failure: return "failure";
    case EffectKind::Guard: return "guard";
    }
    return "arrival";
}

netsim::SimTime effect_life(EffectKind kind, bool big) {
    using std::chrono::milliseconds;
    switch (kind) {
    case EffectKind::Arrival: return milliseconds(3);
    case EffectKind::Learn: return milliseconds(4);
    case EffectKind::Flood: return milliseconds(4);
    case EffectKind::Drop: return milliseconds(6);
    case EffectKind::Reply: return big ? milliseconds(10) : milliseconds(6);
    case EffectKind::Failure: return milliseconds(6);
    case EffectKind::Guard: return milliseconds(8);
    }
    return milliseconds(3);
}

void add_effects(Scene& scene, const netsim::Scenario& layout, const netsim::Simulation& simulation, netsim::SimTime now,
                 Intensity intensity) {
    if (intensity == Intensity::Off) return;
    const auto longest = effect_life(EffectKind::Reply, true);
    const auto& records = simulation.trace().records();
    std::vector<Effect> found;
    std::size_t index = records.size();
    // Records are never ahead of the instant shown: scan back from the end,
    // newest first, and keep the newest when there are too many.
    while (index > 0 && records[index - 1].time > now - longest && found.size() < max_effects) {
        --index;
        std::vector<Effect> made;
        effects_for(layout, simulation, index, now, made);
        for (auto it = made.rbegin(); it != made.rend() && found.size() < max_effects; ++it) found.push_back(std::move(*it));
    }
    std::reverse(found.begin(), found.end());
    scene.effects = std::move(found);

    for (const auto& effect : scene.effects) draw(scene, effect, intensity);
    if (intensity != Intensity::Full) return;
    for (const auto& effect : scene.effects) {
        if (scene.shapes.size() >= max_shapes) break;
        particles(scene, effect, max_shapes - scene.shapes.size());
    }
    // A device hops when a frame arrives and shakes when its ping fails.
    std::map<int, Point> nudges;
    for (const auto& effect : scene.effects) {
        auto& nudge = nudges[effect.device];
        if (effect.kind == EffectKind::Arrival) nudge.y = std::min(nudge.y, -4 * std::sin(pi * effect.age));
        if (effect.kind == EffectKind::Failure) nudge.x = 3 * std::sin(effect.age * 6 * pi) * (1 - effect.age);
    }
    for (const auto& [device, nudge] : nudges)
        if (device > 0 && (std::abs(nudge.x) > 0.05f || std::abs(nudge.y) > 0.05f)) scene.nudges.emplace_back(device, nudge);
}

} // namespace resend
