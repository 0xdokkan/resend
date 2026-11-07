#include "sim/simulation.hpp"

#include "sim/node.hpp"

#include <cassert>
#include <utility>

namespace netsim {
namespace {
template<class... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};
} // namespace

Simulation::Simulation(Scenario scenario) : scenario_(std::move(scenario)) {
    assert(validate(scenario_).empty());
    for (const auto& device : scenario_.devices)
        nodes_.emplace(device.id, device.kind == DeviceKind::Host ? make_host() : make_switch());
}
Simulation::~Simulation() = default;
Simulation::Simulation(Simulation&&) noexcept = default;
Simulation& Simulation::operator=(Simulation&&) noexcept = default;

Node* Simulation::node(DeviceId device) const {
    const auto it = nodes_.find(device);
    return it == nodes_.end() ? nullptr : it->second.get();
}

void Simulation::record(TraceRecord record) {
    record.episode = episode_;
    trace_.add(now(), record);
}

bool Simulation::step() {
    auto due = scheduler_.pop();
    if (!due) return false;
    ++episode_;
    std::visit(Overloaded{
                   [&](const Delivery& delivery) { deliver(delivery); },
                   [&](const Timer& timer) {
                       Context context{*this, timer.device};
                       node(timer.device)->timer(context, timer);
                   },
               },
               due->payload);
    return true;
}

void Simulation::run_until(SimTime time) {
    while (const auto next = scheduler_.next_time()) {
        if (*next > time) break;
        step();
    }
    scheduler_.advance_to(time);
}

void Simulation::deliver(const Delivery& delivery) {
    const auto& target = *scenario_.device(delivery.to.device);
    Context context{*this, target.id};
    const Arrival arrival{delivery.to.port, delivery.link, delivery.pdu};
    const bool up = medium_.up(delivery.link);
    if (!up || !target.powered) {
        context.note(arrival, TraceKind::FrameDropped, up ? Reason::DeviceDown : Reason::LinkDown);
        return;
    }
    context.note(arrival, TraceKind::FrameReceived);
    node(target.id)->receive(context, arrival);
}

std::optional<PingId> Simulation::start_ping(DeviceId source, Ipv4Address destination, Problem& problem) {
    const auto* host = scenario_.device(source);
    problem = !host ? Problem::NoSuchDevice
        : host->kind != DeviceKind::Host ? Problem::SourceNotHost
        : !host->powered ? Problem::SourcePoweredOff
        : !is_unicast(destination) ? Problem::InvalidDestination
        : Problem::None;
    if (ok(problem)) {
        const auto& address = *host->ports.front().ipv4;
        if (address.contains(destination) && !Ipv4Prefix(destination, address.length()).usable_host())
            problem = Problem::BroadcastDestination;
    }
    if (!ok(problem)) return std::nullopt;

    const PingId id{static_cast<std::uint32_t>(pings_.size() + 1)};
    pings_.push_back({.id = id, .source = source, .destination = destination, .started = now()});
    scheduler_.schedule(now(), Timer{source, TimerKind::PingStart, id.value});
    return id;
}

const PingStatus* Simulation::ping(PingId id) const {
    if (id.value == 0 || id.value > pings_.size()) return nullptr;
    return &pings_[id.value - 1];
}

std::vector<ArpCacheEntry> Simulation::arp_cache(DeviceId device) const {
    const auto* item = node(device);
    return item ? item->arp_cache() : std::vector<ArpCacheEntry>{};
}

std::vector<MacTableEntry> Simulation::mac_table(DeviceId device) const {
    const auto* item = node(device);
    return item ? item->mac_table() : std::vector<MacTableEntry>{};
}

bool Context::transmit(std::uint8_t port, PduId pdu, std::uint32_t value) {
    const auto path = simulation_.medium_.path({device_, port});
    if (!path) return false;
    const auto start = now(), arrival = start + path->delay;
    simulation_.scheduler_.schedule(arrival, Delivery{path->link, path->to, pdu});
    record({.kind = TraceKind::FrameSent, .port = port, .link = path->link, .pdu = pdu, .value = value});
    simulation_.trace_.add(Transmission{.pdu = pdu, .link = path->link, .from = device_, .to = path->to.device,
                                        .from_port = port, .to_port = path->to.port, .start = start, .arrival = arrival,
                                        .record = simulation_.trace_.records().back().seq});
    return true;
}

TimerHandle Context::schedule(SimTime delay, TimerKind kind, std::uint64_t key) {
    return simulation_.scheduler_.schedule(now() + delay, Timer{device_, kind, key});
}

void Context::record(TraceRecord record) {
    record.device = device_;
    simulation_.record(record);
}

void Context::note(const Arrival& arrival, TraceKind kind, Reason reason) {
    record({.kind = kind, .reason = reason, .port = arrival.port, .link = arrival.link, .pdu = arrival.pdu});
}

void Context::finish_ping(PingId id, PingState state, Reason reason, const Arrival* arrival) {
    auto& status = ping(id);
    status.state = state;
    status.reason = reason;
    status.finished = now();
    TraceRecord entry{.kind = state == PingState::Succeeded ? TraceKind::PingSucceeded : TraceKind::PingFailed,
                      .reason = reason, .value = id.value};
    if (arrival) {
        entry.port = arrival->port;
        entry.link = arrival->link;
        entry.pdu = arrival->pdu;
    }
    record(entry);
}

} // namespace netsim
