#include "scenario/scenario_json.hpp"

#include "core/json.hpp"
#include "core/text.hpp"

#include <initializer_list>
#include <limits>
#include <utility>

namespace netsim {
namespace {
using json::Value;
using text::tr;

const char* kind_name(DeviceKind kind) {
    switch (kind) {
    case DeviceKind::Host: return "host";
    case DeviceKind::Switch: return "switch";
    case DeviceKind::Router: return "router";
    }
    return "host";
}

Value port_ref(PortRef port) {
    Value out = Value::object();
    out.set("device", port.device.value);
    out.set("port", port.port + 1);
    return out;
}

bool default_port(const DeviceConfig& device, std::size_t index) {
    const auto& port = device.ports[index];
    return port.mac == default_mac(device.kind, device.id, static_cast<int>(index)) && port.admin_up && !port.ipv4;
}

// Reads members with the path of each one, so errors say where they are.
class Reader {
public:
    std::string error;

    bool fail(const std::string& path, const std::string& message) {
        if (error.empty()) error = path + ": " + message;
        return false;
    }
    // Every member of `object` must be one of `known`.
    bool only(const Value& object, const std::string& path, std::initializer_list<std::string_view> known) {
        for (const auto& [key, value] : *object.members()) {
            bool listed = false;
            for (const auto name : known) listed = listed || name == key;
            if (!listed) return fail(path, tr("unknown member \"{0}\"", {key}));
        }
        return true;
    }
    const Value* object(const Value& value, const std::string& path) {
        if (!value.members()) {
            fail(path, tr("expected an object"));
            return nullptr;
        }
        return &value;
    }
    const Value* member(const Value& object, std::string_view key, const std::string& path) {
        const auto* value = object.find(key);
        if (!value) fail(path, tr("missing member \"{0}\"", {key}));
        return value;
    }
    bool text(const Value& object, std::string_view key, const std::string& path, std::string& out, bool required = true) {
        const auto* value = object.find(key);
        if (!value) return !required || fail(path, tr("missing member \"{0}\"", {key}));
        if (!value->string()) return fail(path + "." + std::string(key), tr("expected a string"));
        out = *value->string();
        return true;
    }
    bool flag(const Value& object, std::string_view key, const std::string& path, bool& out) {
        const auto* value = object.find(key);
        if (!value) return true;  // optional, keeps the default
        if (!value->boolean()) return fail(path + "." + std::string(key), tr("expected true or false"));
        out = *value->boolean();
        return true;
    }
    bool number(const Value& object, std::string_view key, const std::string& path, std::int64_t low, std::int64_t high,
                std::int64_t& out, bool required = true) {
        const auto* value = object.find(key);
        if (!value) return !required || fail(path, tr("missing member \"{0}\"", {key}));
        if (!value->integer() || *value->integer() < low || *value->integer() > high)
            return fail(path + "." + std::string(key), tr("expected a whole number from {0} to {1}", {std::to_string(low), std::to_string(high)}));
        out = *value->integer();
        return true;
    }
    bool port_ref(const Value& value, const std::string& path, PortRef& out) {
        if (!object(value, path) || !only(value, path, {"device", "port"})) return false;
        std::int64_t device = 0, port = 0;
        if (!number(value, "device", path, 1, std::numeric_limits<std::uint32_t>::max(), device) ||
            !number(value, "port", path, 1, 64, port))
            return false;
        out = {DeviceId{static_cast<std::uint32_t>(device)}, static_cast<std::uint8_t>(port - 1)};
        return true;
    }
};
} // namespace

std::string scenario_to_json(const Scenario& scenario) {
    Value root = Value::object();
    root.set("format", "resend-scenario");
    root.set("version", scenario_format_version);
    root.set("title", scenario.title);
    root.set("notes", scenario.notes);
    root.set("seed", scenario.seed);
    auto& devices = root.set("devices", Value::array());
    for (const auto& device : scenario.devices) {
        Value item = Value::object();
        item.set("id", device.id.value);
        item.set("kind", kind_name(device.kind));
        item.set("name", device.name);
        item.set("powered", device.powered);
        item.set("x", device.x);
        item.set("y", device.y);
        if (device.kind == DeviceKind::Switch) item.set("ageing", format_duration(device.mac_ageing));
        if (device.gateway) item.set("gateway", device.gateway->to_string());
        bool all_default = true;
        for (std::size_t i = 0; i < device.ports.size(); ++i) all_default = all_default && default_port(device, i);
        // Switch ports are written only when one differs from the default.
        if (!all_default || device.kind != DeviceKind::Switch) {
            auto& ports = item.set("ports", Value::array());
            for (const auto& port : device.ports) {
                Value entry = Value::object();
                entry.set("mac", port.mac.to_string());
                entry.set("up", port.admin_up);
                if (port.ipv4) entry.set("ipv4", port.ipv4->to_string());
                ports.push(std::move(entry));
            }
        }
        if (device.kind == DeviceKind::Router) {
            auto& routes = item.set("routes", Value::array());
            for (const auto& route : device.routes) {
                Value entry = Value::object();
                entry.set("destination", route.destination.to_string());
                entry.set("via", route.next_hop.to_string());
                routes.push(std::move(entry));
            }
        }
        devices.push(std::move(item));
    }
    auto& links = root.set("links", Value::array());
    for (const auto& cable : scenario.links) {
        Value item = Value::object();
        item.set("id", cable.id.value);
        item.set("a", port_ref(cable.a));
        item.set("b", port_ref(cable.b));
        item.set("delay", format_duration(cable.delay));
        item.set("up", cable.up);
        links.push(std::move(item));
    }
    return json::write(root);
}

std::optional<Scenario> scenario_from_json(std::string_view text, std::string& error) {
    json::Error syntax;
    const auto document = json::parse(text, syntax);
    if (!document) {
        error = tr("line {0}, column {1}", {std::to_string(syntax.line), std::to_string(syntax.column)}) + ": " + syntax.message;
        return std::nullopt;
    }
    Reader read;
    const auto finish = [&]() -> std::optional<Scenario> {
        error = read.error;
        return std::nullopt;
    };
    if (!read.object(*document, "file")) return finish();
    std::string format;
    std::int64_t version = 0;
    if (!read.text(*document, "format", "file", format)) return finish();
    if (format != "resend-scenario") {
        read.fail("format", tr("this is not a resend scenario file"));
        return finish();
    }
    if (!read.number(*document, "version", "file", 1, std::numeric_limits<std::int32_t>::max(), version)) return finish();
    if (version > scenario_format_version) {
        read.fail("version", tr("this file needs a newer version of resend (format {0})", {std::to_string(version)}));
        return finish();
    }
    if (!read.only(*document, "file", {"format", "version", "title", "notes", "seed", "devices", "links"})) return finish();

    Scenario scenario;
    std::int64_t seed = 1;
    if (!read.text(*document, "title", "file", scenario.title, false) ||
        !read.text(*document, "notes", "file", scenario.notes, false) ||
        !read.number(*document, "seed", "file", 0, std::numeric_limits<std::uint32_t>::max(), seed, false))
        return finish();
    scenario.seed = static_cast<std::uint32_t>(seed);

    const auto* devices = read.member(*document, "devices", "file");
    if (!devices) return finish();
    if (!devices->array_items()) {
        read.fail("devices", tr("expected an array"));
        return finish();
    }
    for (std::size_t index = 0; index < devices->array_items()->size(); ++index) {
        const auto& item = (*devices->array_items())[index];
        const auto path = "devices[" + std::to_string(index) + "]";
        if (!read.object(item, path) ||
            !read.only(item, path, {"id", "kind", "name", "powered", "x", "y", "ageing", "gateway", "ports", "routes"}))
            return finish();
        DeviceConfig device;
        std::int64_t id = 0, x = 0, y = 0;
        std::string kind, ageing, gateway;
        if (!read.number(item, "id", path, 1, std::numeric_limits<std::uint32_t>::max(), id) ||
            !read.text(item, "kind", path, kind) || !read.text(item, "name", path, device.name) ||
            !read.flag(item, "powered", path, device.powered) ||
            !read.number(item, "x", path, -max_coordinate, max_coordinate, x, false) ||
            !read.number(item, "y", path, -max_coordinate, max_coordinate, y, false) ||
            !read.text(item, "ageing", path, ageing, false) || !read.text(item, "gateway", path, gateway, false))
            return finish();
        if (kind != "host" && kind != "switch" && kind != "router") {
            read.fail(path + ".kind", tr("expected \"host\", \"switch\" or \"router\""));
            return finish();
        }
        if (item.find("ageing")) {
            const auto parsed = parse_duration(ageing);
            if (kind != "switch") {
                read.fail(path + ".ageing", tr("only switches have an ageing time"));
                return finish();
            }
            if (!parsed) {
                read.fail(path + ".ageing", tr("expected a duration such as 300s"));
                return finish();
            }
            device.mac_ageing = *parsed;
        }
        if (item.find("gateway")) {
            if (kind != "host") {
                read.fail(path + ".gateway", tr("only hosts have a default gateway"));
                return finish();
            }
            device.gateway = Ipv4Address::parse(gateway);
            if (!device.gateway) {
                read.fail(path + ".gateway", tr("expected an IPv4 address such as 192.168.1.1"));
                return finish();
            }
        }
        device.id = DeviceId{static_cast<std::uint32_t>(id)};
        device.kind = kind == "host" ? DeviceKind::Host : kind == "switch" ? DeviceKind::Switch : DeviceKind::Router;
        device.x = static_cast<int>(x);
        device.y = static_cast<int>(y);
        device.ports = default_ports(device.kind, device.id);
        if (const auto* ports = item.find("ports")) {
            if (!ports->array_items()) {
                read.fail(path + ".ports", tr("expected an array"));
                return finish();
            }
            if (!valid_port_count(device.kind, ports->array_items()->size())) {
                read.fail(path + ".ports", device.kind == DeviceKind::Host     ? tr("a host has {0} port", {std::to_string(host_ports)})
                                               : device.kind == DeviceKind::Switch ? tr("a switch has {0} ports", {std::to_string(switch_ports)})
                                                                                   : tr("a router has {0} to {1} ports", {std::to_string(min_router_ports), std::to_string(max_router_ports)}));
                return finish();
            }
            device.ports.resize(ports->array_items()->size());
            for (std::size_t p = 0; p < device.ports.size(); ++p) device.ports[p].mac = default_mac(device.kind, device.id, static_cast<int>(p));
            for (std::size_t p = 0; p < ports->array_items()->size(); ++p) {
                const auto& entry = (*ports->array_items())[p];
                const auto port_path = path + ".ports[" + std::to_string(p) + "]";
                if (!read.object(entry, port_path) || !read.only(entry, port_path, {"mac", "up", "ipv4"})) return finish();
                auto& port = device.ports[p];
                std::string mac, address;
                if (!read.text(entry, "mac", port_path, mac, false) || !read.flag(entry, "up", port_path, port.admin_up) ||
                    !read.text(entry, "ipv4", port_path, address, false))
                    return finish();
                if (entry.find("mac")) {
                    const auto parsed = MacAddress::parse(mac);
                    if (!parsed) {
                        read.fail(port_path + ".mac", tr("expected a MAC address such as 02:00:00:00:00:01"));
                        return finish();
                    }
                    port.mac = *parsed;
                }
                if (entry.find("ipv4")) {
                    port.ipv4 = Ipv4Prefix::parse(address);
                    if (!port.ipv4) {
                        read.fail(port_path + ".ipv4", tr("expected an IPv4 address with its prefix, such as 192.168.1.10/24"));
                        return finish();
                    }
                }
            }
        }
        if (const auto* routes = item.find("routes")) {
            if (device.kind != DeviceKind::Router) {
                read.fail(path + ".routes", tr("only routers have routes"));
                return finish();
            }
            if (!routes->array_items()) {
                read.fail(path + ".routes", tr("expected an array"));
                return finish();
            }
            for (std::size_t r = 0; r < routes->array_items()->size(); ++r) {
                const auto& entry = (*routes->array_items())[r];
                const auto route_path = path + ".routes[" + std::to_string(r) + "]";
                std::string destination, via;
                if (!read.object(entry, route_path) || !read.only(entry, route_path, {"destination", "via"}) ||
                    !read.text(entry, "destination", route_path, destination) || !read.text(entry, "via", route_path, via))
                    return finish();
                const auto network = Ipv4Prefix::parse(destination);
                if (!network) {
                    read.fail(route_path + ".destination", tr("expected a network with its prefix, such as 10.1.0.0/16"));
                    return finish();
                }
                const auto next_hop = Ipv4Address::parse(via);
                if (!next_hop) {
                    read.fail(route_path + ".via", tr("expected an IPv4 address such as 192.168.1.1"));
                    return finish();
                }
                device.routes.push_back({*network, *next_hop});
            }
        }
        scenario.devices.push_back(std::move(device));
    }

    const auto* links = read.member(*document, "links", "file");
    if (!links) return finish();
    if (!links->array_items()) {
        read.fail("links", tr("expected an array"));
        return finish();
    }
    for (std::size_t index = 0; index < links->array_items()->size(); ++index) {
        const auto& item = (*links->array_items())[index];
        const auto path = "links[" + std::to_string(index) + "]";
        if (!read.object(item, path) || !read.only(item, path, {"id", "a", "b", "delay", "up"})) return finish();
        LinkConfig cable;
        std::int64_t id = 0;
        std::string delay;
        const auto* a = read.member(item, "a", path);
        const auto* b = a ? read.member(item, "b", path) : nullptr;
        if (!read.number(item, "id", path, 1, std::numeric_limits<std::uint32_t>::max(), id) || !a || !b ||
            !read.port_ref(*a, path + ".a", cable.a) || !read.port_ref(*b, path + ".b", cable.b) ||
            !read.text(item, "delay", path, delay, false) || !read.flag(item, "up", path, cable.up))
            return finish();
        cable.id = LinkId{static_cast<std::uint32_t>(id)};
        if (item.find("delay")) {
            const auto parsed = parse_duration(delay);
            if (!parsed) {
                read.fail(path + ".delay", tr("expected a duration such as 10ms or 250us"));
                return finish();
            }
            cable.delay = *parsed;
        }
        scenario.links.push_back(cable);
    }
    return scenario;
}

} // namespace netsim
