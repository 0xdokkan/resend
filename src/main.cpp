#include "app_controller.hpp"
#include "core/text.hpp"

#include <slint-platform.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <cstdio>
#endif

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

#if defined(SLINT_FEATURE_RENDERER_SOFTWARE)
// A window that is only ever drawn into memory (--offscreen): it needs no
// display, and its size is exactly the one asked for, whatever a window
// manager would have made of it.
class OffscreenWindow : public slint::platform::WindowAdapter {
public:
    explicit OffscreenWindow(slint::PhysicalSize size) : size_(size) {}
    slint::platform::AbstractRenderer& renderer() override { return renderer_; }
    slint::PhysicalSize size() override { return size_; }
    void set_size(slint::PhysicalSize size) override { size_ = size; }
    // The size reaches the window once the event loop runs, as a window
    // manager's answer would, so whatever follows it sees it change.
    void settle() {
        window().dispatch_resize_event(
            slint::LogicalSize({static_cast<float>(size_.width), static_cast<float>(size_.height)}));
    }

private:
    slint::platform::SoftwareRenderer renderer_{slint::platform::SoftwareRenderer::RepaintBufferType::NewBuffer};
    slint::PhysicalSize size_;
};

// Timers and queued tasks, and nothing else: there is no input and no screen.
class OffscreenPlatform : public slint::platform::Platform {
public:
    explicit OffscreenPlatform(slint::PhysicalSize size) : size_(size) {}
    std::unique_ptr<slint::platform::WindowAdapter> create_window_adapter() override {
        auto window = std::make_unique<OffscreenWindow>(size_);
        window_ = window.get();  // Slint keeps it for as long as the program runs
        return window;
    }
    void run_event_loop() override {
        quit_ = false;
        bool settled = false;
        while (!quit_) {
            slint::platform::update_timers_and_animations();
            if (!settled && window_) {
                window_->settle();
                settled = true;
            }
            std::deque<Task> tasks;
            {
                const std::lock_guard lock(mutex_);
                tasks.swap(tasks_);
            }
            for (auto& task : tasks) std::move(task).run();
            if (quit_) break;
            const auto wait = slint::platform::duration_until_next_timer_update().value_or(5ms);
            std::this_thread::sleep_for(std::min<std::chrono::milliseconds>(wait, 5ms));
        }
    }
    void quit_event_loop() override { quit_ = true; }
    void run_in_event_loop(Task task) override {
        const std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
    }

private:
    slint::PhysicalSize size_;
    OffscreenWindow* window_ = nullptr;
    std::atomic<bool> quit_{false};
    std::mutex mutex_;
    std::deque<Task> tasks_;
};

bool draw_offscreen(slint::LogicalSize size) {
    slint::platform::set_platform(std::make_unique<OffscreenPlatform>(
        slint::PhysicalSize({static_cast<uint32_t>(size.width), static_cast<uint32_t>(size.height)})));
    return true;
}
#else
bool draw_offscreen(slint::LogicalSize) { return false; }
#endif

#if defined(__linux__)
// Slint 1.18.1 lists the system's fonts through fontique 0.11.1, which stops
// the program when fontconfig knows no font at all, as in a bare container,
// although resend draws only with the fonts built into it. So fontconfig is
// pointed at a configuration that adds the directory where those fonts are
// installed to the system's own: it then always knows at least these.
std::string xml_escaped(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else out += c;
    }
    return out;
}

void provide_fonts() {
    namespace fs = std::filesystem;
    if (std::getenv("FONTCONFIG_FILE") || std::getenv("FONTCONFIG_PATH")) return;
    std::vector<fs::path> candidates;
    std::error_code error;
    const auto program = fs::read_symlink("/proc/self/exe", error);
    if (!error) candidates.push_back(program.parent_path().parent_path() / "share" / "resend" / "fonts");
#if defined(RESEND_FONT_DIR)
    candidates.emplace_back(RESEND_FONT_DIR);  // a build run from its tree
#endif
    const auto found = std::find_if(candidates.begin(), candidates.end(),
                                    [](const fs::path& dir) { return fs::exists(dir / "Inter-Regular.ttf"); });
    if (found == candidates.end()) return;
    fs::path cache;
    if (const char* chosen = std::getenv("XDG_CACHE_HOME"); chosen && *chosen) cache = chosen;
    else if (const char* home = std::getenv("HOME"); home && *home) cache = fs::path(home) / ".cache";
    else return;
    cache /= "resend";
    fs::create_directories(cache, error);
    if (error) return;
    const auto config = "<?xml version=\"1.0\"?>\n<!DOCTYPE fontconfig SYSTEM \"urn:fontconfig:fonts.dtd\">\n<fontconfig>\n"
                        "  <include ignore_missing=\"yes\">/etc/fonts/fonts.conf</include>\n"
                        "  <dir>" + xml_escaped(found->string()) + "</dir>\n"
                        "  <cachedir>" + xml_escaped((cache / "fontconfig").string()) + "</cachedir>\n</fontconfig>\n";
    const auto path = cache / "fonts.conf";
    std::ostringstream current;
    if (std::ifstream existing(path); existing) current << existing.rdbuf();
    if (current.str() != config) {
        std::ofstream out(path, std::ios::trunc);
        out << config;
        if (!out) return;
    }
    setenv("FONTCONFIG_FILE", path.c_str(), 0);
}
#else
void provide_fonts() {}
#endif

#if defined(_WIN32)
// On Windows resend is a windowed program, so it has no console of its own.
// Started from one, it writes its messages there: --help, --snapshot, errors.
void attach_console() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    std::ios::sync_with_stdio();
}
#else
void attach_console() {}
#endif

// The name the desktop knows the program by: its desktop file, its icon and
// its AppStream metadata in packaging/linux/ carry the same.
constexpr const char* app_id = "io.github._0xdokkan.resend";

constexpr const char* usage =
    "resend - network sandbox (Slint / C++20)\n"
    "usage: resend [OPTIONS] [FILE]\n"
    "  --scenario FILE   Open a scenario or lab file instead of the sample LAN; FILE alone does the same\n"
    "  --check-goals     With a lab: check its goals once the network is open (after --ping)\n"
    "  --snapshot PATH   Save a rendered PPM preview, then exit\n"
    "  --ping IPV4       Start a ping from the selected host (the first one) at startup\n"
    "  --traceroute IPV4 Start a traceroute instead\n"
    "  --at TIME         Show this simulated instant, e.g. 15ms or 1.5s (with --ping)\n"
    "  --select NAME     Then select this device, e.g. to show its inspector\n"
    "  --terminal LINE   Open the selected device's terminal and run LINE in it; give it again for more lines\n"
    "  --click X,Y       With --snapshot: click there first, in logical pixels, e.g. to open a menu; again for more clicks\n"
    "  --point X,Y       With --snapshot: rest the pointer there, e.g. to show a button's tip\n"
    "  --theme ID        Use this theme (blueberry, bubblegum, picnic) for this session\n"
    "  --silent          Open no sound device in this session\n"
    "  --timeline        Open the timeline into its events\n"
    "  --size WxH        The window's size in logical pixels, e.g. 1360x900\n"
    "  --offscreen       With --snapshot: draw without a display, at exactly --size (1360x900 if not given)\n"
    "  --zoom PERCENT    Zoom the canvas to this, around the middle of the view, e.g. 150\n"
    "  --bench N         With --snapshot: render N frames first and print the time a frame takes\n";
}

int main(int argc, char** argv) {
    attach_console();
    provide_fonts();
    std::string snapshot_path, ping, scenario, select;
    std::vector<std::string> typed;
    std::vector<slint::LogicalPosition> clicks;
    std::optional<slint::LogicalPosition> pointed;
    bool traceroute = false, timeline = false, check_goals = false, silent = false, offscreen = false;
    std::optional<std::string> theme;
    std::optional<netsim::SimTime> at;
    std::optional<slint::LogicalSize> size;
    std::optional<float> zoom;
    int bench = 0;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        const bool has_value = index + 1 < argc;
        if (arg == "--snapshot" && has_value) {
            snapshot_path = argv[++index];
        } else if (arg == "--scenario" && has_value) {
            scenario = argv[++index];
        } else if ((arg == "--ping" || arg == "--traceroute") && has_value) {
            ping = argv[++index];
            traceroute = arg == "--traceroute";
        } else if (arg == "--theme" && has_value) {
            theme = argv[++index];
        } else if (arg == "--silent") {
            silent = true;
        } else if (arg == "--timeline") {
            timeline = true;
        } else if (arg == "--offscreen") {
            offscreen = true;
        } else if (arg == "--check-goals") {
            check_goals = true;
        } else if (arg == "--size" && has_value) {
            const std::string value = argv[++index];
            const auto x = value.find('x');
            try {
                size = slint::LogicalSize({std::stof(value.substr(0, x)), std::stof(value.substr(x + 1))});
            } catch (const std::exception&) {
                std::cerr << "Invalid size: " << value << " (example: 1360x900)\n";
                return 2;
            }
        } else if (arg == "--bench" && has_value) {
            bench = std::max(0, std::atoi(argv[++index]));
        } else if (arg == "--zoom" && has_value) {
            try {
                zoom = std::stof(argv[++index]) / 100;
            } catch (const std::exception&) {
                std::cerr << "Invalid zoom: " << argv[index] << " (example: 150)\n";
                return 2;
            }
        } else if (arg == "--select" && has_value) {
            select = argv[++index];
        } else if (arg == "--terminal" && has_value) {
            typed.emplace_back(argv[++index]);
        } else if ((arg == "--click" || arg == "--point") && has_value) {
            const std::string value = argv[++index];
            const auto comma = value.find(',');
            try {
                const slint::LogicalPosition at({std::stof(value.substr(0, comma)), std::stof(value.substr(comma + 1))});
                if (arg == "--click") clicks.push_back(at);
                else pointed = at;
            } catch (const std::exception&) {
                std::cerr << "Invalid position: " << value << " (example: 640,400)\n";
                return 2;
            }
        } else if (arg == "--at" && has_value) {
            at = netsim::parse_duration(argv[++index]);
            if (!at) {
                std::cerr << "Invalid time for --at: " << argv[index] << " (examples: 15ms, 1.5s)\n";
                return 2;
            }
        } else if (arg == "--help") {
            std::cout << usage;
            return 0;
        } else if (!arg.starts_with("-") && scenario.empty()) {
            scenario = arg;  // as the desktop opens a file with the program
        } else {
            std::cerr << "Unknown option: " << arg << '\n' << usage;
            return 2;
        }
    }

    if (offscreen) {
        if (snapshot_path.empty()) {
            std::cerr << "--offscreen draws a snapshot: give --snapshot PATH too.\n";
            return 2;
        }
        if (!size) size = slint::LogicalSize({1360, 900});
        if (!draw_offscreen(*size)) {
            std::cerr << "This build of Slint has no software renderer to draw off-screen with.\n";
            return 2;
        }
    }
    slint::set_xdg_app_id(app_id);
    auto ui = AppWindow::create();
    resend::AppController controller(ui, {resend::state_directory(), snapshot_path.empty(), theme, snapshot_path.empty() && !silent});
    if (timeline) ui->global<UiApp>().set_timeline_open(true);
    if (size) ui->window().set_size(*size);
    if (!scenario.empty()) {
        std::string error;
        const auto path = resend::path_from_text(scenario);
        const bool readable = resend::is_lab_file(path) ? resend::read_lab_file(path, error).has_value()
                                                        : resend::read_scenario_file(path, error).has_value();
        if (!readable) {
            std::cerr << scenario << ": " << error << '\n';
            return 2;
        }
        controller.open_file(path);
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
    if (check_goals) ui->global<UiLab>().invoke_check();
    if (!select.empty() && !controller.select_device(select)) {
        std::cerr << "No device named " << select << '\n';
        return 2;
    }
    if (!typed.empty()) {
        const auto& terminal = ui->global<UiTerminal>();
        terminal.invoke_open();
        for (const auto& line : typed)
            if (!line.empty()) terminal.invoke_run(line.c_str());
    }
    // Once the window has its size and the network is framed.
    if (zoom)
        slint::Timer::single_shot(200ms, [&] { ui->global<UiCanvas>().invoke_zoom_by(*zoom / controller.canvas().zoom()); });
    int exit_code = 0;
    if (!snapshot_path.empty()) {
        // The pointer acts on the window as laid out, and a tip shows once
        // the pointer has rested: the snapshot waits for both.
        if (!clicks.empty() || pointed)
            slint::Timer::single_shot(300ms, [&] {
                for (const auto& at : clicks) {
                    ui->window().dispatch_pointer_move_event(at);
                    ui->window().dispatch_pointer_press_event(at, slint::PointerEventButton::Left);
                    ui->window().dispatch_pointer_release_event(at, slint::PointerEventButton::Left);
                }
                if (pointed) ui->window().dispatch_pointer_move_event(*pointed);
            });
        slint::Timer::single_shot(!clicks.empty() || pointed ? 1100ms : 550ms, [&] {
            // Each snapshot draws the whole window with the software renderer.
            if (bench > 0) {
                const auto start = std::chrono::steady_clock::now();
                for (int frame = 0; frame < bench; ++frame) (void)ui->window().take_snapshot();
                const std::chrono::duration<double, std::milli> spent = std::chrono::steady_clock::now() - start;
                std::cout << "software frame: " << spent.count() / bench << " ms (" << bench << " frames)\n";
            }
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
