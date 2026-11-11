// Labs: the panel with the brief, the steps and the goals, the check, the
// hints one at a time, the solution shown in place of the learner's network,
// and the library of the labs built in.
#include "app_controller.hpp"

#include "app/labs.hpp"
#include "core/text.hpp"
#include "explain/explain.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace resend {
namespace {
using netsim::text::tr;
namespace lab = netsim::lab;

// Goals whose verdict tells something once met: what the learner found.
bool tells_when_met(const lab::Goal& goal) {
    return goal.kind == lab::GoalKind::Answer || goal.kind == lab::GoalKind::Path || (goal.kind == lab::GoalKind::Reach && goal.expect);
}

std::shared_ptr<slint::VectorModel<slint::SharedString>> texts(const std::vector<std::string>& items, std::size_t count) {
    std::vector<slint::SharedString> out;
    for (std::size_t i = 0; i < std::min(count, items.size()); ++i) out.emplace_back(items[i]);
    return std::make_shared<slint::VectorModel<slint::SharedString>>(out);
}
} // namespace

void AppController::bind_lab() {
    for (const auto& file : lab_files()) {
        std::string error;
        auto parsed = lab::lab_from_json(file.text, error);
        if (!parsed) continue;
        parsed->id = std::string(file.file.substr(0, file.file.rfind('.')));
        library_.push_back(std::move(*parsed));
    }
    const auto& ui = ui_->global<UiLab>();
    ui.set_goals(goals_);
    ui.on_check([this] { check_lab(); });
    ui.on_restart([this] {
        if (!lab_) return;
        back_to_mine();
        ask(tr("Start {0} again?", {lab_->title}), tr("The network goes back to how the lab gives it: your changes to it are lost."),
            {{"restart", tr("Start again"), [this] { open_lab(lab::Lab(*lab_)); }}, {"cancel", tr("Cancel"), {}}});
    });
    ui.on_next_hint([this] {
        if (!lab_ || hints_shown_ >= lab_->hints.size()) return;
        ++hints_shown_;
        refresh_lab();
    });
    ui.on_toggle_solution([this] {
        if (parked_) {
            back_to_mine();
            return;
        }
        if (!lab_) return;
        ask(tr("Show the solution?"),
            tr("The lab's solution takes the place of your network until you go back to it. Your network and its history are kept."),
            {{"show", tr("Show the solution"), [this] { show_solution(); }}, {"cancel", tr("Cancel"), {}}});
    });
    ui.on_answer([this](int index, slint::SharedString text) {
        if (!lab_ || index < 0 || static_cast<std::size_t>(index) >= lab_->goals.size()) return;
        lab_answers_[lab_->goals[static_cast<std::size_t>(index)].id] = std::string(text);
    });
    ui.on_open_lab([this](int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= library_.size()) return;
        close_dialog();
        unless_unsaved([this, index] { open_lab(static_cast<std::size_t>(index)); });
    });
    ui.on_replay([this] { replay_lab(); });
    ui.on_show_library([this] {
        refresh_library();
        ui_->global<UiApp>().set_dialog("labs");
    });
    ui.on_leave([this] {
        if (!lab_) return;
        back_to_mine();
        leave_lab();
        refresh_lab();
        status(tr("Back to the sandbox: the network stays, without the lab's goals and locks."));
    });
}

bool AppController::open_lab(std::size_t index) {
    if (index >= library_.size()) return false;
    open_lab(library_[index]);
    return true;
}

void AppController::open_lab(lab::Lab subject) {
    auto owned = std::make_unique<lab::Lab>(std::move(subject));
    // Not a file of its own: saving asks where, and never overwrites the lab.
    open(owned->network, std::nullopt, owned->title);
    lab_ = std::move(owned);
    workspace_.set_lab(lab_.get());
    ui_->global<UiLab>().set_open(true);
    // What open() showed before the lab was set: its title, its locks.
    refresh_document();
    refresh_inspector(true);
    note(lab_->steps.empty() ? tr("{0}: read the brief, then press Check when you think it is done.", {lab_->title})
                               : tr("{0}: follow the steps, and press Check whenever you want to know where you are.", {lab_->title}));
}

void AppController::leave_lab() {
    if (parked_) {
        workspace_ = std::move(*parked_);
        parked_.reset();
    }
    workspace_.set_lab(nullptr);
    lab_.reset();
    verdicts_.clear();
    checked_state_.reset();
    lab_answers_.clear();
    hints_shown_ = 0;
}

void AppController::check_lab() {
    if (!lab_ || parked_) return;
    const bool before = checked_state_ && lab::all_met(verdicts_);
    std::vector<bool> was_met(lab_->goals.size(), false);
    if (checked_state_ && verdicts_.size() == was_met.size())
        for (std::size_t i = 0; i < verdicts_.size(); ++i) was_met[i] = verdicts_[i].met;
    verdicts_ = lab::check(*lab_, workspace_.design(), &workspace_.simulation(), lab_answers_);
    fresh_goals_.assign(verdicts_.size(), false);
    bool any_fresh = false;
    for (std::size_t i = 0; i < verdicts_.size(); ++i) {
        fresh_goals_[i] = verdicts_[i].met && !was_met[i];
        any_fresh = any_fresh || fresh_goals_[i];
    }
    if (any_fresh)
        fresh_timer_.start(slint::TimerMode::SingleShot, std::chrono::milliseconds(1200), [this] {
            fresh_goals_.clear();
            refresh_lab();
        });
    checked_state_ = workspace_.history().state();
    const auto met = static_cast<std::size_t>(std::count_if(verdicts_.begin(), verdicts_.end(), [](const auto& verdict) { return verdict.met; }));
    const auto& title = lab_->title;
    if (lab::all_met(verdicts_)) {
        auto& solved = settings_.solved_labs;
        if (!lab_->id.empty() && std::find(solved.begin(), solved.end(), lab_->id) == solved.end()) {
            solved.push_back(lab_->id);
            save_settings();
        }
        status(tr("Every goal is met: {0} is solved.", {title}));
        if (!before) celebrate();
        else if (any_fresh) play(CueKind::Goal);
    } else {
        status(tr("{0} of {1} goals met.", {std::to_string(met), std::to_string(verdicts_.size())}));
        if (any_fresh) play(CueKind::Goal);
    }
    refresh_lab();
}

// A command goal is met by the line that ran it, kept as its answer until
// the next check reads it.
void AppController::note_command(const std::string& device, const std::string& line) {
    if (!lab_ || parked_) return;
    for (const auto& goal : lab_->goals)
        if (goal.kind == lab::GoalKind::Command && goal.device == device && lab::command_matches(goal.command, line))
            lab_answers_[goal.id] = line;
}

// The moment: "Lab solved!" drops in over the canvas with confetti, then
// gets out of the way; the panel offers to play again what decided it.
void AppController::celebrate() {
    const auto& ui = ui_->global<UiLab>();
    ui.set_celebration_serial(ui.get_celebration_serial() + 1);
    celebration_start_ = std::chrono::steady_clock::now();
    ui.set_celebration(0);
    play(CueKind::Win);
    celebration_timer_.start(slint::TimerMode::Repeated, std::chrono::milliseconds(16), [this] {
        const std::chrono::duration<float> spent = std::chrono::steady_clock::now() - celebration_start_;
        const float progress = spent.count() / 2.2f;
        const auto& lab = ui_->global<UiLab>();
        if (progress >= 1) {
            lab.set_celebration(-1);
            celebration_timer_.stop();
            return;
        }
        lab.set_celebration(progress);
    });
}

void AppController::replay_lab() {
    if (!lab_ || parked_) return;
    const auto observed = std::any_of(lab_->goals.begin(), lab_->goals.end(), [](const lab::Goal& goal) { return goal.kind == lab::GoalKind::Observed; });
    const lab::Goal* probe = nullptr;
    for (const auto& goal : lab_->goals) {
        const bool probes = (goal.kind == lab::GoalKind::Reach && !goal.expect) || goal.kind == lab::GoalKind::Path ||
                            goal.kind == lab::GoalKind::Answer;
        const auto* host = probes ? scenario().device_named(goal.from) : nullptr;
        if (host && host->kind == netsim::DeviceKind::Host) {
            probe = &goal;
            break;
        }
    }
    const auto& run = ui_->global<UiRun>();
    if (observed || !probe) {
        // What the learner saw: their own run, from its start.
        if (!workspace_.next_event_time() && workspace_.end_time() == netsim::SimTime{}) {
            status(tr("Ping from a host first: there is nothing to play again yet."));
            return;
        }
        travel(netsim::SimTime{});
        if (!playback_.playing()) run.invoke_run_toggle();
        status(tr("Your run plays again from its start."));
        return;
    }
    // What the check saw: the probe's ping on a fresh run of the network.
    run.invoke_reset();
    select_device(probe->from);
    run.set_target(probe->to.to_string().c_str());
    run.set_traceroute(probe->kind == lab::GoalKind::Path || (probe->kind == lab::GoalKind::Answer && probe->quantity == lab::Quantity::Hops));
    run.set_count("1");
    start_ping();
    if (workspace_.next_event_time() && !playback_.playing()) run.invoke_run_toggle();
    status(tr("{0} pings {1} again, as the check did.", {probe->from, probe->to.to_string()}));
}

void AppController::refresh_lab() {
    const auto& ui = ui_->global<UiLab>();
    ui.set_active(lab_ != nullptr);
    ui.set_showing_solution(parked_ != nullptr);
    if (!lab_) {
        goals_->set_vector({});
        ui.set_steps(texts({}, 0));
        ui.set_hints(texts({}, 0));
        ui.set_hint_count(0);
        ui.set_solved(false);
        return;
    }
    ui.set_title(lab_->title.c_str());
    ui.set_kind(netsim::explain::kind_text(lab_->kind).c_str());
    ui.set_brief(lab_->brief.c_str());
    ui.set_steps(texts(lab_->steps, lab_->steps.size()));
    ui.set_hints(texts(lab_->hints, hints_shown_));
    ui.set_hint_count(static_cast<int>(lab_->hints.size()));

    // The verdicts hold for the network they were made on, the learner's.
    const auto& mine = parked_ ? *parked_ : workspace_;
    const bool current = checked_state_ && *checked_state_ == mine.history().state() && verdicts_.size() == lab_->goals.size();
    const auto& network = mine.design();
    std::size_t met = 0;
    for (std::size_t i = 0; i < lab_->goals.size(); ++i) {
        const auto& goal = lab_->goals[i];
        GoalView row{};
        row.text = goal.text.c_str();
        row.asks = goal.kind == lab::GoalKind::Answer;
        if (const auto answer = lab_answers_.find(goal.id); answer != lab_answers_.end()) row.answer = answer->second.c_str();
        row.state = "open";
        if (current) {
            const auto& verdict = verdicts_[i];
            met += verdict.met ? 1 : 0;
            row.state = verdict.met ? "met" : "unmet";
            row.fresh = i < fresh_goals_.size() && fresh_goals_[i];
            if (!verdict.met || tells_when_met(goal)) row.detail = netsim::explain::verdict_text(*lab_, verdict, network).c_str();
        }
        if (i < goals_->row_count()) {
            if (const auto shown = goals_->row_data(i); !shown || !(*shown == row)) goals_->set_row_data(i, row);
        } else {
            goals_->push_back(row);
        }
    }
    while (goals_->row_count() > lab_->goals.size()) goals_->erase(goals_->row_count() - 1);

    const bool solved = current && met == lab_->goals.size();
    ui.set_solved(solved);
    std::string progress;
    if (!checked_state_) progress = lab_->steps.empty() ? tr("Press Check to see which goals are met.") : tr("Follow the steps, then press Check.");
    else if (!current) progress = tr("The network changed since the last check: press Check again.");
    else if (solved) progress = tr("Every goal is met: lab solved!");
    else progress = tr("{0} of {1} met", {std::to_string(met), std::to_string(lab_->goals.size())});
    ui.set_progress(progress.c_str());
}

void AppController::refresh_library() {
    std::vector<LabEntryView> rows;
    const std::string* topic = nullptr;
    for (const auto& item : library_) {
        const bool first = !topic || *topic != item.topic;
        topic = &item.topic;
        const auto& solved = settings_.solved_labs;
        rows.push_back({item.title.c_str(), item.topic.c_str(), netsim::explain::kind_text(item.kind).c_str(),
                        std::find(solved.begin(), solved.end(), item.id) != solved.end(), first});
    }
    ui_->global<UiLab>().set_library(std::make_shared<slint::VectorModel<LabEntryView>>(rows));
}

void AppController::show_solution() {
    if (!lab_ || parked_) return;
    playback_.pause();
    parked_ = std::make_unique<Workspace>(std::move(workspace_));
    workspace_ = Workspace(lab_->solution.network);
    show_workspace();
    refresh_lab();
    status(tr("The solution of {0}: Back to my network returns to yours.", {lab_->title}));
}

void AppController::back_to_mine() {
    if (!parked_) return;
    playback_.pause();
    workspace_ = std::move(*parked_);
    parked_.reset();
    show_workspace();
    refresh_lab();
    status(tr("Your network is back."));
}

} // namespace resend
