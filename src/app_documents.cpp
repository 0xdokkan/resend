// Files and the dialogs around them, the examples, autosave and recovery,
// the look and the other settings.
#include "app_controller.hpp"

#include "app/examples.hpp"
#include "core/text.hpp"
#include "scenario/scenario_json.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <memory>
#include <system_error>

namespace resend {
namespace fs = std::filesystem;

namespace {
using netsim::text::tr;

// A file name from a title: "Office floor" gives "office-floor.json".
std::string file_name_for(const std::string& title) {
    std::string name;
    for (const char c : title) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte)) name += static_cast<char>(std::tolower(byte));
        else if (!name.empty() && name.back() != '-') name += '-';
    }
    while (!name.empty() && name.back() == '-') name.pop_back();
    return (name.empty() ? std::string("network") : name) + ".json";
}
} // namespace

void AppController::bind_documents() {
    const auto& app = ui_->global<UiApp>();
    app.on_new_document([this] {
        unless_unsaved([this] {
            open(netsim::Scenario{});
            note(tr("A new network: add hosts and switches from the palette."));
        });
    });
    app.on_open_document([this] { unless_unsaved([this] { show_file_dialog(false); }); });
    app.on_open_example([this](int index) { open_example(index); });
    app.on_save([this] { save(); });
    app.on_save_as([this] {
        after_save_ = {};
        show_file_dialog(true);
    });
    app.on_quit([this] { quit(); });
    app.on_about([this] { ui_->global<UiApp>().set_dialog("about"); });

    app.on_browse_enter([this](slint::SharedString name) {
        if (browser_) browser_->enter(std::string(name));
        browse_refresh();
    });
    app.on_browse_up([this] {
        if (browser_) browser_->up();
        browse_refresh();
    });
    app.on_browse_home([this] {
        if (browser_) browser_->open(home_directory());
        browse_refresh();
    });
    app.on_browse_pick([this](slint::SharedString picked) {
        const std::string name(picked);
        if (saving_) {
            ui_->global<UiApp>().set_file_name(name.c_str());
            return;
        }
        if (!browser_) return;
        const auto path = browser_->folder() / path_from_text(name);
        std::string error;
        if (is_lab_file(path)) {
            auto lab = read_lab_file(path, error);
            if (!lab) {
                ui_->global<UiApp>().set_browser_error(tr("{0} cannot be opened: {1}", {name, error}).c_str());
                return;
            }
            close_dialog();
            open_lab(std::move(*lab));
            return;
        }
        auto scenario = read_scenario_file(path, error);
        if (!scenario) {
            ui_->global<UiApp>().set_browser_error(tr("{0} cannot be opened: {1}", {name, error}).c_str());
            return;
        }
        close_dialog();
        open(std::move(*scenario), path);
    });
    app.on_browse_confirm([this] { confirm_save_as(); });
    app.on_answer([this](slint::SharedString id) {
        const std::string chosen(id);
        auto answers = std::exchange(answers_, {});
        ui_->global<UiApp>().set_dialog("");
        const auto found = std::find_if(answers.begin(), answers.end(), [&](const Answer& answer) { return answer.id == chosen; });
        if (found != answers.end() && found->action) found->action();
        dialog_left();
    });
    app.on_close_dialog([this] { close_dialog(); });
    app.on_dismiss_dialog([this] { dismiss_dialog(); });
}

void AppController::refresh_examples() {
    std::vector<ExampleView> rows;
    for (const auto& example : examples()) {
        std::string error;
        const auto scenario = netsim::scenario_from_json(example.text, error);
        const auto title = scenario && !scenario->title.empty() ? scenario->title : std::string(example.file);
        rows.push_back({title.c_str(), std::string(example.file).c_str()});
    }
    ui_->global<UiApp>().set_examples(std::make_shared<slint::VectorModel<ExampleView>>(rows));
}

void AppController::open(netsim::Scenario scenario, std::optional<fs::path> file, std::string name) {
    playback_.pause();
    leave_lab();
    const auto title = scenario.title;
    workspace_.replace(std::move(scenario));
    document_ = std::move(file);
    // An untitled network goes by its title, e.g. an example's.
    document_title_ = name.empty() ? title : std::move(name);
    autosaved_.reset();
    recovery_.clear();
    terminals_.clear();
    show_workspace();
    note(tr("{0} opened.", {document_name()}));
}

void AppController::show_workspace() {
    // Another workspace may have the generation the views were made for.
    generation_ = ~std::uint64_t{0};
    filter_ = {};
    events_->set_vector({});
    shown_events_.clear();
    listed_ = 0;
    selected_event_ = selected_pdu_ = -1;
    follow_events_ = true;
    ui_->global<UiTimeline>().set_tab(0);

    // The first host is selected, ready to ping another one.
    const auto& devices = workspace_.scenario().devices;
    const auto host = std::find_if(devices.begin(), devices.end(), [](const auto& device) { return device.kind == netsim::DeviceKind::Host; });
    const auto other = host == devices.end() ? devices.end()
        : std::find_if(std::next(host), devices.end(), [](const auto& device) { return device.kind == netsim::DeviceKind::Host; });
    canvas_.select(host != devices.end() ? Selection{Selection::Kind::Device, host->id.value} : Selection{});
    canvas_.validate();
    if (other != devices.end() && other->ports.front().ipv4)
        ui_->global<UiRun>().set_target(other->ports.front().ipv4->address().to_string().c_str());
    canvas_.fit();
    refresh_all();
    sync_playback();
}

bool AppController::open_file(const fs::path& path) {
    std::string error;
    if (is_lab_file(path)) {
        auto lab = read_lab_file(path, error);
        if (!lab) {
            status(tr("{0} cannot be opened: {1}", {path_text(path.filename()), error}));
            return false;
        }
        open_lab(std::move(*lab));
        return true;
    }
    auto scenario = read_scenario_file(path, error);
    if (!scenario) {
        status(tr("{0} cannot be opened: {1}", {path_text(path.filename()), error}));
        return false;
    }
    open(std::move(*scenario), path);
    return true;
}

void AppController::open_example(int index) {
    const auto list = examples();
    if (index < 0 || index >= static_cast<int>(list.size())) return;
    unless_unsaved([this, index] {
        const auto& example = examples()[static_cast<std::size_t>(index)];
        std::string error;
        if (auto scenario = netsim::scenario_from_json(example.text, error)) open(std::move(*scenario));
    });
}

std::string AppController::document_name() const {
    if (document_) return path_text(document_->filename());
    return document_title_.empty() ? tr("Untitled") : document_title_;
}

void AppController::refresh_document() {
    const auto& app = ui_->global<UiApp>();
    app.set_document(document_name().c_str());
    app.set_modified(workspace_.modified());
    const auto& design = workspace_.design();
    app.set_title((lab_ ? lab_->title : design.title).c_str());
    app.set_notes(lab_ ? tr("The brief, the steps and the goals are in the lab panel: G shows or hides it.").c_str() : design.notes.c_str());
    const auto* undo = workspace_.history().undoable();
    const auto* redo = workspace_.history().redoable();
    app.set_can_undo(undo != nullptr);
    app.set_can_redo(redo != nullptr);
    app.set_undo_text((undo ? tr("Undo {0}", {undo->label}) : tr("Undo")).c_str());
    app.set_redo_text((redo ? tr("Redo {0}", {redo->label}) : tr("Redo")).c_str());
    refresh_lab();
}

void AppController::ask(const std::string& title, const std::string& message, std::vector<Answer> answers) {
    std::vector<slint::SharedString> ids, texts;
    for (const auto& answer : answers) {
        ids.emplace_back(answer.id.c_str());
        texts.emplace_back(answer.text.c_str());
    }
    answers_ = std::move(answers);
    const auto& app = ui_->global<UiApp>();
    app.set_dialog_title(title.c_str());
    app.set_dialog_message(message.c_str());
    app.set_dialog_answers(std::make_shared<slint::VectorModel<slint::SharedString>>(ids));
    app.set_dialog_answer_texts(std::make_shared<slint::VectorModel<slint::SharedString>>(texts));
    app.set_dialog("confirm");
}

void AppController::close_dialog() {
    answers_.clear();
    after_save_ = {};
    ui_->global<UiApp>().set_dialog("");
    dialog_left();
}

void AppController::dialog_left() {
    // A dialog has the keyboard while it is open. The window gives it back
    // to the canvas when it sees the dialog go, but it looks once per turn
    // of the event loop: said here, it does not depend on that.
    if (ui_->global<UiApp>().get_dialog().empty()) ui_->invoke_focus_canvas();
}

void AppController::dismiss_dialog() {
    const auto& app = ui_->global<UiApp>();
    if (app.get_dialog() != "confirm") {
        // The first run's choices stay for the next start: none was made.
        close_dialog();
        return;
    }
    // A question is left by its Cancel, or by its one answer when it only
    // tells something. One that offers no way back waits for an answer.
    const auto cancel = std::find_if(answers_.begin(), answers_.end(), [](const Answer& answer) { return answer.id == "cancel"; });
    if (cancel != answers_.end()) app.invoke_answer("cancel");
    else if (answers_.size() == 1) app.invoke_answer(answers_.front().id.c_str());
}

void AppController::unless_unsaved(std::function<void()> action) {
    // The learner's network is the one to keep, not a solution in view.
    back_to_mine();
    if (!workspace_.modified()) {
        action();
        return;
    }
    ask(tr("Save the changes to {0}?", {document_name()}), tr("Your changes will be lost if you don't save them."),
        {{"save", tr("Save"), [this, action] { save(action); }},
         {"discard", tr("Don't save"), action},
         {"cancel", tr("Cancel"), {}}});
}

void AppController::save(std::function<void()> then) {
    back_to_mine();
    if (!document_) {
        after_save_ = std::move(then);
        show_file_dialog(true);
        return;
    }
    if (write(*document_) && then) then();
}

void AppController::show_file_dialog(bool saving) {
    saving_ = saving;
    if (!browser_) browser_.emplace(document_ ? document_->parent_path() : home_directory());
    else browser_->refresh();
    const auto& app = ui_->global<UiApp>();
    app.set_file_name((document_ ? path_text(document_->filename()) : file_name_for(document_title_.empty() ? workspace_.design().title : document_title_)).c_str());
    browse_refresh();
    app.set_dialog(saving ? "save" : "open");
}

void AppController::browse_refresh() {
    if (!browser_) return;
    std::vector<FileView> rows;
    for (const auto& entry : browser_->entries()) rows.push_back({entry.name.c_str(), entry.folder});
    const auto& app = ui_->global<UiApp>();
    app.set_folder(path_text(browser_->folder()).c_str());
    app.set_entries(std::make_shared<slint::VectorModel<FileView>>(rows));
    app.set_browser_error(browser_->error().c_str());
}

void AppController::confirm_save_as() {
    if (!browser_) return;
    const auto& app = ui_->global<UiApp>();
    const auto target = browser_->target(std::string(app.get_file_name()));
    if (!target) {
        app.set_browser_error(tr("Type a name for the file, without folders.").c_str());
        return;
    }
    const auto finish = [this](const fs::path& path) {
        if (!write(path)) return;
        auto then = std::exchange(after_save_, {});
        ui_->global<UiApp>().set_dialog("");
        if (then) then();
        dialog_left();
    };
    std::error_code ignored;
    if (!fs::exists(*target, ignored)) {
        finish(*target);
        return;
    }
    // The question replaces the file dialog; Cancel brings it back.
    const auto path = *target;
    ask(tr("Replace {0}?", {path_text(path.filename())}),
        tr("A file with this name already exists in {0}. Replacing it overwrites its contents.", {path_text(path.parent_path())}),
        {{"replace", tr("Replace"), [finish, path] { finish(path); }},
         {"cancel", tr("Cancel"), [this] { show_file_dialog(true); }}});
}

bool AppController::write(const fs::path& path) {
    std::string error;
    if (!write_scenario_file(path, workspace_.design(), error)) {
        after_save_ = {};
        ask(tr("The network could not be saved"), tr("{0}: {1}", {path_text(path), error}), {{"ok", tr("OK"), {}}});
        return false;
    }
    document_ = path;
    document_title_.clear();
    workspace_.mark_saved();
    recovery_.clear();
    autosaved_.reset();
    refresh_document();
    status(tr("Saved {0}.", {path_text(path.filename())}));
    return true;
}

void AppController::quit() {
    unless_unsaved([this] {
        recovery_.clear();
        ui_->hide();
        slint::quit_event_loop();
    });
}

bool AppController::close_requested() {
    back_to_mine();
    if (!workspace_.modified()) return true;
    quit();
    return false;
}

bool AppController::offer_recovery() {
    auto found = recovery_.abandoned();
    if (!found) return false;
    const auto shared = std::make_shared<Recovery::Found>(std::move(*found));
    const auto name = shared->document ? path_text(shared->document->filename()) : tr("an untitled network");
    ask(tr("Recover the unsaved network?"),
        tr("resend closed before the changes to {0} were saved. Recover the network as it was then?", {name}),
        {{"recover", tr("Recover"),
          [this, shared] {
              open(shared->scenario, shared->document, shared->document ? std::string() : tr("Recovered network"));
              workspace_.mark_modified();
              Recovery::discard(*shared);
              refresh_document();
              status(tr("Network recovered. Save it to keep it."));
          }},
         {"discard", tr("Discard"), [shared] { Recovery::discard(*shared); }}});
    return true;
}

void AppController::autosave() {
    // While the solution is in view, the learner's network waits unchanged.
    if (parked_) return;
    if (!workspace_.modified()) {
        if (autosaved_) {
            recovery_.clear();
            autosaved_.reset();
        }
        return;
    }
    const auto state = workspace_.history().state();
    if (autosaved_ == state) return;
    if (recovery_.save(workspace_.design(), document_)) autosaved_ = state;
}

void AppController::apply_settings() {
    ui_->global<Motion>().set_effects(settings_.effects.c_str());
    apply_sound();
    refresh_examples();
    refresh_library();
    refresh_all();
}

void AppController::save_settings() { resend::save_settings(state_directory_, settings_); }

} // namespace resend
