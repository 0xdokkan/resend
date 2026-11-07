// The inspector: the properties of the selected device or cable, its
// tables, and the edits made from it.
#include "app_controller.hpp"

#include "core/text.hpp"
#include "scenario/edit.hpp"

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
} // namespace

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
}

bool AppController::change(const std::vector<netsim::Edit>& edits, const std::string& label) {
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
    if (device) {
        const bool host = device->kind == netsim::DeviceKind::Host;
        inspector.set_kind(host ? "host" : "switch");
        inspector.set_heading(device->name.c_str());
        inspector.set_powered(device->powered);
        if (fields) {
            inspector.set_name(device->name.c_str());
            const auto& address = device->ports.front().ipv4;
            inspector.set_address(host && address ? address->address().to_string().c_str() : "");
            inspector.set_prefix(host && address ? std::to_string(address->length()).c_str() : "");
            inspector.set_ageing(host ? "" : netsim::format_duration(device->mac_ageing).c_str());
        }
        std::vector<PortView> rows;
        for (std::size_t index = 0; index < device->ports.size(); ++index) {
            const netsim::PortRef ref{device->id, static_cast<std::uint8_t>(index)};
            const auto* link = network.link_at(ref);
            PortView row;
            row.index = static_cast<int>(index);
            row.label = tr("Port {0}", {std::to_string(index + 1)}).c_str();
            row.mac = device->ports[index].mac.to_string().c_str();
            row.up = device->ports[index].admin_up;
            row.cable = link ? netsim::explain::port_text(network, link->a == ref ? link->b : link->a).c_str() : "";
            row.link_id = link ? static_cast<int>(link->id.value) : -1;
            rows.push_back(row);
        }
        ports_->set_vector(rows);
    } else if (cable) {
        inspector.set_kind("cable");
        inspector.set_heading(tr("Cable").c_str());
        inspector.set_cable_a(netsim::explain::port_text(network, cable->a).c_str());
        inspector.set_cable_b(netsim::explain::port_text(network, cable->b).c_str());
        inspector.set_cable_up(cable->up);
        if (fields) inspector.set_delay(netsim::format_duration(cable->delay).c_str());
        ports_->set_vector({});
    } else {
        inspector.set_kind("");
        inspector.set_heading("");
        ports_->set_vector({});
    }
    refresh_tables();
}

// The ARP cache of the selected host or the MAC table of the selected
// switch, at the instant shown. Rows are replaced only when their text
// changes: the playback calls this on every frame.
void AppController::refresh_tables() {
    const auto& selection = canvas_.selection();
    const auto* device = selection.kind == Selection::Kind::Device ? scenario().device(netsim::DeviceId{selection.id}) : nullptr;
    std::vector<std::vector<std::string>> rows;
    std::string note;
    const auto now = simulation().now();
    if (device && device->kind == netsim::DeviceKind::Host) {
        for (const auto& entry : simulation().arp_cache(device->id)) {
            const bool waiting = entry.state == netsim::ArpState::Incomplete;
            rows.push_back({entry.ip.to_string(), waiting ? std::string("—") : entry.mac.to_string(),
                            waiting ? tr("incomplete") : tr("reachable"),
                            waiting ? (entry.queued == 1 ? tr("1 datagram waits") : tr("{0} datagrams wait", {std::to_string(entry.queued)}))
                                    : seconds(entry.expires - now)});
        }
        if (rows.empty()) note = tr("No mappings learned yet. Send a ping to watch ARP.");
    } else if (device) {
        for (const auto& entry : simulation().mac_table(device->id))
            rows.push_back({entry.mac.to_string(), std::to_string(entry.port + 1), seconds(now - entry.updated)});
        if (rows.empty()) note = tr("No addresses learned yet. The switch learns from incoming frames.");
    }
    ui_->global<UiInspector>().set_table_note(note.c_str());
    if (rows == shown_table_) return;
    shown_table_ = rows;
    std::vector<std::shared_ptr<slint::Model<slint::StandardListViewItem>>> models;
    for (const auto& row : rows) {
        std::vector<slint::StandardListViewItem> items;
        for (const auto& cell : row) items.push_back({cell.c_str()});
        models.push_back(std::make_shared<slint::VectorModel<slint::StandardListViewItem>>(items));
    }
    table_->set_vector(models);
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
            status(netsim::explain::describe(netsim::Problem::InvalidName));
            return;
        }
        edits.push_back(netsim::Rename{device->id, name});
    }
    if (device->kind == netsim::DeviceKind::Host) {
        const auto prefix = whole_number(std::string(inspector.get_prefix()));
        if (!prefix) {
            status(tr("The prefix length is a whole number, for example 24."));
            return;
        }
        auto problem = netsim::Problem::None;
        const auto address = netsim::edit::host_address(scenario(), device->id, std::string(inspector.get_address()), *prefix, problem);
        if (!address) {
            status(netsim::explain::describe(problem));
            return;
        }
        if (address->address != device->ports.front().ipv4) edits.push_back(*address);
    } else {
        const auto ageing = netsim::parse_duration(std::string(inspector.get_ageing()));
        if (!ageing) {
            status(tr("Write the ageing time as a duration, for example 300s."));
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

void AppController::apply_cable() {
    const auto& selection = canvas_.selection();
    const auto* cable = selection.kind == Selection::Kind::Cable ? scenario().link(netsim::LinkId{selection.id}) : nullptr;
    if (!cable) return;
    const auto delay = netsim::parse_duration(std::string(ui_->global<UiInspector>().get_delay()));
    if (!delay) {
        status(tr("Write the delay as a duration, for example 10ms or 250us."));
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
