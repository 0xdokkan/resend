// Regression tests: each case's complete output — records, frames, cables,
// pings and explanations — compared with a golden file in tests/golden.
// The files were first produced while the prototype's engine still checked
// every record and text. To accept an intended change, run with
// RESEND_UPDATE_GOLDEN=1 and review the diff.
#include "check.hpp"
#include "explain/explain.hpp"
#include "regression_cases.hpp"
#include "sim/simulation.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

namespace {
using netsim::format_ms;

std::string name_of(const netsim::Simulation& simulation, netsim::DeviceId id) {
    if (id.value == 0) return "network";
    const auto* device = simulation.known_device(id);
    return device ? device->name : "device " + std::to_string(id.value);
}
template<class T>
std::string id_text(const std::optional<T>& id) { return id ? std::to_string(id->value) : "-"; }

std::string frame_text(const netsim::EthernetFrame& frame) {
    std::ostringstream out;
    out << frame.src.to_string() << " > " << frame.dst.to_string();
    if (const auto* arp = frame.arp()) {
        out << "  ARP " << (arp->op == netsim::ArpPacket::Op::Request ? "request" : "reply") << " sender "
            << arp->sender_mac.to_string() << ' ' << arp->sender_ip.to_string() << " target " << arp->target_mac.to_string() << ' '
            << arp->target_ip.to_string();
    } else {
        const auto& packet = *frame.ipv4();
        out << "  IPv4 " << packet.src.to_string() << " > " << packet.dst.to_string() << " ttl " << int{packet.ttl} << " id "
            << packet.identification << " proto " << int{packet.protocol} << "  ICMP type " << int{packet.icmp.type} << " code "
            << int{packet.icmp.code} << " identifier " << packet.icmp.identifier << " sequence " << packet.icmp.sequence
            << " data " << packet.icmp.data_size;
    }
    return out.str();
}

std::string run(const regression::Case& item) {
    netsim::Simulation simulation(item.build());
    netsim::explain::Explainer explainer;
    regression::Driver driver{simulation, explainer};
    std::ostringstream out;
    out << "# " << item.name << '\n';
    if (item.script) {
        item.script(driver);
        driver.finish();
    }
    for (const auto& ping : item.pings) {
        driver.ping(ping.source, ping.destination.c_str());
        driver.finish();
    }

    if (!simulation.edits().empty()) out << "\n## edits\n";
    for (const auto& entry : simulation.edits()) out << format_ms(entry.time) << "  edit " << entry.edit.index() << '\n';

    out << "\n## pings\n";
    for (const auto& status : simulation.pings()) {
        out << status.id.value << "  " << name_of(simulation, status.source) << " > " << status.destination.to_string() << "  "
            << (status.state == netsim::PingState::Succeeded ? "succeeded" : "failed") << ' '
            << netsim::explain::name(status.reason) << "  " << format_ms(status.started) << " .. " << format_ms(status.finished) << '\n';
        for (const auto& echo : status.echoes)
            out << "  seq " << echo.sequence << "  " << (echo.state == netsim::EchoState::Replied ? "replied" : "lost") << ' '
                << netsim::explain::name(echo.reason) << "  " << format_ms(echo.sent) << " .. " << format_ms(echo.finished)
                << "  record " << echo.record << '\n';
    }

    out << "\n## records\n";
    for (const auto& record : simulation.trace().records()) {
        out << format_ms(record.time) << "  #" << record.seq << " e" << record.episode << "  " << name_of(simulation, record.device)
            << " port " << (record.port ? std::to_string(*record.port + 1) : "-") << "  " << netsim::explain::name(record.kind);
        if (record.reason != netsim::Reason::None) out << ' ' << netsim::explain::name(record.reason);
        out << "  link " << id_text(record.link) << " frame " << id_text(record.pdu) << " value " << record.value << '\n';
    }

    out << "\n## frames\n";
    const auto& frames = simulation.trace().pdus();
    for (std::size_t i = 0; i < frames.size(); ++i) out << i << "  " << frame_text(frames[i]) << '\n';

    out << "\n## transmissions\n";
    for (const auto& sent : simulation.trace().transmissions())
        out << "frame " << sent.pdu.value << " link " << sent.link.value << "  " << name_of(simulation, sent.from) << " port "
            << sent.from_port + 1 << " > " << name_of(simulation, sent.to) << " port " << sent.to_port + 1 << "  "
            << format_ms(sent.start) << " .. " << format_ms(sent.arrival) << "  record " << sent.record
            << (sent.stopped ? "  stopped" : "") << '\n';

    out << "\n## events\n";
    for (const auto& event : explainer.events()) {
        out << format_ms(event.time) << "  [" << event.protocol << "] " << event.summary << "  ("
            << (event.simulator ? "simulator" : event.success ? "ok" : "failed")
            << ", from " << id_text(event.from) << " to " << id_text(event.to) << " link " << id_text(event.link) << " frame "
            << id_text(event.pdu) << ")\n";
        std::istringstream lines(event.detail);
        for (std::string line; std::getline(lines, line);) out << "    " << line << '\n';
    }
    return out.str();
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
} // namespace

int main() {
    const std::filesystem::path golden = RESEND_GOLDEN_DIR;
    const char* update = std::getenv("RESEND_UPDATE_GOLDEN");
    const bool write = update && std::string(update) == "1";
    int changed = 0;
    for (const auto& item : regression::cases()) {
        const auto path = golden / (std::string(item.file) + ".txt");
        const auto output = run(item);
        if (write) {
            std::ofstream(path, std::ios::binary) << output;
            continue;
        }
        if (read_file(path) != output) {
            std::cerr << "Differs from " << path.string() << ":\n" << output << '\n';
            ++changed;
        }
        CHECK(read_file(path) == output);
    }
    if (write) std::cout << "Golden files written to " << golden.string() << ".\n";
    std::cout << "Regression tests passed: " << test::checks << " checks.\n";
    return changed == 0 ? 0 : 1;
}
