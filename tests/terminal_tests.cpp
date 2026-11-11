// The device terminal (M8): the queries print the network at the instant
// shown, the changes are the inspector's edits, the applications start
// pings and print as ping does, and a command outside the subset is
// refused. Every lab's solution can be typed, and 50 random commands are
// undone by 50 undos.
#include "app/workspace.hpp"
#include "check.hpp"
#include "lab/lab.hpp"
#include "scenario/scenario_json.hpp"
#include "term/terminal.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

// CHECK of an expression with commas of its own, as braced lists have.
#define CHECK_ALL(...) CHECK((__VA_ARGS__))

namespace {
namespace term = netsim::term;
using netsim::DeviceId;
using netsim::Ipv4Address;
using netsim::Ipv4Prefix;
using netsim::PortRef;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
netsim::Scenario example(const std::string& name) {
    std::string error;
    auto scenario = netsim::scenario_from_json(read_file(std::filesystem::path(RESEND_EXAMPLES_DIR) / (name + ".json")), error);
    if (!scenario) std::cerr << name << ": " << error << '\n';
    CHECK(scenario.has_value());
    return *scenario;
}
bool has(const std::string& text, const std::string& part) {
    if (text.find(part) != std::string::npos) return true;
    std::cerr << "  no \"" << part << "\" in:\n" << text;
    return false;
}
Ipv4Address address(const char* text) { return *Ipv4Address::parse(text); }
DeviceId named(const netsim::Scenario& network, const char* name) { return network.device_named(name)->id; }

// A query, which must succeed and change nothing.
std::string query(const char* line, DeviceId device, const netsim::Scenario& network, const netsim::Simulation* simulation = nullptr) {
    const auto result = term::run(line, device, network, simulation);
    if (result.error) std::cerr << "  " << line << ": " << result.output;
    CHECK(!result.error && result.edits.empty() && !result.start);
    return result.output;
}
std::string refused(const char* line, DeviceId device, const netsim::Scenario& network) {
    const auto result = term::run(line, device, network, nullptr);
    if (!result.error) std::cerr << "  " << line << " was not refused\n";
    CHECK(result.error && result.edits.empty() && !result.start);
    return result.output;
}
std::vector<netsim::Edit> edits(const char* line, DeviceId device, const netsim::Scenario& network) {
    const auto result = term::run(line, device, network, nullptr);
    if (result.error) std::cerr << "  " << line << ": " << result.output;
    CHECK(!result.error && !result.edits.empty() && !result.label.empty());
    return result.edits;
}

void queries() {
    auto network = netsim::sample_lan();
    const DeviceId a{1}, sw{3};
    const auto addresses = query("ip addr", a, network);
    CHECK(has(addresses, "1: p1: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 state UP\n"));
    CHECK(has(addresses, "    link/ether " + network.device(a)->ports[0].mac.to_string() + " brd ff:ff:ff:ff:ff:ff\n"));
    CHECK(has(addresses, "    inet 192.168.1.10/24 brd 192.168.1.255 scope global p1\n"));
    // Short forms and sudo, as Linux takes them.
    CHECK(query("sudo ip address show dev p1", a, network) == addresses);
    CHECK(query("ip a s p1", a, network) == addresses);
    CHECK(query("ip -br a", a, network) == "p1               UP             192.168.1.10/24\n");
    CHECK(query("ip r", a, network) == "192.168.1.0/24 dev p1 proto kernel scope link src 192.168.1.10\n");
    CHECK(query("ip neigh", a, network).empty());
    CHECK(query("ip route get 192.168.1.20", a, network) == "192.168.1.20 dev p1 src 192.168.1.10\n");
    CHECK(has(refused("ip route get 10.9.9.9", a, network), "Network is unreachable"));
    // The switch: its ports and its table, no addresses.
    CHECK(has(query("ip link", sw, network), "3: p3: <NO-CARRIER,BROADCAST,MULTICAST,UP> mtu 1500 master br0 state DOWN\n"));
    CHECK(query("bridge fdb", sw, network).empty());
    CHECK(has(query("bridge link", sw, network), "1: p1: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 master br0 state forwarding\n"));
    CHECK(has(refused("bridge fdb", a, network), "not a switch"));
    CHECK(has(refused("ip addr add 10.0.0.1/24 dev p1", sw, network), "switch"));

    // After a ping, the tables are the simulation's at that instant.
    netsim::Simulation simulation(network);
    netsim::Problem problem{};
    CHECK(simulation.start_ping(a, address("192.168.1.20"), problem).has_value());
    while (simulation.step()) {}
    const auto neighbours = query("ip n", a, simulation.scenario(), &simulation);
    CHECK(neighbours == "192.168.1.20 dev p1 lladdr " + network.device(DeviceId{2})->ports[0].mac.to_string() + " REACHABLE\n");
    const auto fdb = query("bridge fdb show", sw, simulation.scenario(), &simulation);
    CHECK(has(fdb, network.device(a)->ports[0].mac.to_string() + " dev p1 master br0\n"));
    CHECK(has(fdb, network.device(DeviceId{2})->ports[0].mac.to_string() + " dev p2 master br0\n"));

    // A cable down: no carrier, and the connected route says so.
    network.links[0].up = false;
    CHECK(has(query("ip link", a, network), "<NO-CARRIER,BROADCAST,MULTICAST,UP> mtu 1500 state DOWN"));
    CHECK(has(query("ip route", a, network), "src 192.168.1.10 linkdown\n"));

    // Routers: connected networks, static routes, the route of one address.
    const auto routed = example("two-lans-router");
    const auto router = named(routed, "Router"), pc = named(routed, "PC-A");
    CHECK(query("ip route", router, routed) ==
          "10.1.0.0/24 dev p1 proto kernel scope link src 10.1.0.1\n10.2.0.0/24 dev p2 proto kernel scope link src 10.2.0.1\n");
    CHECK(query("ip route", pc, routed) == "default via 10.1.0.1 dev p1\n10.1.0.0/24 dev p1 proto kernel scope link src 10.1.0.10\n");
    CHECK(query("ip route get 10.2.0.20", pc, routed) == "10.2.0.20 via 10.1.0.1 dev p1 src 10.1.0.10\n");
    CHECK(query("ip route get 10.2.0.20", router, routed) == "10.2.0.20 dev p2 src 10.2.0.1\n");
    // A gateway resend keeps and does not use is said, not hidden.
    const auto wrong = example("wrong-gateway");
    CHECK(has(query("ip route", named(wrong, "PC-C"), wrong), "# resend keeps the default gateway 10.2.0.1, but does not use it"));
    const auto loop = example("routing-loop");
    CHECK(has(query("ip route", named(loop, "R1"), loop), "default via 10.12.0.2 dev p2\n"));
    // Off, a device has no terminal.
    auto off = netsim::sample_lan();
    off.device(a)->powered = false;
    CHECK(has(refused("ip addr", a, off), "PC-A is off"));
    CHECK(!term::run("help", a, off, nullptr).error);
}

void changes() {
    const auto network = netsim::sample_lan();
    const DeviceId a{1};
    using netsim::Edit;
    CHECK_ALL(edits("ip addr del 192.168.1.10/24 dev p1", a, network) == std::vector<Edit>{netsim::SetAddress{PortRef{a, 0}, std::nullopt}});
    CHECK(has(refused("ip addr add 10.0.0.5/24 dev p1", a, network), "already has 192.168.1.10/24"));
    CHECK(has(refused("ip addr add 192.168.1.10/24 dev p1", a, network), "File exists"));
    CHECK(has(refused("ip addr del 192.168.1.11/24 dev p1", a, network), "Cannot assign requested address"));
    CHECK(has(refused("ip addr add 192.168.1.10 dev p1", a, network), "prefix length"));
    CHECK(has(refused("ip addr add 192.168.1.10/24", a, network), "port is missing"));
    CHECK(has(refused("ip addr add 192.168.1.10/24 dev p2", a, network), "Cannot find device \"p2\""));
    CHECK(has(refused("ip addr add 192.168.1.10/24 dev p1 brd +", a, network), "\"brd\" is not in resend's"));
    CHECK_ALL(edits("ip link set p1 down", a, network) == std::vector<Edit>{netsim::SetPortUp{PortRef{a, 0}, false}});
    CHECK_ALL(edits("ip link set dev p1 up", a, network) == std::vector<Edit>{netsim::SetPortUp{PortRef{a, 0}, true}});
    CHECK(has(refused("ip link set p1 sideways", a, network), "up or down"));
    CHECK_ALL(edits("ip route add default via 192.168.1.1", a, network) ==
          std::vector<Edit>{netsim::SetGateway{a, address("192.168.1.1")}});
    CHECK(has(refused("ip route add default via 10.9.9.9", a, network), "Nexthop has invalid gateway"));
    CHECK(has(refused("ip route add 10.0.0.0/8 via 192.168.1.1", a, network), "one route out"));
    CHECK(has(refused("ip route del default", a, network), "No such process"));

    const auto routed = example("two-lans-router");
    const auto pc = named(routed, "PC-A"), router = named(routed, "Router");
    CHECK(has(refused("ip route add default via 10.1.0.2", pc, routed), "File exists"));
    CHECK_ALL(edits("ip route replace default via 10.1.0.2", pc, routed) == std::vector<Edit>{netsim::SetGateway{pc, address("10.1.0.2")}});
    CHECK_ALL(edits("ip route del default", pc, routed) == std::vector<Edit>{netsim::SetGateway{pc, std::nullopt}});
    const std::vector<netsim::Route> added = {{*Ipv4Prefix::parse("10.9.0.0/16"), address("10.2.0.20")}};
    CHECK_ALL(edits("ip route add 10.9.0.0/16 via 10.2.0.20", router, routed) == std::vector<Edit>{netsim::SetRoutes{router, added}});
    CHECK_ALL(edits("ip r a 10.9.0.0/16 via 10.2.0.20 dev p2", router, routed) == std::vector<Edit>{netsim::SetRoutes{router, added}});
    CHECK(has(refused("ip route add 10.9.0.0/16 via 10.2.0.20 dev p1", router, routed), "Nexthop has invalid gateway"));
    CHECK(has(refused("ip route add 10.9.0.1/16 via 10.2.0.20", router, routed), "Invalid prefix for given prefix length"));
    CHECK(has(refused("ip route add 10.1.0.0/24 via 10.2.0.20", router, routed), "File exists"));
    CHECK(has(refused("ip route add 10.9.0.0/16", router, routed), "through a next hop"));
    CHECK(has(refused("ip route del 10.1.0.0/24", router, routed), "comes with a port's address"));
    auto with_route = routed;
    with_route.device(router)->routes = added;
    CHECK_ALL(edits("ip route del 10.9.0.0/16", router, with_route) == std::vector<Edit>{netsim::SetRoutes{router, {}}});
    CHECK(has(refused("ip route add 10.9.0.0/16 via 10.2.0.30", router, with_route), "File exists"));
    const std::vector<netsim::Route> replaced = {{*Ipv4Prefix::parse("10.9.0.0/16"), address("10.2.0.30")}};
    CHECK_ALL(edits("ip route replace 10.9.0.0/16 via 10.2.0.30", router, with_route) == std::vector<Edit>{netsim::SetRoutes{router, replaced}});
    // A disabled port: Linux takes no route through it.
    auto down = routed;
    down.device(router)->ports[1].admin_up = false;
    CHECK(has(refused("ip route add 10.9.0.0/16 via 10.2.0.20", router, down), "Nexthop has invalid gateway"));
    CHECK(has(refused("ip route add 10.9.0.0/16 via 10.2.0.20 dev p2", router, down), "Nexthop device is not up"));

    // Outside the subset: refused, saying what is there.
    CHECK(has(refused("ifconfig", a, network), "Linux replaced it with ip"));
    CHECK(has(refused("nmap 192.168.1.20", a, network), "command not found"));
    CHECK(has(refused("ip -s a", a, network), "Option \"-s\" is not in resend's ip"));
    CHECK(has(refused("ip addr flush dev p1", a, network), "Command \"flush\" is unknown"));
    CHECK(has(refused("ip tunnel", a, network), "Object \"tunnel\" is unknown"));
    CHECK(has(refused("ip neigh flush all", a, network), "only shows the ARP cache"));
    CHECK(has(refused("ip link set p0 up", a, network), "does not exist"));
    CHECK(has(refused("ip link set p01 up", a, network), "does not exist"));
    CHECK(term::run("clear", a, network, nullptr).clear);
    CHECK(has(term::run("help ip", a, network, nullptr).output, "ip route add default via 192.168.1.1"));
}

void applications() {
    const auto network = netsim::sample_lan();
    const DeviceId a{1};
    auto start = term::run("ping 192.168.1.20", a, network, nullptr).start;
    CHECK(start && start->destination == address("192.168.1.20") && start->options.count == 4 && start->default_count);
    start = term::run("ping -c 2 -i 0.5 -W2 -t 5 -n 192.168.1.20", a, network, nullptr).start;
    CHECK(start && start->options.count == 2 && !start->default_count && start->options.ttl == 5);
    CHECK(start->options.interval == std::chrono::milliseconds(500) && start->options.timeout == std::chrono::seconds(2));
    CHECK(has(refused("ping -c 0 192.168.1.20", a, network), "out of range"));
    CHECK(has(refused("ping -i 0.001 192.168.1.20", a, network), "bad timing interval"));
    CHECK(has(refused("ping -f 192.168.1.20", a, network), "option -f is not in resend's ping"));
    CHECK(has(refused("ping PC-B", a, network), "PC-B's is 192.168.1.20"));
    CHECK(has(refused("ping", a, network), "Usage: ping"));
    CHECK(has(refused("ping 192.168.1.20", DeviceId{3}, network), "only hosts start pings"));
    start = term::run("traceroute -I -m 5 10.2.0.20", a, network, nullptr).start;
    CHECK(start && start->options.traceroute && start->options.count == 5);

    // What ping prints, line by line as the run goes, then its statistics.
    netsim::Simulation simulation(network);
    netsim::Problem problem{};
    const auto id = simulation.start_ping(a, address("192.168.1.20"), problem, {.count = 2});
    CHECK(id.has_value());
    simulation.run_until(std::chrono::milliseconds(90));
    auto text = term::application_output(simulation, *simulation.ping(*id));
    CHECK(text == "PING 192.168.1.20 (192.168.1.20) 56(84) bytes of data.\n64 bytes from 192.168.1.20: icmp_seq=1 ttl=64 time=80.0 ms\n");
    while (simulation.step()) {}
    text = term::application_output(simulation, *simulation.ping(*id));
    CHECK(has(text, "64 bytes from 192.168.1.20: icmp_seq=2 ttl=64 time=40.0 ms\n"));
    CHECK(has(text, "\n--- 192.168.1.20 ping statistics ---\n2 packets transmitted, 2 received, 0% packet loss, time "));
    CHECK(has(text, "rtt min/avg/max/mdev = 40.000/60.000/80.000/20.000 ms\n"));

    // A gateway nobody has: the host says its own ARP got no answer.
    const auto wrong = example("wrong-gateway");
    netsim::Simulation failed(wrong);
    const auto lost = failed.start_ping(named(wrong, "PC-A"), address("10.2.0.20"), problem);
    while (failed.step()) {}
    text = term::application_output(failed, *failed.ping(*lost));
    CHECK(has(text, "From 10.1.0.10 icmp_seq=1 Destination Host Unreachable\n"));
    CHECK(has(text, "1 packets transmitted, 0 received, 100% packet loss"));
    // Traceroute, hop by hop.
    const auto routed = example("two-lans-router");
    netsim::Simulation traced(routed);
    const auto trace = traced.start_ping(named(routed, "PC-A"), address("10.2.0.20"), problem, {.count = 30, .traceroute = true});
    while (traced.step()) {}
    text = term::application_output(traced, *traced.ping(*trace));
    CHECK(has(text, "traceroute to 10.2.0.20 (10.2.0.20), 30 hops max, 60 byte packets\n"));
    CHECK(has(text, " 1  10.1.0.1 (10.1.0.1)  "));
    CHECK(has(text, " 2  10.2.0.20 (10.2.0.20)  "));
}

void canonical_and_completion() {
    CHECK(term::canonical("ip a") == "ip address show");
    CHECK(term::canonical("sudo ip address show") == "ip address show");
    CHECK(term::canonical("ip n") == "ip neighbour show");
    CHECK(term::canonical("ip link set p1 up") == "ip link set dev p1 up");
    CHECK(term::canonical("ip link set dev p1 up") == "ip link set dev p1 up");
    CHECK(term::canonical("ip r add 0.0.0.0/0 via 10.1.0.1") == "ip route add default via 10.1.0.1");
    CHECK(term::canonical("ping -c 3 10.0.0.1") == "ping 10.0.0.1 -c 3");
    CHECK(term::canonical("ping -n -c3 10.0.0.1") == "ping 10.0.0.1 -c 3");
    CHECK(term::canonical("bridge f") == "bridge fdb show");
    CHECK(!term::canonical("frobnicate"));
    CHECK(!term::canonical("ip"));

    const auto network = netsim::sample_lan();
    const DeviceId a{1}, sw{3};
    auto completion = term::complete("", a, network);
    CHECK(completion.candidates == std::vector<std::string>({"ip", "ping", "traceroute", "help", "clear"}));
    CHECK(term::complete("i", a, network).line == "ip ");
    CHECK(term::complete("ip a", a, network).line == "ip address ");
    CHECK(term::complete("ip link set ", a, network).line == "ip link set p1 ");
    completion = term::complete("ip link set p1 ", a, network);
    CHECK(completion.line == "ip link set p1 " && completion.candidates == std::vector<std::string>({"up", "down"}));
    CHECK(term::complete("ip link set p1 d", a, network).line == "ip link set p1 down ");
    CHECK(term::complete("ip address del ", a, network).line == "ip address del 192.168.1.10/24 ");
    CHECK(term::complete("ip address del 192.168.1.10/24 ", a, network).line == "ip address del 192.168.1.10/24 dev ");
    CHECK(term::complete("ip route add ", a, network).line == "ip route add default ");
    CHECK(term::complete("ip route add default ", a, network).line == "ip route add default via ");
    CHECK(term::complete("b", sw, network).line == "bridge ");
    CHECK(term::complete("b", a, network).line == "b");
    completion = term::complete("ip link set p", sw, network);
    CHECK(completion.line == "ip link set p" && completion.candidates.size() == 8);
    CHECK(term::complete("help t", a, network).line == "help traceroute ");

    // The menus: what can be run here, complete or with a blank to fill.
    const auto host_menus = term::menus(a, network);
    CHECK(host_menus.size() == 2 && host_menus[0].family == "ip" && host_menus[1].family == "ping");
    const auto has_entry = [](const term::Menu& menu, const std::string& command, bool complete) {
        return std::any_of(menu.entries.begin(), menu.entries.end(),
                           [&](const term::MenuEntry& entry) { return entry.command == command && entry.complete == complete; });
    };
    CHECK(has_entry(host_menus[0], "ip address", true));
    CHECK(has_entry(host_menus[0], "ip address del 192.168.1.10/24 dev p1", true));
    CHECK(has_entry(host_menus[0], "ip link set p1 down", true));
    CHECK(has_entry(host_menus[0], "ip route add default via ADDRESS", false));
    CHECK(has_entry(host_menus[1], "ping -c 3 192.168.1.20", true));
    const auto switch_menus = term::menus(sw, network);
    CHECK(switch_menus.size() == 2 && switch_menus[1].family == "bridge" && has_entry(switch_menus[1], "bridge fdb", true));
    // Every entry of every menu runs, once its blanks are filled.
    for (const auto& item : {netsim::sample_lan(), example("two-lans-router")})
        for (const auto& device : item.devices)
            for (const auto& menu : term::menus(device.id, item))
                for (const auto& entry : menu.entries) {
                    if (!entry.complete) {
                        CHECK(entry.command.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos);
                        continue;
                    }
                    const auto result = term::run(entry.command, device.id, item, nullptr);
                    if (result.error) std::cerr << "  " << device.name << ": " << entry.command << ": " << result.output;
                    CHECK(!result.error);
                }
}

// The configuration a device has, as the terminal changes it.
struct Config {
    std::vector<std::optional<Ipv4Prefix>> addresses;
    std::vector<bool> up;
    std::optional<Ipv4Address> gateway;
    std::vector<netsim::Route> routes;
    bool operator==(const Config&) const = default;
};
Config config_of(const netsim::DeviceConfig& device) {
    Config config;
    for (const auto& port : device.ports) config.addresses.push_back(port.ipv4), config.up.push_back(port.admin_up);
    config.gateway = device.gateway;
    config.routes = device.routes;
    return config;
}

// Every lab's solution, typed in the terminals of its devices, configures
// them as the solution does; scripts/compare-linux.py types the same
// commands on Linux.
void every_solution_can_be_typed() {
    int labs = 0, typed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(RESEND_LABS_DIR)) {
        std::string error;
        const auto lab = netsim::lab::lab_from_json(read_file(entry.path()), error);
        if (!lab) std::cerr << entry.path() << ": " << error << '\n';
        CHECK(lab.has_value());
        ++labs;
        auto network = netsim::lab::starting_configuration(*lab);
        CHECK(network.links == lab->solution.network.links);
        for (const auto& device : term::commands_between(network, lab->solution.network)) {
            const auto id = network.device_named(device.device)->id;
            for (const auto& line : device.commands) {
                const auto result = term::run(line, id, network, nullptr);
                if (result.error) std::cerr << "  " << lab->id << ", " << device.device << ": " << line << ": " << result.output;
                CHECK(!result.error && !result.start);
                for (const auto& change : result.edits) CHECK(netsim::edit::apply(network, change) == netsim::Problem::None);
                ++typed;
            }
        }
        for (const auto& device : lab->solution.network.devices) {
            const bool same = config_of(*network.device_named(device.name)) == config_of(device);
            if (!same) std::cerr << "  " << lab->id << ": " << device.name << " is not configured as the solution\n";
            CHECK(same);
        }
    }
    CHECK(labs >= 10 && typed >= 20);
}

// 50 random commands typed in terminals, then 50 undos: the network as it was.
void random_commands_are_undone() {
    for (const auto& start : {example("two-lans-router"), example("routing-loop"), netsim::sample_lan()}) {
        resend::Workspace workspace(start);
        std::mt19937 random(7);
        const auto pick = [&](const std::vector<std::string>& choices) {
            return choices[std::uniform_int_distribution<std::size_t>(0, choices.size() - 1)(random)];
        };
        // Addresses on the networks the devices are on, and a few beside them.
        std::vector<std::string> networks = {"10.77.0."};
        for (const auto& device : start.devices)
            for (const auto& port : device.ports)
                if (port.ipv4) {
                    const auto text = port.ipv4->network().to_string();
                    networks.push_back(text.substr(0, text.rfind('.') + 1));
                }
        int changed = 0;
        for (int i = 0; i < 50; ++i) {
            const auto& devices = workspace.scenario().devices;
            const auto& device = devices[std::uniform_int_distribution<std::size_t>(0, devices.size() - 1)(random)];
            const auto port = "p" + std::to_string(std::uniform_int_distribution<std::size_t>(1, device.ports.size())(random));
            const auto host = pick(networks) + pick({"1", "2", "5", "10", "20", "99"});
            std::string line = pick({
                "ip address add " + host + "/24 dev " + port,
                device.ports[0].ipv4 ? "ip address del " + device.ports[0].ipv4->to_string() + " dev p1" : "ip a",
                "ip link set " + port + " down",
                "ip link set " + port + " up",
                "ip route add default via " + host,
                "ip route replace default via " + host,
                "ip route del default",
                "ip route add 10.9." + pick({"0", "1"}) + ".0/24 via " + host,
                "ip route del 10.9.0.0/24",
                "ip neigh",
                "bridge fdb",
            });
            const auto result = term::run(line, device.id, workspace.scenario(), &workspace.simulation());
            if (!result.edits.empty() && workspace.change(result.edits, result.label).ok()) ++changed;
            // Sometimes the network runs on, so later changes are made while it runs.
            if (i % 10 == 9) {
                (void)workspace.ping(start.devices.front().id, address("10.2.0.20"));
                workspace.run_until(workspace.simulation().now() + std::chrono::milliseconds(15));
            }
        }
        if (changed < 5) std::cerr << "  only " << changed << " changes on " << start.title << '\n';
        CHECK(changed >= 5);
        for (int i = 0; i < 50; ++i) (void)workspace.undo();
        CHECK(workspace.design() == start);
    }
}

} // namespace

int main() {
    queries();
    changes();
    applications();
    canonical_and_completion();
    every_solution_can_be_typed();
    random_commands_are_undone();
    std::cout << "Terminal tests passed: " << test::checks << " checks.\n";
    return 0;
}
