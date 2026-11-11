// The device terminal (M8): a session for each device, what is typed run
// with src/term/, changes made as the inspector makes them, and the output
// of pings and traceroutes printed as the simulation plays them.

#include "app_controller.hpp"

#include "core/text.hpp"
#include "explain/explain.hpp"
#include "term/terminal.hpp"

#include <algorithm>

namespace resend {
namespace {
using netsim::text::tr;

// What a session keeps on screen and in its history.
constexpr std::size_t kept_entries = 200;
constexpr std::size_t kept_history = 100;

// What a command printed, as blocks of one kind each: a line that starts
// with "# " is a remark of resend's, the others are `kind`.
void print(std::vector<TerminalBlockView>& blocks, std::string_view text, const char* kind) {
    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        const char* of = line.starts_with("# ") ? "note" : kind;
        if (!blocks.empty() && blocks.back().kind == of) {
            blocks.back().text = slint::SharedString(std::string(blocks.back().text) + '\n' + std::string(line));
        } else {
            TerminalBlockView block{};
            block.kind = of;
            block.text = slint::SharedString(std::string(line));
            blocks.push_back(std::move(block));
        }
    }
}

} // namespace

void AppController::bind_terminal() {
    const auto& terminal = ui_->global<UiTerminal>();
    terminal.on_run([this](const slint::SharedString& line) { run_terminal(std::string(line)); });
    terminal.on_complete([this](const slint::SharedString& line) {
        const auto* device = selected_device();
        if (!device) return;
        const auto completion = netsim::term::complete(std::string(line), device->id, scenario());
        const auto& ui = ui_->global<UiTerminal>();
        ui.set_line(slint::SharedString(completion.line));
        std::string candidates;
        for (const auto& candidate : completion.candidates) candidates += (candidates.empty() ? "" : "   ") + candidate;
        ui.set_candidates(slint::SharedString(candidates));
    });
    terminal.on_history([this](int step) {
        const auto* device = selected_device();
        if (!device) return;
        auto& session = terminals_[device->id.value];
        const auto& ui = ui_->global<UiTerminal>();
        if (session.browsing > session.history.size()) session.browsing = session.history.size();
        if (step < 0) {
            if (session.browsing == session.history.size()) session.draft = std::string(ui.get_line());
            if (session.browsing == 0) return;
            --session.browsing;
        } else {
            if (session.browsing >= session.history.size()) return;
            ++session.browsing;
        }
        ui.set_line(slint::SharedString(session.browsing == session.history.size() ? session.draft : session.history[session.browsing]));
    });
    terminal.on_choose([this](const slint::SharedString& command, bool complete) {
        const std::string text(command);
        const auto& ui = ui_->global<UiTerminal>();
        if (complete) {
            run_terminal(text);
            return;
        }
        // The first blank to fill, in capitals, is selected for typing over.
        ui.set_line(slint::SharedString(text));
        const auto start = std::find_if(text.begin(), text.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
        const auto end = std::find_if(start, text.end(), [](char c) { return !(c >= 'A' && c <= 'Z'); });
        ui.set_blank_start(static_cast<int>(start - text.begin()));
        ui.set_blank_end(static_cast<int>(end - text.begin()));
        ui.set_candidates(slint::SharedString(tr("Type over {0}, then press Enter.", {std::string(start, end)})));
    });
    terminal.on_clear([this] {
        const auto* device = selected_device();
        if (!device) return;
        terminals_[device->id.value].entries.clear();
        ui_->global<UiTerminal>().set_candidates("");
        refresh_terminal(false);
    });
    terminal.on_open([this] {
        const auto* device = selected_device();
        if (!device) {
            status(tr("Select a device first: its terminal opens in the inspector."));
            return;
        }
        auto& app = ui_->global<UiApp>();
        app.set_hud_hidden(false);
        app.set_inspector_open(true);
        ui_->global<UiInspector>().set_tab(2);
        refresh_terminal(true);
        const auto& ui = ui_->global<UiTerminal>();
        ui.set_focus_serial(ui.get_focus_serial() + 1);
    });
}

void AppController::run_terminal(const std::string& line) {
    const auto* device = selected_device();
    const auto& ui = ui_->global<UiTerminal>();
    if (!device) return;
    const auto id = device->id;
    const auto name = device->name;
    auto& session = terminals_[id.value];
    const auto words = netsim::term::words(line);
    if (!words.empty() && (session.history.empty() || session.history.back() != line)) session.history.push_back(line);
    if (session.history.size() > kept_history) session.history.erase(session.history.begin());
    session.browsing = session.history.size();
    session.draft.clear();
    ui.set_line("");
    ui.set_candidates("");

    TerminalEntry entry;
    entry.prompt = netsim::term::prompt(*device);
    entry.line = line;
    const auto result = netsim::term::run(line, id, scenario(), &simulation());
    if (result.clear) {
        session.entries.clear();
        refresh_terminal(false);
        return;
    }
    entry.output = result.output;
    entry.error = result.error;
    if (!result.edits.empty()) {
        // As the inspector: one step to undo, refused with the same words.
        if (!change(result.edits, result.label)) entry.refusal = std::string(ui_->global<UiApp>().get_status()) + '\n';
    }
    if (result.start) {
        const auto outcome = workspace_.ping(id, result.start->destination, result.start->options);
        if (outcome.in_past) {
            entry.refusal = past_text() + '\n';
            status(past_text());
        } else if (!outcome.ok()) {
            entry.refusal = words.front() + ": " + netsim::explain::describe(outcome.problem) + '\n';
            status(netsim::explain::describe(outcome.problem));
        } else {
            const auto& pings = simulation().pings();
            entry.ping = pings.back().id;
            entry.source = id;
            entry.destination = result.start->destination;
            entry.started = pings.back().started;
            entry.default_count = result.start->default_count;
            follow_events_ = true;
            status(result.start->options.traceroute ? tr("Traceroute started from {0}'s terminal.", {name})
                                                    : tr("Ping started from {0}'s terminal.", {name}));
            sync();
            refresh_events();
            refresh_scene();
            refresh_run();
            refresh_document();
            // A ping on Linux runs as soon as it is typed: so does this one.
            if (!playback_.playing()) ui_->global<UiRun>().invoke_run_toggle();
        }
    }
    session.entries.push_back(std::move(entry));
    if (session.entries.size() > kept_entries) session.entries.erase(session.entries.begin());
    note_command(name, line);
    refresh_terminal(false);
}

void AppController::refresh_terminal(bool all) {
    const auto& ui = ui_->global<UiTerminal>();
    const auto* device = selected_device();
    if (all) {
        ui.set_available(device && device->powered);
        ui.set_note(slint::SharedString(!device             ? tr("Select a device to use its terminal.")
                                        : !device->powered ? tr("{0} is off: power it on to use its terminal.", {device->name})
                                                           : std::string{}));
        ui.set_prompt(device ? slint::SharedString(netsim::term::prompt(*device)) : slint::SharedString());
        std::vector<TerminalMenuView> menus;
        if (device && device->powered)
            for (const auto& menu : netsim::term::menus(device->id, scenario())) {
                std::vector<TerminalEntryView> entries;
                for (const auto& entry : menu.entries) entries.push_back({entry.command.c_str(), entry.text.c_str(), entry.complete});
                menus.push_back({menu.family.c_str(), std::make_shared<slint::VectorModel<TerminalEntryView>>(std::move(entries))});
            }
        terminal_menus_->set_vector(std::move(menus));
    }
    std::string screen;
    std::vector<TerminalBlockView> blocks;
    if (device) {
        const auto found = terminals_.find(device->id.value);
        if (found != terminals_.end())
            for (const auto& entry : found->second.entries) {
                TerminalBlockView typed{};
                typed.kind = "command";
                typed.prompt = slint::SharedString(entry.prompt);
                typed.text = slint::SharedString(entry.line);
                blocks.push_back(std::move(typed));
                std::string output = entry.output;
                if (entry.ping) {
                    // The ping as the run shows it: nothing yet before it starts.
                    const auto* ping = simulation().ping(*entry.ping);
                    if (ping && ping->source == entry.source && ping->destination == entry.destination && ping->started == entry.started)
                        output += netsim::term::application_output(simulation(), *ping, entry.default_count);
                    else if (simulation().now() < entry.started)
                        output += tr("# It starts at t = {0}.", {netsim::format_ms(entry.started)}) + '\n';
                    else
                        output += tr("# That run was reset.") + '\n';
                }
                const auto first = blocks.size();
                print(blocks, output, entry.error ? "error" : "output");
                if (!entry.refusal.empty()) {
                    // Apart from what was printed before it, also when that is an error too.
                    TerminalBlockView refusal{};
                    refusal.kind = "error";
                    std::string text = entry.refusal;
                    while (!text.empty() && text.back() == '\n') text.pop_back();
                    refusal.text = slint::SharedString(text);
                    blocks.push_back(std::move(refusal));
                }
                screen += entry.prompt + ' ' + entry.line + '\n';
                for (auto at = first; at < blocks.size(); ++at) screen += std::string(blocks[at].text) + '\n';
            }
    }
    if (screen != shown_screen_) {
        shown_screen_ = screen;
        ui.set_screen(slint::SharedString(screen));
        // Row by row: a ping printing its lines redraws its block, not the screen.
        const auto kept = std::min(blocks.size(), static_cast<std::size_t>(terminal_blocks_->row_count()));
        for (std::size_t row = 0; row < kept; ++row)
            if (!(*terminal_blocks_->row_data(row) == blocks[row])) terminal_blocks_->set_row_data(row, blocks[row]);
        while (static_cast<std::size_t>(terminal_blocks_->row_count()) > blocks.size()) terminal_blocks_->erase(terminal_blocks_->row_count() - 1);
        for (auto row = kept; row < blocks.size(); ++row) terminal_blocks_->push_back(blocks[row]);
    }
}

} // namespace resend
