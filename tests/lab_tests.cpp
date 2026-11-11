// Labs: their files, the goals checked on the simulation, the locks, and
// the labs shipped in labs/.
#include "app/labs.hpp"
#include "app/workspace.hpp"
#include "check.hpp"
#include "explain/explain.hpp"
#include "lab/lab.hpp"
#include "sim_support.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>

namespace {
using namespace std::chrono_literals;
using namespace simtest;
using netsim::DeviceKind;
using netsim::LinkId;
using netsim::PortRef;
namespace lab = netsim::lab;
using lab::Finding;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

lab::Lab load(const std::string& id) {
    std::string error;
    auto loaded = lab::lab_from_json(read_file(std::filesystem::path(RESEND_LABS_DIR) / (id + ".json")), error);
    if (!loaded) std::cerr << id << ": " << error << '\n';
    CHECK(loaded.has_value());
    loaded->id = id;
    return *loaded;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string error_of(const std::string& text) {
    std::string error;
    CHECK(!lab::lab_from_json(text, error));
    return error;
}

std::size_t goal_index(const lab::Lab& subject, const std::string& id) {
    const auto it = std::find_if(subject.goals.begin(), subject.goals.end(), [&](const auto& goal) { return goal.id == id; });
    CHECK(it != subject.goals.end());
    return static_cast<std::size_t>(it - subject.goals.begin());
}

void shipped_labs() {
    // Every file in labs/ is built into the app, as it is on disk, and the
    // other way round; each one reads, writes back the same, and proves
    // itself. Their goals have distinct ids and their topics are few.
    std::set<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(RESEND_LABS_DIR))
        if (entry.path().extension() == ".json") files.insert(entry.path().filename().string());
    const auto embedded = resend::lab_files();
    CHECK(embedded.size() == files.size() && embedded.size() >= 10);
    std::set<std::string> topics;
    for (const auto& file : embedded) {
        CHECK(files.contains(std::string(file.file)));
        CHECK(read_file(std::filesystem::path(RESEND_LABS_DIR) / std::string(file.file)) == file.text);
        std::string error;
        auto loaded = lab::lab_from_json(file.text, error);
        CHECK(loaded.has_value() && error.empty());
        CHECK(lab::validate(*loaded).empty());
        const auto issues = lab::verify(*loaded);
        for (const auto& issue : issues) std::cerr << file.file << ": " << issue << '\n';
        CHECK(issues.empty());
        auto again = lab::lab_from_json(lab::lab_to_json(*loaded), error);
        CHECK(again && *again == *loaded);
        topics.insert(loaded->topic);
    }
    CHECK(topics.size() >= 3 && topics.size() <= 5);
    // The library lists them by topic: each topic's labs are together.
    std::set<std::string> done;
    std::string topic;
    for (const auto& file : embedded) {
        std::string error;
        const auto loaded = lab::lab_from_json(file.text, error);
        if (loaded->topic == topic) continue;
        CHECK(!done.contains(loaded->topic));
        done.insert(topic = loaded->topic);
    }
    // The library starts with the first lab of the course.
    CHECK(embedded.front().file == "first-network.json");
}

void file_errors() {
    const auto minimal = [](const std::string& goals, const std::string& extra = {}) {
        return R"({"format": "resend-lab", "version": 1, "kind": "challenge", "topic": "T", "title": "A", "brief": "B",
                   "network": {"devices": [{"id": 1, "kind": "host", "name": "PC-A", "x": 0, "y": 0,
                                            "ports": [{"ipv4": "10.0.0.1/24"}]}], "links": []},
                   "goals": )" + goals + extra + R"(, "solution": {"network": {"devices": [], "links": []}}})";
    };
    std::string error;
    const auto parsed = lab::lab_from_json(minimal(R"([{"id": "g", "kind": "reach", "text": "x", "from": "PC-A", "to": "10.0.0.2"}])"), error);
    CHECK(parsed && error.empty() && parsed->goals.size() == 1 && parsed->goals[0].to == ip("10.0.0.2"));
    CHECK(parsed->kind == lab::Kind::Challenge && parsed->network.devices.size() == 1);

    CHECK(contains(error_of("{"), "line 1"));
    CHECK(error_of(R"({"format": "resend", "version": 1})") == "format: this is not a resend lab file");
    CHECK(contains(error_of(R"({"format": "resend-lab", "version": 2})"), "newer version of resend"));
    CHECK(contains(error_of(minimal("[]")), "goals: expected at least one goal"));
    CHECK(contains(error_of(minimal(R"([{"id": "g", "kind": "wish", "text": "x"}])")), "goals[0].kind: expected one of"));
    CHECK(contains(error_of(minimal(R"([{"id": "g", "kind": "reach", "text": "x", "from": "PC-A", "to": "10.0.0.300"}])")),
                   "goals[0].to: expected an IPv4 address"));
    CHECK(contains(error_of(minimal(R"([{"id": "g", "kind": "reach", "text": "x", "from": "PC-A", "to": "10.0.0.2", "expect": "Bad"}])")),
                   "goals[0].expect"));
    CHECK(contains(error_of(minimal(R"([{"id": "g", "kind": "config", "text": "x", "device": "PC-A",
                                        "address_in": "10.0.0.0/24", "gateway": "10.0.0.1"}])")),
                   "expected exactly one of"));
    CHECK(contains(error_of(minimal(R"([{"id": "g", "kind": "config", "text": "x", "device": "PC-A", "up": true}])")),
                   "missing member \"port\""));
    CHECK(contains(error_of(minimal(R"([{"id": "g", "kind": "config", "text": "x", "device": "PC-A", "gateway": "10.0.0.1",
                                        "via": "10.0.0.2"}])")),
                   "goals[0].via: only a route has a next hop"));
    CHECK(contains(error_of(minimal(R"([{"id": "g", "kind": "observed", "text": "x", "device": "PC-A", "record": "arp-reply",
                                        "for": "10.0.0.2"}])")),
                   "only an ARP request asks for an address"));
    CHECK(contains(error_of(minimal(R"([{"id": "g", "kind": "kept", "text": "x", "devices": []}])")), "goals[0].devices"));
    CHECK(contains(error_of(minimal(R"([{"id": "g", "kind": "reach", "text": "x", "from": "PC-A", "to": "10.0.0.2", "colour": 1}])")),
                   "unknown member \"colour\""));
    CHECK(contains(error_of(minimal(R"([{"id": "g", "kind": "reach", "text": "x", "from": "PC-A", "to": "10.0.0.2"}])",
                                    R"(, "locks": [{"device": "PC-A", "settings": ["colour"]}])")),
                   "locks[0].settings[0]"));

    // What reads but makes no sense is for validate(), in words for authors.
    auto subject = *parsed;
    subject.goals.push_back(subject.goals[0]);
    subject.goals.back().from = "PC-Z";
    subject.locks.push_back({"Router"});
    subject.solution.answers["g"] = "1";
    const auto issues = lab::validate(subject);
    const auto reported = [&](const std::string& part) {
        return std::any_of(issues.begin(), issues.end(), [&](const auto& issue) { return contains(issue, part); });
    };
    CHECK(reported("goal \"g\": the id is used twice"));
    CHECK(reported("no device is named \"PC-Z\""));
    CHECK(reported("locks: \"Router\" is not in the network"));
    CHECK(reported("\"g\" is not a goal that asks a question"));
    // Names the learner is to give count as devices.
    subject = *parsed;
    subject.goals[0].from = "PC-Z";
    subject.names = {"PC-Z"};
    CHECK(lab::validate(subject).empty());
}

void locks_refuse_edits() {
    // The missing gateway: the router and PC-A are locked, PC-B is the one
    // to repair.
    const auto subject = load("missing-gateway");
    const DeviceId pc_a{1}, router{3}, switch_2{4}, pc_b{5};
    resend::Workspace workspace(subject.network);
    workspace.set_lab(&subject);
    CHECK(workspace.lab() == &subject);

    // Before anything has happened, and once the network has run: a locked
    // edit changes nothing, neither the network nor the history nor the run.
    for (const bool running : {false, true}) {
        if (running) {
            CHECK(workspace.ping(pc_a, ip("192.168.2.20")).ok());
            workspace.run_until(netsim::SimTime(5ms));
        }
        const auto before = workspace.scenario();
        const auto steps = workspace.history().size();
        Hash hash;
        add_trace(hash, workspace.simulation());

        auto outcome = workspace.change(netsim::SetGateway{pc_a, ip("192.168.1.254")}, "Gateway");
        CHECK(outcome.problem == Problem::Locked && outcome.lock && outcome.lock->device == "PC-A");
        CHECK(outcome.lock->settings == lab::Gateway);  // what the edit touches of what is locked
        // A cable of the router is the router's.
        outcome = workspace.change(netsim::SetLinkUp{LinkId{2}, false}, "Cable");
        CHECK(outcome.problem == Problem::Locked && outcome.lock->device == "Router" && outcome.lock->settings == lab::Cables);
        // So is the cable of a device removed beside it.
        outcome = workspace.change(netsim::RemoveDevice{switch_2}, "Remove");
        CHECK(outcome.problem == Problem::Locked && outcome.lock->device == "Router");
        // All edits or none.
        outcome = workspace.change({netsim::SetGateway{pc_b, ip("192.168.2.1")}, netsim::Rename{router, "R"}}, "Two");
        CHECK(outcome.problem == Problem::Locked && outcome.lock->device == "Router");

        Hash after;
        add_trace(after, workspace.simulation());
        CHECK(workspace.scenario() == before && workspace.history().size() == steps && after.value == hash.value);
        // The words for it.
        CHECK(netsim::explain::lock_text(*outcome.lock) == "The lab keeps Router as it is: its name cannot be changed.");
    }

    // Positions are never locked; PC-B is free; undo goes back through them.
    CHECK(workspace.change(netsim::MoveDevice{router, 10, 20}, "Move").ok());
    CHECK(workspace.change(netsim::SetGateway{pc_b, ip("192.168.2.1")}, "Gateway").ok());
    CHECK(workspace.undo().ok() && !workspace.scenario().device(pc_b)->gateway);
    CHECK(workspace.redo().ok() && workspace.scenario().device(pc_b)->gateway);

    // Without the lab nothing is locked.
    workspace.set_lab(nullptr);
    CHECK(workspace.change(netsim::SetGateway{pc_a, ip("192.168.1.254")}, "Gateway").ok());
}

void lock_texts() {
    using namespace netsim::explain;
    CHECK(settings_text(lab::Addresses | lab::Gateway) == "addresses and default gateway");
    CHECK(settings_text(lab::Name | lab::Routes | lab::Cables) == "name, routes and cables");
    CHECK(settings_text(lab::all_settings) == "everything");
    CHECK(lock_text({"R1", lab::Routes}) == "The lab keeps R1 as it is: its routes cannot be changed.");
    CHECK(lock_text({"R1", lab::all_settings}) == "The lab keeps R1 as it is: it cannot be changed.");
    CHECK(kind_text(lab::Kind::Troubleshooting) == "Troubleshooting");
    // Only some settings locked: the others may change.
    auto subject = load("missing-gateway");
    subject.locks = {{"PC-A", lab::Gateway}};
    const auto& network = subject.network;
    const lab::DeviceSettings gateway_lock{"PC-A", lab::Gateway};
    CHECK(lab::locked(subject, network, netsim::SetGateway{DeviceId{1}, ip("192.168.1.2")}) == gateway_lock);
    CHECK(!lab::locked(subject, network, netsim::Rename{DeviceId{1}, "Desk"}));
    CHECK(!lab::locked(subject, network, netsim::MoveDevice{DeviceId{1}, 1, 1}));
}

void verdicts_follow_the_network() {
    // The same network gets the same verdict, whatever the learner's run
    // remembers: a probe starts from time zero with empty tables.
    const auto subject = load("missing-gateway");
    const auto reach = goal_index(subject, "reach");
    const auto broken = lab::check(subject, subject.network, nullptr, {});
    CHECK(broken.size() == subject.goals.size() && !lab::all_met(broken));
    CHECK(broken[reach].finding == Finding::NoReply && !broken[reach].met);
    CHECK(lab::check(subject, subject.network, nullptr, {}) == broken);
    const auto words = netsim::explain::verdict_text(subject, broken[reach], subject.network);
    CHECK(words == "PC-A does not reach 192.168.2.20: the request arrives, but PC-B cannot send its reply back: PC-B has no default gateway.");

    const DeviceId pc_a{1}, pc_b{5};
    resend::Workspace workspace(subject.network);
    workspace.set_lab(&subject);
    CHECK(workspace.change(netsim::SetGateway{pc_b, ip("192.168.2.1")}, "Gateway").ok());
    CHECK(workspace.ping(pc_a, ip("192.168.2.20")).ok());
    workspace.run_until(netsim::SimTime(2s));
    const auto repaired = lab::check(subject, workspace.design(), &workspace.simulation(), {});
    CHECK(lab::all_met(repaired));
    CHECK(netsim::explain::verdict_text(subject, repaired[reach], workspace.design()) == "PC-A reaches 192.168.2.20.");

    // Broken again while every table is warm: the probe does not care.
    CHECK(workspace.change(netsim::SetGateway{pc_b, std::nullopt}, "No gateway").ok());
    CHECK(workspace.design() == subject.network);
    CHECK(lab::check(subject, workspace.design(), &workspace.simulation(), {}) == broken);
}

void first_network() {
    // Built as the solution, but without default gateways: the addresses and
    // ports are right, the ping finds no route.
    const auto subject = load("first-network");
    const auto as_given = lab::check(subject, subject.network, nullptr, {});
    for (const auto& verdict : as_given) CHECK(!verdict.met && verdict.finding == Finding::NoDevice);
    CHECK(netsim::explain::verdict_text(subject, as_given[0], subject.network) == "There is no device named PC-A yet.");

    auto network = subject.solution.network;
    for (const auto& name : {"PC-A", "PC-B"})
        CHECK(netsim::edit::apply(network, netsim::SetGateway{network.device_named(name)->id, std::nullopt}) == Problem::None);
    const auto verdicts = lab::check(subject, network, nullptr, {});
    const auto reach = goal_index(subject, "reach");
    for (std::size_t i = 0; i < verdicts.size(); ++i) CHECK(verdicts[i].met == (i != reach));
    CHECK(verdicts[reach].finding == Finding::NoReply && verdicts[reach].reason == Reason::NoRoute);
    CHECK(netsim::explain::verdict_text(subject, verdicts[reach], network) ==
          "PC-A does not reach 192.168.2.10: PC-A's request has no route: PC-A has no default gateway.");
    CHECK(lab::all_met(lab::check(subject, subject.solution.network, nullptr, {})));

    // Devices the learner adds come as out of their boxes.
    netsim::DeviceConfig host;
    host.id = DeviceId{9};
    host.kind = DeviceKind::Host;
    host.ports.resize(1);
    const auto host_edits = lab::new_device_edits(subject, host);
    const netsim::Edit no_address = netsim::SetAddress{PortRef{DeviceId{9}, 0}, std::nullopt};
    CHECK(host_edits.size() == 1 && host_edits[0] == no_address);
    netsim::DeviceConfig router;
    router.id = DeviceId{7};
    router.kind = DeviceKind::Router;
    router.ports.resize(4);
    const auto router_edits = lab::new_device_edits(subject, router);
    const netsim::Edit port_down = netsim::SetPortUp{PortRef{DeviceId{7}, 3}, false};
    CHECK(router_edits.size() == 4 && router_edits[3] == port_down);
    // A lab that says nothing leaves them as they are.
    CHECK(lab::new_device_edits(load("missing-gateway"), router).empty());

    // In the workspace, in the same step as the device: one undo takes both.
    resend::Workspace workspace(subject.network);
    workspace.set_lab(&subject);
    CHECK(workspace.change({netsim::AddDevice{DeviceKind::Host, 0, 0}, netsim::AddDevice{DeviceKind::Router, 0, 0}}, "Add").ok());
    const auto& devices = workspace.scenario().devices;
    CHECK(devices.size() == 2 && !devices[0].ports[0].ipv4 && devices[1].ports.size() == 4);
    CHECK(std::none_of(devices[1].ports.begin(), devices[1].ports.end(), [](const auto& port) { return port.admin_up; }));
    CHECK(workspace.history().size() == 1);
    CHECK(workspace.undo().ok() && workspace.scenario().devices.empty());
    CHECK(workspace.redo().ok() && workspace.scenario().devices.size() == 2 && !workspace.scenario().devices[0].ports[0].ipv4);
    // Without the lab, a device comes as always.
    workspace.set_lab(nullptr);
    CHECK(workspace.change(netsim::AddDevice{DeviceKind::Host, 0, 0}, "Add").ok() && workspace.scenario().devices.back().ports[0].ipv4);
}

void observed_and_answers() {
    const auto subject = load("watch-arp-and-ping");
    const auto run = lab::solution_run(subject);
    lab::Answers answers = subject.solution.answers;
    const auto with_run = lab::check(subject, subject.solution.network, &run, answers);
    CHECK(lab::all_met(with_run));
    // Without a run, nothing is observed.
    const auto without = lab::check(subject, subject.solution.network, nullptr, answers);
    bool unseen = false;
    for (std::size_t i = 0; i < without.size(); ++i) {
        const auto& goal = subject.goals[i];
        if (goal.kind == lab::GoalKind::Observed) {
            CHECK(without[i].finding == Finding::NotSeen && !without[i].met);
            CHECK(!netsim::explain::verdict_text(subject, without[i], subject.solution.network).empty());
            unseen = true;
        }
    }
    CHECK(unseen);
    // Answers: written as the learner may, wrong, or missing.
    for (const auto& goal : subject.goals) {
        if (goal.kind != lab::GoalKind::Answer) continue;
        const auto index = goal_index(subject, goal.id);
        auto given = answers;
        if (goal.quantity == lab::Quantity::FirstHopMac) {
            given[goal.id] = " 02-00-00-00-00-02 ";
            CHECK(lab::check(subject, index, subject.solution.network, &run, given).met);
            given[goal.id] = "02:00:00:00:00:03";
        } else {
            given[goal.id] = "0" + answers.at(goal.id);
            CHECK(lab::check(subject, index, subject.solution.network, &run, given).met);
            given[goal.id] = "1" + answers.at(goal.id);
        }
        auto verdict = lab::check(subject, index, subject.solution.network, &run, given);
        CHECK(!verdict.met && verdict.finding == Finding::WrongAnswer);
        given.erase(goal.id);
        verdict = lab::check(subject, index, subject.solution.network, &run, given);
        CHECK(!verdict.met && verdict.finding == Finding::NoAnswer);
        CHECK(netsim::explain::verdict_text(subject, verdict, subject.solution.network) == "Write your answer.");
    }
}

void kept_and_paths() {
    // The routing loop: R2's routes must stay; the traceroute shows the path.
    const auto subject = load("routing-loop");
    const auto solved = lab::check(subject, subject.solution.network, nullptr, {});
    CHECK(lab::all_met(solved));
    const auto broken = lab::check(subject, subject.network, nullptr, {});
    CHECK(!lab::all_met(broken));
    for (std::size_t i = 0; i < subject.goals.size(); ++i) {
        if (subject.goals[i].kind != lab::GoalKind::Kept) continue;
        auto network = subject.solution.network;
        const auto* r2 = network.device_named(subject.goals[i].kept.front().device);
        CHECK(r2 != nullptr);
        CHECK(netsim::edit::apply(network, netsim::SetRoutes{r2->id, {}}) == Problem::None);
        const auto verdict = lab::check(subject, i, network, nullptr, {});
        CHECK(!verdict.met && verdict.finding == Finding::Changed && verdict.setting == lab::Routes);
        CHECK(contains(netsim::explain::verdict_text(subject, verdict, network), "routes is not as the lab gave it"));
    }

    const auto routes = load("static-routes");
    const auto path = lab::check(routes, routes.network, nullptr, {});
    bool checked = false;
    for (std::size_t i = 0; i < routes.goals.size(); ++i) {
        if (routes.goals[i].kind != lab::GoalKind::Path) continue;
        CHECK(!path[i].met);
        checked = true;
    }
    CHECK(checked);
}
void documented_lab() {
    // The complete labs in docs/lab-format.md read and prove themselves;
    // the other blocks there are parts of one.
    const auto text = read_file(std::filesystem::path(RESEND_DOCS_DIR) / "lab-format.md");
    int count = 0;
    for (auto start = text.find("```json\n"); start != std::string::npos; start = text.find("```json\n", start)) {
        start += 8;
        const auto end = text.find("```", start);
        CHECK(end != std::string::npos);
        const auto block = text.substr(start, end - start);
        start = end;
        if (!block.starts_with("{\n  \"format\": \"resend-lab\"")) continue;
        std::string error;
        const auto loaded = lab::lab_from_json(block, error);
        if (!loaded) std::cerr << "docs/lab-format.md: " << error << '\n';
        CHECK(loaded.has_value());
        const auto issues = lab::verify(*loaded);
        for (const auto& issue : issues) std::cerr << "docs/lab-format.md: " << issue << '\n';
        CHECK(issues.empty());
        ++count;
    }
    CHECK(count >= 1);
}

} // namespace

int main() {
    shipped_labs();
    file_errors();
    locks_refuse_edits();
    lock_texts();
    verdicts_follow_the_network();
    first_network();
    observed_and_answers();
    kept_and_paths();
    documented_lab();
    std::cout << "lab tests passed (" << test::checks << " checks)\n";
}
