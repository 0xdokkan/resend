// The exit check of milestone M4: the inspector's fields match tshark's,
// position, size and value, for every frame of the example scenarios.
// Prints SKIPPED without tshark.
#include "check.hpp"
#include "scenario/scenario_json.hpp"
#include "sim_support.hpp"
#include "wire/wire.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace {
namespace fs = std::filesystem;
using namespace simtest;

struct Shown {
    std::size_t pos = 0, size = 0;
    std::string show, bytes;
};
// Per frame, from 1, the fields tshark shows by name.
using Dissection = std::map<int, std::map<std::string, Shown>>;

std::string attribute(const std::string& line, const std::string& name) {
    const auto key = " " + name + "=\"";
    const auto start = line.find(key);
    if (start == std::string::npos) return {};
    const auto from = start + key.size();
    return line.substr(from, line.find('"', from) - from);
}

Dissection run_tshark(const fs::path& capture) {
    Dissection result;
    const auto command = std::string("\"") + RESEND_TSHARK + "\" -r \"" + capture.string() + "\" -T pdml";
    FILE* pipe = popen(command.c_str(), "r");
    CHECK(pipe != nullptr);
    if (!pipe) return result;
    std::string text;
    char buffer[4096];
    while (const auto count = std::fread(buffer, 1, sizeof buffer, pipe)) text.append(buffer, count);
    CHECK(pclose(pipe) == 0);
    int frame = 0;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const auto line = text.substr(start, end - start);
        start = end + 1;
        if (line.find("<field name=\"num\"") != std::string::npos) frame = std::stoi(attribute(line, "show"));
        if (line.find("<field name=\"") == std::string::npos) continue;
        const auto name = attribute(line, "name");
        const auto unmasked = attribute(line, "unmaskedvalue");
        const auto pos = attribute(line, "pos"), size = attribute(line, "size");
        if (pos.empty() || size.empty()) continue;
        result[frame].emplace(name, Shown{std::stoul(pos), std::stoul(size), attribute(line, "show"), unmasked.empty() ? attribute(line, "value") : unmasked});
    }
    return result;
}

std::string hex(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t length) {
    static constexpr char numerals[] = "0123456789abcdef";
    std::string text;
    for (std::size_t at = offset; at < offset + length; ++at) {
        text += numerals[bytes[at] >> 4];
        text += numerals[bytes[at] & 0xfU];
    }
    return text;
}

// Pings from the first host to every address of the network, twice.
Simulation run(const netsim::Scenario& scenario) {
    Simulation simulation(scenario);
    const netsim::DeviceConfig* source = nullptr;
    for (const auto& device : scenario.devices) {
        if (device.kind != netsim::DeviceKind::Host) continue;
        if (!source) {
            source = &device;
            continue;
        }
        Problem problem{};
        simulation.start_ping(source->id, device.ports[0].ipv4->address(), problem, {.count = 2});
    }
    drain(simulation);
    return simulation;
}
} // namespace

int main() {
    if (std::string(RESEND_TSHARK).empty()) {
        std::cout << "SKIPPED: tshark not found\n";
        return 0;
    }
    const auto folder = fs::temp_directory_path() / "resend-dissect";
    fs::create_directories(folder);
    int frames = 0, fields = 0;
    for (const auto& item : fs::directory_iterator(RESEND_EXAMPLES_DIR)) {
        if (item.path().extension() != ".json") continue;
        std::ifstream file(item.path(), std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        std::string error;
        const auto scenario = netsim::scenario_from_json(text, error);
        CHECK(scenario.has_value());
        if (!scenario) continue;
        const auto simulation = run(*scenario);
        const auto capture = folder / (item.path().stem().string() + ".pcap");
        {
            std::ofstream out(capture, std::ios::binary);
            netsim::wire::write_pcap(out, simulation.trace());
        }
        const auto shown = run_tshark(capture);
        int number = 0;
        for (const auto& sent : simulation.trace().transmissions()) {
            ++number;
            ++frames;
            const auto bytes = netsim::wire::encode(simulation.trace().pdu(sent.pdu));
            const auto found = shown.find(number);
            CHECK(found != shown.end());
            if (found == shown.end()) continue;
            for (const auto& layer : netsim::wire::dissect(bytes)) {
                for (const auto& field : layer.fields) {
                    ++fields;
                    const auto it = found->second.find(field.name);
                    const bool same = it != found->second.end() && it->second.pos == field.offset && it->second.size == field.length &&
                        it->second.show == field.value && it->second.bytes == hex(bytes, field.offset, field.length);
                    CHECK(same);
                    if (!same) std::cout << "  " << item.path().filename().string() << " frame " << number << " " << field.name << ": ours " << field.offset
                                         << "+" << field.length << " " << field.value << ", tshark's "
                                         << (it == found->second.end() ? std::string("missing") : std::to_string(it->second.pos) + "+" + std::to_string(it->second.size) + " " + it->second.show)
                                         << '\n';
                }
            }
        }
    }
    fs::remove_all(folder);
    CHECK(frames > 100);
    std::cout << "Dissection matches tshark: " << frames << " frames, " << fields << " fields, " << test::checks << " checks.\n";
}
