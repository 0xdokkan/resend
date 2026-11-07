#include "app_controller.hpp"

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
    "  --snapshot PATH   Save a rendered PPM preview, then exit\n"
    "  --ping IPV4       Start a ping from the selected host (PC-A) at startup\n"
    "  --at TIME         Show this simulated instant, e.g. 15ms or 1.5s (with --ping)\n";
}

int main(int argc, char** argv) {
    std::string snapshot_path, ping;
    std::optional<netsim::SimTime> at;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        const bool has_value = index + 1 < argc;
        if (arg == "--snapshot" && has_value) snapshot_path = argv[++index];
        else if (arg == "--ping" && has_value) ping = argv[++index];
        else if (arg == "--at" && has_value) {
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
    resend::AppController controller(ui);
    if (!ping.empty()) {
        ui->set_target_ip(ping.c_str());
        ui->invoke_ping();
        if (!controller.engine().pending()) {
            std::cerr << "The ping could not start: " << std::string(ui->get_status_text().data()) << '\n';
            return 2;
        }
        if (at) controller.seek(*at);
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
    }
    ui->run();
    return exit_code;
}
