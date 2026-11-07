#include "app_controller.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

using namespace std::chrono_literals;

namespace {
bool snapshot(const slint::Window& window, const std::string& path) {
    const auto pixels = window.take_snapshot();
    if (!pixels) return false;
    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << pixels->width() << ' ' << pixels->height() << "\n255\n";
    for (const auto& pixel : *pixels) {
        file.put(static_cast<char>(pixel.r));
        file.put(static_cast<char>(pixel.g));
        file.put(static_cast<char>(pixel.b));
    }
    return static_cast<bool>(file);
}

constexpr const char* usage =
    "resend - network sandbox (Slint / C++20)\n"
    "  --scenario FILE   Open a scenario file instead of the sample LAN\n"
    "  --lang CODE       Use this language, e.g. en or it, instead of the one chosen\n"
    "  --snapshot PATH   Save a rendered PPM preview, then exit\n"
    "  --ping IPV4       Start a ping from the selected host (the first one) at startup\n"
    "  --traceroute IPV4 Start a traceroute instead\n"
    "  --at TIME         Show this simulated instant, e.g. 15ms or 1.5s (with --ping)\n"
    "  --select NAME     Then select this device, e.g. to show its inspector\n";
}

int main(int argc, char** argv) {
    std::string snapshot_path, ping, scenario, select;
    bool traceroute = false;
    std::optional<std::string> language;
    std::optional<netsim::SimTime> at;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        const bool has_value = index + 1 < argc;
        if (arg == "--snapshot" && has_value) {
            snapshot_path = argv[++index];
        } else if (arg == "--scenario" && has_value) {
            scenario = argv[++index];
        } else if (arg == "--lang" && has_value) {
            language = argv[++index];
            const auto known = netsim::text::languages();
            if (std::find(known.begin(), known.end(), *language) == known.end()) {
                std::cerr << "Unknown language: " << *language << " (available:";
                for (const auto& code : known) std::cerr << ' ' << code;
                std::cerr << ")\n";
                return 2;
            }
        } else if ((arg == "--ping" || arg == "--traceroute") && has_value) {
            ping = argv[++index];
            traceroute = arg == "--traceroute";
        } else if (arg == "--select" && has_value) {
            select = argv[++index];
        } else if (arg == "--at" && has_value) {
            at = netsim::parse_duration(argv[++index]);
            if (!at) {
                std::cerr << "Invalid time for --at: " << argv[index] << " (examples: 15ms, 1.5s)\n";
                return 2;
            }
        } else if (arg == "--help") {
            std::cout << usage;
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << '\n' << usage;
            return 2;
        }
    }

    auto ui = AppWindow::create();
    resend::AppController controller(ui, {resend::state_directory(), language});
    if (!scenario.empty()) {
        std::string error;
        if (!resend::read_scenario_file(resend::path_from_text(scenario), error)) {
            std::cerr << scenario << ": " << error << '\n';
            return 2;
        }
        controller.open_file(resend::path_from_text(scenario));
    }
    if (!ping.empty()) {
        ui->global<UiRun>().set_target(ping.c_str());
        ui->global<UiRun>().set_traceroute(traceroute);
        ui->global<UiRun>().invoke_ping();
        if (controller.simulation().pings().empty()) {
            std::cerr << "The ping could not start: " << std::string(ui->global<UiApp>().get_status()) << '\n';
            return 2;
        }
        if (at) controller.seek(*at);
    }
    if (!select.empty() && !controller.select_device(select)) {
        std::cerr << "No device named " << select << '\n';
        return 2;
    }
    int exit_code = 0;
    if (!snapshot_path.empty()) {
        slint::Timer::single_shot(550ms, [&] {
            if (!snapshot(ui->window(), snapshot_path)) {
                std::cerr << "Failed to save Slint snapshot.\n";
                exit_code = 1;
            }
            slint::quit_event_loop();
        });
    } else {
        // A network an earlier session could not save is offered back.
        controller.offer_recovery();
    }
    ui->run();
    return exit_code;
}
