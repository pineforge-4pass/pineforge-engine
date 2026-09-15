#include "l4c_native_route_guard.hpp"
#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle_fixture_config_shim.hpp"
// Native lifecycle clocks and a literal engine hook/rebind control.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/compat/pine/exit_lifecycle.hpp>
#include <cstdio>
#include <functional>
using namespace pineforge;
using namespace pineforge::exit_legs;
namespace {
int checks = 0, failed = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (0)
struct Words {
    std::vector<uint64_t> values;
    void u(uint64_t x) { values.push_back(x); }
    void i(int64_t x) { u(static_cast<uint64_t>(x)); }
    void b(bool x) { u(x); }
    void d(double x) { uint64_t bits; std::memcpy(&bits, &x, sizeof(bits)); u(bits); }
};
auto facts(const Lifecycle& x) { Words w; x.visit(w); return w.values; }
Action action(const Lifecycle& x, Frame f, Operation op) {
    return {x.target(), x.revision(), f, std::move(op)};
}
Lifecycle window(Domain domain) {
    Lifecycle x; x.attach(1, 1); x.set_trail_price(110);
    const Frame excluded{1, 10, domain, Phase::Observation};
    auto request = action(x, excluded,
        Suspend{{Leg::Stop, Leg::Limit}, {}, ObservationWindow{excluded, 104, 104}, {}});
    CHECK(x.apply(x.target(), request) == Result::Applied);
    return x;
}
void observation_windows() {
    for (Domain domain : {Domain::Ordinary, Domain::Coof, Domain::Magnifier,
                          Domain::MagnifierCoof, Domain::RawTicks}) {
        for (Fold fold : {Fold::Prefix, Fold::Continue}) {
            auto x = window(domain); const auto before = facts(x);
            for (int64_t bar : {9, 10}) for (Phase phase : {Phase::Observation, Phase::AfterMargin}) {
                const auto excluded = action(x, {2, bar, domain, phase}, Observe{999, 1, 1, fold});
                CHECK(x.apply(x.target(), excluded) == Result::InvalidAction);
                CHECK(facts(x) == before);
            }
            CHECK(x.trail_best() == 104 && x.trail_prefix() == 104);
            CHECK(!x.available(Leg::Trail, 10) && x.available(Leg::Trail, 11));
            auto later = action(x, {2, 11, domain, Phase::Observation}, Observe{105, 99, 1, fold});
            CHECK(x.apply(x.target(), later) == Result::Applied);
            CHECK(x.trail_best() == 105 && x.trail_prefix() == 104);
            const auto updated = facts(x);
            CHECK(x.apply(x.target(), later) == Result::Replay);
            CHECK(facts(x) == updated);
            auto continue_later = action(x, {3, 11, domain, Phase::Observation}, Observe{106, 98, 1, Fold::Continue});
            CHECK(x.apply(x.target(), continue_later) == Result::Applied);
            CHECK(x.trail_best() == 106 && x.trail_prefix() == 104);
        }
    }
    // Domain conversion is an explicit caller selection, never comparison of
    // unrelated bar coordinates. The shared causal event sequence still holds.
    auto cross = window(Domain::Ordinary);
    auto selected = action(cross, {2, 1, Domain::RawTicks, Phase::Observation}, Observe{105, 99, 1, Fold::Prefix});
    CHECK(cross.apply(cross.target(), selected) == Result::Applied);
    CHECK(cross.trail_best() == 105);
    auto future_window = window(Domain::Ordinary);
    Frame future{10, 10, Domain::Ordinary, Phase::Observation};
    auto resuspend = action(future_window, {2, 9, Domain::Ordinary, Phase::Observation},
        Suspend{{Leg::Stop}, {}, ObservationWindow{future, 104, 104}, {}});
    CHECK(future_window.apply(future_window.target(), resuspend) == Result::Applied);
    const auto before = facts(future_window);
    auto too_early = action(future_window, {3, 11, Domain::RawTicks, Phase::Observation}, Observe{999, 1, 1, Fold::Prefix});
    CHECK(future_window.apply(future_window.target(), too_early) == Result::InvalidAction);
    CHECK(facts(future_window) == before);
}
Lifecycle staged(Domain domain) {
    Lifecycle previous; previous.attach(2, 1); previous.set_stop_price(95);
    Lifecycle x; x.attach(3, 1); x.set_stop_price(90);
    const Frame request{10, 10, domain, Phase::Observation};
    const auto create = action(x, request, StageReplacement{{2, previous.definition(2), {request, {}, 0}}});
    CHECK(x.apply(x.target(), create) == Result::Applied); return x;
}
void completion_clocks() {
    for (Domain domain : {Domain::Ordinary, Domain::Coof, Domain::Magnifier,
                          Domain::MagnifierCoof, Domain::RawTicks}) {
        auto x = staged(domain); const auto before = facts(x);
        const Frame occurrence{11, 10, domain, Phase::AfterMargin};
        for (Frame receipt : {Frame{12, 9, domain, Phase::AfterMargin},
                              Frame{12, 10, domain, Phase::Observation}}) {
            const auto early = action(x, receipt, CompleteBarrier{occurrence, x.release_barrier()});
            CHECK(x.apply(x.target(), early) == Result::InvalidAction);
            CHECK(facts(x) == before);
        }
        // Event ordering applies even when coordinates are from another domain.
        auto future = occurrence; future.event = 13;
        auto early_event = action(x, {12, 10, domain, Phase::AfterMargin}, CompleteBarrier{future, x.release_barrier()});
        CHECK(x.apply(x.target(), early_event) == Result::InvalidAction);
        CHECK(facts(x) == before);
        auto good = action(x, {12, 10, domain, Phase::AfterMargin}, CompleteBarrier{occurrence, x.release_barrier()});
        CHECK(x.apply(x.target(), good) == Result::Applied);
        CHECK(!x.pending_replacement() && !x.dormant());
        const auto after = facts(x); CHECK(x.apply(x.target(), good) == Result::Replay); CHECK(facts(x) == after);
    }
    auto routed = staged(Domain::Ordinary); const auto before = facts(routed);
    auto inconsistent = action(routed, {12, 9, Domain::Ordinary, Phase::AfterMargin},
        CompleteBarrier{{11, 1, Domain::Coof, Phase::AfterMargin}, routed.release_barrier()});
    CHECK(routed.apply(routed.target(), inconsistent) == Result::InvalidAction);
    CHECK(facts(routed) == before);
    auto future_cross = action(routed, {12, 1, Domain::Coof, Phase::AfterMargin},
        CompleteBarrier{{13, 11, Domain::Ordinary, Phase::AfterMargin}, routed.release_barrier()});
    CHECK(routed.apply(routed.target(), future_cross) == Result::InvalidAction);
    CHECK(facts(routed) == before);
    auto selected = action(routed, {12, 1, Domain::Coof, Phase::AfterMargin},
        CompleteBarrier{{11, 1, Domain::Coof, Phase::AfterMargin}, routed.release_barrier()});
    CHECK(routed.apply(routed.target(), selected) == Result::Applied);
    CHECK(!routed.pending_replacement());
}
class HookBook : public pineforge::source::PineStrategyHost {
    bool bound_ = false;
    std::vector<pineforge::source::L4cPendingOrder> observed_;
public:
    HookBook() { initial_capital_ = 100000; commission_value_ = 0; margin_long_ = margin_short_ = 0; }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("E", true, absent(), absent(), 1);
        if (bar_index_ != 1) return;
        CHECK(position_qty_ == 1);
        strategy_exit("X", "E", absent(), 95);
        strategy_exit("X", "E", absent(), 90);
        observed_ = l4c_pending_orders();
        bound_ = true;
    }
    void exercise() {
        const Bar bars[] = {{100,100,100,100,1,0}, {100,100,100,100,1,60000}};
        run(bars, 2); CHECK(last_error().empty()); CHECK(bound_);
        CHECK(position_qty_ == 1 && trades_.empty());
        CHECK(observed_.size() == 1);
        CHECK(!observed_.empty());
        if (observed_.size() != 1) return;
        const auto& x = observed_.front();
        CHECK(x.id == "X" && x.from_entry == "E");
        CHECK(x.type == pineforge::source::L4cOrderType::EXIT);
        if (!x.leg_activation.bounds()) return;
        const auto& receipt = *x.leg_activation.bounds();
        CHECK(receipt.stop_first_bar >= 1);
        CHECK(receipt.position_cycle > 0);
        CHECK(receipt.limit_first_bar >= receipt.stop_first_bar);
        CHECK(receipt.limit_first_bar >= 1);
    }
};
}
int main() {
    try { observation_windows(); completion_clocks(); HookBook book; book.exercise(); }
    catch (const std::exception& e) { ++failed; std::fprintf(stderr, "EXCEPTION %s\n", e.what()); }
    std::printf("exit lifecycle clocks: %d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
