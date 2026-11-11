// The command palette (Ctrl+K): every action of the menus, with its key,
// found by typing part of its name, and the actions of the context menu and
// of the first run, run by name.
#include "app_controller.hpp"

#include "core/text.hpp"
#include "look/look.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace resend {
namespace {
using netsim::text::tr;

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Speeds of the Simulation ▸ Speed menu, as it names them.
struct Speed {
    const char* id;
    const char* title;
    float value;
};
constexpr Speed speeds[] = {
    {"speed:0.25", "0.25×", 0.25f}, {"speed:0.5", "0.5×", 0.5f}, {"speed:1", "1×", 1}, {"speed:2", "2×", 2}, {"speed:4", "4×", 4},
};
} // namespace

void AppController::bind_commands() {
    const auto& app = ui_->global<UiApp>();
    app.on_set_theme([this](slint::SharedString id) { set_theme(std::string(id)); });
    app.on_run_command([this](slint::SharedString id) { run_command(std::string(id)); });
    app.on_filter_commands([this](slint::SharedString) { refresh_commands(); });
    app.on_show_commands([this] {
        const auto& ui = ui_->global<UiApp>();
        ui.set_command_filter("");
        refresh_commands();
        ui.set_dialog("commands");
    });
    app.on_show_keys([this] { ui_->global<UiApp>().set_dialog("keys"); });
}

// Every action, in the order of the menus, filtered by what is typed; the
// palette lists each one under the name its menu item has.
void AppController::refresh_commands() {
    const auto& app = ui_->global<UiApp>();
    const auto& run = ui_->global<UiRun>();
    std::vector<CommandView> all;
    const auto add = [&](std::string id, std::string title, const std::string& group, std::string keys = {}, bool enabled = true) {
        all.push_back({id.c_str(), title.c_str(), group.c_str(), keys.c_str(), enabled});
    };
    const auto file = tr("File"), edit = tr("Edit"), view = tr("View"), simulation = tr("Simulation"), help = tr("Help");
    add("new", tr("New"), file, "Ctrl+N");
    add("open", tr("Open…"), file, "Ctrl+O");
    const auto examples = app.get_examples();
    for (std::size_t i = 0; examples && i < examples->row_count(); ++i)
        add("example:" + std::to_string(i), std::string(examples->row_data(i)->title), tr("Open example"));
    add("labs", tr("Labs…"), file, ui_->global<UiLab>().get_active() ? "" : "G");
    add("save", tr("Save"), file, "Ctrl+S");
    add("save-as", tr("Save as…"), file, "Ctrl+Shift+S");
    add("quit", tr("Quit"), file, "Ctrl+Q");
    add("undo", std::string(app.get_undo_text()), edit, "Ctrl+Z", app.get_can_undo());
    add("redo", std::string(app.get_redo_text()), edit, "Ctrl+Y", app.get_can_redo());
    add("add-host", tr("Add host"), edit, "1");
    add("add-switch", tr("Add switch"), edit, "2");
    add("add-router", tr("Add router"), edit, "3");
    add("delete", tr("Delete selection"), edit, tr("Delete"));
    add("commands", tr("Command palette"), edit, "Ctrl+K");
    add("fit", tr("Fit the network in view"), view, "0");
    add("zoom-in", tr("Zoom in"), view, "+");
    add("zoom-out", tr("Zoom out"), view, "−");
    add("actual-size", tr("Actual size"), view);
    add("palette", tr("Palette"), view, "B");
    add("inspector", tr("Inspector"), view, "I");
    add("timeline", tr("Timeline"), view, "L");
    add("lab-panel", tr("Lab panel"), view, "G", ui_->global<UiLab>().get_active());
    add("hud", tr("Hide the HUD"), view, "H");
    for (const auto intensity : {Intensity::Full, Intensity::Reduced, Intensity::Off})
        add("effects:" + std::string(name(intensity)),
            intensity == Intensity::Full ? tr("Full") : intensity == Intensity::Reduced ? tr("Reduced") : tr("Off"), tr("Effects"), "E");
    add("sound:on", tr("Sound on"), tr("Sound"), settings_.sound ? "" : "M");
    add("sound:off", tr("Sound off"), tr("Sound"), settings_.sound ? "M" : "");
    add("sound:quiet", tr("Quiet"), tr("Sound"));
    add("sound:medium", tr("Medium"), tr("Sound"));
    add("sound:loud", tr("Loud"), tr("Sound"));
    for (const auto& theme : look::palettes().themes) add("theme:" + theme.id, theme.title, tr("Theme"));
    add("run", run.get_running() ? tr("Pause") : tr("Run"), simulation, tr("Space"), run.get_pending());
    add("step", tr("Step"), simulation, ".", run.get_pending() && !run.get_running());
    add("latest", tr("Go to the latest instant"), simulation, tr("End"));
    add("reset", tr("Reset"), simulation, "R");
    for (const auto& speed : speeds) add(speed.id, speed.title, tr("Speed"), "]");
    add("keys", tr("Keyboard shortcuts"), help, "?");
    add("about", tr("About resend"), help);

    const auto filter = lower(std::string(app.get_command_filter()));
    std::vector<CommandView> shown;
    for (const auto& command : all)
        if (filter.empty() || lower(std::string(command.title)).find(filter) != std::string::npos ||
            lower(std::string(command.group)).find(filter) != std::string::npos)
            shown.push_back(command);
    commands_->set_vector(shown);
}

void AppController::run_command(const std::string& id) {
    const auto& app = ui_->global<UiApp>();
    const auto& run = ui_->global<UiRun>();
    const auto& canvas = ui_->global<UiCanvas>();
    // A command chosen in the palette closes it first; one from a menu or
    // from the canvas has no dialog to close.
    if (app.get_dialog() == "commands") close_dialog();
    const auto starts = [&](std::string_view prefix) { return id.rfind(prefix, 0) == 0; };
    if (id == "new") app.invoke_new_document();
    else if (id == "open") app.invoke_open_document();
    else if (starts("example:")) app.invoke_open_example(std::stoi(id.substr(8)));
    else if (id == "labs") ui_->global<UiLab>().invoke_show_library();
    else if (id == "save") app.invoke_save();
    else if (id == "save-as") app.invoke_save_as();
    else if (id == "quit") app.invoke_quit();
    else if (id == "undo") app.invoke_undo();
    else if (id == "redo") app.invoke_redo();
    else if (id == "add-host") app.invoke_add_device("host");
    else if (id == "add-switch") app.invoke_add_device("switch");
    else if (id == "add-router") app.invoke_add_device("router");
    else if (id == "delete") app.invoke_delete_selection();
    else if (id == "commands") app.invoke_show_commands();
    else if (id == "fit") canvas.invoke_fit();
    else if (id == "zoom-in") canvas.invoke_zoom_by(1.15f);
    else if (id == "zoom-out") canvas.invoke_zoom_by(1 / 1.15f);
    else if (id == "actual-size") canvas.invoke_reset_zoom();
    else if (id == "palette") app.set_palette_open(!app.get_palette_open());
    else if (id == "inspector") app.set_inspector_open(!app.get_inspector_open());
    else if (id == "timeline") app.set_timeline_open(!app.get_timeline_open());
    else if (id == "lab-panel") {
        const auto& lab = ui_->global<UiLab>();
        if (lab.get_active()) lab.set_open(!lab.get_open());
    }
    else if (id == "hud") app.set_hud_hidden(!app.get_hud_hidden());
    else if (starts("effects:")) app.invoke_set_effects(id.substr(8).c_str());
    else if (starts("sound:")) ui_->global<UiSound>().invoke_set_sound(id.substr(6).c_str());
    else if (starts("theme:")) set_theme(id.substr(6));
    else if (id == "run") run.invoke_run_toggle();
    else if (id == "step") run.invoke_step();
    else if (id == "latest") run.invoke_latest();
    else if (id == "reset") run.invoke_reset();
    else if (starts("speed:")) {
        for (const auto& speed : speeds)
            if (id == speed.id) run.invoke_set_speed(speed.value);
    } else if (id == "keys") app.invoke_show_keys();
    else if (id == "about") app.invoke_about();
    // The context menu of the canvas acts on the selection.
    else if (id == "power") ui_->global<UiInspector>().invoke_toggle_power();
    else if (id == "toggle-cable") ui_->global<UiInspector>().invoke_toggle_cable();
    else if (id == "ping-from-here") {
        if (const auto* device = selected_device())
            status(tr("{0} sends the next ping: type its destination and press Send.", {device->name}));
    }
    else if (id == "ping-to-here") {
        // The device right-clicked becomes the destination; the source stays
        // the host selected before it.
        const auto* target = selected_device();
        if (!target) return;
        std::optional<netsim::Ipv4Address> address;
        for (const auto& port : target->ports)
            if (port.ipv4) {
                address = port.ipv4->address();
                break;
            }
        if (!address) {
            status(tr("{0} has no IPv4 address to ping.", {target->name}));
            return;
        }
        const auto source_id = last_host_ == target->id.value ? previous_host_ : last_host_;
        const auto* source = scenario().device(netsim::DeviceId{source_id});
        if (!source || source->id == target->id) {
            status(tr("Select the host to ping from first."));
            return;
        }
        ui_->global<UiRun>().set_target(address->to_string().c_str());
        canvas_changed(canvas_.select({Selection::Kind::Device, source->id.value}));
        status(tr("{0} will ping {1} ({2}): press Send.", {source->name, target->name, address->to_string()}));
    }
    // The first run.
    else if (starts("welcome-")) {
        settings_.welcomed = true;
        save_settings();
        close_dialog();
        if (id == "welcome-ping") status(tr("The sample network is ready: press Send, then Run."));
        else if (id == "welcome-lab") open_lab(std::size_t{0});
        else if (id == "welcome-empty") app.invoke_new_document();
        else if (id == "welcome-examples") {
            app.set_command_filter(tr("Open example").c_str());
            refresh_commands();
            app.set_dialog("commands");
        }
    }
}

} // namespace resend
