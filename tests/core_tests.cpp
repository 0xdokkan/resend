#include "check.hpp"
#include "core/address.hpp"
#include "core/ids.hpp"
#include "sim/scheduler.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using netsim::Ipv4Address;
using netsim::MacAddress;
using netsim::SimTime;

void ipv4_parsing() {
    for (const char* bad : {"", "1.2.3", "1.2.3.256", "1.2.3.4junk", "01.2.3.4", "1.2.3.4.5", "1..2.3",
                            ".1.2.3", "1.2.3.", " 1.2.3.4", "1.2.3.4 ", "+1.2.3.4", "1.2.-3.4", "1.2.3.0x4", "1234.1.1.1"})
        CHECK(!Ipv4Address::parse(bad));
    CHECK(Ipv4Address::parse("192.168.1.20")->bits() == 0xc0a80114U);
    CHECK(Ipv4Address::parse("0.0.0.0")->bits() == 0);
    CHECK(Ipv4Address::parse("255.255.255.255")->bits() == 0xffffffffU);
    for (const char* text : {"10.0.0.1", "192.168.1.20", "0.0.0.0", "255.255.255.255", "172.16.254.3"})
        CHECK(Ipv4Address::parse(text)->to_string() == text);
    // Numeric order: tables sorted by address list .9 before .10.
    CHECK(*Ipv4Address::parse("192.168.1.9") < *Ipv4Address::parse("192.168.1.10"));
}

void prefix_masks() {
    CHECK(netsim::prefix_mask(0) == 0);
    CHECK(netsim::prefix_mask(8) == 0xff000000U);
    CHECK(netsim::prefix_mask(24) == 0xffffff00U);
    CHECK(netsim::prefix_mask(31) == 0xfffffffeU);
    CHECK(netsim::prefix_mask(32) == 0xffffffffU);
}

void mac_addresses() {
    CHECK(MacAddress::local(1).to_string() == "02:00:00:00:00:01");
    CHECK(MacAddress::local(0xa1b2c3d4).to_string() == "02:00:a1:b2:c3:d4");
    CHECK(MacAddress::broadcast().to_string() == "ff:ff:ff:ff:ff:ff");
    CHECK(MacAddress::broadcast().is_broadcast());
    CHECK(!MacAddress::local(7).is_broadcast());
    // Locally administered (bit 1) and unicast (bit 0 clear) in the first octet.
    CHECK((MacAddress::local(7).bytes()[0] & 0x03) == 0x02);
    CHECK(MacAddress::local(1) < MacAddress::local(2));
    CHECK(MacAddress::local(1) == MacAddress::local(1));
}

void typed_ids() {
    const netsim::DeviceId a{3}, b{4};
    CHECK(a < b && a != b && a == netsim::DeviceId{3});
    // DeviceId and LinkId do not compare: `a == netsim::LinkId{3}` must not compile.
}

void scheduler_order() {
    netsim::Scheduler<int> queue;
    CHECK(queue.empty() && !queue.pop());
    queue.schedule(20ms, 1);
    queue.schedule(10ms, 2);
    queue.schedule(20ms, 3);
    queue.schedule(10ms, 4);
    queue.schedule(5us, 5);
    CHECK(queue.size() == 5);
    std::vector<int> order;
    std::vector<SimTime> times;
    while (auto due = queue.pop()) {
        order.push_back(due->payload);
        times.push_back(due->time);
        CHECK(queue.now() == due->time);
    }
    // Time first; equal times keep the order in which they were scheduled.
    CHECK((order == std::vector<int>{5, 2, 4, 1, 3}));
    CHECK((times == std::vector<SimTime>{5us, 10ms, 10ms, 20ms, 20ms}));
    CHECK(queue.now() == 20ms);
    CHECK(queue.empty());
}

void scheduler_cancel() {
    netsim::Scheduler<std::string> queue;
    auto keep = queue.schedule(1s, "keep");
    auto drop = queue.schedule(500ms, "drop");
    CHECK(keep && drop);
    CHECK(queue.cancel(drop));
    CHECK(!drop);                // the handle is cleared
    CHECK(!queue.cancel(drop));  // and cancelling it again does nothing
    CHECK(queue.size() == 1);
    auto due = queue.pop();
    CHECK(due && due->payload == "keep" && due->time == 1s);
    CHECK(!queue.cancel(keep));  // already run
    CHECK(queue.empty() && !queue.pop());

    // Entries scheduled while running, at the current time, run after the
    // ones already due at that time.
    netsim::Scheduler<int> chain;
    chain.schedule(0ns, 1);
    chain.schedule(0ns, 2);
    CHECK(chain.pop()->payload == 1);
    chain.schedule(chain.now(), 3);
    auto timer = chain.schedule(chain.now() + 1ns, 4);
    CHECK(chain.pop()->payload == 2);
    CHECK(chain.pop()->payload == 3);
    CHECK(chain.cancel(timer));
    CHECK(!chain.pop());
    CHECK(chain.now() == 0ns);  // cancelled entries never move the clock
}

void sim_time_resolution() {
    // 1500 bytes at 1 Gb/s: 12 µs, exactly representable.
    const SimTime serialization{1500 * 8};
    CHECK(serialization == 12us);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(SimTime{80'000'000}).count() == 80);
}
} // namespace

int main() {
    ipv4_parsing();
    prefix_masks();
    mac_addresses();
    typed_ids();
    scheduler_order();
    scheduler_cancel();
    sim_time_resolution();
    std::cout << "Core tests passed: " << test::checks << " checks.\n";
}
