// Canvas spike: interactive by default, or a timed benchmark.
//   canvas-spike [--small] [--per-link] [--bench drag|pan|zoom|scroll|idle] [--seconds N]
//                [--snapshot FILE.ppm] [--menu] [--menu-pick]
// Run benchmarks with SLINT_DEBUG_PERFORMANCE=refresh_full_speed,console to get FPS.
#include "spike_app.hpp"

#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
bool save_snapshot(const slint::Window& window, const std::string& path) {
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

// A point where nothing is hit, so a left press starts panning.
spike::Vec2 empty_point(const spike::CanvasController& canvas, float width, float height) {
    for (float y = 20; y < height; y += 13)
        for (float x = 20; x < width; x += 13)
            if (canvas.hit_test({x, y}).kind == spike::HitKind::None) return {x, y};
    return {5, 5};
}
} // namespace

int main(int argc, char** argv) {
    bool small = false, per_link = false, menu = false, menu_pick = false;
    std::string bench, snapshot_path;
    double seconds = 5;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--small") small = true;
        else if (arg == "--per-link") per_link = true;
        else if (arg == "--menu") menu = true;  // right-click a card before the snapshot
        else if (arg == "--menu-pick") menu = menu_pick = true;  // ...then choose "Delete selection"
        else if (arg == "--bench" && i + 1 < argc) bench = argv[++i];
        else if (arg == "--seconds" && i + 1 < argc) seconds = std::stod(argv[++i]);
        else if (arg == "--snapshot" && i + 1 < argc) snapshot_path = argv[++i];
    }

    auto ui = SpikeWindow::create();
    spike::SpikeApp app(ui, per_link);
    if (small) app.small_scene();
    else app.stress_scene(150, 250, 20000);

    long ticks = 0;
    Clock::duration busy{};
    slint::Timer driver;
    slint::Timer::single_shot(300ms, [&] {
        app.fit();
        auto& window = ui->window();
        const float width = ui->get_canvas_width(), height = ui->get_canvas_height();
        spike::Vec2 grab = empty_point(app.canvas(), width, height);
        if (bench == "drag") {
            const auto& n = app.canvas().nodes()[app.canvas().nodes().size() / 2];
            grab = app.canvas().to_screen({n.pos.x + 40, n.pos.y + 30});
        }
        if (menu) {
            const auto& n = app.canvas().nodes().front();
            const auto at = app.canvas().to_screen({n.pos.x + 40, n.pos.y + 30});
            window.dispatch_pointer_press_event(slint::LogicalPosition({at.x, at.y}), slint::PointerEventButton::Right);
            window.dispatch_pointer_release_event(slint::LogicalPosition({at.x, at.y}), slint::PointerEventButton::Right);
            if (menu_pick) {
                // The menu opens at the pointer; its first entry sits just below.
                slint::Timer::single_shot(200ms, [&, at, id = n.id] {
                    const slint::LogicalPosition entry({at.x + 60, at.y + 20});
                    ui->window().dispatch_pointer_move_event(entry);
                    ui->window().dispatch_pointer_press_event(entry, slint::PointerEventButton::Left);
                    ui->window().dispatch_pointer_release_event(entry, slint::PointerEventButton::Left);
                    std::cout << "menu entry removed the card: " << (app.canvas().node(id) ? "no" : "yes") << '\n';
                });
            }
        }
        if (bench == "drag" || bench == "pan")
            window.dispatch_pointer_press_event(slint::LogicalPosition({grab.x, grab.y}), slint::PointerEventButton::Left);
        const auto start = Clock::now();
        driver.start(slint::TimerMode::Repeated, 2ms, [&, grab, start] {
            const auto before = Clock::now();
            const double t = std::chrono::duration<double>(before - start).count();
            if (bench == "drag" || bench == "pan") {
                const float r = 60;
                window.dispatch_pointer_move_event(slint::LogicalPosition(
                    {grab.x + r * static_cast<float>(std::cos(t * 3)) - r, grab.y + r * static_cast<float>(std::sin(t * 3))}));
            } else if (bench == "zoom") {
                window.dispatch_pointer_scroll_event(slint::LogicalPosition({width / 2, height / 2}), 0,
                                                     std::fmod(t, 2.0) < 1.0 ? 1.f : -1.f);
            } else if (bench == "scroll") {
                ui->invoke_scroll_events(std::fmod(t, 10.0) < 5.0 ? 37.f : -37.f);
            }
            busy += Clock::now() - before;
            ++ticks;
        });
        if (!bench.empty() || !snapshot_path.empty()) {
            slint::Timer::single_shot(std::chrono::milliseconds(static_cast<long>(bench.empty() ? 600 : seconds * 1000)), [&] {
                driver.stop();
                if (!snapshot_path.empty() && !save_snapshot(ui->window(), snapshot_path)) std::cerr << "snapshot failed\n";
                if (!bench.empty())
                    std::cout << "bench=" << bench << " paths=" << (per_link ? "per-link" : "single")
                              << " updates=" << ticks << " avg_update_us="
                              << (ticks ? std::chrono::duration<double, std::micro>(busy).count() / static_cast<double>(ticks) : 0)
                              << '\n';
                slint::quit_event_loop();
            });
        }
    });
    ui->run();
}
