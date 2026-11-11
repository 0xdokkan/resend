#pragma once

#include "app.h"
#include "app/canvas.hpp"
#include "app/cues.hpp"
#include "app/files.hpp"
#include "app/playback.hpp"
#include "app/scene.hpp"
#include "app/timeline.hpp"
#include "app/workspace.hpp"
#include "audio/output.hpp"
#include "audio/synth.hpp"
#include "explain/explain.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
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
        // Offers the first-run choice if it was never made (the app does,
        // the tests do not).
        bool welcome = false;
        // A theme for this session only, instead of the one chosen.
        std::optional<std::string> theme;
        // Plays through the sound card (the app does; the tests and the
        // snapshots use the null output, and log what would be heard).
        bool audio = false;
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
    // The sounds played, oldest first, the last few hundred (G5), and how
    // many there were in all.
    const std::vector<Play>& played() const { return played_; }
    std::uint64_t played_count() const { return played_count_; }
    const audio::Output& sound_output() const { return *output_; }

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
    // A theme of assets/palettes.json; kept in the settings.
    void set_theme(const std::string& id);
    // Runs an action of the command palette, as its menu item or key would.
    void run_command(const std::string& id);
    // Opens a lab of the library, or one read from a file: its network, with
    // its locks and goals. Leaves the network in view unasked: callers ask
    // about unsaved changes first.
    bool open_lab(std::size_t index);
    void open_lab(netsim::lab::Lab lab);
    // The lab being done, and the verdicts of its last check.
    const netsim::lab::Lab* lab() const { return lab_.get(); }
    const std::vector<netsim::lab::Verdict>& verdicts() const { return verdicts_; }
    // The labs built in, in the order of the library.
    const std::vector<netsim::lab::Lab>& library() const { return library_; }
    // The names of the sprites the window has a property for.
    static std::span<const std::string_view> sprite_properties();
    // Every sprite painted into UiSprites, as a property or in an array.
    static std::vector<std::string_view> painted_sprites();

private:
    // Canvas, playback and timeline (app_controller.cpp).
    void bind_callbacks();
    // A message for the learner: what an action did, or why it did nothing.
    // It becomes the status and is shown as a toast.
    void status(const std::string& value);
    // The same, for what the screen already shows (an edit applied, a file
    // opened): the status only, with no toast.
    void note(const std::string& value);
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
    // Runs a playback command and sounds the records it adds. `real` is
    // the real time it stands for: a frame's, or a separate moment.
    template<class Command>
    void playback_command(Command command, std::chrono::nanoseconds real = CueMixer::merge_window);
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

    // The look (app_look.cpp) and the command palette (app_commands.cpp).
    void apply_theme();
    void refresh_units();
    void paint_grid();
    void refresh_dashes();
    void bind_commands();
    void refresh_commands();
    void set_effects(Intensity intensity);
    // The device terminal (app_terminal.cpp, M8).
    void bind_terminal();
    void run_terminal(const std::string& line);
    // `all`: the prompt and the menus too, not only the screen.
    void refresh_terminal(bool all);
    // A command typed on `device`, for the lab's command goals.
    void note_command(const std::string& device, const std::string& line);

    // Sound (G5): on or off, a level, and what plays.
    void bind_sound();
    void apply_sound();  // the settings' sound, to the synthesizer and the HUD
    void set_sound(bool on);
    void set_volume(const std::string& level);
    void play(const std::vector<Play>& plays);
    void play(CueKind kind);
    // A short answer to an action, shown for a few seconds.
    void toast(const std::string& text);
    // A value the inspector refused: said in the status and under its field.
    void refuse(const std::string& field, const std::string& text);

    // Labs (app_lab.cpp).
    void bind_lab();
    void refresh_lab();
    void refresh_library();
    void check_lab();
    void celebrate();
    // Plays again what decided the lab: the first probe goal's ping, or the
    // learner's own run from its start.
    void replay_lab();
    // Back to the sandbox: no lab, no locks.
    void leave_lab();
    // The solution in view, the learner's network kept aside; and back.
    void show_solution();
    void back_to_mine();
    // After the workspace was replaced: the views start over on its network.
    void show_workspace();

    // Files, dialogs, examples, settings (app_documents.cpp).
    void bind_documents();
    struct Answer {
        std::string id, text;
        std::function<void()> action;
    };
    void ask(const std::string& title, const std::string& message, std::vector<Answer> answers);
    void close_dialog();
    // Escape in a dialog: its Cancel or Close.
    void dismiss_dialog();
    // After a dialog closed: the keyboard is the canvas's again, unless
    // another dialog opened in its place.
    void dialog_left();
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
    // The settings on the window, and every text made in C++ made again.
    void apply_settings();
    void save_settings();
    void refresh_examples();

    slint::ComponentHandle<AppWindow> ui_;
    std::filesystem::path state_directory_;
    Settings settings_;
    std::optional<std::string> session_theme_;
    Workspace workspace_;
    CanvasController canvas_{workspace_};
    PlaybackController playback_{workspace_};
    netsim::explain::Explainer explainer_;
    std::uint64_t generation_ = 0;

    std::shared_ptr<slint::VectorModel<NodeView>> nodes_;
    std::shared_ptr<slint::VectorModel<LinkView>> links_;
    std::shared_ptr<slint::VectorModel<PacketView>> packets_;
    std::shared_ptr<slint::VectorModel<MarkerView>> markers_;
    std::shared_ptr<slint::VectorModel<ShapeView>> shapes_;
    std::shared_ptr<slint::VectorModel<EventView>> events_;
    std::shared_ptr<slint::VectorModel<FieldRow>> fields_;
    std::shared_ptr<slint::VectorModel<HexRow>> hex_;
    std::shared_ptr<slint::VectorModel<PortView>> ports_;
    std::shared_ptr<slint::VectorModel<RouteView>> routes_;
    std::shared_ptr<slint::VectorModel<std::shared_ptr<slint::Model<slint::StandardListViewItem>>>> table_;
    std::shared_ptr<slint::VectorModel<std::shared_ptr<slint::Model<slint::StandardListViewItem>>>> routing_;
    slint::Timer frame_timer_;
    slint::Timer autosave_timer_;
    slint::Timer units_timer_;
    slint::Timer geometry_timer_;
    float shown_scale_ = 0, shown_zoom_ = 0;
    int toast_serial_ = 0;
    // The hosts selected last and before it: a right click on the device to
    // ping selects that one, and the ping still starts from the host before.
    std::uint32_t last_host_ = 0, previous_host_ = 0;
    std::shared_ptr<slint::VectorModel<CommandView>> commands_;
    std::shared_ptr<slint::VectorModel<TableColumn>> table_columns_;
    std::shared_ptr<slint::VectorModel<TableColumn>> routing_columns_;
    std::chrono::steady_clock::time_point last_frame_;

    // Packets drawn, in canvas units, to find one under the pointer.
    std::vector<PacketSprite> shown_packets_;
    // Port activity per row of nodes_, to update only rows that change.
    std::vector<unsigned> shown_activity_;
    // How much the canvas moves, as the settings say.
    Intensity intensity_ = Intensity::Full;
    // What plays: the output goes before the synthesizer it reads.
    audio::Synth synth_;
    std::unique_ptr<audio::Output> output_;
    CueEngine cues_;
    CueMixer mixer_;
    std::chrono::nanoseconds cue_clock_{};
    std::chrono::steady_clock::time_point last_click_{};
    std::vector<Play> played_;
    std::uint64_t played_count_ = 0;

    // A terminal's line and what it printed; a ping or a traceroute it
    // started prints as the run goes, found again by its id and start.
    struct TerminalEntry {
        std::string prompt;  // as it was when the line was typed
        std::string line;
        std::string output;
        bool error = false;   // the output says why the command was not run
        std::string refusal;  // why a change or a ping it asked for was not made
        std::optional<netsim::PingId> ping;
        netsim::DeviceId source{};
        netsim::Ipv4Address destination{};
        netsim::SimTime started{};
        bool default_count = false;
    };
    struct TerminalSession {
        std::vector<TerminalEntry> entries;
        std::vector<std::string> history;
        std::size_t browsing = 0;  // in the history; its size when typing a new line
        std::string draft;         // the line being typed when browsing began
    };
    std::map<std::uint32_t, TerminalSession> terminals_;  // by device id
    std::string shown_screen_;
    std::shared_ptr<slint::VectorModel<TerminalMenuView>> terminal_menus_;
    std::shared_ptr<slint::VectorModel<TerminalBlockView>> terminal_blocks_;
    EventFilter filter_;
    std::vector<std::size_t> shown_events_;  // event indexes, as listed
    std::size_t listed_ = 0;                 // events the filter has seen
    std::vector<std::vector<std::string>> shown_table_;
    // A router's port addresses as typed and not yet applied, by port.
    std::vector<std::optional<std::string>> typed_addresses_;
    int selected_event_ = -1;
    int selected_pdu_ = -1;
    bool follow_events_ = true;

    // The lab being done, which the workspace's locks point to.
    std::unique_ptr<netsim::lab::Lab> lab_;
    std::vector<netsim::lab::Lab> library_;
    std::vector<netsim::lab::Verdict> verdicts_;
    // The state of the history the verdicts are for: a change makes them old.
    std::optional<std::uint64_t> checked_state_;
    netsim::lab::Answers lab_answers_;
    std::size_t hints_shown_ = 0;
    // The goals the last check found met that the one before had not: they
    // glow for a moment (G4.3).
    std::vector<bool> fresh_goals_;
    slint::Timer fresh_timer_;
    // The moment a lab is solved: its progress from 0 to 1, driven by a
    // timer while it plays, then nothing is redrawn.
    std::chrono::steady_clock::time_point celebration_start_;
    slint::Timer celebration_timer_;
    // The learner's workspace while the solution is in view.
    std::unique_ptr<Workspace> parked_;
    std::shared_ptr<slint::VectorModel<GoalView>> goals_;

    std::optional<std::filesystem::path> document_;
    std::string document_title_;  // the name of an untitled network, e.g. an example's
    Recovery recovery_;
    std::optional<std::uint64_t> autosaved_;  // history state last autosaved
    std::optional<FileBrowser> browser_;
    bool saving_ = false;
    bool ready_ = false;  // constructed: what is said from now on answers the learner
    std::vector<Answer> answers_;
    std::function<void()> after_save_;
};

} // namespace resend
