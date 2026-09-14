#pragma once
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_pending_intent.hpp>
#include <stdexcept>
namespace lifecycle_fixture {
inline void apply(pineforge::source::PendingOrder& o, pineforge::exit_legs::Operation op, int64_t bar = 0) {
    using namespace pineforge::exit_legs;
    if (!o.incarnation) throw std::logic_error("literal lifecycle fixture requires instruction identity");
    if (!o.legs.target().incarnation) o.legs.attach(o.incarnation,
        o.leg_activation.bounds() ? o.leg_activation.bounds()->position_cycle : 0);
    const uint64_t event = o.legs.last_action() ? o.legs.last_action()->cause.event + 1 : 1;
    const Action action{o.legs.target(), o.legs.revision(), {event, bar, Domain::Ordinary, Phase::Observation}, std::move(op)};
    if (o.legs.apply(o.legs.target(), action) != Result::Applied) throw std::logic_error("literal lifecycle setup rejected");
}
inline void suspend(pineforge::source::PendingOrder& o, std::optional<int64_t> excluded = {},
                    std::optional<int64_t> held = {}) {
    using namespace pineforge::exit_legs;
    std::optional<ObservationWindow> window;
    if (excluded) window = ObservationWindow{{0,*excluded,Domain::Ordinary,Phase::Observation},o.legs.trail_best(),o.legs.trail_prefix()};
    std::optional<Barrier> hold;
    if (held) hold = Barrier{{0,*held,Domain::Ordinary,Phase::Observation}};
    apply(o, Suspend{{Leg::Stop,Leg::Limit},hold,window,{}});
}
inline void restore(pineforge::source::PendingOrder& o) {
    using namespace pineforge::exit_legs; apply(o, Restore{{Leg::Stop,Leg::Limit,Leg::Trail}});
}
inline void stage(pineforge::source::PendingOrder& o) {
    using namespace pineforge::exit_legs;
    apply(o, StageReplacement{{o.incarnation,o.legs.definition(o.incarnation),{{0,0,Domain::Ordinary,Phase::Observation}}}});
}
} // namespace lifecycle_fixture
