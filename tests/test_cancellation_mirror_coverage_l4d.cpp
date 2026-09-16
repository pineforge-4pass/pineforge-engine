// A29 CHECK-parity native-route twin. Base body copied from ab9714be;
// rewrite only owner-private drives/reads while retaining literal checks.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost
#define PendingOrder L4dPendingOrder
#define pending_orders_ l4d_pending_rows()
#define OrderType L4dOrderType
#define ShortSeedCollisionRole L4dShortSeedRole
#define is_first_tick_ is_first_tick()
#define coof_fill_recalc_active_ l4d_coof_fill_recalc_active()
#define coof_cursor_is_bar_close_ l4d_coof_cursor_is_bar_close()

// Bounded native cancellation coverage.  Each case starts with a fresh
// resting order, mutates exactly one cancellation leaf through the public
// receipt API, and checks both the broker hash and the C mirror snapshot.
// No Pine/reference/corpus/grader execution is involved.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/pineforge.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;
using pineforge::source::L4dCancellationReceiptView;

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failures; \
} } while (0)

class Probe final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true);
    }

    using Mutation = std::function<void(L4dCancellationReceiptView&)>;

    void mutate(const Mutation& mutation) {
        CHECK(!pending_orders_.empty());
        if (pending_orders_.empty()) return;
        if (auto* receipt = adapter_.fixture_mutable_cancellation(0)) {
            L4dCancellationReceiptView view(*receipt);
            mutation(view);
        }
    }

    uint64_t hash() const { return broker_state_hash(); }
};

void fresh(Probe& p) {
    const Bar bar{100, 101, 99, 100, 1, 0};
    p.run(&bar, 1); // market entry remains pending until a second bar
}

pf_pending_order_v1_t mirror(Probe& p) {
    pf_pending_order_v1_t out{};
    CHECK(strategy_pending_order_get(&p, 0, &out, sizeof(out)) == 0);
    return out;
}

CancellationTarget target(uint64_t inc, int64_t owner, uint64_t revision) {
    return CancellationTarget{inc, owner, revision};
}

template <typename Receipt>
void cancel_with(Receipt& c, CancellationCause cause,
                 uint64_t source, int64_t sequence,
                 CancellationTarget t) {
    CHECK(c.cancel(cause, source, sequence, t, t) == CancellationResult::Applied);
}

struct Pin {
    const char* name;
    Probe::Mutation mutate;
    std::function<bool(const pf_pending_order_v1_t&, const pf_pending_order_v1_t&)> changed;
};

std::vector<Pin> pins() {
    const auto t = target(700, 3, 9);
    return {
        {"cause", [=](auto& c) { cancel_with(c, CancellationCause::Dependency, 11, 1, t); },
         [](const auto& a, const auto& b) { return a.cancellation_cause != b.cancellation_cause; }},
        {"state", [=](auto& c) { cancel_with(c, CancellationCause::Dependency, 12, 2, t); },
         [](const auto& a, const auto& b) { return a.cancellation_state != b.cancellation_state; }},
        {"source_incarnation", [=](auto& c) { cancel_with(c, CancellationCause::Dependency, 101, 3, t); },
         [](const auto& a, const auto& b) { return a.cancellation_source_incarnation != b.cancellation_source_incarnation; }},
        {"source_sequence", [=](auto& c) { cancel_with(c, CancellationCause::Dependency, 102, 77, t); },
         [](const auto& a, const auto& b) { return a.cancellation_source_sequence != b.cancellation_source_sequence; }},
        {"target_incarnation", [=](auto& c) { cancel_with(c, CancellationCause::Dependency, 13, 4, target(701, 3, 9)); },
         [](const auto& a, const auto& b) { return a.cancellation_target_incarnation != b.cancellation_target_incarnation; }},
        {"target_owner", [=](auto& c) { cancel_with(c, CancellationCause::Dependency, 14, 5, target(702, 4, 9)); },
         [](const auto& a, const auto& b) { return a.cancellation_target_owner != b.cancellation_target_owner; }},
        {"target_revision", [=](auto& c) { cancel_with(c, CancellationCause::Dependency, 15, 6, target(703, 3, 10)); },
         [](const auto& a, const auto& b) { return a.cancellation_target_revision != b.cancellation_target_revision; }},
        {"claim_consumed", [](auto& c) { CHECK(c.bind_close_claim(12.5, 0.0)); },
         [](const auto& a, const auto& b) { return a.cancellation_close_claim_consumed != b.cancellation_close_claim_consumed; }},
        {"claim_retired", [](auto& c) { CHECK(c.bind_close_claim(1.0, 12.5)); },
         [](const auto& a, const auto& b) { return a.cancellation_close_claim_retired != b.cancellation_close_claim_retired; }},
        {"claim_release", [](auto& c) {
             CHECK(c.bind_close_claim(1.0, 0.5));
             const auto t = target(704, 3, 9);
             cancel_with(c, CancellationCause::Dependency, 16, 7, t);
             double ledger = 0.0;
             CHECK(c.release_close_claim_once(ledger));
         },
         [](const auto& a, const auto& b) { return a.cancellation_close_claim_release != b.cancellation_close_claim_release; }},
    };
}

void check_hash_and_mirror_leaf_pins() {
    for (const Pin& pin : pins()) {
        Probe p;
        fresh(p);
        const auto before_hash = p.hash();
        const auto before = mirror(p);
        p.mutate(pin.mutate);
        const auto after_hash = p.hash();
        const auto after = mirror(p);
        if (before_hash == after_hash)
            std::fprintf(stderr, "FAIL hash pin %s unchanged\n", pin.name), ++failures;
        CHECK(pin.changed(before, after));
    }
}

void check_replay_and_invalid_target_no_effect() {
    Probe p;
    fresh(p);
    const auto before_hash = p.hash();
    const auto before = mirror(p);
    p.mutate([](auto& c) {
        const auto good = target(800, 1, 2);
        CHECK(c.cancel(CancellationCause::Dependency, 17, 8,
                       target(801, 1, 2), good) == CancellationResult::Invalid);
    });
    CHECK(p.hash() == before_hash);
    const auto after = mirror(p);
    CHECK(std::memcmp(&before, &after, sizeof(before)) == 0);

    p.mutate([](auto& c) {
        const auto good = target(800, 1, 2);
        CHECK(c.cancel(CancellationCause::Dependency, 17, 8, good, good)
              == CancellationResult::Applied);
        CHECK(c.cancel(CancellationCause::Dependency, 17, 8, good, good)
              == CancellationResult::Replay);
        CHECK(c.cancel(CancellationCause::Replacement, 17, 8, good, good)
              == CancellationResult::AlreadyTerminal);
    });
}

void check_claim_inputs_fail_closed() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (const auto pair : std::vector<std::pair<double, double>>{
            {nan, 1.0}, {1.0, nan}, {nan, -1.0}, {-1.0, 0.0},
            {std::numeric_limits<double>::infinity(), 0.0},
            {1.0, std::numeric_limits<double>::infinity()}}) {
        OrderCancellationReceipt c;
        CHECK(!c.bind_close_claim(pair.first, pair.second));
        CHECK(c.close_claim_release() == CloseClaimRelease::Unbound);
    }
    OrderCancellationReceipt c;
    CHECK(c.bind_close_claim(1.0, 0.5));
    const auto t = target(900, 1, 2);
    cancel_with(c, CancellationCause::Dependency, 21, 9, t);
    double nan_ledger = nan;
    CHECK(!c.release_close_claim_once(nan_ledger));
    CHECK(c.close_claim_release() == CloseClaimRelease::Pending);
    double ledger = 0.0;
    CHECK(c.release_close_claim_once(ledger));
    CHECK(!c.release_close_claim_once(ledger));
    CHECK(std::abs(ledger - 1.5) < 1e-12);
}

void check_atomic_cancel_and_release() {
    OrderCancellationReceipt c;
    CHECK(c.bind_close_claim(2.0, 0.25));
    const auto t = target(901, 4, 6);
    double ledger = 3.0;
    CHECK(c.cancel_and_release(CancellationCause::Dependency, 77, 8,
                               t, t, &ledger) == CancellationResult::Applied);
    CHECK(c.cancelled() && c.close_claim_release() == CloseClaimRelease::Released);
    CHECK(std::abs(ledger - 5.25) < 1e-12);

    OrderCancellationReceipt invalid;
    CHECK(invalid.bind_close_claim(2.0, 0.25));
    const auto before = invalid;
    double nan_ledger = std::numeric_limits<double>::quiet_NaN();
    CHECK(invalid.cancel_and_release(CancellationCause::Dependency, 77, 8,
                                     t, t, &nan_ledger) == CancellationResult::Invalid);
    CHECK(!invalid.cancelled());
    CHECK(invalid.close_claim_release() == before.close_claim_release());
    CHECK(invalid.source_incarnation() == before.source_incarnation());
}
}

int main() {
    check_hash_and_mirror_leaf_pins();
    check_replay_and_invalid_target_no_effect();
    check_claim_inputs_fail_closed();
    check_atomic_cancel_and_release();
    std::printf("cancellation mirror/hash coverage: %d failures\n", failures);
    return failures ? 1 : 0;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
