#include "explain/explain.hpp"
#include "scenario/scenario_json.hpp"
#include "sim/simulation.hpp"
#include "wire/wire.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr const char* usage =
    "resend-cli - run a resend network without the interface\n"
    "\n"
    "Usage: resend-cli [options] [DESTINATION]\n"
    "  DESTINATION            ping this IPv4 address from the first host\n"
    "                         (192.168.1.20 on the sample LAN when nothing else is asked)\n"
    "  --scenario FILE        use a scenario file instead of the sample LAN\n"
    "  --ping SOURCE IPV4     ping from the host named SOURCE; repeat it to run pings at once\n"
    "  --traceroute SOURCE IPV4\n"
    "                         trace the route from SOURCE with Echo Requests of TTL 1, 2, ...\n"
    "  --count N              Echo Requests per ping, 1 to 1000 (default 1)\n"
    "  --interval TIME        time between the requests of a ping (default 1s)\n"
    "  --timeout TIME         how long each request waits for its reply (default 1s)\n"
    "  --ttl N                time to live of ping requests, 1 to 255 (default 64)\n"
    "  --max-hops N           hops a traceroute tries, 1 to 255 (default 30)\n"
    "\n"
    "Changes while the simulation runs; each happens at time 0 unless --at comes first:\n"
    "  --at TIME              run the next --ping or change at this simulated time, e.g. 1.5s\n"
    "  --link-down ID         disable the cable with this id\n"
    "  --link-up ID           enable it again\n"
    "  --unplug ID            remove the cable\n"
    "  --power-off NAME       power off a device; it forgets everything it learned\n"
    "  --power-on NAME        power it on again\n"
    "  --set-ip NAME ADDRESS  give a host another address, e.g. 192.168.1.21/24\n"
    "  --set-gateway NAME IPV4\n"
    "                         give a host another default gateway; \"none\" removes it\n"
    "  --disconnect           disable the last cable before starting\n"
    "\n"
    "Output:\n"
    "  --trace                print the trace records instead of the explanations\n"
    "  --pcap FILE            write every frame sent to FILE, for Wireshark or tcpdump\n"
    "  --pcap-link ID         write only the frames on the cable with this id\n"
    "  --check                only read and validate the scenario\n"
    "  --help                 show this text\n"
    "\n"
    "Exit status: 0 every ping got a reply, 1 a ping got none, 2 invalid input,\n"
    "3 the simulation did not finish.\n"
    "Models Ethernet switching, ARP, IPv4 with static routing, ICMP echo and errors.\n";

// A command from the command line, run at its time in the order given.
struct Command {
    netsim::SimTime at{};
    std::string option;    // "--ping", "--link-down", ...
    std::string name;      // a host or device name, or a cable id
    std::string argument;  // an address, if any
};

int invalid(const std::string& message) {
    std::cerr << message << '\n';
    return 2;
}

std::string device_name(const netsim::Simulation& simulation, netsim::DeviceId id) {
    if (id.value == 0) return "network";
    const auto* device = simulation.known_device(id);
    return device ? device->name : "device " + std::to_string(id.value);
}

std::optional<std::uint32_t> number(const std::string& text) {
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

void print_event(const netsim::explain::Event& event) {
    std::cout << netsim::format_ms(event.time) << "  [" << event.protocol << "] " << event.summary << '\n';
    std::string line;
    for (const char c : event.detail + "\n") {
        if (c != '\n') {
            line += c;
            continue;
        }
        std::cout << "    " << line << '\n';
        line.clear();
    }
}

void print_record(const netsim::Simulation& simulation, const netsim::TraceRecord& record) {
    std::cout << netsim::format_ms(record.time) << "  #" << record.seq << " e" << record.episode << "  "
              << device_name(simulation, record.device);
    if (record.port) std::cout << " p" << *record.port + 1;
    std::cout << "  " << netsim::explain::name(record.kind);
    if (record.reason != netsim::Reason::None) std::cout << " (" << netsim::explain::name(record.reason) << ')';
    if (record.link) std::cout << "  link " << record.link->value;
    if (record.pdu) std::cout << "  frame " << record.pdu->value;
    if (record.value) std::cout << "  value " << record.value;
    std::cout << '\n';
}

// What ping prints: a line per request, then the statistics.
void print_ping(const netsim::Simulation& simulation, const netsim::PingStatus& ping) {
    const auto* source = simulation.known_device(ping.source);
    const auto& records = simulation.trace().records();
    std::cout << "PING " << ping.destination.to_string() << " from " << (source ? source->name : "?") << ": "
              << netsim::ping_data_size << " data bytes\n";
    netsim::SimTime low = netsim::SimTime::max(), high{}, total{};
    for (const auto& echo : ping.echoes) {
        if (echo.state == netsim::EchoState::Replied) {
            const auto rtt = echo.finished - echo.sent;
            low = std::min(low, rtt);
            high = std::max(high, rtt);
            total += rtt;
            const auto& record = records.at(echo.record);
            const auto* reply = record.pdu ? simulation.trace().pdu(*record.pdu).ipv4() : nullptr;
            std::cout << 8 + (reply ? reply->icmp.data_size : netsim::ping_data_size) << " bytes from " << ping.destination.to_string()
                      << ": icmp_seq=" << echo.sequence << " ttl=" << (reply ? int{reply->ttl} : int{netsim::default_ttl})
                      << " time=" << netsim::format_ms(rtt) << '\n';
        } else if (echo.state == netsim::EchoState::Lost && echo.responder != netsim::Ipv4Address{}) {
            // As ping prints the ICMP errors about its requests.
            const auto* text = echo.reason == netsim::Reason::TtlExceeded ? "Time to live exceeded"
                : echo.reason == netsim::Reason::HostUnreachable          ? "Destination Host Unreachable"
                                                                          : "Destination Net Unreachable";
            std::cout << "From " << echo.responder.to_string() << " icmp_seq=" << echo.sequence << ' ' << text << '\n';
        } else if (echo.state == netsim::EchoState::Lost) {
            std::cout << "icmp_seq=" << echo.sequence << " lost: " << netsim::explain::describe(echo.reason) << '\n';
        }
    }
    const auto sent = ping.echoes.size();
    const auto replies = static_cast<std::size_t>(ping.replies());
    std::cout << "--- " << ping.destination.to_string() << " ping statistics ---\n"
              << sent << " packets transmitted, " << replies << " received, "
              << (sent ? static_cast<double>(sent - replies) * 100.0 / static_cast<double>(sent) : 0.0) << "% packet loss\n";
    if (replies) {
        const auto avg = total / static_cast<std::int64_t>(replies);
        const auto plain = [](netsim::SimTime time) {
            const auto text = netsim::format_ms(time);
            return text.substr(0, text.size() - 3);
        };
        std::cout << "rtt min/avg/max = " << plain(low) << '/' << plain(avg) << '/' << plain(high) << " ms\n";
    }
}
// What traceroute prints: a line per hop, "*" for a hop that did not
// answer, "!N" and "!H" for a network or host unreachable.
void print_traceroute(const netsim::Simulation& simulation, const netsim::PingStatus& trace) {
    const auto* source = simulation.known_device(trace.source);
    std::cout << "traceroute to " << trace.destination.to_string() << " from " << (source ? source->name : "?") << ", "
              << trace.options.count << " hops max\n";
    for (const auto& echo : trace.echoes) {
        std::cout << (echo.sequence < 10 ? " " : "") << echo.sequence << "  ";
        if (echo.responder == netsim::Ipv4Address{}) {
            std::cout << (echo.state == netsim::EchoState::Lost && echo.reason != netsim::Reason::EchoTimeout
                              ? "lost: " + netsim::explain::describe(echo.reason) : std::string("*"))
                      << '\n';
            continue;
        }
        std::cout << echo.responder.to_string() << "  " << netsim::format_ms(echo.finished - echo.sent)
                  << (echo.reason == netsim::Reason::NetUnreachable ? " !N" : echo.reason == netsim::Reason::HostUnreachable ? " !H" : "")
                  << '\n';
    }
}
} // namespace

int main(int argc, char** argv) {
    std::string file, destination, pcap;
    std::optional<netsim::LinkId> pcap_link;
    std::vector<Command> commands;
    netsim::PingOptions options;
    std::uint16_t max_hops = netsim::default_max_hops;
    std::optional<netsim::SimTime> at;
    bool disconnect = false, trace = false, check = false;
    const auto duration = [](const char* text, netsim::SimTime& out) {
        const auto parsed = netsim::parse_duration(text);
        if (parsed) out = *parsed;
        return parsed.has_value();
    };
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        const auto has = [&](int values) { return index + values < argc; };
        const auto add = [&](std::string name, std::string argument = {}) {
            commands.push_back({at.value_or(netsim::SimTime{}), arg, std::move(name), std::move(argument)});
            at.reset();
        };
        if (arg == "--help") {
            std::cout << usage;
            return 0;
        } else if (arg == "--scenario" && has(1)) {
            file = argv[++index];
        } else if ((arg == "--ping" || arg == "--traceroute") && has(2)) {
            add(argv[index + 1], argv[index + 2]);
            index += 2;
        } else if (arg == "--count" && has(1)) {
            const auto count = number(argv[++index]);
            if (!count || *count < 1 || *count > netsim::max_ping_count) return invalid("--count: expected a number from 1 to 1000");
            options.count = static_cast<std::uint16_t>(*count);
        } else if ((arg == "--ttl" || arg == "--max-hops") && has(1)) {
            const auto value = number(argv[++index]);
            if (!value || *value < 1 || *value > 255) return invalid(arg + ": expected a number from 1 to 255");
            if (arg == "--ttl") options.ttl = static_cast<std::uint8_t>(*value);
            else max_hops = static_cast<std::uint16_t>(*value);
        } else if ((arg == "--interval" || arg == "--timeout") && has(1)) {
            if (!duration(argv[++index], arg == "--interval" ? options.interval : options.timeout))
                return invalid(arg + ": expected a duration such as 1s or 200ms");
        } else if (arg == "--at" && has(1)) {
            netsim::SimTime time{};
            if (!duration(argv[++index], time)) return invalid("--at: expected a duration such as 1.5s");
            at = time;
        } else if ((arg == "--link-down" || arg == "--link-up" || arg == "--unplug" || arg == "--power-off" || arg == "--power-on") &&
                   has(1)) {
            add(argv[++index]);
        } else if ((arg == "--set-ip" || arg == "--set-gateway") && has(2)) {
            add(argv[index + 1], argv[index + 2]);
            index += 2;
        } else if (arg == "--disconnect") {
            disconnect = true;
        } else if (arg == "--trace") {
            trace = true;
        } else if (arg == "--pcap" && has(1)) {
            pcap = argv[++index];
        } else if (arg == "--pcap-link" && has(1)) {
            const auto id = number(argv[++index]);
            if (!id) return invalid("--pcap-link: expected a cable id");
            pcap_link = netsim::LinkId{*id};
        } else if (arg == "--check") {
            check = true;
        } else if (!arg.starts_with("--") && destination.empty()) {
            destination = arg;
        } else {
            std::cerr << "Unknown or incomplete option: " << arg << "\n\n" << usage;
            return 2;
        }
    }
    if (at) return invalid("--at must come before a --ping or a change.");
    if (pcap_link && pcap.empty()) return invalid("--pcap-link needs --pcap FILE.");

    netsim::Scenario scenario = netsim::sample_lan();
    if (!file.empty()) {
        std::ifstream input(file, std::ios::binary);
        if (!input) return invalid(file + ": cannot open the file");
        const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        std::string error;
        auto loaded = netsim::scenario_from_json(text, error);
        if (!loaded) return invalid(file + ": " + error);
        scenario = std::move(*loaded);
    }
    const auto issues = netsim::validate(scenario);
    for (const auto& issue : issues)
        std::cerr << (file.empty() ? "scenario" : file) << ": " << issue.subject << ": " << netsim::explain::describe(issue.problem) << '\n';
    if (!issues.empty()) return 2;
    if (check) {
        std::cout << (scenario.title.empty() ? file : scenario.title) << ": " << scenario.devices.size() << " devices, "
                  << scenario.links.size() << " cables, valid.\n";
        return 0;
    }
    if (disconnect && !scenario.links.empty()) scenario.links.back().up = false;

    const bool pinging = std::any_of(commands.begin(), commands.end(),
                                     [](const auto& item) { return item.option == "--ping" || item.option == "--traceroute"; });
    if (!destination.empty() || !pinging)
        commands.insert(commands.begin(), {netsim::SimTime{}, "--ping", "", destination.empty() ? "192.168.1.20" : destination});
    std::stable_sort(commands.begin(), commands.end(), [](const auto& a, const auto& b) { return a.at < b.at; });

    netsim::Simulation simulation(scenario);
    netsim::explain::Explainer explainer;
    std::size_t printed = 0;
    const auto print_new = [&] {
        if (trace) {
            const auto& records = simulation.trace().records();
            for (; printed < records.size(); ++printed) print_record(simulation, records[printed]);
            return;
        }
        explainer.update(simulation);
        for (; printed < explainer.events().size(); ++printed) print_event(explainer.events()[printed]);
    };
    int budget = 1'000'000;
    const auto run_to = [&](std::optional<netsim::SimTime> time) {
        while (budget > 0) {
            const auto next = simulation.next_event_time();
            if (!next || (time && *next > *time)) break;
            simulation.step();
            print_new();
            --budget;
        }
        if (time && budget > 0) simulation.run_until(*time);
        return budget > 0;
    };

    std::vector<netsim::PingId> started;
    for (const auto& command : commands) {
        if (!run_to(command.at)) break;
        const auto label = command.option + " " + command.name + (command.argument.empty() ? "" : " " + command.argument);
        const auto* device = command.name.empty() ? nullptr : simulation.scenario().device_named(command.name);
        const auto cable = number(command.name);
        netsim::Problem problem = netsim::Problem::None;
        if (command.option == "--ping" || command.option == "--traceroute") {
            if (command.name.empty()) {
                const auto& devices = simulation.scenario().devices;
                const auto host = std::find_if(devices.begin(), devices.end(), [](const auto& item) { return item.kind == netsim::DeviceKind::Host; });
                if (host == devices.end()) return invalid("The network has no host to ping from.");
                device = &*host;
            }
            if (!device) return invalid("No device is named \"" + command.name + "\".");
            const auto address = netsim::Ipv4Address::parse(command.argument);
            problem = netsim::Problem::InvalidDestination;
            auto chosen = options;
            if (command.option == "--traceroute") chosen = {.count = max_hops, .timeout = options.timeout, .traceroute = true};
            const auto id = address ? simulation.start_ping(device->id, *address, problem, chosen) : std::nullopt;
            if (!id) return invalid(device->name + " → " + command.argument + ": " + netsim::explain::describe(problem));
            started.push_back(*id);
        } else if (command.option == "--link-down" || command.option == "--link-up" || command.option == "--unplug") {
            if (!cable) return invalid(label + ": expected a cable id");
            const netsim::LinkId link{*cable};
            problem = command.option == "--unplug" ? simulation.apply(netsim::Disconnect{link})
                                                   : simulation.apply(netsim::SetLinkUp{link, command.option == "--link-up"});
        } else if (command.option == "--power-off" || command.option == "--power-on") {
            if (!device) return invalid("No device is named \"" + command.name + "\".");
            problem = simulation.apply(netsim::SetPowered{device->id, command.option == "--power-on"});
        } else if (command.option == "--set-ip") {
            if (!device) return invalid("No device is named \"" + command.name + "\".");
            const auto address = netsim::Ipv4Prefix::parse(command.argument);
            if (!address) return invalid(label + ": expected an address with its prefix, such as 192.168.1.21/24");
            problem = simulation.apply(netsim::SetAddress{{device->id, 0}, *address});
        } else if (command.option == "--set-gateway") {
            if (!device) return invalid("No device is named \"" + command.name + "\".");
            const auto gateway = netsim::Ipv4Address::parse(command.argument);
            if (!gateway && command.argument != "none") return invalid(label + ": expected an IPv4 address such as 192.168.1.1, or none");
            problem = simulation.apply(netsim::SetGateway{device->id, gateway});
        }
        if (!netsim::ok(problem)) return invalid(label + ": " + netsim::explain::describe(problem));
        print_new();
    }
    if (!run_to(std::nullopt) || simulation.pending()) {
        std::cerr << "The simulation did not finish within its event budget.\n";
        return 3;
    }

    if (!pcap.empty()) {
        std::ofstream out(pcap, std::ios::binary);
        if (!out) return invalid(pcap + ": cannot write the file");
        netsim::wire::write_pcap(out, simulation.trace(), pcap_link);
    }

    bool all = true;
    for (const auto id : started) {
        const auto& status = *simulation.ping(id);
        all = all && status.state == netsim::PingState::Succeeded;
        std::cout << '\n';
        if (status.options.traceroute) print_traceroute(simulation, status);
        else print_ping(simulation, status);
    }
    const auto& records = simulation.trace().records();
    const auto storms = std::count_if(records.begin(), records.end(), [](const auto& record) {
        return record.kind == netsim::TraceKind::SimulatorGuard;
    });
    if (storms > 0)
        std::cout << "\nStorm protection: the simulator stopped " << storms << (storms == 1 ? " frame" : " frames")
                  << " going round an Ethernet loop.\n";
    return all ? 0 : 1;
}
