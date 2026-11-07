#include "sim/ping_app.hpp"

namespace netsim {

void PingApp::sent(Context& context, PingId id) {
    waiting_[id.value] = context.schedule(echo_timeout, TimerKind::EchoTimeout, id.value);
}

bool PingApp::reply(Context& context, const Arrival& arrival, Ipv4Address from, const IcmpMessage& message) {
    for (auto it = waiting_.begin(); it != waiting_.end(); ++it) {
        const PingId id{it->first};
        if (static_cast<std::uint16_t>(id.value) != message.identifier || context.ping(id).destination != from) continue;
        context.cancel(it->second);
        waiting_.erase(it);
        context.finish_ping(id, PingState::Succeeded, Reason::None, &arrival);
        return true;
    }
    return false;
}

void PingApp::timeout(Context& context, PingId id) {
    waiting_.erase(id.value);
    context.finish_ping(id, PingState::Failed, Reason::EchoTimeout);
}

} // namespace netsim
