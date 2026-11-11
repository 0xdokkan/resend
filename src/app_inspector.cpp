// The inspector: the properties of the selected device or cable, its
// tables, and the edits made from it.
#include "app_controller.hpp"

#include "core/text.hpp"
#include "scenario/edit.hpp"
#include "sim/routing.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <string>

namespace resend {
namespace {
using netsim::text::tr;

std::string seconds(netsim::SimTime time) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f s", std::chrono::duration<double>(time).count());
    return buffer;
}

// A whole number, all of `text`.
std::optional<int> whole_number(const std::string& text) {
    try {
        std::size_t consumed = 0;
        const int value = std::stoi(text, &consumed);
        if (consumed == text.size()) return value;
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

std::string trimmed(std::string text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

using TableModel = slint::VectorModel<std::shared_ptr<slint::Model<slint::StandardListViewItem>>>;

// Characters in a UTF-8 text: the monospace gives each the same width.
int characters(const std::string& text) {
    return static_cast<int>(std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

// Each column as wide as its title and its widest cell.
void set_columns(slint::VectorModel<TableColumn>& model, const std::vector<std::string>& titles,
                 const std::vector<std::vector<std::string>>& rows) {
    std::vector<TableColumn> columns;
    for (std::size_t i = 0; i < titles.size(); ++i) {
        int chars = characters(titles[i]);
        for (const auto& row : rows)
            if (i < row.size()) chars = std::max(chars, characters(row[i]));
        columns.push_back({titles[i].c_str(), chars});
    }
    const auto same = model.row_count() == columns.size() && [&] {
        for (std::size_t i = 0; i < columns.size(); ++i)
            if (model.row_data(i) != columns[i]) return false;
        return true;
    }();
    if (!same) model.set_vector(columns);
}

void set_table(TableModel& model, const std::vector<std::vector<std::string>>& rows) {
    std::vector<std::shared_ptr<slint::Model<slint::StandardListViewItem>>> models;
    for (const auto& row : rows) {
        std::vector<slint::StandardListViewItem> items;
        for (const auto& cell : row) items.push_back({cell.c_str()});
        models.push_back(std::make_shared<slint::VectorModel<slint::StandardListViewItem>>(items));
    }
    model.set_vector(models);
}

// Why a router does not use one of its static routes.
std::string inactive_note(const netsim::DeviceConfig& router, const netsim::Route& route) {
    const auto& ports = router.ports;
    if (std::any_of(ports.begin(), ports.end(), [&](const auto& port) { return port.ipv4 && port.ipv4->network() == route.destination.network() && port.ipv4->length() == route.destination.length(); }))
        return tr("Not used: a connected network has the same destination.");
    if (std::any_of(ports.begin(), ports.end(), [&](const auto& port) { return port.ipv4 && port.ipv4->address() == route.next_hop; }))
        return tr("Not used: the next hop is an address of this router.");
    return tr("Not used: the next hop is on none of the router's networks.");
}
} // namespace

const netsim::DeviceConfig* AppController::selected_device() const {
    const auto& selection = canvas_.selection();
    return selection.kind == Selection::Kind::Device ? scenario().device(netsim::DeviceId{selection.id}) : nullptr;
}

void AppController::bind_inspector() {
    const auto& inspector = ui_->global<UiInspector>();
    inspector.on_apply([this] { apply_device(); });
    inspector.on_toggle_power([this] {
        const auto& selection = canvas_.selection();
        const auto* device = selection.kind == Selection::Kind::Device ? scenario().device(netsim::DeviceId{selection.id}) : nullptr;
        if (!device) return;
        const bool power = !device->powered;
        change({netsim::SetPowered{device->id, power}}, power ? tr("Power on {0}", {device->name}) : tr("Power off {0}", {device->name}));
    });
    inspector.on_remove_device([this] { canvas_changed(canvas_.key(CanvasController::Key::Delete)); });
    inspector.on_set_port_up([this](int port, bool up) {
        const auto& selection = canvas_.selection();
        const auto* device = selection.kind == Selection::Kind::Device ? scenario().device(netsim::DeviceId{selection.id}) : nullptr;
        if (!device || port < 0 || port >= static_cast<int>(device->ports.size())) return;
        const netsim::PortRef ref{device->id, static_cast<std::uint8_t>(port)};
        const auto name = netsim::explain::port_text(scenario(), ref);
        change({netsim::SetPortUp{ref, up}}, up ? tr("Enable {0}", {name}) : tr("Disable {0}", {name}));
    });
    inspector.on_select_cable([this](int id) {
        if (id >= 0) canvas_changed(canvas_.select({Selection::Kind::Cable, static_cast<std::uint32_t>(id)}));
    });
    inspector.on_apply_cable([this] { apply_cable(); });
    inspector.on_toggle_cable([this] {
        const auto& selection = canvas_.selection();
        const auto* cable = selection.kind == Selection::Kind::Cable ? scenario().link(netsim::LinkId{selection.id}) : nullptr;
        if (!cable) return;
        const auto ends = netsim::explain::port_text(scenario(), cable->a) + " — " + netsim::explain::port_text(scenario(), cable->b);
        change({netsim::SetLinkUp{cable->id, !cable->up}}, cable->up ? tr("Disable cable {0}", {ends}) : tr("Enable cable {0}", {ends}));
    });
    inspector.on_unplug([this] { canvas_changed(canvas_.key(CanvasController::Key::Delete)); });
    inspector.on_port_address_edited([this](int port, slint::SharedString text) {
        if (port >= 0 && port < static_cast<int>(typed_addresses_.size())) typed_addresses_[static_cast<std::size_t>(port)] = std::string(text);
    });
    inspector.on_set_port_count([this](int count) { set_port_count(count); });
    inspector.on_add_route([this] { add_route(); });
    inspector.on_remove_route([this](int index) { remove_route(index); });
}

void AppController::refuse(const std::string& field, const std::string& text) {
    note(text);  // said under the field
    const auto& inspector = ui_->global<UiInspector>();
    inspector.set_error_field(field.c_str());
    inspector.set_error_text(text.c_str());
}

bool AppController::change(const std::vector<netsim::Edit>& edits, const std::string& label) {
    const auto& inspector = ui_->global<UiInspector>();
    inspector.set_error_field("");
    inspector.set_error_text("");
    const auto before = workspace_.commands().size();
    if (!report(workspace_.change(edits, label), label, before)) return false;
    network_changed();
    return true;
}

// `fields` fills the text fields too: when the selection or the network
// changed, not while the playback runs and the user may be typing.
void AppController::refresh_inspector(bool fields) {
    const auto& inspector = ui_->global<UiInspector>();
    const auto& network = scenario();
    const auto& selection = canvas_.selection();
    const auto* device = selection.kind == Selection::Kind::Device ? network.device(netsim::DeviceId{selection.id}) : nullptr;
    const auto* cable = selection.kind == Selection::Kind::Cable ? network.link(netsim::LinkId{selection.id}) : nullptr;
    const auto& canvas = ui_->global<UiCanvas>();
    canvas.set_selected_device(device ? static_cast<int>(device->id.value) : -1);
    if (device && device->kind == netsim::DeviceKind::Host && device->id.value != last_host_) {
        previous_host_ = last_host_;
        last_host_ = device->id.value;
    }
    canvas.set_selected_powered(device && device->powered);
    canvas.set_selected_cable_up(cable && cable->up);
    if (fields) {
        inspector.set_error_field("");
        inspector.set_error_text("");
    }
    std::string lock_note;
    if (device && lab_)
        for (const auto& lock : lab_->locks)
            if (const auto* given = lab_->network.device_named(lock.device); given && given->id == device->id)
                lock_note = netsim::explain::lock_text({device->name, lock.settings});
    inspector.set_lock_note(lock_note.c_str());
    if (device) {
        const bool host = device->kind == netsim::DeviceKind::Host;
        const bool router = device->kind == netsim::DeviceKind::Router;
        inspector.set_kind(host ? "host" : router ? "router" : "switch");
        canvas.set_selected_kind(host ? "host" : router ? "router" : "switch");
        inspector.set_heading(device->name.c_str());
        inspector.set_powered(device->powered);
        if (fields) {
            inspector.set_name(device->name.c_str());
            const auto& address = device->ports.front().ipv4;
            inspector.set_address(host && address ? address->address().to_string().c_str() : "");
            inspector.set_prefix(host && address ? std::to_string(address->length()).c_str() : "");
            inspector.set_gateway(host && device->gateway ? device->gateway->to_string().c_str() : "");
            inspector.set_ageing(device->kind == netsim::DeviceKind::Switch ? netsim::format_duration(device->mac_ageing).c_str() : "");
            inspector.set_route_destination("");
            inspector.set_route_via("");
            typed_addresses_.assign(device->ports.size(), std::nullopt);
        }
        inspector.set_port_count(static_cast<int>(device->ports.size()));
        inspector.set_min_ports(static_cast<int>(netsim::min_router_ports));
        inspector.set_max_ports(static_cast<int>(netsim::max_router_ports));
        std::vector<PortView> rows;
        for (std::size_t index = 0; index < device->ports.size(); ++index) {
            const netsim::PortRef ref{device->id, static_cast<std::uint8_t>(index)};
            const auto* link = network.link_at(ref);
            PortView row{};
            row.index = static_cast<int>(index);
            row.label = tr("Port {0}", {std::to_string(index + 1)}).c_str();
            row.mac = device->ports[index].mac.to_string().c_str();
            row.up = device->ports[index].admin_up;
            row.cable = link ? netsim::explain::port_text(network, link->a == ref ? link->b : link->a).c_str() : "";
            row.link_id = link ? static_cast<int>(link->id.value) : -1;
            const auto& address = device->ports[index].ipv4;
            row.address = router && address ? address->to_string().c_str() : "";
            rows.push_back(row);
        }
        ports_->set_vector(rows);
        // A router's static routes, and the table it builds from them.
        std::vector<RouteView> routes;
        std::vector<std::vector<std::string>> table;
        if (router) {
            const auto entries = netsim::routing_table(*device);
            const auto connected = entries.size() - device->routes.size();
            for (std::size_t index = 0; index < device->routes.size(); ++index) {
                const auto& route = device->routes[index];
                const bool active = entries[connected + index].active();
                routes.push_back({static_cast<int>(index), route.destination.to_string().c_str(), route.next_hop.to_string().c_str(), active,
                                  active ? "" : inactive_note(*device, route).c_str()});
            }
            for (const auto& entry : entries)
                table.push_back({entry.destination.to_string(), entry.connected ? tr("connected") : entry.next_hop.to_string(),
                                 entry.port ? std::to_string(*entry.port + 1) : std::string("—"), entry.active() ? tr("active") : tr("not used")});
        }
        routes_->set_vector(routes);
        set_table(*routing_, table);
        auto widest = table;
        widest.push_back({"255.255.255.255/32", "255.255.255.255", "8", tr("not used")});
        widest.push_back({"", tr("connected"), "—", tr("active")});
        set_columns(*routing_columns_, {tr("Network"), tr("Next hop"), tr("Port"), tr("State")}, widest);
    } else if (cable) {
        inspector.set_kind("cable");
        canvas.set_selected_kind("cable");
        inspector.set_heading(tr("Cable").c_str());
        inspector.set_cable_a(netsim::explain::port_text(network, cable->a).c_str());
        inspector.set_cable_b(netsim::explain::port_text(network, cable->b).c_str());
        inspector.set_cable_up(cable->up);
        if (fields) inspector.set_delay(netsim::format_duration(cable->delay).c_str());
        ports_->set_vector({});
        routes_->set_vector({});
    } else {
        inspector.set_kind("");
        canvas.set_selected_kind("");
        inspector.set_heading("");
        ports_->set_vector({});
        routes_->set_vector({});
    }
    refresh_tables();
    refresh_terminal(true);
}

// The ARP cache of the selected host or router or the MAC table of the
// selected switch, at the instant shown. Rows are replaced only when their
// text changes: the playback calls this on every frame.
void AppController::refresh_tables() {
    const auto* device = selected_device();
    std::vector<std::vector<std::string>> rows;
    std::string note;
    const auto now = simulation().now();
    if (device && device->kind != netsim::DeviceKind::Switch) {
        const bool router = device->kind == netsim::DeviceKind::Router;
        for (const auto& entry : simulation().arp_cache(device->id)) {
            const bool waiting = entry.state == netsim::ArpState::Incomplete;
            std::vector<std::string> row{entry.ip.to_string(), waiting ? std::string("—") : entry.mac.to_string()};
            if (router) row.push_back(std::to_string(entry.port + 1));
            row.push_back(waiting ? tr("incomplete") : tr("reachable"));
            row.push_back(waiting ? (entry.queued == 1 ? tr("1 datagram waits") : tr("{0} datagrams wait", {std::to_string(entry.queued)}))
                                  : seconds(entry.expires - now));
            rows.push_back(row);
        }
        if (rows.empty()) note = tr("No mappings learned yet. Send a ping to watch ARP.");
    } else if (device) {
        for (const auto& entry : simulation().mac_table(device->id))
            rows.push_back({entry.mac.to_string(), std::to_string(entry.port + 1), seconds(now - entry.updated)});
        if (rows.empty()) note = tr("No addresses learned yet. The switch learns from incoming frames.");
    }
    ui_->global<UiInspector>().set_table_note(note.c_str());
    if (device) {
        // Wide enough for the longest values the model allows, so that no
        // cell is ever cut, then for what the table holds.
        std::vector<std::string> titles;
        std::vector<std::vector<std::string>> widest = rows;
        if (device->kind == netsim::DeviceKind::Switch) {
            titles = {tr("MAC address"), tr("Port"), tr("Age")};
            widest.push_back({"ff:ff:ff:ff:ff:ff", "8", "1000000.0 s"});
        } else if (device->kind == netsim::DeviceKind::Router) {
            titles = {tr("Address"), "MAC", tr("Port"), tr("State"), tr("Expires in")};
            widest.push_back({"255.255.255.255", "ff:ff:ff:ff:ff:ff", "8", tr("incomplete"), tr("{0} datagrams wait", {"999"})});
        } else {
            titles = {tr("Address"), "MAC", tr("State"), tr("Expires in")};
            widest.push_back({"255.255.255.255", "ff:ff:ff:ff:ff:ff", tr("incomplete"), tr("{0} datagrams wait", {"999"})});
        }
        set_columns(*table_columns_, titles, widest);
    }
    if (rows == shown_table_) return;
    shown_table_ = rows;
    set_table(*table_, rows);
}

void AppController::apply_device() {
    const auto& selection = canvas_.selection();
    const auto* device = selection.kind == Selection::Kind::Device ? scenario().device(netsim::DeviceId{selection.id}) : nullptr;
    if (!device) return;
    const auto& inspector = ui_->global<UiInspector>();
    std::vector<netsim::Edit> edits;
    const std::string name(inspector.get_name());
    if (name != device->name) {
        if (!netsim::valid_name(name)) {
            refuse("name", netsim::explain::describe(netsim::Problem::InvalidName));
            return;
        }
        edits.push_back(netsim::Rename{device->id, name});
    }
    if (device->kind == netsim::DeviceKind::Router) {
        for (std::size_t index = 0; index < device->ports.size() && index < typed_addresses_.size(); ++index) {
            if (!typed_addresses_[index]) continue;
            const auto text = trimmed(*typed_addresses_[index]);
            std::optional<netsim::Ipv4Prefix> address;
            if (!text.empty()) {
                address = netsim::Ipv4Prefix::parse(text);
                if (!address) {
                    refuse("port-" + std::to_string(index),
                           tr("Write the address of port {0} with its prefix length, for example 10.1.0.1/24.", {std::to_string(index + 1)}));
                    return;
                }
            }
            if (address != device->ports[index].ipv4)
                edits.push_back(netsim::SetAddress{{device->id, static_cast<std::uint8_t>(index)}, address});
        }
    } else if (device->kind == netsim::DeviceKind::Host) {
        // An empty address leaves the host without one, whatever the prefix says.
        const auto address_text = trimmed(std::string(inspector.get_address()));
        const auto prefix = address_text.empty() ? std::optional<int>(24) : whole_number(std::string(inspector.get_prefix()));
        if (!prefix) {
            refuse("prefix", tr("The prefix length is a whole number, for example 24."));
            return;
        }
        auto problem = netsim::Problem::None;
        const auto address = netsim::edit::host_address(scenario(), device->id, address_text, *prefix, problem);
        if (!address) {
            refuse(problem == netsim::Problem::InvalidPrefix ? "prefix" : "address", netsim::explain::describe(problem));
            return;
        }
        if (address->address != device->ports.front().ipv4) edits.push_back(*address);
        const auto gateway_text = trimmed(std::string(inspector.get_gateway()));
        std::optional<netsim::Ipv4Address> gateway;
        if (!gateway_text.empty()) {
            gateway = netsim::Ipv4Address::parse(gateway_text);
            if (!gateway) {
                refuse("gateway", tr("Write the default gateway as an IPv4 address, for example 192.168.1.1, or leave it empty."));
                return;
            }
        }
        if (gateway != device->gateway) edits.push_back(netsim::SetGateway{device->id, gateway});
    } else {
        const auto ageing = netsim::parse_duration(std::string(inspector.get_ageing()));
        if (!ageing) {
            refuse("ageing", tr("Write the ageing time as a duration, for example 300s."));
            return;
        }
        if (*ageing != device->mac_ageing) edits.push_back(netsim::SetAgeing{device->id, *ageing});
    }
    if (edits.empty()) {
        status(tr("Nothing to change."));
        return;
    }
    change(edits, tr("Configure {0}", {device->name}));
}

void AppController::set_port_count(int count) {
    const auto* device = selected_device();
    if (!device || device->kind != netsim::DeviceKind::Router || count < 0) return;
    for (auto port = static_cast<std::size_t>(count); port < device->ports.size(); ++port)
        if (scenario().link_at({device->id, static_cast<std::uint8_t>(port)})) {
            status(tr("Port {0} has a cable: unplug it first.", {std::to_string(port + 1)}));
            return;
        }
    const auto ports = netsim::edit::resized_ports(*device, static_cast<std::size_t>(count));
    change({netsim::SetPorts{device->id, ports}}, tr("{0} ports on {1}", {std::to_string(count), device->name}));
}

void AppController::add_route() {
    const auto* device = selected_device();
    if (!device || device->kind != netsim::DeviceKind::Router) return;
    const auto& inspector = ui_->global<UiInspector>();
    const auto destination = netsim::Ipv4Prefix::parse(trimmed(std::string(inspector.get_route_destination())));
    if (!destination) {
        refuse("route-destination", tr("Write the network with its prefix length, for example 10.2.0.0/24, or 0.0.0.0/0 for the default route."));
        return;
    }
    const auto via = netsim::Ipv4Address::parse(trimmed(std::string(inspector.get_route_via())));
    if (!via) {
        refuse("route-via", tr("Write the next hop as an IPv4 address, for example 10.12.0.2: a neighbour router on a connected network."));
        return;
    }
    auto routes = device->routes;
    routes.push_back({*destination, *via});
    change({netsim::SetRoutes{device->id, routes}},
           tr("Add the route {0} via {1} on {2}", {destination->to_string(), via->to_string(), device->name}));
}

void AppController::remove_route(int index) {
    const auto* device = selected_device();
    if (!device || device->kind != netsim::DeviceKind::Router || index < 0 || index >= static_cast<int>(device->routes.size())) return;
    auto routes = device->routes;
    const auto route = routes[static_cast<std::size_t>(index)];
    routes.erase(routes.begin() + index);
    change({netsim::SetRoutes{device->id, routes}},
           tr("Remove the route {0} via {1} on {2}", {route.destination.to_string(), route.next_hop.to_string(), device->name}));
}

void AppController::apply_cable() {
    const auto& selection = canvas_.selection();
    const auto* cable = selection.kind == Selection::Kind::Cable ? scenario().link(netsim::LinkId{selection.id}) : nullptr;
    if (!cable) return;
    const auto delay = netsim::parse_duration(std::string(ui_->global<UiInspector>().get_delay()));
    if (!delay) {
        refuse("delay", tr("Write the delay as a duration, for example 10ms or 250us."));
        return;
    }
    if (*delay == cable->delay) {
        status(tr("Nothing to change."));
        return;
    }
    const auto ends = netsim::explain::port_text(scenario(), cable->a) + " — " + netsim::explain::port_text(scenario(), cable->b);
    change({netsim::SetDelay{cable->id, *delay}}, tr("Set the delay of cable {0}", {ends}));
}

} // namespace resend
