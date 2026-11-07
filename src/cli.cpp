#include "network.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    netsim::Engine engine;
    engine.seed_demo();
    if (argc > 1 && std::string(argv[1]) == "--help") {
        std::cout << "resend: Ethernet / ARP / IPv4 / ICMP sandbox core\n"
                     "Usage: resend-cli [destination IPv4] [--disconnect]\n"
                     "Default LAN: Host 1 192.168.1.10/24 -> switch -> Host 2 192.168.1.20/24\n"
                     "This prototype does not implement TCP, routing or IPv6.\n";
        return 0;
    }
    const std::string target = argc > 1 ? argv[1] : "192.168.1.20";
    if (argc > 2 && std::string(argv[2]) == "--disconnect" && !engine.links().empty())
        engine.set_link_enabled(engine.links().back().id, false);
    if (!engine.start_ping(1, target)) {
        std::cerr << engine.last_error() << '\n';
        return 2;
    }
    std::size_t printed = 0;
    auto print_new = [&] {
        while (printed < engine.events().size()) {
            const auto& event = engine.events()[printed++];
            std::cout << event.time_ms << " ms  [" << event.protocol << "] "
                      << event.summary << '\n' << "  " << event.detail << '\n';
        }
    };
    print_new();
    std::size_t budget = 10000;
    while (engine.pending() && budget-- > 0) {
        engine.step();
        print_new();
    }
    if (engine.pending()) {
        std::cerr << "Simulation event budget exceeded.\n";
        return 3;
    }
    return engine.ping_succeeded() ? 0 : 1;
}
