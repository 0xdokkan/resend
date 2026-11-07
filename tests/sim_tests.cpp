#include "check.hpp"
#include "core/rng.hpp"
#include "explain/explain.hpp"
#include "scenario/scenario_json.hpp"
#include "sim_support.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using netsim::DeviceId;
using netsim::DeviceKind;
using netsim::Ipv4Address;
using netsim::LinkId;
using netsim::PingState;
using netsim::Problem;
using netsim::Reason;
using netsim::Scenario;
using netsim::Simulation;
using netsim::TraceKind;
namespace edit = netsim::edit;
using namespace simtest;

void concurrent_pings() {
    // A→B and C→D at the same instant: two conversations through one switch.
    const auto scenario = star(4);
    const DeviceId a{2}, b{3}, c{4}, d{5};
    Simulation simulation(scenario);
    const auto first = ping(simulation, a, address_of(scenario, b));
    const auto second = ping(simulation, c, address_of(scenario, d));
    CHECK(first == netsim::PingId{1} && second == netsim::PingId{2});
    CHECK(simulation.ping(first)->state == PingState::Running);
    drain(simulation);
    for (const auto id : {first, second}) {
        const auto& status = *simulation.ping(id);
        CHECK(status.state == PingState::Succeeded && status.reason == Reason::None);
        CHECK(status.started == 0ms && status.finished == 80ms);
    }
    CHECK(simulation.ping(first)->source == a && simulation.ping(first)->destination == address_of(scenario, b));
    CHECK(count(simulation, TraceKind::PingStarted) == 2 && count(simulation, TraceKind::PingSucceeded) == 2);
    // Each ping carries its own id as the ICMP identifier.
    std::vector<std::uint16_t> identifiers;
    for (const auto& frame : simulation.trace().pdus())
        if (const auto* packet = frame.ipv4(); packet && packet->icmp.type == netsim::icmp::echo_request)
            identifiers.push_back(packet->icmp.identifier);
    CHECK((identifiers == std::vector<std::uint16_t>{1, 2}));
    // The switch learned all four PCs; each PC learned only its peer.
    CHECK(simulation.mac_table(DeviceId{1}).size() == 4);
    CHECK(simulation.arp_cache(a).size() == 1 && simulation.arp_cache(a)[0].ip == address_of(scenario, b));
    CHECK(simulation.arp_cache(d).size() == 1 && simulation.arp_cache(d)[0].ip == address_of(scenario, c));

    netsim::explain::Explainer explainer;
    explainer.update(simulation);
    const auto& events = explainer.events();
    CHECK(std::count_if(events.begin(), events.end(), [](const auto& event) {
              return event.summary.starts_with("Ping succeeded");
          }) == 2);
    explainer.update(simulation);  // nothing new: nothing added
    CHECK(explainer.events().size() == events.size());
}

void one_resolution_for_two_pings() {
    // Two pings from PC-A to PC-B at once: one ARP request, and both Echo
    // Requests leave together when the reply arrives.
    const auto scenario = netsim::sample_lan();
    Simulation simulation(scenario);
    const auto first = ping(simulation, DeviceId{1}, ip("192.168.1.20"));
    const auto second = ping(simulation, DeviceId{1}, ip("192.168.1.20"));
    drain(simulation);
    CHECK(arp_requests(simulation, DeviceId{1}) == 1);
    CHECK(simulation.ping(first)->state == PingState::Succeeded && simulation.ping(second)->state == PingState::Succeeded);
    CHECK(simulation.ping(first)->finished == 80ms && simulation.ping(second)->finished == 80ms);
    // Datagrams are numbered per host as they leave.
    std::vector<std::pair<int, int>> numbers;  // identification, ICMP identifier
    for (const auto& frame : simulation.trace().pdus())
        if (const auto* packet = frame.ipv4()) numbers.emplace_back(packet->identification, packet->icmp.identifier);
    CHECK((numbers == std::vector<std::pair<int, int>>{{1, 1}, {2, 2}, {1, 1}, {2, 2}}));
}

void pings_in_both_directions() {
    // PC-A pings PC-B while PC-B pings PC-A. Each learns the other from the
    // other's request and sends its Echo Request at once, without waiting
    // for the reply to its own request.
    const auto scenario = netsim::sample_lan();
    Simulation simulation(scenario);
    const auto there = ping(simulation, DeviceId{1}, ip("192.168.1.20"));
    const auto back = ping(simulation, DeviceId{2}, ip("192.168.1.10"));
    drain(simulation);
    CHECK(arp_requests(simulation, DeviceId{1}) == 1 && arp_requests(simulation, DeviceId{2}) == 1);
    CHECK(simulation.ping(there)->state == PingState::Succeeded && simulation.ping(there)->finished == 60ms);
    CHECK(simulation.ping(back)->state == PingState::Succeeded && simulation.ping(back)->finished == 60ms);
}

void queued_pings_fail_together() {
    const auto scenario = netsim::sample_lan();
    Simulation simulation(scenario);
    const auto first = ping(simulation, DeviceId{1}, ip("192.168.1.99"));
    simulation.run_until(500ms);
    const auto second = ping(simulation, DeviceId{1}, ip("192.168.1.99"));
    drain(simulation);
    CHECK(simulation.now() == 3s);
    CHECK(arp_requests(simulation, DeviceId{1}) == 3);
    CHECK(count(simulation, TraceKind::ArpTimeout, DeviceId{1}) == 3);
    for (const auto id : {first, second}) {
        CHECK(simulation.ping(id)->state == PingState::Failed && simulation.ping(id)->reason == Reason::ArpUnresolved);
        CHECK(simulation.ping(id)->finished == 3s);
    }
    CHECK(simulation.ping(second)->started == 500ms);
    // The attempts are numbered in the records.
    std::vector<std::uint32_t> attempts;
    for (const auto& record : simulation.trace().records())
        if (record.kind == TraceKind::ArpTimeout) attempts.push_back(record.value);
    CHECK((attempts == std::vector<std::uint32_t>{1, 2, 3}));
    CHECK(simulation.arp_cache(DeviceId{1}).empty());
}

void refused_pings() {
    auto scenario = netsim::sample_lan();
    CHECK(edit::set_powered(scenario, DeviceId{2}, false) == Problem::None);
    Simulation simulation(scenario);
    const auto refused = [&](DeviceId source, const char* destination) {
        Problem problem = Problem::None;
        CHECK(!simulation.start_ping(source, ip(destination), problem));
        return problem;
    };
    CHECK(refused(DeviceId{42}, "192.168.1.20") == Problem::NoSuchDevice);
    CHECK(refused(DeviceId{3}, "192.168.1.20") == Problem::SourceNotHost);
    CHECK(refused(DeviceId{2}, "192.168.1.10") == Problem::SourcePoweredOff);
    for (const auto* address : {"0.0.0.0", "0.1.2.3", "127.0.0.1", "224.0.0.1", "239.1.1.1", "240.0.0.1", "255.255.255.255"})
        CHECK(refused(DeviceId{1}, address) == Problem::InvalidDestination);
    CHECK(refused(DeviceId{1}, "192.168.1.0") == Problem::BroadcastDestination);
    CHECK(refused(DeviceId{1}, "192.168.1.255") == Problem::BroadcastDestination);
    CHECK(!simulation.pending() && simulation.pings().empty() && simulation.trace().records().empty());
    CHECK(!simulation.ping(netsim::PingId{1}) && !simulation.ping(netsim::PingId{0}));

    // Outside the subnet: accepted, and fails at once for lack of a router.
    const auto remote = ping(simulation, DeviceId{1}, ip("10.0.0.1"));
    // Its own address: succeeds at once, without a frame.
    const auto local = ping(simulation, DeviceId{1}, ip("192.168.1.10"));
    drain(simulation);
    CHECK(simulation.now() == 0ms && simulation.trace().pdus().empty());
    CHECK(simulation.ping(remote)->state == PingState::Failed && simulation.ping(remote)->reason == Reason::NoRoute);
    CHECK(simulation.ping(local)->state == PingState::Succeeded);
    // A powered-off destination never answers.
    const auto silent = ping(simulation, DeviceId{1}, ip("192.168.1.20"));
    drain(simulation);
    CHECK(simulation.ping(silent)->reason == Reason::ArpUnresolved);
    CHECK(count(simulation, TraceKind::FrameDropped, DeviceId{2}, Reason::DeviceDown) == 3);
}

void carrier_and_delays() {
    // PC-A —1 ms— Switch —25 ms— PC-B; PC-C's switch port is disabled.
    auto scenario = star(3);
    scenario.links[0].delay = 1ms;
    scenario.links[1].delay = 25ms;
    scenario.device(DeviceId{1})->ports[2].admin_up = false;
    const DeviceId a{2}, b{3}, c{4};
    Simulation simulation(scenario);
    const auto id = ping(simulation, a, address_of(scenario, b));
    drain(simulation);
    CHECK(simulation.ping(id)->finished == 2 * (1ms + 25ms) * 2);
    const auto& first = simulation.trace().transmissions().at(0);
    const auto& second = simulation.trace().transmissions().at(1);
    CHECK(first.from == a && first.start == 0ms && first.arrival == 1ms);
    CHECK(second.from == DeviceId{1} && second.to == b && second.start == 1ms && second.arrival == 26ms);
    // The flood skipped the disabled port: one copy only.
    CHECK(simulation.trace().transmissions().at(2).start == 26ms);
    CHECK(count(simulation, TraceKind::FrameReceived, c) == 0);

    // PC-C has no carrier either: the far end of its cable is disabled.
    const auto lonely = ping(simulation, c, address_of(scenario, a));
    drain(simulation);
    CHECK(simulation.ping(lonely)->reason == Reason::ArpUnresolved);
    CHECK(count(simulation, TraceKind::FrameDropped, c, Reason::NoCarrier) == 3);
    // Nor does a cable that is down, from either end.
    scenario.device(DeviceId{1})->ports[2].admin_up = true;
    scenario.links[0].up = false;
    Simulation unplugged(scenario);
    ping(unplugged, a, address_of(scenario, b));
    ping(unplugged, b, address_of(scenario, a));
    drain(unplugged);
    CHECK(count(unplugged, TraceKind::FrameDropped, a, Reason::NoCarrier) == 3);
    CHECK(count(unplugged, TraceKind::FrameSent, DeviceId{1}) == 3);  // B's requests reach C, never A
    CHECK(count(unplugged, TraceKind::FrameReceived, a) == 0);
}

void steps_and_episodes() {
    const auto scenario = netsim::sample_lan();
    Simulation simulation(scenario);
    CHECK(!simulation.next_event_time() && !simulation.step());
    const auto id = ping(simulation, DeviceId{1}, ip("192.168.1.20"));
    // Nothing happens until the simulation runs.
    CHECK(simulation.trace().records().empty() && simulation.next_event_time() == 0ms);
    CHECK(simulation.step());
    const auto& records = simulation.trace().records();
    CHECK(records.size() == 2);
    CHECK(records[0].kind == TraceKind::PingStarted && records[0].value == id.value && records[0].device == DeviceId{1});
    CHECK(records[1].kind == TraceKind::FrameSent && records[1].value == 1);  // first ARP attempt
    CHECK(records[0].episode == 1 && records[1].episode == 1);
    simulation.run_until(9ms);
    CHECK(simulation.now() == 9ms && records.size() == 2);
    simulation.run_until(10ms);
    CHECK(records.size() == 6);
    for (std::size_t i = 2; i < records.size(); ++i) CHECK(records[i].episode == 2 && records[i].device == DeviceId{3});
    simulation.run_until(5s);
    CHECK(simulation.now() == 5s && !simulation.pending());
    // Episodes never go back, and every record belongs to one.
    for (std::size_t i = 1; i < records.size(); ++i) {
        CHECK(records[i].episode >= records[i - 1].episode && records[i].episode > 0);
        CHECK(records[i].seq == i && records[i].time >= records[i - 1].time);
    }
    // A ping started later starts from the current time.
    const auto later = ping(simulation, DeviceId{2}, ip("192.168.1.10"));
    drain(simulation);
    CHECK(simulation.ping(later)->started == 5s && simulation.ping(later)->finished == 5s + 40ms);
}

void point_to_point_prefixes() {
    // RFC 3021: on a /31 both addresses are hosts.
    Scenario scenario;
    DeviceId a{}, b{}, c{};
    CHECK(edit::add_device(scenario, DeviceKind::Host, 0, 0, &a) == Problem::None);
    CHECK(edit::add_device(scenario, DeviceKind::Host, 0, 0, &b) == Problem::None);
    CHECK(edit::add_device(scenario, DeviceKind::Host, 0, 0, &c) == Problem::None);
    CHECK(edit::configure_host(scenario, a, "A", "10.0.0.0", 31) == Problem::None);
    CHECK(edit::configure_host(scenario, b, "B", "10.0.0.1", 31) == Problem::None);
    CHECK(edit::configure_host(scenario, c, "C", "10.0.0.2", 32) == Problem::None);
    CHECK(edit::connect(scenario, a, b) == Problem::None);
    Simulation simulation(scenario);
    const auto across = ping(simulation, a, ip("10.0.0.1"));
    const auto back = ping(simulation, b, ip("10.0.0.0"));
    // A /32 has no neighbours: everything else is remote.
    const auto alone = ping(simulation, c, ip("10.0.0.3"));
    drain(simulation);
    CHECK(simulation.ping(across)->state == PingState::Succeeded && simulation.ping(back)->state == PingState::Succeeded);
    CHECK(simulation.ping(alone)->reason == Reason::NoRoute);
}

void moved_simulation() {
    // A simulation can be moved mid-run, as the interface does when it
    // rebuilds one; the moved one carries on exactly.
    const auto scenario = netsim::sample_lan();
    Simulation reference(scenario);
    ping(reference, DeviceId{1}, ip("192.168.1.20"));
    drain(reference);

    Simulation original(scenario);
    ping(original, DeviceId{1}, ip("192.168.1.20"));
    original.run_until(25ms);
    Simulation moved(std::move(original));
    Simulation assigned(netsim::Scenario{});
    assigned = std::move(moved);
    drain(assigned);
    CHECK(assigned.trace().records() == reference.trace().records());
    CHECK(assigned.trace().pdus() == reference.trace().pdus());
    CHECK(assigned.pings() == reference.pings());
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void example_lessons() {
    // What each example file sets out to show.
    struct Lesson {
        const char* file;
        const char* source;
        const char* destination;
        PingState state;
        Reason reason;
    };
    const Lesson lessons[] = {
        {"sample-lan.json", "PC-A", "192.168.1.20", PingState::Succeeded, Reason::None},
        {"two-switches.json", "PC-A", "192.168.1.20", PingState::Succeeded, Reason::None},
        {"two-switches.json", "PC-C", "192.168.1.10", PingState::Succeeded, Reason::None},
        {"mismatched-masks.json", "PC-A", "192.168.2.20", PingState::Failed, Reason::EchoTimeout},
        {"unplugged.json", "PC-A", "192.168.1.20", PingState::Failed, Reason::ArpUnresolved},
        {"office-floor.json", "Desk-1", "10.0.0.22", PingState::Succeeded, Reason::None},
        {"office-floor.json", "Desk-1", "10.0.0.23", PingState::Failed, Reason::ArpUnresolved},
        {"switch-loop.json", "PC-A", "192.168.1.20", PingState::Failed, Reason::ArpUnresolved},
    };
    for (const auto& lesson : lessons) {
        std::string error;
        const auto scenario = netsim::scenario_from_json(read_file(std::filesystem::path(RESEND_EXAMPLES_DIR) / lesson.file), error);
        CHECK(scenario.has_value());
        Simulation simulation(*scenario);
        const auto id = ping(simulation, scenario->device_named(lesson.source)->id, ip(lesson.destination));
        drain(simulation);
        CHECK(simulation.ping(id)->state == lesson.state && simulation.ping(id)->reason == lesson.reason);
    }
    // In "Two switches" the reply to PC-A is forwarded, never flooded.
    std::string error;
    const auto scenario = netsim::scenario_from_json(read_file(std::filesystem::path(RESEND_EXAMPLES_DIR) / "two-switches.json"), error);
    Simulation simulation(*scenario);
    ping(simulation, DeviceId{1}, ip("192.168.1.20"));
    drain(simulation);
    CHECK(count(simulation, TraceKind::FrameFlooded) == 2);  // the request, at each switch
    CHECK(count(simulation, TraceKind::FrameReceived, DeviceId{3}) == 1);

    // In "Switch loop" the guard stops each ARP request; with one cable of
    // the ring disabled, as the notes suggest, the ping succeeds.
    const auto ring = netsim::scenario_from_json(read_file(std::filesystem::path(RESEND_EXAMPLES_DIR) / "switch-loop.json"), error);
    Simulation looped(*ring);
    ping(looped, DeviceId{1}, ip("192.168.1.20"));
    drain(looped);
    CHECK(count(looped, TraceKind::SimulatorGuard) == 3);
    for (const auto& cable : ring->links) {
        if (cable.a.device.value <= 2 || cable.b.device.value <= 2) continue;  // a PC's cable
        Simulation opened(*ring);
        CHECK(opened.apply(netsim::SetLinkUp{cable.id, false}) == Problem::None);
        const auto id = ping(opened, DeviceId{1}, ip("192.168.1.20"));
        drain(opened);
        CHECK(opened.ping(id)->state == PingState::Succeeded && count(opened, TraceKind::SimulatorGuard) == 0);
    }
}

struct Tally {
    int records = 0, succeeded = 0, refused = 0;
    int failed[16] = {};
};

// A random loop-free network with a few pings, all drawn from `seed`.
void random_run(std::uint64_t seed, Hash& hash, Tally& tally) {
    netsim::Rng rng(seed);
    Scenario scenario;
    std::vector<DeviceId> hosts;
    const auto devices = rng.between(2, 14);
    for (std::int64_t i = 0; i < devices; ++i) {
        const auto kind = rng.chance(1, 3) ? DeviceKind::Switch : DeviceKind::Host;
        DeviceId id{};
        // One draw per statement: compilers evaluate function arguments in different orders.
        const auto x = static_cast<int>(rng.between(0, 800));
        const auto y = static_cast<int>(rng.between(0, 600));
        CHECK(edit::add_device(scenario, kind, x, y, &id) == Problem::None);
        if (kind == DeviceKind::Host) {
            hosts.push_back(id);
            // Now and then another mask, another subnet or a duplicate address;
            // refused combinations keep the default.
            if (rng.chance(1, 4)) {
                const auto third = rng.chance(1, 3) ? 2U : 1U;
                const auto fourth = static_cast<std::uint32_t>(rng.between(1, 30));
                const Ipv4Address address(0xc0a80000U | third << 8 | fourth);
                edit::configure_host(scenario, id, scenario.device(id)->name, address.to_string(), static_cast<int>(rng.between(16, 30)));
            }
        }
        if (i > 0 && rng.chance(4, 5)) edit::connect(scenario, id, DeviceId{static_cast<std::uint32_t>(rng.between(1, i))});
    }
    for (auto& cable : scenario.links) {
        cable.delay = delays[rng.below(std::size(delays))];
        cable.up = !rng.chance(1, 12);
    }
    for (auto& device : scenario.devices) {
        device.powered = !rng.chance(1, 15);
        for (auto& port : device.ports) port.admin_up = !rng.chance(1, 30);
    }
    CHECK(netsim::validate(scenario).empty());

    Simulation simulation(scenario);
    const auto pings = hosts.empty() ? 0 : rng.between(1, 4);
    for (std::int64_t i = 0; i < pings; ++i) {
        simulation.run_until(simulation.now() + std::chrono::milliseconds(rng.between(0, 50)));
        const auto source = hosts[rng.below(hosts.size())];
        const auto target = hosts[rng.below(hosts.size())];
        const auto destination = rng.chance(1, 6) ? Ipv4Address(0xc0a80100U | static_cast<std::uint32_t>(rng.between(0, 255)))
                                                  : address_of(scenario, target);
        Problem problem = Problem::None;
        if (!simulation.start_ping(source, destination, problem)) ++tally.refused;
        hash.add(static_cast<std::uint64_t>(problem));
    }
    drain(simulation);
    add_trace(hash, simulation);
    tally.records += static_cast<int>(simulation.trace().records().size());
    for (const auto& status : simulation.pings()) {
        CHECK(status.state != PingState::Running);
        if (status.state == PingState::Succeeded) ++tally.succeeded;
        else ++tally.failed[static_cast<int>(status.reason)];
    }
}

void random_topologies() {
    // The trace of 1,000 random networks, hashed. The constant is the same
    // on every platform and compiler; it changes only when the model does,
    // and then on purpose.
    constexpr std::uint64_t expected = 0x5691783f588de4ea;
    Hash hash;
    Tally tally;
    for (std::uint64_t seed = 1; seed <= 1000; ++seed) random_run(seed, hash, tally);
    std::cout << "  1000 random networks: " << tally.records << " records, " << tally.succeeded << " pings succeeded, "
              << tally.failed[static_cast<int>(Reason::ArpUnresolved)] << " unresolved, "
              << tally.failed[static_cast<int>(Reason::EchoTimeout)] << " timed out, "
              << tally.failed[static_cast<int>(Reason::NoRoute)] << " without route, " << tally.refused << " refused; hash 0x"
              << std::hex << hash.value << std::dec << '\n';
    // The generator covers every outcome.
    CHECK(tally.succeeded > 100 && tally.refused > 0);
    CHECK(tally.failed[static_cast<int>(Reason::ArpUnresolved)] > 0);
    CHECK(tally.failed[static_cast<int>(Reason::EchoTimeout)] > 0);
    CHECK(tally.failed[static_cast<int>(Reason::NoRoute)] > 0);
    CHECK(hash.value == expected);

    // And the same seed twice gives the same run.
    Hash once, twice;
    Tally ignored;
    random_run(77, once, ignored);
    random_run(77, twice, ignored);
    CHECK(once.value == twice.value);
}
} // namespace

int main() {
    concurrent_pings();
    one_resolution_for_two_pings();
    pings_in_both_directions();
    queued_pings_fail_together();
    refused_pings();
    carrier_and_delays();
    steps_and_episodes();
    point_to_point_prefixes();
    moved_simulation();
    example_lessons();
    random_topologies();
    std::cout << "Simulation tests passed: " << test::checks << " checks.\n";
}
