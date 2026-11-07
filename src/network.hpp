#pragma once

#include "core/address.hpp"
#include "sim/scheduler.hpp"
#include "sim/trace.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace netsim {

enum class DeviceKind { Host, Switch };

struct Device {
    int id{};
    std::string name;
    DeviceKind kind{DeviceKind::Host};
    float x{}, y{};
    Ipv4Address ip;  // hosts only
    int prefix{24};
    bool enabled{true};
    MacAddress mac;
};

struct Link {
    int id{}, a{}, a_port{}, b{}, b_port{};
    bool enabled{true};
};

struct Event {
    std::uint64_t time_ms{};
    std::string protocol, summary, detail;
    int from{-1}, to{-1}, link_id{-1};
    bool success{true};
    int pdu{-1};  // the frame the event is about, as an index in the trace
};

struct ArpEntry { Ipv4Address ip; MacAddress mac; };
struct MacEntry { MacAddress mac; int port{}; };

// A deliberately bounded Ethernet LAN model: static IPv4 /1../30,
// one port per host, eight ports per switch, no L2 loops, router or TCP.
// Events use simulated time; presentation speed cannot change results.
// Every step appends structured records to trace(); events() holds the
// readable explanation of the same steps for the current interface.
class Engine {
public:
    int add_device(DeviceKind kind, float x, float y);
    bool connect(int a, int b);
    bool remove_device(int id);
    bool remove_link(int id);
    bool configure_host(int id, const std::string& name,
                        const std::string& ip, int prefix);
    bool rename_device(int id, const std::string& name);
    bool set_link_enabled(int id, bool enabled);
    bool set_device_enabled(int id, bool enabled);
    bool move_device(int id, float x, float y);
    bool start_ping(int source_id, const std::string& destination_ip);
    bool step();
    // Processes every event due at or before `time`, then moves the clock to
    // `time`, so commands issued afterwards start from there.
    void run_until(SimTime time);
    std::optional<SimTime> next_event_time() const { return scheduler_.next_time(); }
    bool pending() const;
    void reset_simulation();
    void clear();
    void seed_demo();

    const std::vector<Device>& devices() const { return devices_; }
    const std::vector<Link>& links() const { return links_; }
    const std::vector<Event>& events() const { return events_; }
    const Trace& trace() const { return trace_; }
    const Device* device(int id) const;
    const Link* link(int id) const;
    std::vector<ArpEntry> arp_table(int id) const;
    std::vector<MacEntry> mac_table(int id) const;
    const std::string& last_error() const { return last_error_; }
    SimTime now() const { return scheduler_.now(); }
    std::uint64_t time_ms() const;
    bool ping_succeeded() const { return ping_succeeded_; }

private:
    enum class ActionKind { ArpProbe, Deliver, ArpTimeout, EchoTimeout, LocalPing, NoRoute };
    struct Action {
        ActionKind kind{};
        int from{-1}, to{-1}, link_id{-1}, port{-1}, attempt{};
        PduId pdu{};
    };
    using Queue = Scheduler<Action>;
    std::vector<Device> devices_;
    std::vector<Link> links_;
    std::vector<Event> events_;
    Trace trace_;
    std::map<int, std::map<Ipv4Address, MacAddress>> arp_;
    std::map<int, std::map<MacAddress, int>> mac_;
    Queue scheduler_;
    // The pending ARP probe or timeout, and the Echo timeout: cancelled as
    // soon as the reply they wait for arrives.
    Queue::Handle arp_timer_, echo_timer_;
    int next_device_{1}, next_link_{1}, ping_source_{-1};
    std::uint64_t next_ping_{}, current_ping_{};
    Ipv4Address ping_destination_;
    std::string last_error_;
    bool ping_active_{}, awaiting_arp_{}, ping_succeeded_{};
    Device* mutable_device(int id);
    bool fail(const std::string& message);
    void record(TraceRecord record);
    bool transmit(int from, int port, PduId pdu);
    void deliver(const Action& action);
    void send_echo_request();
    void emit(const std::string& protocol, const std::string& summary,
              const std::string& detail, int from = -1, int to = -1,
              int link_id = -1, bool success = true, int pdu = -1);
    int free_port(int id) const;
    bool connected(int a, int b) const;
};

} // namespace netsim
