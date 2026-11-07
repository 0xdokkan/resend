#include "check.hpp"
#include "core/json.hpp"
#include "core/rng.hpp"
#include "scenario/edit.hpp"
#include "scenario/scenario_json.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

namespace {
using namespace std::chrono_literals;
using netsim::DeviceId;
using netsim::DeviceKind;
using netsim::LinkId;
using netsim::Problem;
using netsim::Scenario;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool has(const std::vector<netsim::Issue>& issues, Problem problem, const std::string& subject = {}) {
    return std::any_of(issues.begin(), issues.end(), [&](const auto& issue) {
        return issue.problem == problem && (subject.empty() || issue.subject == subject);
    });
}

void json_values() {
    netsim::json::Error error;
    const auto parse = [&](std::string_view text) { return netsim::json::parse(text, error); };

    const auto value = parse(R"( {"a": [1, -2, true, false, null, "x"], "b": {}, "c": [] } )");
    CHECK(value && value->members() && value->members()->size() == 3);
    CHECK(value->members()->front().first == "a");
    const auto& items = *value->find("a")->array_items();
    CHECK(items.size() == 6 && *items[0].integer() == 1 && *items[1].integer() == -2);
    CHECK(*items[2].boolean() && !*items[3].boolean() && items[4].is_null() && *items[5].string() == "x");
    CHECK(!value->find("missing"));

    // Whole numbers across the full signed 64-bit range, nothing more.
    CHECK(*parse("9223372036854775807")->integer() == INT64_MAX);
    CHECK(*parse("-9223372036854775808")->integer() == INT64_MIN);
    CHECK(*parse("-0")->integer() == 0);
    for (const auto* bad : {"9223372036854775808", "-9223372036854775809", "1.5", "1e3", "01", "-", "+1", ".5"})
        CHECK(!parse(bad));

    // Strings: escapes, \u with surrogate pairs, raw UTF-8; nothing invalid.
    CHECK(*parse(R"("a\"\\\/\b\f\n\r\t")")->string() == "a\"\\/\b\f\n\r\t");
    CHECK(*parse(R"("\u00e9\u20ac\ud83d\ude00")")->string() == "\u00e9\u20ac\U0001F600");
    CHECK(*parse("\"caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80\"")->string() == "caf\u00e9 \u20ac \U0001F600");
    for (const auto* bad : {R"("\ud83d")", R"("\ude00")", R"("\ud83d\u0041")", R"("\x")", R"("\u12")", "\"tab\there\"",
                            "\"\xc3\"", "\"\xc0\xaf\"", "\"\xed\xa0\x80\"", "\"\xf5\x80\x80\x80\"", "\"\xff\"", "\"open"})
        CHECK(!parse(bad));

    // Structure.
    for (const auto* bad : {"", " ", "[1,]", "{\"a\":1,}", "[1 2]", "{\"a\" 1}", "{a:1}", "{\"a\":1,\"a\":2}", "[1]]",
                            "nul", "tru", "{\"a\":1} x", "[", "{"})
        CHECK(!parse(bad));
    CHECK(parse("[]").has_value() && parse("{}").has_value() && parse(" \t\r\n[ ] ").has_value());

    // Errors say where: line and column, counted from 1.
    CHECK(!parse("{\n  \"a\": 1,\n  \"b\": tru\n}"));
    CHECK(error.line == 3 && error.column == 8 && error.message == "expected a value");
    CHECK(!parse("[1,\n 2,\n 3.5]"));
    CHECK(error.line == 3 && error.column == 3 && error.message == "only whole numbers are allowed");

    // At most 64 levels of nesting.
    CHECK(parse(std::string(64, '[') + std::string(64, ']')).has_value());
    CHECK(!parse(std::string(65, '[') + std::string(65, ']')).has_value());
    CHECK(error.message == "nesting is too deep");

    // Writing: member order kept, two-space indentation, escapes, final newline.
    netsim::json::Value root = netsim::json::Value::object();
    root.set("z", 1);
    root.set("a", "line\nbreak \"quoted\" \x01");
    root.set("list", netsim::json::Value::array()).push(true);
    root.set("empty", netsim::json::Value::object());
    const auto text = netsim::json::write(root);
    CHECK(text == "{\n  \"z\": 1,\n  \"a\": \"line\\nbreak \\\"quoted\\\" \\u0001\",\n  \"list\": [\n    true\n  ],\n  \"empty\": {}\n}\n");
    CHECK(parse(text) == root);
}

void random_numbers() {
    // The same seed gives the same numbers everywhere: mt19937_64's output is
    // fixed by the standard (its 10000th value from the default seed is
    // 9981545732273789042), and the range mapping is ours.
    netsim::Rng standard(5489);
    for (int i = 1; i < 10000; ++i) standard.next();
    CHECK(standard.next() == 9981545732273789042ULL);

    netsim::Rng a(42), b(42), c(43);
    bool differs = false;
    for (int i = 0; i < 100; ++i) {
        const auto x = a.below(1000);
        CHECK(x == b.below(1000) && x < 1000);
        differs = differs || x != c.below(1000);
    }
    CHECK(differs);
    netsim::Rng range(7);
    int low = 0, high = 0;
    for (int i = 0; i < 2000; ++i) {
        const auto value = range.between(-3, 3);
        CHECK(value >= -3 && value <= 3);
        low += value == -3;
        high += value == 3;
    }
    CHECK(low > 0 && high > 0);
    netsim::Rng coin(1);
    int heads = 0;
    for (int i = 0; i < 1000; ++i) heads += coin.chance(1, 2);
    CHECK(heads > 400 && heads < 600);
    CHECK(!coin.chance(0, 5) && coin.chance(5, 5));
}

void validation_rules() {
    CHECK(netsim::validate(netsim::sample_lan()).empty());
    CHECK(netsim::validate(Scenario{}).empty());
    const auto broken = [](auto change) {
        auto scenario = netsim::sample_lan();
        change(scenario);
        return netsim::validate(scenario);
    };

    CHECK(has(broken([](Scenario& s) { s.devices[1].id = DeviceId{1}; }), Problem::DuplicateDeviceId, "PC-B"));
    CHECK(has(broken([](Scenario& s) { s.devices[1].id = DeviceId{0}; }), Problem::InvalidDeviceId, "PC-B"));
    CHECK(has(broken([](Scenario& s) { s.devices[0].name = ""; }), Problem::InvalidName, "device 1"));
    CHECK(has(broken([](Scenario& s) { s.devices[0].name = "tab\there"; }), Problem::InvalidName));
    CHECK(has(broken([](Scenario& s) { s.devices[0].name = std::string(49, 'x'); }), Problem::InvalidName));
    CHECK(has(broken([](Scenario& s) { s.devices[2].ports.pop_back(); }), Problem::WrongPortCount, "Switch-01"));
    CHECK(has(broken([](Scenario& s) { s.devices[0].ports[0].ipv4.reset(); }), Problem::MissingAddress, "PC-A"));
    CHECK(has(broken([](Scenario& s) { s.devices[0].ports[0].ipv4 = netsim::Ipv4Prefix::parse("192.168.1.0/24"); }),
              Problem::InvalidHostAddress, "PC-A 192.168.1.0/24"));
    CHECK(has(broken([](Scenario& s) { s.devices[0].ports[0].ipv4 = netsim::Ipv4Prefix::parse("192.168.1.255/24"); }),
              Problem::InvalidHostAddress));
    CHECK(has(broken([](Scenario& s) { s.devices[0].ports[0].ipv4 = netsim::Ipv4Prefix::parse("127.0.0.1/8"); }),
              Problem::InvalidHostAddress));
    CHECK(has(broken([](Scenario& s) { s.devices[0].ports[0].ipv4 = netsim::Ipv4Prefix::parse("10.0.0.1/0"); }),
              Problem::InvalidHostAddress));
    CHECK(has(broken([](Scenario& s) { s.devices[2].ports[3].ipv4 = netsim::Ipv4Prefix::parse("10.0.0.1/8"); }),
              Problem::AddressOnSwitch, "Switch-01"));
    CHECK(has(broken([](Scenario& s) { s.links[1].id = LinkId{1}; }), Problem::DuplicateLinkId,
              "PC-B port 1 — Switch-01 port 2"));
    CHECK(has(broken([](Scenario& s) { s.links[1].id = LinkId{0}; }), Problem::InvalidLinkId));
    CHECK(has(broken([](Scenario& s) { s.links[1].a.device = DeviceId{9}; }), Problem::NoSuchDevice,
              "device 9 port 1 — Switch-01 port 2"));
    CHECK(has(broken([](Scenario& s) { s.links[1].b.port = 8; }), Problem::NoSuchPort));
    CHECK(has(broken([](Scenario& s) { s.links[1].b.port = 0; }), Problem::PortInUse, "Switch-01 port 1"));
    CHECK(has(broken([](Scenario& s) { s.links[1].a = {DeviceId{3}, 5}; }), Problem::SameDevice));
    CHECK(has(broken([](Scenario& s) { s.links[1].delay = 0ns; }), Problem::InvalidDelay));
    CHECK(has(broken([](Scenario& s) { s.links[1].delay = 11s; }), Problem::InvalidDelay));
    CHECK(broken([](Scenario& s) { s.links[1].delay = 1us; }).empty());
    CHECK(broken([](Scenario& s) { s.links[1].delay = 10s; }).empty());
    CHECK(has(broken([](Scenario& s) { s.links.push_back({LinkId{3}, {DeviceId{3}, 4}, {DeviceId{3}, 5}}); }),
              Problem::SameDevice));
    // A second cable between the switch and a third switch closes no loop;
    // a cable back to the first switch does, even while it is down.
    CHECK(has(broken([](Scenario& s) {
              s.devices.push_back({DeviceId{4}, DeviceKind::Switch, "Switch-02", true, netsim::default_ports(DeviceKind::Switch, DeviceId{4})});
              s.links.push_back({LinkId{3}, {DeviceId{3}, 6}, {DeviceId{4}, 0}});
              s.links.push_back({LinkId{4}, {DeviceId{4}, 1}, {DeviceId{3}, 7}, 10ms, false});
          }),
              Problem::LoopNotSupported, "Switch-02 port 2 — Switch-01 port 8"));
    CHECK(has(broken([](Scenario& s) {
              for (std::uint32_t id = 4; id <= 101; ++id)
                  s.devices.push_back({DeviceId{id}, DeviceKind::Switch, "S" + std::to_string(id), true,
                                       netsim::default_ports(DeviceKind::Switch, DeviceId{id})});
          }),
              Problem::TooManyDevices, "101"));
    // Valid but questionable networks pass: two PCs with one address,
    // masks that disagree, /31 and /32 interfaces.
    CHECK(broken([](Scenario& s) { s.devices[1].ports[0].ipv4 = s.devices[0].ports[0].ipv4; }).empty());
    CHECK(broken([](Scenario& s) { s.devices[1].ports[0].ipv4 = netsim::Ipv4Prefix::parse("192.168.1.20/16"); }).empty());
    CHECK(broken([](Scenario& s) { s.devices[0].ports[0].ipv4 = netsim::Ipv4Prefix::parse("10.0.0.0/31"); }).empty());
    CHECK(broken([](Scenario& s) { s.devices[0].ports[0].ipv4 = netsim::Ipv4Prefix::parse("10.0.0.7/32"); }).empty());
}

void edits() {
    namespace edit = netsim::edit;
    Scenario scenario;
    DeviceId a{}, b{}, sw{};
    CHECK(edit::add_device(scenario, DeviceKind::Host, 10, 20, &a) == Problem::None);
    CHECK(edit::add_device(scenario, DeviceKind::Host, 30, 40, &b) == Problem::None);
    CHECK(edit::add_device(scenario, DeviceKind::Switch, 50, 60, &sw) == Problem::None);
    CHECK(a == DeviceId{1} && b == DeviceId{2} && sw == DeviceId{3});
    CHECK(scenario.device(a)->name == "PC-1" && scenario.device(sw)->name == "Switch-3");
    CHECK(scenario.device(a)->ports[0].ipv4->to_string() == "192.168.1.10/24");
    CHECK(scenario.device(b)->ports[0].ipv4->to_string() == "192.168.1.11/24");
    CHECK(scenario.device(sw)->ports.size() == 8 && !scenario.device(sw)->ports[0].ipv4);
    CHECK(scenario.device(a)->x == 10 && scenario.device(a)->y == 20);

    LinkId first{}, second{};
    CHECK(edit::connect(scenario, a, sw, &first) == Problem::None);
    CHECK(edit::connect(scenario, b, sw, &second) == Problem::None);
    CHECK((scenario.link(second)->b == netsim::PortRef{sw, 1}));
    CHECK(scenario.link(second)->delay == 10ms && scenario.link(second)->up);
    DeviceId c{};
    CHECK(edit::add_device(scenario, DeviceKind::Host, 0, 0, &c) == Problem::None);
    const auto before = scenario;
    // Every refused edit leaves the scenario as it was.
    CHECK(edit::connect(scenario, a, c) == Problem::NoFreePort);
    CHECK(edit::connect(scenario, a, b) == Problem::LoopNotSupported);
    CHECK(edit::connect(scenario, a, a) == Problem::SameDevice);
    CHECK(edit::connect(scenario, a, DeviceId{42}) == Problem::NoSuchDevice);
    CHECK(edit::connect(scenario, {a, 0}, {sw, 2}) == Problem::PortInUse);
    CHECK(edit::connect(scenario, {sw, 9}, {b, 0}) == Problem::NoSuchPort);
    CHECK(edit::configure_host(scenario, sw, "S", "10.0.0.1", 8) == Problem::NotAHost);
    CHECK(edit::configure_host(scenario, a, "", "10.0.0.1", 8) == Problem::InvalidName);
    CHECK(edit::configure_host(scenario, a, "PC", "10.0.0.1", 0) == Problem::InvalidPrefix);
    CHECK(edit::configure_host(scenario, a, "PC", "10.0.0.1", 33) == Problem::InvalidPrefix);
    for (const auto* address : {"bad", "10.0.0.0", "10.255.255.255", "127.0.0.1", "224.0.0.1", "0.1.2.3", "01.2.3.4"})
        CHECK(edit::configure_host(scenario, a, "PC", address, 8) == Problem::InvalidHostAddress);
    CHECK(edit::rename(scenario, a, "\x7f") == Problem::InvalidName);
    CHECK(edit::remove_device(scenario, DeviceId{42}) == Problem::NoSuchDevice);
    CHECK(edit::remove_link(scenario, LinkId{42}) == Problem::NoSuchLink);
    CHECK(edit::set_link_up(scenario, LinkId{42}, false) == Problem::NoSuchLink);
    CHECK(edit::set_powered(scenario, DeviceId{42}, false) == Problem::NoSuchDevice);
    CHECK(edit::move(scenario, DeviceId{42}, 0, 0) == Problem::NoSuchDevice);
    CHECK(scenario == before);

    CHECK(edit::configure_host(scenario, a, "Gateway side", "10.0.0.0", 31) == Problem::None);
    CHECK(edit::configure_host(scenario, b, "Loopback-like", "10.9.9.9", 32) == Problem::None);
    CHECK(scenario.device(a)->name == "Gateway side");
    CHECK(edit::rename(scenario, sw, "Core") == Problem::None && scenario.device(sw)->name == "Core");
    CHECK(edit::set_link_up(scenario, first, false) == Problem::None && !scenario.link(first)->up);
    CHECK(edit::set_powered(scenario, b, false) == Problem::None && !scenario.device(b)->powered);
    CHECK(edit::move(scenario, sw, -5, 7) == Problem::None && scenario.device(sw)->x == -5);
    CHECK(netsim::validate(scenario).empty());

    // A loop is refused whatever the state of the cables in it.
    DeviceId sw2{};
    CHECK(edit::add_device(scenario, DeviceKind::Switch, 0, 0, &sw2) == Problem::None);
    CHECK(edit::connect(scenario, sw, sw2) == Problem::None);
    CHECK(edit::connect(scenario, sw2, sw) == Problem::LoopNotSupported);
    CHECK(edit::joined(scenario, a, sw2) && !edit::joined(scenario, a, DeviceId{42}));
    CHECK(edit::free_port(scenario, sw) == 3 && !edit::free_port(scenario, a));

    // Removing a device removes its cables; ids are never reused.
    CHECK(edit::remove_device(scenario, sw) == Problem::None);
    CHECK(scenario.links.empty() && scenario.devices.size() == 4);
    DeviceId next{};
    CHECK(edit::add_device(scenario, DeviceKind::Host, 0, 0, &next) == Problem::None && next == DeviceId{6});
    CHECK(edit::connect(scenario, a, next) == Problem::None);
    CHECK(edit::remove_link(scenario, scenario.links.front().id) == Problem::None && scenario.links.empty());

    Scenario full;
    for (int i = 0; i < 100; ++i) CHECK(edit::add_device(full, DeviceKind::Switch, 0, 0) == Problem::None);
    CHECK(edit::add_device(full, DeviceKind::Host, 0, 0) == Problem::TooManyDevices);
    CHECK(full.devices.size() == 100);
}

void scenario_files() {
    // Every example reads, passes validation and writes back byte for byte.
    const std::filesystem::path examples = RESEND_EXAMPLES_DIR;
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(examples)) {
        if (entry.path().extension() != ".json") continue;
        ++count;
        const auto text = read_file(entry.path());
        std::string error;
        const auto scenario = netsim::scenario_from_json(text, error);
        if (!scenario) std::cerr << entry.path() << ": " << error << '\n';
        CHECK(scenario.has_value());
        CHECK(netsim::validate(*scenario).empty());
        CHECK(netsim::scenario_to_json(*scenario) == text);
        CHECK(!scenario->title.empty() && !scenario->notes.empty());
    }
    CHECK(count >= 5);
    std::string error;
    CHECK(netsim::scenario_from_json(read_file(examples / "sample-lan.json"), error) == netsim::sample_lan());

    // Everything a scenario holds survives the round trip.
    auto custom = netsim::sample_lan();
    custom.title = "Ünïcode \"title\"";
    custom.notes = "Line one\nline two";
    custom.seed = 4'000'000'000U;
    custom.devices[0].powered = false;
    custom.devices[0].x = -40;
    custom.devices[2].ports[5].admin_up = false;
    custom.devices[2].ports[6].mac = *netsim::MacAddress::parse("0a:bb:cc:dd:ee:ff");
    custom.devices[1].ports[0].ipv4 = netsim::Ipv4Prefix::parse("10.1.2.3/31");
    custom.links[0].delay = 1500us;
    custom.links[1].up = false;
    const auto text = netsim::scenario_to_json(custom);
    CHECK(netsim::scenario_from_json(text, error) == custom);
    CHECK(text.find("\"delay\": \"1500us\"") != std::string::npos);
    // Switch ports are listed only when one differs from the default.
    CHECK(netsim::scenario_to_json(netsim::sample_lan()).find("02:10:00:00:00:03") == std::string::npos);
    CHECK(text.find("02:10:00:00:00:03") != std::string::npos);

    // Optional members take their defaults.
    const auto minimal = netsim::scenario_from_json(R"({"format": "resend-scenario", "version": 1,
        "devices": [{"id": 1, "kind": "host", "name": "A", "ports": [{"ipv4": "10.0.0.1/8"}]},
                    {"id": 2, "kind": "switch", "name": "S"}],
        "links": [{"id": 1, "a": {"device": 1, "port": 1}, "b": {"device": 2, "port": 3}}]})", error);
    CHECK(minimal.has_value());
    CHECK(minimal->seed == 1 && minimal->title.empty());
    CHECK(minimal->devices[0].powered && minimal->devices[0].x == 0);
    CHECK(minimal->devices[0].ports[0].mac == netsim::default_mac(DeviceKind::Host, DeviceId{1}, 0));
    CHECK(minimal->links[0].b.port == 2 && minimal->links[0].delay == 10ms && minimal->links[0].up);
    CHECK(netsim::validate(*minimal).empty());
}

void documented_examples() {
    // Every JSON example in docs/scenario-format.md reads and validates.
    const auto text = read_file(std::filesystem::path(RESEND_DOCS_DIR) / "scenario-format.md");
    int count = 0;
    for (auto start = text.find("```json\n"); start != std::string::npos; start = text.find("```json\n", start)) {
        start += 8;
        const auto end = text.find("```", start);
        CHECK(end != std::string::npos);
        std::string error;
        const auto scenario = netsim::scenario_from_json(text.substr(start, end - start), error);
        if (!scenario) std::cerr << "docs/scenario-format.md: " << error << '\n';
        CHECK(scenario.has_value() && netsim::validate(*scenario).empty());
        ++count;
        start = end;
    }
    CHECK(count >= 1);
}

void scenario_file_errors() {
    const auto failure = [](std::string_view text) {
        std::string error;
        const auto scenario = netsim::scenario_from_json(text, error);
        CHECK(!scenario && !error.empty());
        return error;
    };
    const std::string head = R"({"format": "resend-scenario", "version": 1, )";
    CHECK(failure("{\n  \"format\": resend\n}") == "line 2, column 13: expected a value");
    CHECK(failure("[]") == "file: expected an object");
    CHECK(failure(R"({"format": "other", "version": 1})") == "format: this is not a resend scenario file");
    CHECK(failure(R"({"version": 1})") == "file: missing member \"format\"");
    CHECK(failure(R"({"format": "resend-scenario", "version": 2, "devices": [], "links": []})") ==
          "version: this file needs a newer version of resend (format 2)");
    CHECK(failure(R"({"format": "resend-scenario", "version": 0})") == "file.version: expected a whole number from 1 to 2147483647");
    CHECK(failure(head + R"("devices": [], "links": [], "extra": 1})") == "file: unknown member \"extra\"");
    CHECK(failure(head + R"("links": []})") == "file: missing member \"devices\"");
    CHECK(failure(head + R"("devices": {}, "links": []})") == "devices: expected an array");
    CHECK(failure(head + R"("devices": [{"id": 1, "kind": "router", "name": "R"}], "links": []})") ==
          "devices[0].kind: expected \"host\" or \"switch\"");
    CHECK(failure(head + R"("devices": [{"id": 0, "kind": "host", "name": "A"}], "links": []})") ==
          "devices[0].id: expected a whole number from 1 to 4294967295");
    CHECK(failure(head + R"("devices": [{"id": 1, "kind": "host", "name": "A", "powered": 1}], "links": []})") ==
          "devices[0].powered: expected true or false");
    CHECK(failure(head + R"("devices": [{"id": 1, "kind": "host", "name": "A", "ports": [{}, {}]}], "links": []})") ==
          "devices[0].ports: a host has 1 port");
    CHECK(failure(head + R"("devices": [{"id": 1, "kind": "host", "name": "A", "ports": [{"ipv4": "10.0.0.1"}]}], "links": []})") ==
          "devices[0].ports[0].ipv4: expected an IPv4 address with its prefix, such as 192.168.1.10/24");
    CHECK(failure(head + R"("devices": [{"id": 1, "kind": "host", "name": "A", "ports": [{"mac": "02:00"}]}], "links": []})") ==
          "devices[0].ports[0].mac: expected a MAC address such as 02:00:00:00:00:01");
    CHECK(failure(head + R"("devices": [{"id": 1, "kind": "host", "name": "A", "ports": [{"vlan": 3}]}], "links": []})") ==
          "devices[0].ports[0]: unknown member \"vlan\"");
    CHECK(failure(head + R"("devices": [], "links": [{"id": 1, "a": {"device": 1, "port": 0}, "b": {"device": 2, "port": 1}}]})") ==
          "links[0].a.port: expected a whole number from 1 to 64");
    CHECK(failure(head + R"("devices": [], "links": [{"id": 1, "a": {"device": 1, "port": 1}}]})") ==
          "links[0]: missing member \"b\"");
    CHECK(failure(head + R"("devices": [], "links": [{"id": 1, "a": {"device": 1, "port": 1}, "b": {"device": 2, "port": 1}, "delay": "10 ms"}]})") ==
          "links[0].delay: expected a duration such as 10ms or 250us");
    // A file that reads but describes an impossible network fails validation.
    std::string error;
    const auto loop = netsim::scenario_from_json(head + R"("devices": [{"id": 1, "kind": "switch", "name": "S"}],
        "links": [{"id": 1, "a": {"device": 1, "port": 1}, "b": {"device": 1, "port": 2}}]})", error);
    CHECK(loop && has(netsim::validate(*loop), Problem::SameDevice));
}
} // namespace

int main() {
    json_values();
    random_numbers();
    validation_rules();
    edits();
    scenario_files();
    documented_examples();
    scenario_file_errors();
    std::cout << "Scenario tests passed: " << test::checks << " checks.\n";
}
