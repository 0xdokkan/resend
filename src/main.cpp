#include "app_controller.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
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
}

int main(int argc, char** argv) {
    std::string snapshot_path;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--snapshot" && index + 1 < argc) snapshot_path = argv[++index];
        else if (arg == "--help") {
            std::cout << "resend - sandbox Slint / C++20\n"
                         "  --snapshot PATH   Save a rendered PPM preview, then exit\n";
            return 0;
        }
    }

    auto ui = AppWindow::create();
    resend::AppController controller(ui);
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
