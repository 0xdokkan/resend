#include "term/terminal.hpp"

#include "explain/explain.hpp"
#include "sim/routing.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <map>
#include <sstream>

namespace netsim::term {
namespace {

using Words = std::vector<std::string>;

// resend's ping without -c: Linux would go on until stopped.
constexpr std::uint16_t default_ping_count = 4;

bool matches(std::string_view word, std::string_view full) { return !word.empty() && full.starts_with(word); }

Result fail(std::string text) {
    Result result;
    result.error = true;
    result.output = std::move(text);
    if (!result.output.empty() && result.output.back() != '\n') result.output += '\n';
    return result;
}
Result print(std::string text) {
    Result result;
    result.output = std::move(text);
    return result;
}

std::string join(const Words& words, std::size_t from = 0) {
    std::string out;
    for (std::size_t i = from; i < words.size(); ++i) out += (i > from ? " " : "") + words[i];
    return out;
}

// --- The network as a Linux machine shows it ------------------------------

struct Port {
    std::uint8_t index = 0;
    const PortConfig* config = nullptr;
    bool carrier = false;
};

bool has_carrier(const Scenario& network, const DeviceConfig& device, std::uint8_t index) {
    const auto& port = device.ports[index];
    if (!device.powered || !port.admin_up) return false;
    const PortRef ref{device.id, index};
    const auto* link = network.link_at(ref);
    const auto peer = network.peer(ref);
    if (!link || !link->up || !peer) return false;
    const auto* other = network.device(peer->device);
    return other && other->powered && peer->port < other->ports.size() && other->ports[peer->port].admin_up;
}

std::vector<Port> ports_of(const Scenario& network, const DeviceConfig& device) {
    std::vector<Port> out;
    for (std::size_t i = 0; i < device.ports.size(); ++i) {
        const auto index = static_cast<std::uint8_t>(i);
        out.push_back({index, &device.ports[i], has_carrier(network, device, index)});
    }
    return out;
}

std::string flags(const Port& port) {
    if (!port.config->admin_up) return "<BROADCAST,MULTICAST>";
    return port.carrier ? "<BROADCAST,MULTICAST,UP,LOWER_UP>" : "<NO-CARRIER,BROADCAST,MULTICAST,UP>";
}
std::string state(const Port& port) { return port.config->admin_up && port.carrier ? "UP" : "DOWN"; }

std::string pad(std::string text, std::size_t width) {
    if (text.size() < width) text.resize(width, ' ');
    else text += ' ';
    return text;
}

std::string link_lines(const DeviceConfig& device, const Port& port) {
    std::ostringstream out;
    out << (port.index + 1) << ": " << port_name(port.index) << ": " << flags(port) << " mtu 1500";
    if (device.kind == DeviceKind::Switch) out << " master br0";
    out << " state " << state(port) << '\n';
    out << "    link/ether " << port.config->mac.to_string() << " brd ff:ff:ff:ff:ff:ff\n";
    return out.str();
}

std::string address_line(const Port& port) {
    if (!port.config->ipv4) return {};
    const auto& prefix = *port.config->ipv4;
    std::string line = "    inet " + prefix.to_string();
    if (prefix.length() <= 30) line += " brd " + prefix.broadcast().to_string();
    return line + " scope global " + port_name(port.index) + '\n';
}

// "time=40.0 ms": three significant digits, as iputils prints them.
std::string rtt_text(SimTime time) {
    const double ms = std::chrono::duration<double, std::milli>(time).count();
    char buffer[32];
    if (ms < 1) std::snprintf(buffer, sizeof buffer, "%.3f", ms);
    else if (ms < 10) std::snprintf(buffer, sizeof buffer, "%.2f", ms);
    else if (ms < 100) std::snprintf(buffer, sizeof buffer, "%.1f", ms);
    else std::snprintf(buffer, sizeof buffer, "%.0f", ms);
    return buffer;
}
std::string ms3(SimTime time) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.3f", std::chrono::duration<double, std::milli>(time).count());
    return buffer;
}

// The port of a host or router whose network holds `address`; enabled, as
// Linux asks of a next hop, unless `any`.
std::optional<std::uint8_t> port_towards(const DeviceConfig& device, Ipv4Address address, bool any = false) {
    std::optional<std::uint8_t> best;
    int length = -1;
    for (std::size_t i = 0; i < device.ports.size(); ++i) {
        const auto& port = device.ports[i];
        if ((!any && !port.admin_up) || !port.ipv4 || !port.ipv4->contains(address) || port.ipv4->address() == address) continue;
        if (port.ipv4->length() > length) best = static_cast<std::uint8_t>(i), length = port.ipv4->length();
    }
    return best;
}

std::string device_kind(const DeviceConfig& device) {
    return device.kind == DeviceKind::Host ? "a host" : device.kind == DeviceKind::Switch ? "a switch" : "a router";
}

// --- Queries ---------------------------------------------------------------

std::optional<std::uint8_t> port_argument(const Words& args, std::size_t& at, const DeviceConfig& device, std::string& error) {
    if (at >= args.size()) return std::nullopt;
    if (args[at] == "dev") ++at;
    if (at >= args.size()) {
        error = "Command line is not complete. Try \"help\".";
        return std::nullopt;
    }
    const auto index = port_index(args[at], device);
    if (!index) {
        error = "Device \"" + args[at] + "\" does not exist.\n# " + device.name + "'s ports are p1 to p" + std::to_string(device.ports.size()) + '.';
        return std::nullopt;
    }
    ++at;
    return index;
}

Result show_links(const Scenario& network, const DeviceConfig& device, const Words& args, std::size_t at, bool addresses, bool brief) {
    std::string error;
    const auto only = port_argument(args, at, device, error);
    if (!error.empty()) return fail(error);
    if (at < args.size()) return fail("Error: either \"dev\" is duplicate, or \"" + args[at] + "\" is a garbage.");
    std::string out;
    for (const auto& port : ports_of(network, device)) {
        if (only && port.index != *only) continue;
        if (brief) {
            out += pad(port_name(port.index), 17) + pad(state(port), 15);
            if (addresses) out += port.config->ipv4 ? port.config->ipv4->to_string() : "";
            else out += port.config->mac.to_string() + ' ' + flags(port);
            while (!out.empty() && out.back() == ' ') out.pop_back();
            out += '\n';
            continue;
        }
        out += link_lines(device, port);
        if (addresses) out += address_line(port);
    }
    return print(out);
}

std::string connected_line(const Scenario& network, const DeviceConfig& device, std::uint8_t index) {
    const auto& prefix = *device.ports[index].ipv4;
    std::string line = prefix.canonical().to_string() + " dev " + port_name(index) + " proto kernel scope link src " + prefix.address().to_string();
    if (!has_carrier(network, device, index)) line += " linkdown";
    return line + '\n';
}
std::string destination_text(const Ipv4Prefix& prefix) { return prefix.length() == 0 ? "default" : prefix.to_string(); }

Result show_routes(const Scenario& network, const DeviceConfig& device) {
    std::string defaults, others, notes;
    if (device.kind == DeviceKind::Host) {
        const auto& port = device.ports.front();
        if (device.gateway) {
            if (port_towards(device, *device.gateway, true)) {
                defaults += "default via " + device.gateway->to_string() + " dev p1\n";
            } else {
                notes += "# resend keeps the default gateway " + device.gateway->to_string() + ", but does not use it: ";
                notes += port.ipv4 ? "it is not on p1's network, " + port.ipv4->canonical().to_string() + ".\n"
                                   : "p1 has no address.\n";
            }
        }
        if (port.ipv4) others += connected_line(network, device, 0);
        return print(defaults + others + notes);
    }
    std::vector<std::pair<Ipv4Prefix, std::string>> lines;
    for (const auto& route : routing_table(device)) {
        if (route.connected) {
            if (route.port) lines.push_back({route.destination, connected_line(network, device, *route.port)});
            continue;
        }
        if (!route.active()) {
            notes += "# resend keeps " + destination_text(route.destination) + " via " + route.next_hop.to_string() +
                     ", but does not use it: ";
            const bool same = std::any_of(device.ports.begin(), device.ports.end(),
                                          [&](const PortConfig& port) { return port.ipv4 && port.ipv4->canonical() == route.destination; });
            notes += same ? "a port of " + device.name + " is on that network.\n"
                          : route.next_hop.to_string() + " is on none of " + device.name + "'s networks.\n";
            continue;
        }
        const auto line = destination_text(route.destination) + " via " + route.next_hop.to_string() + " dev " + port_name(*route.port) + '\n';
        if (route.destination.length() == 0) defaults += line;
        else lines.push_back({route.destination, line});
    }
    std::stable_sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) {
        return a.first.address() != b.first.address() ? a.first.address() < b.first.address() : a.first.length() < b.first.length();
    });
    for (const auto& line : lines) others += line.second;
    return print(defaults + others + notes);
}

Result route_get(const DeviceConfig& device, const Words& args, std::size_t at) {
    if (at >= args.size()) return fail("Command line is not complete. Try \"help ip\".");
    const auto destination = Ipv4Address::parse(args[at]);
    if (!destination) return fail("Error: any valid prefix is expected rather than \"" + args[at] + "\".");
    if (at + 1 < args.size()) return fail("Error: \"" + args[at + 1] + "\" is not in resend's ip route get.");
    const auto source_of = [&](std::uint8_t port) { return device.ports[port].ipv4->address().to_string(); };
    if (device.kind == DeviceKind::Host) {
        const auto& port = device.ports.front();
        if (port.ipv4) {
            if (port.ipv4->contains(*destination))
                return print(destination->to_string() + " dev p1 src " + source_of(0) + '\n');
            if (device.gateway && port_towards(device, *device.gateway, true))
                return print(destination->to_string() + " via " + device.gateway->to_string() + " dev p1 src " + source_of(0) + '\n');
        }
        return fail("RTNETLINK answers: Network is unreachable");
    }
    const auto route = lookup(routing_table(device), *destination);
    if (!route) return fail("RTNETLINK answers: Network is unreachable");
    std::string line = destination->to_string();
    if (!route->connected) line += " via " + route->next_hop.to_string();
    const auto& own = device.ports[*route->port].ipv4;
    line += " dev " + port_name(*route->port) + (own ? " src " + own->address().to_string() : std::string{});
    return print(line + '\n');
}

Result show_neighbours(const DeviceConfig& device, const Simulation* simulation) {
    if (device.kind == DeviceKind::Switch)
        return print("# " + device.name + " has no IPv4: what it learned is in bridge fdb.\n");
    std::string out;
    if (!simulation) return print(out);
    for (const auto& entry : simulation->arp_cache(device.id)) {
        out += entry.ip.to_string() + " dev " + port_name(entry.port);
        out += entry.state == ArpState::Reachable ? " lladdr " + entry.mac.to_string() + " REACHABLE\n" : " INCOMPLETE\n";
    }
    return print(out);
}

// --- Changes ---------------------------------------------------------------

Result change(std::vector<Edit> edits, const DeviceConfig& device, const Words& words) {
    Result result;
    result.edits = std::move(edits);
    result.label = device.name + ": " + join(words);
    return result;
}

Result address_change(const DeviceConfig& device, const Words& words, const Words& args, std::size_t at, bool add) {
    if (device.kind == DeviceKind::Switch)
        return fail("Error: " + device.name + " is a switch: its ports have no IPv4 address in resend.");
    if (at >= args.size()) return fail("Command line is not complete. Try \"help ip\".");
    const auto& text = args[at++];
    if (text.find('/') == std::string::npos)
        return fail("Error: " + text + " needs its prefix length, e.g. " + text + "/24.\n# Without one, Linux takes /32: a network of one address.");
    const auto prefix = Ipv4Prefix::parse(text);
    if (!prefix) return fail("Error: any valid prefix is expected rather than \"" + text + "\".");
    if (at >= args.size() || args[at] != "dev")
        return fail("Error: the port is missing: ip address " + std::string(add ? "add " : "del ") + text + " dev p1");
    ++at;
    if (at >= args.size()) return fail("Command line is not complete. Try \"help ip\".");
    const auto port = port_index(args[at], device);
    if (!port) return fail("Cannot find device \"" + args[at] + "\"\n# " + device.name + "'s ports are p1 to p" + std::to_string(device.ports.size()) + '.');
    if (++at < args.size()) return fail("Error: \"" + args[at] + "\" is not in resend's ip address " + (add ? "add." : "del."));
    const auto& current = device.ports[*port].ipv4;
    const PortRef ref{device.id, *port};
    if (add) {
        if (current && *current == *prefix) return fail("RTNETLINK answers: File exists");
        if (current)
            return fail("Error: " + port_name(*port) + " already has " + current->to_string() + ", and resend gives a port one address.\n# Remove it first: ip address del " +
                        current->to_string() + " dev " + port_name(*port));
        return change({SetAddress{ref, *prefix}}, device, words);
    }
    if (!current || *current != *prefix) return fail("RTNETLINK answers: Cannot assign requested address");
    return change({SetAddress{ref, std::nullopt}}, device, words);
}

Result link_set(const DeviceConfig& device, const Words& words, const Words& args, std::size_t at) {
    std::string error;
    const auto port = port_argument(args, at, device, error);
    if (!error.empty()) return fail(error);
    if (!port) return fail("Not enough of information: \"dev\" argument is required.");
    if (at >= args.size()) return fail("Command line is not complete: ip link set " + port_name(*port) + " up, or down.");
    const auto& what = args[at];
    if (what != "up" && what != "down")
        return fail("Error: \"" + what + "\" is not in resend's ip link set, which turns a port up or down.");
    if (++at < args.size()) return fail("Error: \"" + args[at] + "\" is not in resend's ip link set.");
    return change({SetPortUp{PortRef{device.id, *port}, what == "up"}}, device, words);
}

Result route_change(const DeviceConfig& device, const Words& words, const Words& args, std::size_t at, std::string_view verb) {
    if (device.kind == DeviceKind::Switch) return fail("Error: " + device.name + " is a switch: it has no routes in resend.");
    if (at >= args.size()) return fail("Command line is not complete. Try \"help ip\".");
    std::optional<Ipv4Prefix> destination;
    const auto& target = args[at++];
    if (target == "default" || target == "0.0.0.0/0") {
        destination = Ipv4Prefix{};
    } else {
        destination = Ipv4Prefix::parse(target.find('/') == std::string::npos ? target + "/32" : target);
        if (!destination) return fail("Error: any valid prefix is expected rather than \"" + target + "\".");
        if (destination->canonical() != *destination) return fail("Error: Invalid prefix for given prefix length.");
    }
    std::optional<Ipv4Address> via;
    std::optional<std::uint8_t> dev;
    while (at < args.size()) {
        if (args[at] == "via" && at + 1 < args.size()) {
            via = Ipv4Address::parse(args[at + 1]);
            if (!via) return fail("Error: inet address is expected rather than \"" + args[at + 1] + "\".");
            at += 2;
        } else if (args[at] == "dev" && at + 1 < args.size()) {
            dev = port_index(args[at + 1], device);
            if (!dev) return fail("Cannot find device \"" + args[at + 1] + "\"");
            at += 2;
        } else {
            return fail("Error: \"" + args[at] + "\" is not in resend's ip route, which knows via and dev.");
        }
    }
    const bool remove = verb == "del";
    if (!remove && !via) return fail("Error: resend's routes go through a next hop: ip route " + std::string(verb) + ' ' + target + " via ADDRESS");
    // As Linux: the next hop must be on the network of an enabled port.
    if (via) {
        const auto port = port_towards(device, *via);
        if (dev && device.ports[*dev].admin_up == false) return fail("Error: Nexthop device is not up.");
        if (!port || (dev && *port != *dev)) {
            std::string why = "Error: Nexthop has invalid gateway.\n# " + via->to_string() + " is on none of " + device.name + "'s networks";
            return fail(why + (std::any_of(device.ports.begin(), device.ports.end(), [](const PortConfig& p) { return p.ipv4 && !p.admin_up; })
                                   ? " on an enabled port." : "."));
        }
    }

    if (device.kind == DeviceKind::Host) {
        if (destination->length() != 0)
            return fail("Error: a host in resend has one route out, its default gateway: ip route " + std::string(verb) + " default via ADDRESS");
        if (remove) {
            if (!device.gateway || (via && *via != *device.gateway)) return fail("RTNETLINK answers: No such process");
            return change({SetGateway{device.id, std::nullopt}}, device, words);
        }
        if (device.gateway && verb == "add") {
            if (*device.gateway == *via && port_towards(device, *via)) return fail("RTNETLINK answers: File exists");
            if (port_towards(device, *device.gateway))
                return fail("RTNETLINK answers: File exists\n# Replace it: ip route replace default via " + via->to_string());
        }
        return change({SetGateway{device.id, *via}}, device, words);
    }

    auto routes = device.routes;
    const auto found = std::find_if(routes.begin(), routes.end(), [&](const Route& route) {
        return route.destination == *destination && (!remove || !via || route.next_hop == *via);
    });
    const bool connected = std::any_of(device.ports.begin(), device.ports.end(), [&](const PortConfig& port) {
        return port.admin_up && port.ipv4 && port.ipv4->canonical() == *destination;
    });
    if (remove) {
        if (found == routes.end()) {
            if (connected)
                return fail("Error: the route to " + destination->to_string() + " comes with a port's address.\n# Remove the address to remove it.");
            return fail("RTNETLINK answers: No such process");
        }
        routes.erase(found);
        return change({SetRoutes{device.id, routes}}, device, words);
    }
    if (connected) return fail("RTNETLINK answers: File exists\n# " + destination->to_string() + " is a network of one of " + device.name + "'s ports.");
    if (found != routes.end()) {
        if (verb == "add") return fail("RTNETLINK answers: File exists");
        found->next_hop = *via;
    } else {
        routes.push_back({*destination, *via});
    }
    return change({SetRoutes{device.id, routes}}, device, words);
}

// --- The commands ----------------------------------------------------------

enum class Object { Address, Link, Route, Neighbour };

std::optional<Object> object_named(std::string_view word) {
    if (matches(word, "address")) return Object::Address;
    if (matches(word, "link")) return Object::Link;
    if (matches(word, "route")) return Object::Route;
    if (matches(word, "neighbour") || matches(word, "neighbor")) return Object::Neighbour;
    return std::nullopt;
}
bool is_show(std::string_view word) { return matches(word, "show") || matches(word, "list") || word == "lst"; }
bool is_delete(std::string_view word) { return word == "del" || matches(word, "delete"); }

Result ip(const Scenario& network, const DeviceConfig& device, const Simulation* simulation, const Words& words) {
    std::size_t at = 1;
    bool brief = false;
    for (; at < words.size() && words[at].starts_with('-'); ++at) {
        const auto& option = words[at];
        if (option == "-4") continue;
        if (option == "-br" || option == "-brief") {
            brief = true;
            continue;
        }
        return fail("Option \"" + option + "\" is not in resend's ip, which knows -4 and -br.");
    }
    if (at >= words.size()) return print(help("ip"));
    const auto object = object_named(words[at]);
    if (!object) return fail("Object \"" + words[at] + "\" is unknown, try \"ip help\".\n# resend's ip knows address, link, route and neigh.");
    ++at;
    const std::string verb = at < words.size() ? words[at] : "show";
    const std::size_t rest = at < words.size() ? at + 1 : at;
    const bool host_or_router = device.kind != DeviceKind::Switch;
    switch (*object) {
    case Object::Address:
        if (is_show(verb)) return show_links(network, device, words, rest, host_or_router, brief);
        if (matches(verb, "add")) return address_change(device, words, words, rest, true);
        if (is_delete(verb)) return address_change(device, words, words, rest, false);
        if (verb == "help") return print(help("ip"));
        return fail("Command \"" + verb + "\" is unknown, try \"ip address help\".\n# resend's ip address knows show, add and del.");
    case Object::Link:
        if (is_show(verb)) return show_links(network, device, words, rest, false, brief);
        if (verb == "set") return link_set(device, words, words, rest);
        if (verb == "help") return print(help("ip"));
        return fail("Command \"" + verb + "\" is unknown, try \"ip link help\".\n# resend's ip link knows show and set.");
    case Object::Route:
        if (brief) return fail("Option -br is for ip address and ip link.");
        if (is_show(verb)) {
            if (rest < words.size()) return fail("Error: \"" + words[rest] + "\" is not in resend's ip route show.");
            if (!host_or_router) return print("# " + device.name + " is a switch: it has no routes.\n");
            return show_routes(network, device);
        }
        if (verb == "get") {
            if (!host_or_router) return fail("RTNETLINK answers: Network is unreachable\n# " + device.name + " is a switch: it has no routes.");
            return route_get(device, words, rest);
        }
        if (matches(verb, "add")) return route_change(device, words, words, rest, "add");
        if (is_delete(verb)) return route_change(device, words, words, rest, "del");
        if (matches(verb, "replace")) return route_change(device, words, words, rest, "replace");
        if (verb == "help") return print(help("ip"));
        return fail("Command \"" + verb + "\" is unknown, try \"ip route help\".\n# resend's ip route knows show, get, add, del and replace.");
    case Object::Neighbour:
        if (brief) return fail("Option -br is for ip address and ip link.");
        if (is_show(verb)) {
            if (rest < words.size()) return fail("Error: \"" + words[rest] + "\" is not in resend's ip neigh show.");
            return show_neighbours(device, simulation);
        }
        if (verb == "help") return print(help("ip"));
        return fail("Command \"" + verb + "\" is not in resend's ip neigh, which only shows the ARP cache.\n# Entries come and go with the traffic.");
    }
    return fail("Try \"help\".");
}

Result bridge(const Scenario& network, const DeviceConfig& device, const Simulation* simulation, const Words& words) {
    if (words.size() < 2) return print(help("bridge"));
    const auto& object = words[1];
    const std::string verb = words.size() > 2 ? words[2] : "show";
    if (!is_show(verb)) return fail("Command \"" + verb + "\" is not in resend's bridge, which only shows.");
    if (words.size() > 3) return fail("Error: \"" + words[3] + "\" is not in resend's bridge " + object + " show.");
    if (device.kind != DeviceKind::Switch)
        return fail("# " + device.name + " is " + device_kind(device) + ", not a switch: its neighbours are in ip neigh.");
    if (matches(object, "fdb")) {
        std::string out;
        if (simulation)
            for (const auto& entry : simulation->mac_table(device.id))
                out += entry.mac.to_string() + " dev " + port_name(entry.port) + " master br0\n";
        return print(out);
    }
    if (matches(object, "link")) {
        std::string out;
        for (const auto& port : ports_of(network, device)) {
            if (!network.link_at({device.id, port.index}) && !port.carrier) continue;
            out += std::to_string(port.index + 1) + ": " + port_name(port.index) + ": " + flags(port) + " mtu 1500 master br0 state " +
                   (port.carrier ? "forwarding" : "disabled") + '\n';
        }
        return print(out);
    }
    return fail("Object \"" + object + "\" is unknown, try \"bridge help\".\n# resend's bridge knows fdb and link.");
}

// "-c 3" or "-c3".
std::optional<std::string> option_value(const Words& words, std::size_t& at) {
    const auto& word = words[at];
    if (word.size() > 2) return word.substr(2);
    if (at + 1 >= words.size()) return std::nullopt;
    return words[++at];
}
std::optional<SimTime> seconds(const std::string& text) {
    try {
        std::size_t used = 0;
        const double value = std::stod(text, &used);
        if (used != text.size() || !std::isfinite(value) || value < 0) return std::nullopt;
        return std::chrono::duration_cast<SimTime>(std::chrono::duration<double>(value));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}
std::optional<long> integer(const std::string& text) {
    try {
        std::size_t used = 0;
        const long value = std::stol(text, &used);
        if (used != text.size()) return std::nullopt;
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

Result application(const Scenario& network, const DeviceConfig& device, const Words& words, bool traceroute) {
    const std::string tool = words[0];
    if (device.kind != DeviceKind::Host)
        return fail(tool + ": " + device.name + " is " + device_kind(device) + ": in resend only hosts start pings and traceroutes.");
    Start start;
    start.options.traceroute = traceroute;
    start.options.count = traceroute ? default_max_hops : default_ping_count;
    start.default_count = !traceroute;
    std::optional<Ipv4Address> destination;
    for (std::size_t at = 1; at < words.size(); ++at) {
        const auto& word = words[at];
        if (!word.starts_with('-') || word.size() < 2) {
            if (destination) return fail(tool + ": one destination only: \"" + word + "\" is one too many.");
            destination = Ipv4Address::parse(word);
            if (!destination) {
                const auto* named = network.device_named(word);
                std::string text = tool + ": " + word + ": Name or service not known\n# resend has no names: use an address";
                if (named && !named->ports.empty() && named->ports.front().ipv4)
                    text += ", " + named->name + "'s is " + named->ports.front().ipv4->address().to_string();
                return fail(text + '.');
            }
            continue;
        }
        const char letter = word[1];
        if (letter == 'n' && word.size() == 2) continue;   // numeric: resend has no names anyway
        if (letter == '4' && word.size() == 2) continue;
        if (traceroute && letter == 'I' && word.size() == 2) continue;  // resend's traceroute is ICMP's
        const auto value = option_value(words, at);
        if (!value) return fail(tool + ": option requires an argument -- '" + std::string(1, letter) + "'");
        if (!traceroute && letter == 'c') {
            const auto count = integer(*value);
            if (!count || *count < 1 || *count > max_ping_count)
                return fail("ping: invalid argument: '" + *value + "': out of range: 1 <= value <= " + std::to_string(max_ping_count));
            start.options.count = static_cast<std::uint16_t>(*count);
            start.default_count = false;
        } else if (!traceroute && (letter == 'i' || letter == 'W')) {
            const auto time = seconds(*value);
            if (!time || *time < min_ping_time || *time > max_ping_time)
                return fail("ping: bad " + std::string(letter == 'i' ? "timing interval" : "linger time") + ": " + *value +
                            "\n# resend takes 0.01 to 60 seconds.");
            (letter == 'i' ? start.options.interval : start.options.timeout) = *time;
        } else if (!traceroute && letter == 't') {
            const auto ttl = integer(*value);
            if (!ttl || *ttl < 1 || *ttl > 255) return fail("ping: invalid argument: '" + *value + "': out of range: 1 <= value <= 255");
            start.options.ttl = static_cast<std::uint8_t>(*ttl);
        } else if (traceroute && letter == 'm') {
            const auto hops = integer(*value);
            if (!hops || *hops < 1 || *hops > max_hops) return fail("first hop out of range\n# resend takes -m 1 to 255.");
            start.options.count = static_cast<std::uint16_t>(*hops);
        } else if (traceroute && letter == 'q') {
            if (*value != "1") return fail("traceroute: resend sends one probe per hop: -q 1.");
        } else {
            return fail(tool + ": option -" + std::string(1, letter) + " is not in resend's " + tool + ".\n# Try \"help " + tool + "\".");
        }
    }
    if (!destination) return fail("Usage: " + tool + (traceroute ? " [-m HOPS] ADDRESS" : " [-c COUNT] [-i SECONDS] [-W SECONDS] [-t TTL] ADDRESS"));
    start.destination = *destination;
    Result result;
    result.start = start;
    return result;
}

// The help fits the terminal of the inspector: 42 columns. Sentences are
// one line each, for the screen to wrap; lines that start with "# " are
// resend's own remarks.
const char* const general_help =
    "Commands, as on Linux. Ports: p1, p2, ...\n"
    " ip address     ports and addresses\n"
    " ip link        ports, up or down\n"
    " ip route       the routing table\n"
    " ip neigh       the ARP cache\n"
    " bridge fdb     a switch's MAC table\n"
    " bridge link    a switch's ports\n"
    " ping ADDRESS\n"
    " traceroute ADDRESS\n"
    " clear          empties the screen\n"
    "To change the network:\n"
    " ip address add ADDRESS/LENGTH dev PORT\n"
    " ip address del ADDRESS/LENGTH dev PORT\n"
    " ip link set PORT up|down\n"
    " ip route add PREFIX via ADDRESS\n"
    " ip route add default via ADDRESS\n"
    " ip route replace ...  ip route del ...\n"
    "More with help ip, help ping, help traceroute and help bridge. Short forms work as on Linux: ip a, ip r, ip n.\n"
    "# Tab completes, Up and Down bring back what was typed, Esc goes back to the canvas.\n";

} // namespace

std::string port_name(std::uint8_t port) { return "p" + std::to_string(port + 1); }

std::optional<std::uint8_t> port_index(std::string_view name, const DeviceConfig& device) {
    if (name.size() < 2 || name[0] != 'p') return std::nullopt;
    int number = 0;
    for (const char c : name.substr(1)) {
        if (c < '0' || c > '9' || number > 100) return std::nullopt;
        number = number * 10 + (c - '0');
    }
    if (number < 1 || static_cast<std::size_t>(number) > device.ports.size() || name[1] == '0') return std::nullopt;
    return static_cast<std::uint8_t>(number - 1);
}

std::string prompt(const DeviceConfig& device) { return "root@" + device.name + ":~#"; }

std::vector<std::string> words(std::string_view line) {
    std::vector<std::string> out;
    std::string word;
    for (const char c : line) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!word.empty()) out.push_back(std::move(word)), word.clear();
        } else {
            word += c;
        }
    }
    if (!word.empty()) out.push_back(std::move(word));
    while (!out.empty() && out.front() == "sudo") out.erase(out.begin());
    return out;
}

std::string help(std::string_view command) {
    if (command.empty()) return general_help;
    if (command == "ip")
        return "ip OBJECT [COMMAND]: the ports, their addresses, the routes and the ARP cache.\n"
               " ip address    every port, its address\n"
               " ip link       every port, up or down\n"
               " ip route      the routing table\n"
               " ip neigh      addresses and their MAC\n"
               " ip route get 10.2.0.20\n"
               "               the route it would take\n"
               "To change them:\n"
               " ip address add 192.168.1.10/24 dev p1\n"
               " ip address del 192.168.1.10/24 dev p1\n"
               " ip link set p1 up      (or down)\n"
               " ip route add default via 192.168.1.1\n"
               " ip route add 10.2.0.0/24 via 10.12.0.2\n"
               " ip route replace ...  ip route del ...\n"
               "Short forms: ip a, ip l, ip r, ip n. Options: -4 (resend has only IPv4) and -br, one line a port.\n";
    if (command == "ping")
        return "ping [OPTIONS] ADDRESS: sends Echo Requests from this host and prints each reply as it comes back.\n"
               " -c COUNT    requests to send\n"
               " -i SECONDS  between requests (1)\n"
               " -W SECONDS  to wait for a reply (1)\n"
               " -t TTL      their time to live (64)\n"
               "# Without -c, Linux goes on until Ctrl+C: resend sends 4.\n";
    if (command == "traceroute")
        return "traceroute [-m HOPS] ADDRESS: finds the routers on the way, one Echo Request a hop with a growing TTL, as traceroute -I -q 1 does.\n"
               " -m HOPS     hops to try at most (30)\n";
    if (command == "bridge")
        return "bridge OBJECT: a switch's tables.\n"
               " bridge fdb   the MACs learned, by port\n"
               " bridge link  the ports, forwarding or not\n";
    if (command == "help") return "help [COMMAND]: what the commands do. help ip, help ping, help traceroute, help bridge.\n";
    if (command == "clear") return "clear: empties the screen. What was typed stays in the history.\n";
    return "help: no help for \"" + std::string(command) + "\". Try help on its own.\n";
}

Result run(std::string_view line, DeviceId device_id, const Scenario& network, const Simulation* simulation) {
    const auto words = term::words(line);
    if (words.empty()) return {};
    const auto* device = network.device(device_id);
    if (!device) return fail("This device is gone.");
    const auto& command = words[0];
    if (command == "help") return print(help(words.size() > 1 ? std::string_view(words[1]) : std::string_view{}));
    if (command == "clear") {
        Result result;
        result.clear = true;
        return result;
    }
    if (command == "exit" || command == "logout")
        return print("# The terminal stays open: Esc goes back to the canvas.\n");
    if (!device->powered) return fail(device->name + " is off: power it on to use its terminal.");
    if (command == "ip") return ip(network, *device, simulation, words);
    if (command == "bridge") return bridge(network, *device, simulation, words);
    if (command == "ping") return application(network, *device, words, false);
    if (command == "traceroute") return application(network, *device, words, true);
    if (command == "ifconfig" || command == "route" || command == "arp")
        return fail("bash: " + command + ": command not found\n# Linux replaced it with ip: try ip address, ip route or ip neigh.");
    return fail("bash: " + command + ": command not found\n# Here: ip, bridge, ping, traceroute, help and clear.");
}

std::optional<std::string> canonical(std::string_view line) {
    const auto words = term::words(line);
    if (words.empty()) return std::nullopt;
    const auto& command = words[0];
    if (command == "ip") {
        std::size_t at = 1;
        Words options;
        for (; at < words.size() && words[at].starts_with('-'); ++at)
            if (words[at] != "-4") options.push_back(words[at] == "-brief" ? "-br" : words[at]);
        if (at >= words.size()) return std::nullopt;
        const auto object = object_named(words[at]);
        if (!object) return std::nullopt;
        static const char* const names[] = {"address", "link", "route", "neighbour"};
        std::string out = "ip " + join(options) + (options.empty() ? "" : " ") + names[static_cast<int>(*object)];
        ++at;
        std::string verb = at < words.size() ? words[at++] : "show";
        if (is_show(verb)) verb = "show";
        else if (is_delete(verb)) verb = "del";
        else if (matches(verb, "add")) verb = "add";
        else if (*object == Object::Route && matches(verb, "replace")) verb = "replace";
        out += ' ' + verb;
        // "dev" before a port, as ip writes it.
        for (; at < words.size(); ++at) {
            if (words[at] == "dev") continue;
            if (words[at].size() > 1 && words[at][0] == 'p' && std::isdigit(static_cast<unsigned char>(words[at][1]))) out += " dev";
            out += ' ' + (words[at] == "0.0.0.0/0" ? std::string("default") : words[at]);
        }
        return out;
    }
    if (command == "bridge") {
        if (words.size() < 2) return std::nullopt;
        const std::string object = matches(words[1], "fdb") ? "fdb" : matches(words[1], "link") ? "link" : "";
        if (object.empty()) return std::nullopt;
        return "bridge " + object + " show";
    }
    if (command == "ping" || command == "traceroute") {
        // The destination first, then the options in order.
        std::string destination;
        std::vector<std::string> options;
        for (std::size_t at = 1; at < words.size(); ++at) {
            const auto& word = words[at];
            if (!word.starts_with('-')) {
                destination = word;
                continue;
            }
            if (word == "-n" || word == "-4" || word == "-I") continue;
            std::string option = word.substr(0, 2);
            if (word.size() > 2) option += ' ' + word.substr(2);
            else if (at + 1 < words.size()) option += ' ' + words[++at];
            options.push_back(option);
        }
        if (destination.empty()) return std::nullopt;
        std::sort(options.begin(), options.end());
        std::string out = command + ' ' + destination;
        for (const auto& option : options) out += ' ' + option;
        return out;
    }
    if (command == "help" || command == "clear") return join(words);
    return std::nullopt;
}

std::string application_output(const Simulation& simulation, const PingStatus& ping, bool default_count) {
    std::ostringstream out;
    const auto* source = simulation.known_device(ping.source);
    const auto* port = source && !source->ports.empty() ? &source->ports.front() : nullptr;
    const auto destination = ping.destination.to_string();
    if (ping.options.traceroute) {
        out << "traceroute to " << destination << " (" << destination << "), " << ping.options.count << " hops max, 60 byte packets\n";
        for (const auto& echo : ping.echoes) {
            if (echo.state == EchoState::Waiting) break;
            out << (echo.sequence < 10 ? " " : "") << echo.sequence << "  ";
            if (echo.responder == Ipv4Address{}) {
                out << '*';
                if (echo.state == EchoState::Lost && echo.reason != Reason::EchoTimeout)
                    out << "\n# " << explain::describe(echo.reason);
                out << '\n';
                continue;
            }
            out << echo.responder.to_string() << " (" << echo.responder.to_string() << ")  " << ms3(echo.finished - echo.sent) << " ms"
                << (echo.reason == Reason::NetUnreachable ? " !N" : echo.reason == Reason::HostUnreachable ? " !H" : "") << '\n';
        }
        return out.str();
    }
    out << "PING " << destination << " (" << destination << ") " << ping_data_size << '(' << ping_data_size + 28 << ") bytes of data.\n";
    if (default_count) out << "# No -c: resend sends " << ping.options.count << " requests and stops.\n";
    const auto& records = simulation.trace().records();
    SimTime low = SimTime::max(), high{}, total{};
    double squares = 0;
    std::vector<std::string> notes;
    bool unreachable = false;  // said once, as ping stops there
    for (const auto& echo : ping.echoes) {
        if (echo.state == EchoState::Replied) {
            const auto rtt = echo.finished - echo.sent;
            low = std::min(low, rtt);
            high = std::max(high, rtt);
            total += rtt;
            const double ms = std::chrono::duration<double, std::milli>(rtt).count();
            squares += ms * ms;
            const auto* reply = echo.record < records.size() && records[echo.record].pdu ? simulation.trace().pdu(*records[echo.record].pdu).ipv4() : nullptr;
            out << 8 + (reply ? reply->icmp.data_size : ping_data_size) << " bytes from " << destination << ": icmp_seq=" << echo.sequence
                << " ttl=" << (reply ? int{reply->ttl} : int{default_ttl}) << " time=" << rtt_text(rtt) << " ms\n";
        } else if (echo.state == EchoState::Lost) {
            if (echo.responder != Ipv4Address{}) {
                const char* text = echo.reason == Reason::TtlExceeded ? "Time to live exceeded"
                    : echo.reason == Reason::HostUnreachable          ? "Destination Host Unreachable"
                                                                      : "Destination Net Unreachable";
                out << "From " << echo.responder.to_string() << " icmp_seq=" << echo.sequence << ' ' << text << '\n';
            } else if (echo.reason == Reason::ArpUnresolved && port && port->ipv4) {
                // The host's own ARP got no answer: Linux says so for itself.
                out << "From " << port->ipv4->address().to_string() << " icmp_seq=" << echo.sequence << " Destination Host Unreachable\n";
            } else if ((echo.reason == Reason::NoRoute || echo.reason == Reason::GatewayOffLink || echo.reason == Reason::NoAddress) && !unreachable) {
                out << "ping: connect: Network is unreachable\n";
                unreachable = true;
            }
            if (echo.reason != Reason::EchoTimeout && echo.responder == Ipv4Address{})
                notes.push_back("# icmp_seq=" + std::to_string(echo.sequence) + ": " + explain::describe(echo.reason));
            else if (echo.reason == Reason::EchoTimeout)
                notes.push_back("# icmp_seq=" + std::to_string(echo.sequence) + ": no reply within " + rtt_text(ping.options.timeout) + " ms");
        }
    }
    if (ping.state == PingState::Running) return out.str();
    const auto sent = ping.echoes.size();
    const auto replies = static_cast<std::size_t>(ping.replies());
    const auto lost = sent ? static_cast<int>(std::lround(static_cast<double>(sent - replies) * 100.0 / static_cast<double>(sent))) : 0;
    out << "\n--- " << destination << " ping statistics ---\n"
        << sent << " packets transmitted, " << replies << " received, " << lost << "% packet loss, time "
        << std::chrono::duration_cast<std::chrono::milliseconds>(ping.finished - ping.started).count() << "ms\n";
    if (replies) {
        const double count = static_cast<double>(replies);
        const double mean = std::chrono::duration<double, std::milli>(total).count() / count;
        const double deviation = std::sqrt(std::max(0.0, squares / count - mean * mean));
        char buffer[96];
        std::snprintf(buffer, sizeof buffer, "%.3f/%.3f/%.3f/%.3f", std::chrono::duration<double, std::milli>(low).count(), mean,
                      std::chrono::duration<double, std::milli>(high).count(), deviation);
        out << "rtt min/avg/max/mdev = " << buffer << " ms\n";
    }
    for (const auto& note : notes) out << note << '\n';
    return out.str();
}

// --- Completion and menus ---------------------------------------------------

namespace {

Words first_words(const DeviceConfig& device) {
    Words out = {"ip"};
    if (device.kind == DeviceKind::Switch) out.push_back("bridge");
    if (device.kind == DeviceKind::Host) out.insert(out.end(), {"ping", "traceroute"});
    out.insert(out.end(), {"help", "clear"});
    return out;
}
Words port_words(const DeviceConfig& device) {
    Words out;
    for (std::size_t i = 0; i < device.ports.size(); ++i) out.push_back(port_name(static_cast<std::uint8_t>(i)));
    return out;
}

Words candidates_after(const Words& before, const DeviceConfig& device) {
    if (before.empty()) return first_words(device);
    const auto& command = before[0];
    if (command == "help") return before.size() == 1 ? Words{"ip", "ping", "traceroute", "bridge", "clear"} : Words{};
    if (command == "bridge") {
        if (before.size() == 1) return {"fdb", "link"};
        if (before.size() == 2) return {"show"};
        return {};
    }
    if (command != "ip") return {};
    std::size_t at = 1;
    while (at < before.size() && before[at].starts_with('-')) ++at;
    if (at >= before.size()) return {"address", "link", "route", "neigh", "-br"};
    const auto object = object_named(before[at]);
    if (!object) return {};
    if (at + 1 >= before.size()) {
        switch (*object) {
        case Object::Address: return {"show", "add", "del"};
        case Object::Link: return {"show", "set"};
        case Object::Route: return {"show", "get", "add", "replace", "del"};
        case Object::Neighbour: return {"show"};
        }
    }
    const auto& last = before.back();
    if (last == "dev") return port_words(device);
    const auto& verb = before[at + 1];
    const std::size_t args = before.size() - (at + 2);
    if (*object == Object::Link && verb == "set") {
        if (args == 0) return port_words(device);
        if (args == 1 && port_index(last, device)) return {"up", "down"};
        return {};
    }
    if (*object == Object::Address && (matches(verb, "add") || is_delete(verb))) {
        if (args == 0 && is_delete(verb)) {
            Words out;
            for (const auto& port : device.ports)
                if (port.ipv4) out.push_back(port.ipv4->to_string());
            return out;
        }
        if (args == 1) return {"dev"};
        return {};
    }
    if (*object == Object::Address && is_show(verb) && args == 0) {
        auto out = port_words(device);
        out.insert(out.begin(), "dev");
        return out;
    }
    if (*object == Object::Route && (matches(verb, "add") || matches(verb, "replace") || is_delete(verb))) {
        if (args == 0) {
            Words out = {"default"};
            if (is_delete(verb))
                for (const auto& route : device.routes) out.push_back(destination_text(route.destination));
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        }
        if (args == 1) return {"via"};
        if (last != "via" && args % 2 == 1) return {"dev"};
    }
    return {};
}

} // namespace

Completion complete(std::string_view line, DeviceId device_id, const Scenario& network) {
    Completion completion{std::string(line), {}};
    const auto* device = network.device(device_id);
    if (!device) return completion;
    auto all = term::words(line);
    const bool fresh = line.empty() || line.back() == ' ';
    const std::string last = fresh || all.empty() ? std::string{} : all.back();
    if (!fresh && !all.empty()) all.pop_back();
    Words found;
    for (const auto& candidate : candidates_after(all, *device))
        if (candidate.starts_with(last)) found.push_back(candidate);
    if (found.empty()) return completion;
    // What all of them start with.
    std::string common = found.front();
    for (const auto& candidate : found) {
        std::size_t same = 0;
        while (same < common.size() && same < candidate.size() && common[same] == candidate[same]) ++same;
        common.resize(same);
    }
    std::string head(line.substr(0, line.size() - last.size()));
    completion.line = head + common + (found.size() == 1 ? " " : "");
    if (found.size() > 1) completion.candidates = found;
    return completion;
}

std::vector<Menu> menus(DeviceId device_id, const Scenario& network) {
    std::vector<Menu> out;
    const auto* device = network.device(device_id);
    if (!device) return out;
    Menu ip{"ip", {}};
    const bool routes = device->kind != DeviceKind::Switch;
    if (routes) ip.entries.push_back({"ip address", "Show the ports and their addresses", true});
    ip.entries.push_back({"ip link", "Show the ports: enabled or not, with carrier or not", true});
    if (routes) {
        ip.entries.push_back({"ip route", "Show the routing table: where datagrams go", true});
        ip.entries.push_back({"ip route get ADDRESS", "Show the route a datagram for ADDRESS takes", false});
        ip.entries.push_back({"ip neigh", "Show the ARP cache: the addresses whose MAC is known", true});
    }
    for (std::size_t i = 0; i < device->ports.size(); ++i) {
        const auto index = static_cast<std::uint8_t>(i);
        const auto name = port_name(index);
        const auto& port = device->ports[i];
        if (routes) {
            if (port.ipv4) ip.entries.push_back({"ip address del " + port.ipv4->to_string() + " dev " + name, "Remove " + name + "'s address", true});
            else ip.entries.push_back({"ip address add ADDRESS/LENGTH dev " + name, "Give " + name + " an address", false});
        }
        ip.entries.push_back(port.admin_up ? MenuEntry{"ip link set " + name + " down", "Disable " + name, true}
                                           : MenuEntry{"ip link set " + name + " up", "Enable " + name, true});
    }
    if (device->kind == DeviceKind::Host) {
        if (device->gateway) {
            ip.entries.push_back({"ip route replace default via ADDRESS", "Change the default gateway", false});
            ip.entries.push_back({"ip route del default", "Remove the default gateway", true});
        } else {
            ip.entries.push_back({"ip route add default via ADDRESS", "Set the default gateway: the router for other networks", false});
        }
    } else if (device->kind == DeviceKind::Router) {
        ip.entries.push_back({"ip route add PREFIX via ADDRESS", "Add a static route to a network through a next hop", false});
        ip.entries.push_back({"ip route add default via ADDRESS", "Add a default route", false});
        for (const auto& route : device->routes)
            ip.entries.push_back({"ip route del " + destination_text(route.destination) + " via " + route.next_hop.to_string(),
                                  "Remove the route to " + destination_text(route.destination), true});
    }
    out.push_back(std::move(ip));
    if (device->kind == DeviceKind::Host) {
        Menu ping{"ping", {}};
        ping.entries.push_back({"ping -c 3 ADDRESS", "Ping an address three times", false});
        ping.entries.push_back({"traceroute ADDRESS", "Find the routers on the way to an address", false});
        int shown = 0;
        for (const auto& other : network.devices) {
            if (other.id == device->id || other.kind != DeviceKind::Host || other.ports.empty() || !other.ports.front().ipv4) continue;
            if (++shown > 8) break;
            const auto address = other.ports.front().ipv4->address().to_string();
            ping.entries.push_back({"ping -c 3 " + address, "Ping " + other.name + " three times", true});
        }
        out.push_back(std::move(ping));
    }
    if (device->kind == DeviceKind::Switch) {
        out.push_back({"bridge",
                       {{"bridge fdb", "Show the MAC table: the addresses learned and their port", true},
                        {"bridge link", "Show the ports and whether they forward", true}}});
    }
    return out;
}

std::vector<DeviceCommands> commands_between(const Scenario& before, const Scenario& after) {
    std::vector<DeviceCommands> out;
    for (const auto& device : after.devices) {
        const auto* old = before.device_named(device.name);
        if (!old || old->kind != device.kind || old->ports.size() != device.ports.size()) continue;
        DeviceCommands entry{device.name, {}};
        auto& lines = entry.commands;
        bool ports_changed = false;
        // Addresses first, removed then added; then the ports enabled; then
        // the routes, which Linux accepts only through an enabled port with
        // an address; the ports disabled last.
        for (std::size_t i = 0; i < device.ports.size(); ++i) {
            const auto name = port_name(static_cast<std::uint8_t>(i));
            const auto& was = old->ports[i].ipv4;
            const auto& now = device.ports[i].ipv4;
            if (was && was != now) lines.push_back("ip address del " + was->to_string() + " dev " + name), ports_changed = true;
        }
        for (std::size_t i = 0; i < device.ports.size(); ++i) {
            const auto name = port_name(static_cast<std::uint8_t>(i));
            const auto& was = old->ports[i].ipv4;
            const auto& now = device.ports[i].ipv4;
            if (now && was != now) lines.push_back("ip address add " + now->to_string() + " dev " + name), ports_changed = true;
        }
        for (std::size_t i = 0; i < device.ports.size(); ++i)
            if (device.ports[i].admin_up && !old->ports[i].admin_up)
                lines.push_back("ip link set " + port_name(static_cast<std::uint8_t>(i)) + " up"), ports_changed = true;
        if (device.kind == DeviceKind::Host) {
            if (old->gateway && !device.gateway) lines.push_back("ip route del default");
            // Linux drops a route whose network goes: it is given again.
            if (device.gateway && (device.gateway != old->gateway || ports_changed))
                lines.push_back("ip route replace default via " + device.gateway->to_string());
        } else if (device.kind == DeviceKind::Router) {
            for (const auto& route : old->routes)
                if (std::none_of(device.routes.begin(), device.routes.end(), [&](const Route& r) { return r.destination == route.destination; }))
                    lines.push_back("ip route del " + destination_text(route.destination));
            for (const auto& route : device.routes) {
                const bool same = std::find(old->routes.begin(), old->routes.end(), route) != old->routes.end();
                if (!same || ports_changed)
                    lines.push_back("ip route replace " + destination_text(route.destination) + " via " + route.next_hop.to_string());
            }
        }
        for (std::size_t i = 0; i < device.ports.size(); ++i)
            if (!device.ports[i].admin_up && old->ports[i].admin_up)
                lines.push_back("ip link set " + port_name(static_cast<std::uint8_t>(i)) + " down");
        if (!lines.empty()) out.push_back(std::move(entry));
    }
    return out;
}

} // namespace netsim::term
