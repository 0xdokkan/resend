#include "app/workspace.hpp"

#include <algorithm>
#include <utility>

namespace resend {
namespace {
template<class... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};
} // namespace

Workspace::Workspace(netsim::Scenario scenario) : base_(scenario), design_(scenario), simulation_(std::move(scenario)) {}

bool Workspace::untouched() const {
    return log_.empty() && simulation_.trace().records().empty() && !simulation_.pending();
}

void Workspace::rebuild(netsim::Scenario scenario) {
    base_ = scenario;
    design_ = scenario;
    simulation_ = netsim::Simulation(std::move(scenario));
    log_.clear();
    applied_ = 0;
    horizon_ = {};
    ++generation_;
}

netsim::Problem Workspace::check(const std::vector<netsim::Edit>& edits) const {
    auto trial = design_;
    for (const auto& edit : edits)
        if (const auto problem = netsim::edit::apply(trial, edit); !netsim::ok(problem)) return problem;
    return netsim::Problem::None;
}

netsim::Problem Workspace::make(const Command& command) {
    simulation_.run_until(command.time);
    return std::visit(Overloaded{
                          [&](const netsim::Edit& edit) { return simulation_.apply(edit); },
                          [&](const PingCommand& ping) {
                              auto problem = netsim::Problem::None;
                              simulation_.start_ping(ping.source, ping.destination, problem, ping.options);
                              return problem;
                          },
                      },
                      command.action);
}

netsim::Problem Workspace::record(Command command) {
    command.time = simulation_.now();
    const auto problem = make(command);
    if (!netsim::ok(problem)) return problem;
    const bool silent = std::holds_alternative<netsim::Edit>(command.action) &&
        !netsim::affects_network(std::get<netsim::Edit>(command.action));
    // What was computed past this instant no longer follows from the commands.
    if (!silent) horizon_ = simulation_.now();
    log_.push_back(std::move(command));
    applied_ = log_.size();
    design_ = simulation_.scenario();
    return problem;
}

void Workspace::commit(const std::vector<netsim::Edit>& edits) {
    if (!untouched()) {
        for (const auto& edit : edits) record({{}, edit});
        return;
    }
    // Nothing has happened yet: the edits give a new network, unless they
    // only rename or move, which a simulation takes silently.
    for (const auto& edit : edits) netsim::edit::apply(design_, edit);
    if (std::any_of(edits.begin(), edits.end(), [](const auto& edit) { return netsim::affects_network(edit); })) {
        rebuild(design_);
        return;
    }
    for (const auto& edit : edits) simulation_.apply(edit);
    base_ = design_;
}

Outcome Workspace::change(const netsim::Edit& edit, const std::string& label, bool gesture) {
    return change(std::vector{edit}, label, gesture);
}

Outcome Workspace::change(const std::vector<netsim::Edit>& edits, const std::string& label, bool gesture) {
    if (in_past()) return {.problem = netsim::Problem::None, .in_past = true};
    std::vector<netsim::Edit> forward, backward;
    auto trial = design_;
    // Ids given within this step, so that two new devices get two ids.
    std::uint32_t next_device = 0, next_link = 0;
    for (const auto& requested : edits) {
        auto edit = simulation_.resolve(requested);
        if (auto* item = std::get_if<netsim::AddDevice>(&edit); item && !std::get<netsim::AddDevice>(requested).id) {
            item->id->value = std::max(item->id->value, next_device);
            next_device = item->id->value + 1;
        }
        if (auto* item = std::get_if<netsim::Connect>(&edit); item && !std::get<netsim::Connect>(requested).id) {
            item->id->value = std::max(item->id->value, next_link);
            next_link = item->id->value + 1;
        }
        const auto before = trial;
        if (const auto problem = netsim::edit::apply(trial, edit); !netsim::ok(problem)) return {.problem = problem};
        if (trial == before) continue;
        backward.insert(backward.begin(), *netsim::edit::inverse(before, edit));
        forward.push_back(std::move(edit));
    }
    if (forward.empty()) return {};
    commit(forward);
    history_.push(std::move(forward), std::move(backward), label, gesture);
    return {};
}

Outcome Workspace::undo() {
    if (in_past()) return {.problem = netsim::Problem::None, .in_past = true};
    const auto* step = history_.undoable();
    if (!step) return {};
    if (const auto problem = check(step->backward); !netsim::ok(problem)) return {.problem = problem};
    commit(step->backward);
    history_.undone();
    return {};
}

Outcome Workspace::redo() {
    if (in_past()) return {.problem = netsim::Problem::None, .in_past = true};
    const auto* step = history_.redoable();
    if (!step) return {};
    if (const auto problem = check(step->forward); !netsim::ok(problem)) return {.problem = problem};
    commit(step->forward);
    history_.redone();
    return {};
}

Outcome Workspace::ping(netsim::DeviceId source, netsim::Ipv4Address destination, const netsim::PingOptions& options) {
    if (in_past()) return {.problem = netsim::Problem::None, .in_past = true};
    return {.problem = record({{}, PingCommand{source, destination, options}})};
}

void Workspace::reset() { rebuild(design_); }

void Workspace::replace(netsim::Scenario scenario) {
    rebuild(std::move(scenario));
    history_.clear();
    saved_ = history_.state();
}

std::optional<netsim::SimTime> Workspace::next_event_time() const {
    auto next = simulation_.next_event_time();
    if (in_past() && (!next || log_[applied_].time < *next)) next = log_[applied_].time;
    return next;
}

void Workspace::run_until(netsim::SimTime time) {
    for (; applied_ < log_.size() && log_[applied_].time <= time; ++applied_) make(log_[applied_]);
    simulation_.run_until(time);
    horizon_ = std::max(horizon_, simulation_.now());
}

void Workspace::travel(netsim::SimTime time) {
    if (time < simulation_.now()) {
        simulation_ = netsim::Simulation(base_);
        applied_ = 0;
        ++generation_;
    }
    run_until(time);
}

netsim::SimTime Workspace::end_time() const {
    auto end = std::max(horizon_, simulation_.now());
    if (!log_.empty()) end = std::max(end, log_.back().time);
    return end;
}

} // namespace resend
