#include "explain/explain.hpp"
#include "scenario/scenario_json.hpp"
#include "sim/simulation.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
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
    "  --disconnect           disable the last cable before starting\n"
    "  --trace                print the trace records instead of the explanations\n"
    "  --check                only read and validate the scenario\n"
    "  --help                 show this text\n"
    "\n"
    "Exit status: 0 every ping succeeded, 1 a ping failed, 2 invalid input,\n"
    "3 the simulation did not finish.\n"
    "Models Ethernet switching, ARP, IPv4 on one subnet and ICMP echo; no routers yet.\n";

struct Ping {
    std::string source;  // empty: the first host
    std::string destination;
};

int invalid(const std::string& message) {
    std::cerr << message << '\n';
    return 2;
}

std::string device_name(const netsim::Scenario& scenario, netsim::DeviceId id) {
    const auto* device = scenario.device(id);
    return device ? device->name : "device " + std::to_string(id.value);
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
              << device_name(simulation.scenario(), record.device);
    if (record.port) std::cout << " p" << *record.port + 1;
    std::cout << "  " << netsim::explain::name(record.kind);
    if (record.reason != netsim::Reason::None) std::cout << " (" << netsim::explain::name(record.reason) << ')';
    if (record.link) std::cout << "  link " << record.link->value;
    if (record.pdu) std::cout << "  frame " << record.pdu->value;
    if (record.value) std::cout << "  value " << record.value;
    std::cout << '\n';
}
} // namespace

int main(int argc, char** argv) {
    std::string file, destination;
    std::vector<Ping> pings;
    bool disconnect = false, trace = false, check = false;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--help") {
            std::cout << usage;
            return 0;
        } else if (arg == "--scenario" && index + 1 < argc) {
            file = argv[++index];
        } else if (arg == "--ping" && index + 2 < argc) {
            pings.push_back({argv[index + 1], argv[index + 2]});
            index += 2;
        } else if (arg == "--disconnect") {
            disconnect = true;
        } else if (arg == "--trace") {
            trace = true;
        } else if (arg == "--check") {
            check = true;
        } else if (!arg.starts_with("--") && destination.empty()) {
            destination = arg;
        } else {
            std::cerr << "Unknown or incomplete option: " << arg << "\n\n" << usage;
            return 2;
        }
    }

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

    if (!destination.empty() || pings.empty()) pings.insert(pings.begin(), {"", destination.empty() ? "192.168.1.20" : destination});
    netsim::Simulation simulation(scenario);
    std::vector<std::pair<netsim::PingId, std::string>> started;
    for (const auto& ping : pings) {
        const netsim::DeviceConfig* source = nullptr;
        if (ping.source.empty()) {
            for (const auto& device : scenario.devices)
                if (!source && device.kind == netsim::DeviceKind::Host) source = &device;
            if (!source) return invalid("The network has no host to ping from.");
        } else if (!(source = scenario.device_named(ping.source))) {
            return invalid("No device is named \"" + ping.source + "\".");
        }
        const auto address = netsim::Ipv4Address::parse(ping.destination);
        netsim::Problem problem = netsim::Problem::InvalidDestination;
        const auto id = address ? simulation.start_ping(source->id, *address, problem) : std::nullopt;
        if (!id) return invalid(source->name + " → " + ping.destination + ": " + netsim::explain::describe(problem));
        started.emplace_back(*id, source->name + " → " + ping.destination);
    }

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
    for (int budget = 100'000; simulation.pending() && budget > 0; --budget) {
        simulation.step();
        print_new();
    }
    if (simulation.pending()) {
        std::cerr << "The simulation did not finish within its event budget.\n";
        return 3;
    }

    bool all = true;
    std::cout << '\n';
    for (const auto& [id, label] : started) {
        const auto& status = *simulation.ping(id);
        const bool success = status.state == netsim::PingState::Succeeded;
        all = all && success;
        std::cout << label << ": "
                  << (success ? "reply after " + netsim::format_ms(status.finished - status.started)
                              : "failed, " + netsim::explain::describe(status.reason))
                  << '\n';
    }
    return all ? 0 : 1;
}
