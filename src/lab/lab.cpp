#include "lab/lab.hpp"

#include "term/terminal.hpp"

#include "core/json.hpp"
#include "core/json_reader.hpp"
#include "core/text.hpp"
#include "explain/explain.hpp"
#include "scenario/scenario_json.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace netsim::lab {
namespace {
using json::Value;
using text::tr;

constexpr std::pair<Kind, std::string_view> kinds[] = {
    {Kind::Guided, "guided"}, {Kind::Challenge, "challenge"}, {Kind::Troubleshooting, "troubleshooting"}};
constexpr std::pair<GoalKind, std::string_view> goal_kinds[] = {
    {GoalKind::Reach, "reach"}, {GoalKind::Path, "path"},     {GoalKind::Config, "config"},
    {GoalKind::Observed, "observed"}, {GoalKind::Answer, "answer"}, {GoalKind::Kept, "kept"}, {GoalKind::Command, "command"}};
constexpr std::pair<Setting, std::string_view> settings[] = {
    {Name, "name"}, {Power, "power"}, {Addresses, "addresses"}, {Gateway, "gateway"},
    {Routes, "routes"}, {Ports, "ports"}, {Cables, "cables"}};
constexpr std::pair<Observation, std::string_view> observations[] = {
    {Observation::ArpRequest, "arp-request"}, {Observation::ArpReply, "arp-reply"}, {Observation::Routed, "routed"},
    {Observation::EchoReply, "echo-reply"}, {Observation::MacLearned, "mac-learned"}};
constexpr std::pair<Quantity, std::string_view> quantities[] = {
    {Quantity::ReplyTtl, "reply-ttl"}, {Quantity::Hops, "hops"}, {Quantity::FirstHopMac, "first-hop-mac"}};

template <typename T, std::size_t N>
std::optional<T> find_named(const std::pair<T, std::string_view> (&table)[N], std::string_view text) {
    for (const auto& [value, label] : table)
        if (label == text) return value;
    return std::nullopt;
}
template <typename T, std::size_t N>
std::string_view label_of(const std::pair<T, std::string_view> (&table)[N], T value) {
    for (const auto& [item, label] : table)
        if (item == value) return label;
    return {};
}
template <typename T, std::size_t N>
std::string choices(const std::pair<T, std::string_view> (&table)[N]) {
    std::string out;
    for (std::size_t i = 0; i < N; ++i) out += (i ? (i + 1 == N ? " or " : ", ") : "") + std::string("\"") + std::string(table[i].second) + "\"";
    return out;
}

class LabReader : public json::Reader {
public:
    bool texts(const Value& object, std::string_view key, const std::string& path, std::vector<std::string>& out) {
        const auto* value = object.find(key);
        if (!value) return true;
        const auto where = path + "." + std::string(key);
        if (!value->array_items()) return fail(where, tr("expected an array"));
        for (std::size_t i = 0; i < value->array_items()->size(); ++i) {
            const auto* item = (*value->array_items())[i].string();
            if (!item) return fail(where + "[" + std::to_string(i) + "]", tr("expected a string"));
            out.push_back(*item);
        }
        return true;
    }
    bool address(const Value& object, std::string_view key, const std::string& path, Ipv4Address& out, bool required = true) {
        std::string value;
        if (!text(object, key, path, value, required)) return false;
        if (!object.find(key)) return true;
        const auto parsed = Ipv4Address::parse(value);
        if (!parsed) return fail(path + "." + std::string(key), tr("expected an IPv4 address such as 192.168.1.1"));
        out = *parsed;
        return true;
    }
    bool settings_of(const Value& object, const std::string& path, Settings& out) {
        const auto* value = object.find("settings");
        if (!value) {
            out = all_settings;
            return true;
        }
        const auto where = path + ".settings";
        if (!value->array_items() || value->array_items()->empty()) return fail(where, tr("expected an array"));
        out = 0;
        for (std::size_t i = 0; i < value->array_items()->size(); ++i) {
            const auto* item = (*value->array_items())[i].string();
            const auto setting = item ? find_named(settings, *item) : std::nullopt;
            if (!setting) return fail(where + "[" + std::to_string(i) + "]", tr("expected one of {0}", {choices(settings)}));
            out |= *setting;
        }
        return true;
    }
    bool devices(const Value& object, std::string_view key, const std::string& path, std::vector<DeviceSettings>& out) {
        const auto* value = object.find(key);
        if (!value) return true;
        const auto where = path + "." + std::string(key);
        if (!value->array_items()) return fail(where, tr("expected an array"));
        for (std::size_t i = 0; i < value->array_items()->size(); ++i) {
            const auto& item = (*value->array_items())[i];
            const auto item_path = where + "[" + std::to_string(i) + "]";
            DeviceSettings entry;
            if (!this->object(item, item_path) || !only(item, item_path, {"device", "settings"}) ||
                !text(item, "device", item_path, entry.device) || !settings_of(item, item_path, entry.settings))
                return false;
            out.push_back(std::move(entry));
        }
        return true;
    }
};

bool read_goal(LabReader& read, const Value& item, const std::string& path, Goal& goal) {
    if (!read.object(item, path)) return false;
    std::string kind;
    if (!read.text(item, "id", path, goal.id) || !read.text(item, "kind", path, kind) || !read.text(item, "text", path, goal.text))
        return false;
    const auto parsed = find_named(goal_kinds, kind);
    if (!parsed) return read.fail(path + ".kind", tr("expected one of {0}", {choices(goal_kinds)}));
    goal.kind = *parsed;
    switch (goal.kind) {
    case GoalKind::Reach: {
        std::string expect;
        if (!read.only(item, path, {"id", "kind", "text", "from", "to", "expect"}) || !read.text(item, "from", path, goal.from) ||
            !read.address(item, "to", path, goal.to) || !read.text(item, "expect", path, expect, false))
            return false;
        if (item.find("expect")) {
            goal.expect = reason_named(expect);
            if (!goal.expect || *goal.expect == Reason::None)
                return read.fail(path + ".expect", tr("expected the name of a reason, such as \"NoRoute\""));
        }
        return true;
    }
    case GoalKind::Path: {
        if (!read.only(item, path, {"id", "kind", "text", "from", "to", "hops"}) || !read.text(item, "from", path, goal.from) ||
            !read.address(item, "to", path, goal.to))
            return false;
        std::vector<std::string> hops;
        if (!read.member(item, "hops", path) || !read.texts(item, "hops", path, hops)) return false;
        for (std::size_t i = 0; i < hops.size(); ++i) {
            const auto hop = Ipv4Address::parse(hops[i]);
            if (!hop) return read.fail(path + ".hops[" + std::to_string(i) + "]", tr("expected an IPv4 address such as 192.168.1.1"));
            goal.hops.push_back(*hop);
        }
        if (goal.hops.empty()) return read.fail(path + ".hops", tr("expected at least one address"));
        return true;
    }
    case GoalKind::Config: {
        if (!read.only(item, path, {"id", "kind", "text", "device", "address_in", "port", "gateway", "route", "via", "up", "cable_to"}) ||
            !read.text(item, "device", path, goal.device))
            return false;
        const char* predicates[] = {"address_in", "gateway", "route", "up", "cable_to"};
        const auto count = std::count_if(std::begin(predicates), std::end(predicates), [&](const char* key) { return item.find(key) != nullptr; });
        if (count != 1)
            return read.fail(path, tr("expected exactly one of \"address_in\", \"gateway\", \"route\", \"up\" or \"cable_to\""));
        std::int64_t port = 1;
        if (!read.number(item, "port", path, 1, max_router_ports, port, false)) return false;
        goal.port = static_cast<std::uint8_t>(port - 1);
        if (item.find("address_in")) {
            goal.check = ConfigCheck::AddressIn;
            std::string text;
            if (!read.text(item, "address_in", path, text)) return false;
            const auto prefix = Ipv4Prefix::parse(text);
            if (!prefix) return read.fail(path + ".address_in", tr("expected a network with its prefix, such as 10.1.0.0/16"));
            goal.prefix = *prefix;
            if (!item.find("port")) goal.port = 0xff;  // any port
        } else if (item.find("gateway")) {
            goal.check = ConfigCheck::Gateway;
            Ipv4Address gateway;
            if (!read.address(item, "gateway", path, gateway)) return false;
            goal.address = gateway;
        } else if (item.find("route")) {
            goal.check = ConfigCheck::Route;
            std::string text;
            if (!read.text(item, "route", path, text)) return false;
            const auto prefix = Ipv4Prefix::parse(text);
            if (!prefix) return read.fail(path + ".route", tr("expected a network with its prefix, such as 10.1.0.0/16"));
            goal.prefix = *prefix;
            if (item.find("via")) {
                Ipv4Address via;
                if (!read.address(item, "via", path, via)) return false;
                goal.address = via;
            }
        } else if (item.find("up")) {
            goal.check = ConfigCheck::PortUp;
            if (!item.find("port")) return read.fail(path, tr("missing member \"{0}\"", {"port"}));
            if (!read.flag(item, "up", path, goal.up)) return false;
        } else {
            goal.check = ConfigCheck::Cable;
            if (!read.text(item, "cable_to", path, goal.other)) return false;
        }
        if (goal.check != ConfigCheck::Route && item.find("via")) return read.fail(path + ".via", tr("only a route has a next hop"));
        return true;
    }
    case GoalKind::Observed: {
        std::string record;
        if (!read.only(item, path, {"id", "kind", "text", "device", "record", "for"}) || !read.text(item, "device", path, goal.device) ||
            !read.text(item, "record", path, record))
            return false;
        const auto observation = find_named(observations, record);
        if (!observation) return read.fail(path + ".record", tr("expected one of {0}", {choices(observations)}));
        goal.observation = *observation;
        if (item.find("for")) {
            if (goal.observation != Observation::ArpRequest) return read.fail(path + ".for", tr("only an ARP request asks for an address"));
            Ipv4Address target;
            if (!read.address(item, "for", path, target)) return false;
            goal.address = target;
        }
        return true;
    }
    case GoalKind::Answer: {
        std::string quantity;
        if (!read.only(item, path, {"id", "kind", "text", "from", "to", "quantity"}) || !read.text(item, "from", path, goal.from) ||
            !read.address(item, "to", path, goal.to) || !read.text(item, "quantity", path, quantity))
            return false;
        const auto parsed_quantity = find_named(quantities, quantity);
        if (!parsed_quantity) return read.fail(path + ".quantity", tr("expected one of {0}", {choices(quantities)}));
        goal.quantity = *parsed_quantity;
        return true;
    }
    case GoalKind::Kept:
        if (!read.only(item, path, {"id", "kind", "text", "devices"}) || !read.member(item, "devices", path) ||
            !read.devices(item, "devices", path, goal.kept))
            return false;
        if (goal.kept.empty()) return read.fail(path + ".devices", tr("expected at least one device"));
        return true;
    case GoalKind::Command:
        if (!read.only(item, path, {"id", "kind", "text", "device", "command"}) || !read.text(item, "device", path, goal.device) ||
            !read.text(item, "command", path, goal.command))
            return false;
        if (!term::canonical(goal.command)) return read.fail(path + ".command", tr("expected a command of the terminal, such as ip neigh"));
        return true;
    }
    return false;
}

Value devices_value(const std::vector<DeviceSettings>& list) {
    Value out = Value::array();
    for (const auto& entry : list) {
        Value item = Value::object();
        item.set("device", entry.device);
        if (entry.settings != all_settings) {
            Value names = Value::array();
            for (const auto setting : setting_list)
                if (entry.settings & setting) names.push(std::string(name(setting)));
            item.set("settings", std::move(names));
        }
        out.push(std::move(item));
    }
    return out;
}

Value goal_value(const Goal& goal) {
    Value item = Value::object();
    item.set("id", goal.id);
    item.set("kind", std::string(label_of(goal_kinds, goal.kind)));
    item.set("text", goal.text);
    switch (goal.kind) {
    case GoalKind::Reach:
        item.set("from", goal.from);
        item.set("to", goal.to.to_string());
        if (goal.expect) item.set("expect", std::string(explain::name(*goal.expect)));
        break;
    case GoalKind::Path: {
        item.set("from", goal.from);
        item.set("to", goal.to.to_string());
        Value hops = Value::array();
        for (const auto hop : goal.hops) hops.push(hop.to_string());
        item.set("hops", std::move(hops));
        break;
    }
    case GoalKind::Config:
        item.set("device", goal.device);
        switch (goal.check) {
        case ConfigCheck::AddressIn:
            if (goal.port != 0xff) item.set("port", goal.port + 1);
            item.set("address_in", goal.prefix.to_string());
            break;
        case ConfigCheck::Gateway: item.set("gateway", goal.address->to_string()); break;
        case ConfigCheck::Route:
            item.set("route", goal.prefix.to_string());
            if (goal.address) item.set("via", goal.address->to_string());
            break;
        case ConfigCheck::PortUp:
            item.set("port", goal.port + 1);
            item.set("up", goal.up);
            break;
        case ConfigCheck::Cable: item.set("cable_to", goal.other); break;
        }
        break;
    case GoalKind::Observed:
        item.set("device", goal.device);
        item.set("record", std::string(label_of(observations, goal.observation)));
        if (goal.address) item.set("for", goal.address->to_string());
        break;
    case GoalKind::Answer:
        item.set("from", goal.from);
        item.set("to", goal.to.to_string());
        item.set("quantity", std::string(label_of(quantities, goal.quantity)));
        break;
    case GoalKind::Kept: item.set("devices", devices_value(goal.kept)); break;
    case GoalKind::Command:
        item.set("device", goal.device);
        item.set("command", goal.command);
        break;
    }
    return item;
}

Value texts_value(const std::vector<std::string>& texts) {
    Value out = Value::array();
    for (const auto& item : texts) out.push(item);
    return out;
}
} // namespace

std::string_view name(Kind kind) { return label_of(kinds, kind); }
std::string_view name(Setting setting) { return label_of(settings, setting); }

std::optional<Reason> reason_named(std::string_view text) {
    for (int value = 0; value <= static_cast<int>(Reason::DirectedBroadcast); ++value) {
        const auto reason = static_cast<Reason>(value);
        if (explain::name(reason) == text) return reason;
    }
    return std::nullopt;
}

std::optional<Lab> lab_from_json(std::string_view text, std::string& error) {
    json::Error syntax;
    const auto document = json::parse(text, syntax);
    if (!document) {
        error = tr("line {0}, column {1}", {std::to_string(syntax.line), std::to_string(syntax.column)}) + ": " + syntax.message;
        return std::nullopt;
    }
    LabReader read;
    const auto finish = [&]() -> std::optional<Lab> {
        error = read.error;
        return std::nullopt;
    };
    if (!read.object(*document, "file")) return finish();
    std::string format, kind;
    std::int64_t version = 0;
    if (!read.text(*document, "format", "file", format)) return finish();
    if (format != "resend-lab") {
        read.fail("format", tr("this is not a resend lab file"));
        return finish();
    }
    if (!read.number(*document, "version", "file", 1, std::numeric_limits<std::int32_t>::max(), version)) return finish();
    if (version > lab_format_version) {
        read.fail("version", tr("this file needs a newer version of resend (format {0})", {std::to_string(version)}));
        return finish();
    }
    if (!read.only(*document, "file", {"format", "version", "kind", "topic", "title", "brief", "network", "names", "new_devices",
                                       "locks", "goals", "steps", "hints", "solution"}))
        return finish();
    Lab lab;
    if (!read.text(*document, "kind", "file", kind) || !read.text(*document, "topic", "file", lab.topic) ||
        !read.text(*document, "title", "file", lab.title) || !read.text(*document, "brief", "file", lab.brief) ||
        !read.texts(*document, "names", "file", lab.names) || !read.texts(*document, "steps", "file", lab.steps) ||
        !read.texts(*document, "hints", "file", lab.hints) || !read.devices(*document, "locks", "file", lab.locks))
        return finish();
    const auto parsed_kind = find_named(kinds, kind);
    if (!parsed_kind) {
        read.fail("kind", tr("expected one of {0}", {choices(kinds)}));
        return finish();
    }
    lab.kind = *parsed_kind;

    const auto* network = read.member(*document, "network", "file");
    if (!network) return finish();
    // The networks are written as scenario files of the current version are.
    auto start = scenario_from_value(*network, scenario_format_version, "network", error);
    if (!start) return std::nullopt;
    lab.network = std::move(*start);

    if (const auto* defaults = document->find("new_devices")) {
        if (!read.object(*defaults, "new_devices") || !read.only(*defaults, "new_devices", {"host_address", "router_ports_up"}) ||
            !read.flag(*defaults, "host_address", "new_devices", lab.new_devices.host_address) ||
            !read.flag(*defaults, "router_ports_up", "new_devices", lab.new_devices.router_ports_up))
            return finish();
    }

    const auto* goals = read.member(*document, "goals", "file");
    if (!goals) return finish();
    if (!goals->array_items() || goals->array_items()->empty()) {
        read.fail("goals", tr("expected at least one goal"));
        return finish();
    }
    for (std::size_t index = 0; index < goals->array_items()->size(); ++index) {
        Goal goal;
        if (!read_goal(read, (*goals->array_items())[index], "goals[" + std::to_string(index) + "]", goal)) return finish();
        lab.goals.push_back(std::move(goal));
    }

    const auto* solution = read.member(*document, "solution", "file");
    if (!solution) return finish();
    if (!read.object(*solution, "solution") || !read.only(*solution, "solution", {"network", "commands", "answers"})) return finish();
    const auto* solved = read.member(*solution, "network", "solution");
    if (!solved) return finish();
    auto solved_network = scenario_from_value(*solved, scenario_format_version, "solution.network", error);
    if (!solved_network) return std::nullopt;
    lab.solution.network = std::move(*solved_network);
    if (const auto* commands = solution->find("commands")) {
        if (!commands->array_items()) {
            read.fail("solution.commands", tr("expected an array"));
            return finish();
        }
        for (std::size_t index = 0; index < commands->array_items()->size(); ++index) {
            const auto& item = (*commands->array_items())[index];
            const auto path = "solution.commands[" + std::to_string(index) + "]";
            Command command;
            if (!read.object(item, path) || !read.only(item, path, {"from", "ping", "traceroute"}) || !read.text(item, "from", path, command.from))
                return finish();
            if ((item.find("ping") != nullptr) == (item.find("traceroute") != nullptr)) {
                read.fail(path, tr("expected exactly one of \"ping\" or \"traceroute\""));
                return finish();
            }
            command.traceroute = item.find("traceroute") != nullptr;
            if (!read.address(item, command.traceroute ? "traceroute" : "ping", path, command.to)) return finish();
            lab.solution.commands.push_back(std::move(command));
        }
    }
    if (const auto* answers = solution->find("answers")) {
        if (!read.object(*answers, "solution.answers")) return finish();
        for (const auto& [key, value] : *answers->members()) {
            if (!value.string()) {
                read.fail("solution.answers." + key, tr("expected a string"));
                return finish();
            }
            lab.solution.answers[key] = *value.string();
        }
    }
    return lab;
}

std::string lab_to_json(const Lab& lab) {
    Value root = Value::object();
    root.set("format", "resend-lab");
    root.set("version", lab_format_version);
    root.set("kind", std::string(name(lab.kind)));
    root.set("topic", lab.topic);
    root.set("title", lab.title);
    root.set("brief", lab.brief);
    root.set("network", scenario_to_value(lab.network));
    if (!lab.names.empty()) root.set("names", texts_value(lab.names));
    if (lab.new_devices != NewDevices{}) {
        Value defaults = Value::object();
        defaults.set("host_address", lab.new_devices.host_address);
        defaults.set("router_ports_up", lab.new_devices.router_ports_up);
        root.set("new_devices", std::move(defaults));
    }
    if (!lab.locks.empty()) root.set("locks", devices_value(lab.locks));
    Value goals = Value::array();
    for (const auto& goal : lab.goals) goals.push(goal_value(goal));
    root.set("goals", std::move(goals));
    if (!lab.steps.empty()) root.set("steps", texts_value(lab.steps));
    if (!lab.hints.empty()) root.set("hints", texts_value(lab.hints));
    Value solution = Value::object();
    solution.set("network", scenario_to_value(lab.solution.network));
    if (!lab.solution.commands.empty()) {
        Value commands = Value::array();
        for (const auto& command : lab.solution.commands) {
            Value item = Value::object();
            item.set("from", command.from);
            item.set(command.traceroute ? "traceroute" : "ping", command.to.to_string());
            commands.push(std::move(item));
        }
        solution.set("commands", std::move(commands));
    }
    if (!lab.solution.answers.empty()) {
        Value answers = Value::object();
        for (const auto& [key, value] : lab.solution.answers) answers.set(key, value);
        solution.set("answers", std::move(answers));
    }
    root.set("solution", std::move(solution));
    return json::write(root);
}

std::vector<std::string> validate(const Lab& lab) {
    std::vector<std::string> issues;
    const auto report = [&](std::string issue) { issues.push_back(std::move(issue)); };
    for (const auto& [network, label] : {std::pair{&lab.network, "network"}, std::pair{&lab.solution.network, "solution.network"}})
        for (const auto& issue : netsim::validate(*network))
            report(std::string(label) + ": " + explain::describe(issue.problem) + (issue.subject.empty() ? "" : " (" + issue.subject + ")"));

    std::set<std::string> known(lab.names.begin(), lab.names.end());
    for (const auto& device : lab.network.devices) known.insert(device.name);
    const auto named = [&](const std::string& device, const std::string& where) {
        if (!known.contains(device)) report(where + ": no device is named \"" + device + "\" in the network or in \"names\"");
    };
    for (const auto& lock : lab.locks) {
        if (!lab.network.device_named(lock.device)) report("locks: \"" + lock.device + "\" is not in the network the lab starts from");
    }
    std::set<std::string> ids;
    for (const auto& goal : lab.goals) {
        const auto where = "goal \"" + goal.id + "\"";
        if (goal.id.empty()) report("a goal has no id");
        if (!ids.insert(goal.id).second) report(where + ": the id is used twice");
        if (goal.text.empty()) report(where + ": no text");
        switch (goal.kind) {
        case GoalKind::Reach:
        case GoalKind::Path:
        case GoalKind::Answer: named(goal.from, where); break;
        case GoalKind::Config:
            named(goal.device, where);
            if (goal.check == ConfigCheck::Cable) named(goal.other, where);
            break;
        case GoalKind::Observed:
        case GoalKind::Command: named(goal.device, where); break;
        case GoalKind::Kept:
            for (const auto& item : goal.kept)
                if (!lab.network.device_named(item.device)) report(where + ": \"" + item.device + "\" is not in the network the lab starts from");
            break;
        }
        const bool asks = goal.kind == GoalKind::Answer;
        if (asks && !lab.solution.answers.contains(goal.id)) report(where + ": the solution does not answer it");
        if (goal.kind == GoalKind::Command && !lab.solution.answers.contains(goal.id))
            report(where + ": the solution does not say the command it types");
    }
    for (const auto& [key, value] : lab.solution.answers) {
        const auto it = std::find_if(lab.goals.begin(), lab.goals.end(), [&](const Goal& goal) { return goal.id == key; });
        if (it == lab.goals.end() || (it->kind != GoalKind::Answer && it->kind != GoalKind::Command))
            report("solution.answers: \"" + key + "\" is not a goal that asks a question or a command");
        else if (it->kind == GoalKind::Command && !command_matches(it->command, value))
            report("solution.answers: \"" + value + "\" is not the command goal \"" + key + "\" asks for");
    }
    for (const auto& command : lab.solution.commands) {
        const auto* device = lab.solution.network.device_named(command.from);
        if (!device || device->kind != DeviceKind::Host) report("solution.commands: \"" + command.from + "\" is not a host of the solution");
    }
    if (lab.title.empty()) report("the lab has no title");
    if (lab.brief.empty()) report("the lab has no brief");
    return issues;
}

} // namespace netsim::lab
