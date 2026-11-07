#pragma once

#include "app.h"
#include "app/canvas.hpp"
#include "app/files.hpp"
#include "app/playback.hpp"
#include "app/timeline.hpp"
#include "app/workspace.hpp"
#include "explain/explain.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace resend {

// Connects the workspace, the canvas and the playback to the Slint window:
// callbacks in, models out. The window's globals (ui/state.slint and the
// panels' files) are its only contract with the interface. The callbacks
// capture `this`, so the controller must stay in place.
//
// The implementation is split by area: the canvas, the playback and the
// timeline here, the inspector in app_inspector.cpp, files, dialogs and
// settings in app_documents.cpp.
class AppController {
public:
    struct Options {
        // Where the settings and the autosave files are kept.
        std::filesystem::path state_directory;
        // A language for this session only, instead of the one chosen.
        std::optional<std::string> language;
    };
    AppController(slint::ComponentHandle<AppWindow> ui, Options options);
    ~AppController();
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    const Workspace& workspace() const { return workspace_; }
    const netsim::Scenario& scenario() const { return workspace_.scenario(); }
    const netsim::Simulation& simulation() const { return workspace_.simulation(); }
    const netsim::explain::Explainer& explainer() const { return explainer_; }
    const PlaybackController& playback() const { return playback_; }
    const CanvasController& canvas() const { return canvas_; }
    const std::optional<std::filesystem::path>& document() const { return document_; }
    const Settings& settings() const { return settings_; }

    // Another network: a new simulation, no history. `file` is where it came from.
    void open(netsim::Scenario scenario, std::optional<std::filesystem::path> file = std::nullopt, std::string name = {});
    // Opens a scenario file; says why not in the status bar.
    bool open_file(const std::filesystem::path& path);
    // Moves the animation forward. The frame timer passes the real time
    // elapsed; tests and snapshots pass a chosen one.
    void advance_playback(std::chrono::nanoseconds elapsed);
    // Shows a simulated instant at once, paused, earlier or later than now.
    void seek(netsim::SimTime time);
    // Selects the device with this name. False if there is none.
    bool select_device(std::string_view name);
    // Offers back a network an earlier session left unsaved. True if it asked.
    bool offer_recovery();
    // Writes the autosave file if the network has unsaved changes; the
    // timer calls it every few seconds.
    void autosave();
    // Whether the window may close now. If the network is not saved, asks
    // first and closes later.
    bool close_requested();
    // A language code, or "" to follow the system; kept in the settings.
    void set_language(const std::string& code);

private:
    // Canvas, playback and timeline (app_controller.cpp).
    void bind_callbacks();
    void status(const std::string& value);
    // After the workspace changed: a new simulation starts the events and
    // the playback over; then the events are brought up to date.
    void sync();
    void refresh_all();
    // After the network changed: an edit, an undo, a cable plugged.
    void network_changed();
    void refresh_view();
    void refresh_topology();
    void refresh_scene();
    void refresh_overlay();
    void refresh_events();
    void refresh_explanation();
    void refresh_frame();
    void refresh_run();
    void refresh_document();
    // Applies what a canvas input changed and shows its status.
    void canvas_changed(unsigned change);
    template<class Command>
    void playback_command(Command command);
    void sync_playback();
    void select_event(int index);
    void select_frame(int pdu);
    // A row of the frame's fields; its bytes are highlighted.
    void select_field(int row);
    void travel(netsim::SimTime time);
    void start_ping();
    void undo();
    void redo();
    void add_device(netsim::DeviceKind kind);
    // The status after a change: refused, or done, and when if the network was running.
    bool report(const Outcome& outcome, const std::string& label, std::size_t commands_before);
    std::string past_text() const;

    // The inspector (app_inspector.cpp).
    void bind_inspector();
    void refresh_inspector(bool fields);
    void refresh_tables();
    void apply_device();
    void apply_cable();
    void set_port_count(int count);
    void add_route();
    void remove_route(int index);
    // The selected device, if a device is selected.
    const netsim::DeviceConfig* selected_device() const;
    bool change(const std::vector<netsim::Edit>& edits, const std::string& label);

    // Files, dialogs, examples, settings (app_documents.cpp).
    void bind_documents();
    struct Answer {
        std::string id, text;
        std::function<void()> action;
    };
    void ask(const std::string& title, const std::string& message, std::vector<Answer> answers);
    void close_dialog();
    // Runs `action` once the network is saved or the user agreed to lose it.
    void unless_unsaved(std::function<void()> action);
    void save(std::function<void()> then = {});
    void show_file_dialog(bool saving);
    void browse_refresh();
    void confirm_save_as();
    bool write(const std::filesystem::path& path);
    void open_example(int index);
    void quit();
    std::string document_name() const;
    void apply_language();
    void save_settings();
    void refresh_examples();

    slint::ComponentHandle<AppWindow> ui_;
    std::filesystem::path state_directory_;
    Settings settings_;
    std::optional<std::string> session_language_;
    Workspace workspace_;
    CanvasController canvas_{workspace_};
    PlaybackController playback_{workspace_};
    netsim::explain::Explainer explainer_;
    std::uint64_t generation_ = 0;

    std::shared_ptr<slint::VectorModel<NodeView>> nodes_;
    std::shared_ptr<slint::VectorModel<LinkView>> links_;
    std::shared_ptr<slint::VectorModel<PacketView>> packets_;
    std::shared_ptr<slint::VectorModel<MarkerView>> markers_;
    std::shared_ptr<slint::VectorModel<EventView>> events_;
    std::shared_ptr<slint::VectorModel<FieldRow>> fields_;
    std::shared_ptr<slint::VectorModel<HexRow>> hex_;
    std::shared_ptr<slint::VectorModel<PortView>> ports_;
    std::shared_ptr<slint::VectorModel<RouteView>> routes_;
    std::shared_ptr<slint::VectorModel<std::shared_ptr<slint::Model<slint::StandardListViewItem>>>> table_;
    std::shared_ptr<slint::VectorModel<std::shared_ptr<slint::Model<slint::StandardListViewItem>>>> routing_;
    slint::Timer frame_timer_;
    slint::Timer autosave_timer_;
    std::chrono::steady_clock::time_point last_frame_;

    // Packets drawn, in canvas units, to find one under the pointer.
    std::vector<PacketSprite> shown_packets_;
    // Port activity per row of nodes_, to update only rows that change.
    std::vector<unsigned> shown_activity_;
    EventFilter filter_;
    std::vector<std::size_t> shown_events_;  // event indexes, as listed
    std::size_t listed_ = 0;                 // events the filter has seen
    std::vector<std::vector<std::string>> shown_table_;
    // A router's port addresses as typed and not yet applied, by port.
    std::vector<std::optional<std::string>> typed_addresses_;
    int selected_event_ = -1;
    int selected_pdu_ = -1;
    bool follow_events_ = true;

    std::optional<std::filesystem::path> document_;
    std::string document_title_;  // the name of an untitled network, e.g. an example's
    Recovery recovery_;
    std::optional<std::uint64_t> autosaved_;  // history state last autosaved
    std::optional<FileBrowser> browser_;
    bool saving_ = false;
    std::vector<Answer> answers_;
    std::function<void()> after_save_;
};

} // namespace resend
