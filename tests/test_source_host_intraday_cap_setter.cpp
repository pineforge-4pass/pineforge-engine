// The explicit source risk setter must retain IntradayCap's assignment
// semantics: attach the Pine policy, update its limit, and preserve its day
// ledger when a script changes the limit at statement time.
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdio>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(value) do {                                                        \
    ++checks;                                                                    \
    if (!(value)) {                                                              \
        ++failures;                                                              \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #value);           \
    }                                                                            \
} while (0)

using Cap = compat::pine::IntradayCap;
using CapAttachment = compat::pine::CapAttachment;

class Fixture final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {}

    const Cap& cap() const { return adapter_.cap; }

    bool allows_long() const { return adapter_.allows_risk_direction(true); }
    bool allows_short() const { return adapter_.allows_risk_direction(false); }
    bool drawdown_is_percent() const { return adapter_.max_drawdown_is_percent(); }
    bool intraday_loss_is_percent() const {
        return adapter_.max_intraday_loss_is_percent();
    }

    void seed_latched_day() {
        compat::pine::CapClock clock{};
        clock.timestamp = 1700000000000LL;
        clock.session = "24x7";
        clock.timezone = "UTC";
        clock.chart_day = 14;
        clock.chart_month = 9;

        compat::pine::Calculation calculation{};
        calculation.stream_idle = true;
        calculation.fifo = true;
        calculation.bar = 11;

        compat::pine::MatchedAttempt attempt{};
        attempt.kind = compat::pine::OrderKind::Entry;
        attempt.incarnation = 1;
        attempt.created_bar = calculation.bar;
        attempt.is_long = true;
        attempt.live_side = compat::pine::Side::Flat;
        attempt.pyramiding = 1;

        const auto admission = adapter_.cap.pre_dispatch(
            clock, calculation, attempt, 0);
        (void)adapter_.cap.post_dispatch(
            admission, calculation, attempt, compat::pine::Side::Flat, 0,
            compat::pine::Prices{100.0, 100.0, 100.0, 100.0});
    }
};

void check_cap_matches(const Cap& actual, const Cap& expected) {
    CHECK(actual.attachment() == expected.attachment());
    CHECK(actual.configuration().limit == expected.configuration().limit);
    CHECK(actual.budget().day().has_value() == expected.budget().day().has_value());
    if (actual.budget().day() && expected.budget().day())
        CHECK(*actual.budget().day() == *expected.budget().day());
    CHECK(actual.budget().charged_slots() == expected.budget().charged_slots());
    CHECK(actual.budget().latched() == expected.budget().latched());
    CHECK(actual.budget().transfer().has_value() == expected.budget().transfer().has_value());
    CHECK(actual.due_cause().has_value() == expected.due_cause().has_value());
    CHECK(actual.next_action() == expected.next_action());
}

void test_setter_matches_integer_assignment() {
    Fixture host;
    Cap direct;

    host.set_pine_risk_max_intraday_filled_orders(4);
    direct = 4;
    check_cap_matches(host.cap(), direct);
    CHECK(host.cap().attachment() == CapAttachment::LegacySource);
    CHECK(host.cap().configuration().limit == 4);
    CHECK(host.cap().budget().charged_slots() == 0);

    // Build an identical, already-latched day ledger on both objects. The
    // second assignment must change only the configured limit.
    host.set_pine_risk_max_intraday_filled_orders(1);
    direct = 1;
    host.seed_latched_day();

    compat::pine::CapClock clock{};
    clock.timestamp = 1700000000000LL;
    clock.session = "24x7";
    clock.timezone = "UTC";
    clock.chart_day = 14;
    clock.chart_month = 9;
    compat::pine::Calculation calculation{};
    calculation.stream_idle = true;
    calculation.fifo = true;
    calculation.bar = 11;
    compat::pine::MatchedAttempt attempt{};
    attempt.kind = compat::pine::OrderKind::Entry;
    attempt.incarnation = 1;
    attempt.created_bar = calculation.bar;
    attempt.is_long = true;
    attempt.live_side = compat::pine::Side::Flat;
    attempt.pyramiding = 1;
    const auto admission = direct.pre_dispatch(clock, calculation, attempt, 0);
    (void)direct.post_dispatch(
        admission, calculation, attempt, compat::pine::Side::Flat, 0,
        compat::pine::Prices{100.0, 100.0, 100.0, 100.0});

    CHECK(host.cap().budget().day().has_value());
    CHECK(host.cap().budget().charged_slots() == 1);
    CHECK(host.cap().budget().latched());
    host.set_pine_risk_max_intraday_filled_orders(7);
    direct = 7;
    check_cap_matches(host.cap(), direct);
    CHECK(host.cap().budget().charged_slots() == 1);
    CHECK(host.cap().budget().latched());
}

void test_direction_mapping_matches_pine_convention() {
    Fixture host;

    host.set_pine_risk_direction(-1);
    CHECK(!host.allows_long());
    CHECK(host.allows_short());

    host.set_pine_risk_direction(0);
    CHECK(host.allows_long());
    CHECK(host.allows_short());

    host.set_pine_risk_direction(1);
    CHECK(host.allows_long());
    CHECK(!host.allows_short());
}

void test_percent_flags_are_sticky() {
    Fixture host;

    host.set_pine_risk_max_drawdown(10.0, true);
    host.set_pine_risk_max_drawdown(500.0, false);
    CHECK(host.drawdown_is_percent());

    host.set_pine_risk_max_intraday_loss(10.0, true);
    host.set_pine_risk_max_intraday_loss(500.0, false);
    CHECK(host.intraday_loss_is_percent());
}

} // namespace

int main() {
    test_setter_matches_integer_assignment();
    test_direction_mapping_matches_pine_convention();
    test_percent_flags_are_sticky();
    std::printf("checks=%d failures=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
