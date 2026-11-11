#pragma once

#include "app/history.hpp"
#include "app/playback.hpp"
#include "lab/lab.hpp"
#include "scenario/edit.hpp"
#include "sim/simulation.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace resend {

// A ping as the user started it, to start it again when the run is replayed.
struct PingCommand {
    netsim::DeviceId source{};
    netsim::Ipv4Address destination;
    netsim::PingOptions options{};
};
// Something the user did to the running network, and when.
struct Command {
    netsim::SimTime time{};
    std::variant<netsim::Edit, PingCommand> action;
};

// Whether the workspace took a change, and why not.
struct Outcome {
    netsim::Problem problem = netsim::Problem::None;
    // Commands lie ahead of the instant shown: it is in the past.
    bool in_past = false;
    // With Problem::Locked: the lock of the lab that refused the change.
    std::optional<netsim::lab::DeviceSettings> lock{};
    bool ok() const { return netsim::ok(problem) && !in_past; }
};
// Why a change was refused, in words: the problem, or the lab's lock.
std::string why_refused(const Outcome& outcome);

// The network being edited, the simulation running it and what the user
// did to them, without any Slint type, so tests drive it as the interface
// does.
//
// Before anything has happened an edit simply gives a new network. Once the
// simulation has run, edits and pings are commands made at the current
// instant and recorded with it. Going back in time replays them on a new
// simulation of the network as it was then: the simulation is deterministic,
// so the replay is the same run. While recorded commands lie ahead of the
// instant shown the network cannot be changed; the playback makes them
// again when their time comes.
class Workspace final : public Timeline {
public:
    explicit Workspace(netsim::Scenario scenario = netsim::sample_lan());

    // The network at the instant shown.
    const netsim::Scenario& scenario() const { return simulation_.scenario(); }
    // The latest network: the one edited and saved.
    const netsim::Scenario& design() const { return design_; }
    const netsim::Simulation& simulation() const override { return simulation_; }
    // Changes whenever a new simulation replaces the previous one, so the
    // events and the playback start over.
    std::uint64_t generation() const { return generation_; }

    // Makes the edits as one undoable step, all or none. With `gesture`, the
    // step joins the previous one if it has the same label and the gesture
    // has not ended. Edits that change nothing are left out.
    Outcome change(const std::vector<netsim::Edit>& edits, const std::string& label, bool gesture = false);
    Outcome change(const netsim::Edit& edit, const std::string& label, bool gesture = false);
    void end_gesture() { history_.seal(); }
    Outcome undo();
    Outcome redo();
    const History& history() const { return history_; }

    // Starts a ping from a host at the current instant.
    Outcome ping(netsim::DeviceId source, netsim::Ipv4Address destination, const netsim::PingOptions& options = {});

    // A new simulation of the latest network from time zero; the history stays.
    void reset();
    // Another network, e.g. a file opened: a new simulation and no history.
    void replace(netsim::Scenario scenario);
    // The lab being done, or nothing: its locks refuse edits with
    // Problem::Locked, and the devices added start as it says, in the same
    // step. The lab must outlive its use here.
    void set_lab(const netsim::lab::Lab* lab) { lab_ = lab; }
    const netsim::lab::Lab* lab() const { return lab_; }

    std::optional<netsim::SimTime> next_event_time() const override;
    void run_until(netsim::SimTime time) override;
    // Shows `time`, earlier or later than the current instant: going back
    // replays the commands made up to it on a new simulation.
    void travel(netsim::SimTime time);
    bool in_past() const { return applied_ < log_.size(); }
    // The latest instant the simulation reached or a command was made at.
    netsim::SimTime end_time() const;
    const std::vector<Command>& commands() const { return log_; }

    // Whether the network differs from the one last saved or opened.
    bool modified() const { return history_.state() != saved_; }
    void mark_saved() { saved_ = history_.state(); }
    // E.g. a network recovered after a crash: it is not saved anywhere.
    void mark_modified() { saved_ = unsaved; }

private:
    static constexpr std::uint64_t unsaved = ~std::uint64_t{0};
    bool untouched() const;
    void rebuild(netsim::Scenario scenario);
    // Checks edits whose ids are resolved against the latest network.
    Outcome check(const std::vector<netsim::Edit>& edits) const;
    // The lab's lock that stops `edit` on `before`, as an outcome.
    std::optional<Outcome> refused(const netsim::Scenario& before, const netsim::Edit& edit) const;
    // Makes checked edits: on a new simulation if nothing has happened yet,
    // as commands at the current instant otherwise.
    void commit(const std::vector<netsim::Edit>& edits);
    // On the simulation, after every event due at its instant.
    netsim::Problem make(const Command& command);
    // Makes a command at the current instant and records it.
    netsim::Problem record(Command command);

    netsim::Scenario base_;    // what the simulation was built from
    netsim::Scenario design_;  // after every command
    netsim::Simulation simulation_;
    History history_;
    std::vector<Command> log_;
    std::size_t applied_ = 0;  // commands made on the current simulation
    netsim::SimTime horizon_{};
    std::uint64_t generation_ = 0;
    std::uint64_t saved_ = 0;
    const netsim::lab::Lab* lab_ = nullptr;
};

} // namespace resend
