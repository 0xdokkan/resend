#include "sim/simulation.hpp"

#include "sim/node.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

namespace netsim {
namespace {
template<class... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};

std::unique_ptr<Node> make_node(DeviceKind kind) {
    switch (kind) {
    case DeviceKind::Host: return make_host();
    case DeviceKind::Switch: return make_switch();
    case DeviceKind::Router: return make_router();
    }
    return make_host();
}
} // namespace

int PingStatus::replies() const {
    return static_cast<int>(std::count_if(echoes.begin(), echoes.end(), [](const auto& echo) { return echo.state == EchoState::Replied; }));
}

Simulation::Simulation(Scenario scenario) : scenario_(std::move(scenario)) {
    assert(validate(scenario_).empty());
    for (const auto& device : scenario_.devices) nodes_.emplace(device.id, make_node(device.kind));
    last_device_ = scenario_.next_device_id().value - 1;
    last_link_ = scenario_.next_link_id().value - 1;
}
Simulation::~Simulation() = default;
Simulation::Simulation(Simulation&&) noexcept = default;
Simulation& Simulation::operator=(Simulation&&) noexcept = default;

Node* Simulation::node(DeviceId device) const {
    const auto it = nodes_.find(device);
    return it == nodes_.end() ? nullptr : it->second.get();
}

std::uint64_t Simulation::record(TraceRecord record) {
    record.episode = episode_;
    trace_.add(now(), record);
    return trace_.records().back().seq;
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
    --in_flight_;
    Context context{*this, delivery.to.device};
    const Arrival arrival{delivery.to.port, delivery.link, delivery.pdu};
    // An intact cable still joins the same ports, so the device exists.
    if (!medium_.intact(delivery.link, delivery.serial)) {
        context.note(arrival, TraceKind::FrameDropped, Reason::LinkDown);
        return;
    }
    if (!scenario_.device(delivery.to.device)->powered) {
        context.note(arrival, TraceKind::FrameDropped, Reason::DeviceDown);
        return;
    }
    context.note(arrival, TraceKind::FrameReceived);
    node(delivery.to.device)->receive(context, arrival);
}

Edit Simulation::resolve(Edit edit) const {
    // New devices and cables get ids never used before.
    if (auto* item = std::get_if<AddDevice>(&edit); item && !item->id) item->id = DeviceId{last_device_ + 1};
    if (auto* item = std::get_if<Connect>(&edit); item && !item->id) item->id = LinkId{last_link_ + 1};
    return edit;
}

Problem Simulation::apply(const Edit& requested) {
    const Edit edit = resolve(requested);
    Scenario next = scenario_;
    if (const auto problem = edit::apply(next, edit); !ok(problem)) return problem;
    last_device_ = std::max(last_device_, next.next_device_id().value - 1);
    last_link_ = std::max(last_link_, next.next_link_id().value - 1);
    if (!affects_network(edit)) {
        scenario_ = std::move(next);
        return Problem::None;
    }

    ++episode_;
    EditEntry entry{.time = now(), .edit = edit, .device = std::nullopt, .links = {}};
    TraceRecord note{.kind = TraceKind::EditApplied, .value = static_cast<std::uint32_t>(edits_.size())};
    const auto device = [&](DeviceId id) {
        note.device = id;
        if (const auto* item = scenario_.device(id)) entry.device = *item;
    };
    const auto cable = [&](const LinkConfig* item) {
        if (!item) return;
        note.link = item->id;
        entry.links.push_back(*item);
    };
    std::visit(Overloaded{
                   [&](const SetLinkUp& item) { cable(scenario_.link(item.link)); },
                   [&](const SetDelay& item) { cable(scenario_.link(item.link)); },
                   [&](const SetPortUp& item) {
                       device(item.port.device);
                       note.port = item.port.port;
                       cable(scenario_.link_at(item.port));
                   },
                   [&](const SetPowered& item) { device(item.device); },
                   [&](const SetAddress& item) {
                       device(item.port.device);
                       note.port = item.port.port;
                   },
                   [&](const SetAgeing& item) { device(item.device); },
                   [&](const Rename&) {},
                   [&](const MoveDevice&) {},
                   [&](const Connect&) { note.link = next.links.back().id; },
                   [&](const Disconnect& item) { cable(scenario_.link(item.link)); },
                   [&](const Replug& item) { cable(scenario_.link(item.link)); },
                   [&](const AddDevice&) { note.device = next.devices.back().id; },
                   [&](const RemoveDevice& item) {
                       device(item.device);
                       for (const auto& link : scenario_.links)
                           if (link.a.device == item.device || link.b.device == item.device) entry.links.push_back(link);
                   },
                   [&](const RestoreLink& item) {
                       note.link = item.link.id;
                       entry.links.push_back(item.link);
                   },
                   [&](const RestoreDevice& item) {
                       note.device = item.device.id;
                       entry.device = item.device;
                       for (const auto& link : item.links) entry.links.push_back(link.link);
                   },
                   [&](const SetGateway& item) { device(item.device); },
                   [&](const SetRoutes& item) { device(item.device); },
                   [&](const SetPorts& item) { device(item.device); },
               },
               edit);
    edits_.push_back(std::move(entry));
    record(note);

    // Devices that stop now, removed or powered off, forget everything.
    std::vector<DeviceId> stopping;
    for (const auto& item : scenario_.devices) {
        const auto* after = next.device(item.id);
        if (item.powered && (!after || !after->powered)) stopping.push_back(item.id);
    }
    for (const auto id : stopping) shut_down(id);
    scenario_ = std::move(next);
    std::erase_if(nodes_, [&](const auto& item) { return !scenario_.device(item.first); });
    for (const auto& item : scenario_.devices) {
        auto& slot = nodes_[item.id];
        if (!slot || std::find(stopping.begin(), stopping.end(), item.id) != stopping.end()) slot = make_node(item.kind);
    }
    medium_.update(scenario_);
    return Problem::None;
}

void Simulation::shut_down(DeviceId device) {
    scheduler_.remove_if([&](const Scheduled& entry) {
        const auto* timer = std::get_if<Timer>(&entry);
        return timer && timer->device == device;
    });
    Context context{*this, device};
    for (std::size_t index = 0; index < pings_.size(); ++index)
        if (pings_[index].source == device && pings_[index].state == PingState::Running)
            context.abort_ping(pings_[index].id, Reason::DeviceDown);
}

const DeviceConfig* Simulation::known_device(DeviceId id) const {
    if (const auto* device = scenario_.device(id)) return device;
    for (auto it = edits_.rbegin(); it != edits_.rend(); ++it)
        if (it->device && it->device->id == id) return &*it->device;
    return nullptr;
}

std::uint32_t Simulation::storm_limit() const {
    return std::max<std::uint32_t>(64, 4 * static_cast<std::uint32_t>(scenario_.links.size()));
}

void Simulation::guard(DeviceId device, PduId pdu) {
    stopped_.insert(pdu);
    record({.kind = TraceKind::SimulatorGuard, .device = device, .pdu = pdu, .value = copies_.at(pdu.value)});
    const auto removed = scheduler_.remove_if([&](const Scheduled& entry) {
        const auto* delivery = std::get_if<Delivery>(&entry);
        return delivery && delivery->pdu == pdu;
    });
    for (const auto& entry : removed) {
        trace_.stop(std::get<Delivery>(entry).transmission, now());
        --in_flight_;
    }
}

std::optional<PingId> Simulation::start_ping(DeviceId source, Ipv4Address destination, Problem& problem, const PingOptions& options) {
    const auto* host = scenario_.device(source);
    const auto in_range = [](SimTime time) { return time >= min_ping_time && time <= max_ping_time; };
    problem = !host ? Problem::NoSuchDevice
        : host->kind != DeviceKind::Host ? Problem::SourceNotHost
        : !host->powered ? Problem::SourcePoweredOff
        : !is_unicast(destination) ? Problem::InvalidDestination
        : Problem::None;
    if (ok(problem)) {
        // A host without an address may start a ping: every request is lost
        // for that reason (Reason::NoAddress).
        const auto address = host->ports.front().ipv4.value_or(Ipv4Prefix{});
        if (host->ports.front().ipv4 && address.contains(destination) && !Ipv4Prefix(destination, address.length()).usable_host())
            problem = Problem::BroadcastDestination;
        else if (options.count < 1 || options.count > (options.traceroute ? max_hops : max_ping_count) || options.ttl < 1 ||
                 !in_range(options.interval) || !in_range(options.timeout))
            problem = Problem::InvalidPingOptions;
    }
    if (!ok(problem)) return std::nullopt;

    const PingId id{static_cast<std::uint32_t>(pings_.size() + 1)};
    pings_.push_back({.id = id, .source = source, .destination = destination, .options = options, .started = now()});
    scheduler_.schedule(now(), Timer{source, TimerKind::PingSend, id.value});
    return id;
}

const PingStatus* Simulation::ping(PingId id) const {
    if (id.value == 0 || id.value > pings_.size()) return nullptr;
    return &pings_[id.value - 1];
}

std::vector<ArpCacheEntry> Simulation::arp_cache(DeviceId device) const {
    const auto* item = node(device);
    return item ? item->arp_cache(now()) : std::vector<ArpCacheEntry>{};
}

std::vector<MacTableEntry> Simulation::mac_table(DeviceId device) const {
    const auto* item = node(device);
    const auto* config = scenario_.device(device);
    return item && config ? item->mac_table(now(), *config) : std::vector<MacTableEntry>{};
}

Transmit Context::transmit(std::uint8_t port, PduId pdu, std::uint32_t value) {
    auto& simulation = simulation_;
    if (simulation.stopped_.contains(pdu)) return Transmit::Stopped;
    const auto path = simulation.medium_.path({device_, port});
    if (!path) return Transmit::NoCarrier;
    if (simulation.copies_.size() <= pdu.value) simulation.copies_.resize(pdu.value + 1, 0);
    auto& copies = simulation.copies_[pdu.value];
    if (copies >= simulation.storm_limit()) {
        simulation.guard(device_, pdu);
        return Transmit::Stopped;
    }
    ++copies;
    const auto start = now(), arrival = start + path->delay;
    const auto transmission = simulation.trace_.transmissions().size();
    simulation.scheduler_.schedule(arrival, Delivery{path->link, path->to, pdu, path->serial, transmission});
    ++simulation.in_flight_;
    const auto seq = record({.kind = TraceKind::FrameSent, .port = port, .link = path->link, .pdu = pdu, .value = value});
    simulation.trace_.add(Transmission{.pdu = pdu, .link = path->link, .from = device_, .to = path->to.device,
                                       .from_port = port, .to_port = path->to.port, .start = start, .arrival = arrival,
                                       .record = seq});
    return Transmit::Sent;
}

TimerHandle Context::schedule(SimTime delay, TimerKind kind, std::uint64_t key) {
    return simulation_.scheduler_.schedule(now() + delay, Timer{device_, kind, key});
}

std::uint64_t Context::record(TraceRecord record) {
    record.device = device_;
    return simulation_.record(record);
}

void Context::note(const Arrival& arrival, TraceKind kind, Reason reason) {
    record({.kind = kind, .reason = reason, .port = arrival.port, .link = arrival.link, .pdu = arrival.pdu});
}

EchoStatus& Context::next_echo(PingId id) {
    auto& status = ping(id);
    if (status.echoes.empty()) record({.kind = TraceKind::PingStarted, .value = id.value});
    status.echoes.push_back({.sequence = static_cast<std::uint16_t>(status.echoes.size() + 1), .sent = now()});
    return status.echoes.back();
}

void Context::echo_replied(PingId id, std::uint16_t sequence, const Arrival* arrival) {
    TraceRecord entry{.kind = TraceKind::PingReply, .value = id.value};
    if (arrival) {
        entry.port = arrival->port;
        entry.link = arrival->link;
        entry.pdu = arrival->pdu;
    }
    auto& echo = ping(id).echoes.at(sequence - 1U);
    echo.state = EchoState::Replied;
    echo.finished = now();
    echo.record = record(entry);
    echo.responder = ping(id).destination;
    settle(id, std::nullopt);
}

EchoStatus& Context::lose(PingId id, std::uint16_t sequence, Reason reason, std::optional<PduId> request) {
    auto& echo = ping(id).echoes.at(sequence - 1U);
    echo.state = EchoState::Lost;
    echo.reason = reason;
    echo.finished = now();
    echo.record = record({.kind = TraceKind::PingLost, .reason = reason, .pdu = request, .value = id.value});
    return echo;
}

void Context::echo_lost(PingId id, std::uint16_t sequence, Reason reason, std::optional<PduId> request) {
    lose(id, sequence, reason, request);
    settle(id, std::nullopt);
}

void Context::echo_error(PingId id, std::uint16_t sequence, Reason reason, const Arrival& arrival, Ipv4Address from) {
    auto& echo = ping(id).echoes.at(sequence - 1U);
    echo.state = EchoState::Lost;
    echo.reason = reason;
    echo.finished = now();
    echo.responder = from;
    echo.record = record({.kind = TraceKind::PingLost, .reason = reason, .port = arrival.port, .link = arrival.link,
                          .pdu = arrival.pdu, .value = id.value});
    settle(id, std::nullopt);
}

void Context::abort_ping(PingId id, Reason reason) {
    const auto& echoes = ping(id).echoes;
    for (std::size_t index = 0; index < echoes.size(); ++index)
        if (echoes[index].state == EchoState::Waiting) lose(id, echoes[index].sequence, reason, std::nullopt);
    settle(id, reason);
}

void Context::settle(PingId id, std::optional<Reason> abort) {
    auto& status = ping(id);
    if (status.state != PingState::Running) return;
    const bool waiting = std::any_of(status.echoes.begin(), status.echoes.end(),
                                     [](const auto& echo) { return echo.state == EchoState::Waiting; });
    if (!abort) {
        if (waiting) return;
        if (status.options.traceroute) {
            // A traceroute goes on past the routers that answer and past
            // the hops that do not answer at all.
            const auto& last = status.echoes.back();
            const bool onward = last.state == EchoState::Lost && (last.reason == Reason::TtlExceeded || last.reason == Reason::EchoTimeout);
            if (onward && status.echoes.size() < status.options.count) {
                schedule(SimTime{}, TimerKind::PingSend, id.value);
                return;
            }
        } else if (status.echoes.size() < status.options.count) {
            return;
        }
    }
    const bool replied = status.replies() > 0;
    // A ping fails for its first loss, a traceroute for its last: where it stopped.
    const auto lost = [](const EchoStatus& echo) { return echo.state == EchoState::Lost; };
    const EchoStatus* loss = nullptr;
    if (status.options.traceroute) {
        const auto it = std::find_if(status.echoes.rbegin(), status.echoes.rend(), lost);
        if (it != status.echoes.rend()) loss = &*it;
    } else {
        const auto it = std::find_if(status.echoes.begin(), status.echoes.end(), lost);
        if (it != status.echoes.end()) loss = &*it;
    }
    status.state = replied ? PingState::Succeeded : PingState::Failed;
    status.reason = replied ? Reason::None : loss ? loss->reason : abort.value_or(Reason::None);
    status.finished = now();
    record({.kind = replied ? TraceKind::PingSucceeded : TraceKind::PingFailed, .reason = status.reason, .value = id.value});
}

} // namespace netsim
