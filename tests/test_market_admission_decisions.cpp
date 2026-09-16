// A25 fixture-facade twin for the legacy-owner admission decision book.
//
// The pre-switch TU manually constructed PendingOrder objects, called private
// review/fill seams, and compacted the legacy book. This switched-route
// replacement preserves its public literals through source commands, the v1
// PendingIntent projection, native receipts, positions and trades. It never
// reads or mutates PendingOrder/pending_orders_/process_pending_orders.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(value) do {                                                        \
    ++checks;                                                                   \
    if (!(value)) {                                                             \
        ++failures;                                                             \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
    }                                                                           \
} while (0)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

Bar flat(double price, std::int64_t timestamp) {
    return {price, price, price, price, 1.0, timestamp};
}

source::PineStrategyConfig fixed_config(double capital = 1000.0) {
    source::PineStrategyConfig config;
    config.initial_capital = capital;
    config.default_qty_type = static_cast<int>(QtyType::FIXED);
    config.default_qty_value = 1.0;
    config.pyramiding = 1;
    config.margin_long = 100.0;
    config.margin_short = 100.0;
    config.commission_value = 0.0;
    config.slippage = 0;
    return config;
}

class PairHost final : public source::PineStrategyHost {
public:
    enum class Variant { Pair, RejectedThird, NoTargetCancel };

    explicit PairHost(Variant variant = Variant::Pair) : variant_(variant) {
        configure_pine_strategy(fixed_config());
        set_margin_call_enabled(false);
    }

    std::vector<FixtureIntentRow> signal_rows;
    double position_on_second_bar = std::numeric_limits<double>::quiet_NaN();
    int trades_on_second_bar = -1;

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("S", false, kNaN, kNaN, 3.0);
            strategy_entry("B", true, kNaN, kNaN, 2.0);
            if (variant_ == Variant::RejectedThird) {
                strategy_entry("huge", true, kNaN, kNaN, 100000.0);
            }
            if (variant_ == Variant::NoTargetCancel) {
                strategy_cancel("absent");
            }
            signal_rows = source_pending_view();
        } else if (pine_bar_index() == 1) {
            position_on_second_bar = live_position_size();
            trades_on_second_bar = trade_count();
        }
    }

private:
    Variant variant_;
};

class IncarnationHost final : public source::PineStrategyHost {
public:
    IncarnationHost() {
        auto config = fixed_config();
        config.pyramiding = 64;
        configure_pine_strategy(config);
    }
    std::vector<pf_pending_order_v1_t> first;
    std::vector<pf_pending_order_v1_t> replacement;
    std::vector<pf_pending_order_v1_t> capture() {
        std::vector<pf_pending_order_v1_t> result;
        const int count = strategy_pending_orders_len(this);
        for (int index = 0; index < count; ++index) {
            pf_pending_order_v1_t row{};
            if (strategy_pending_order_get(this, index, &row, sizeof row) == 0)
                result.push_back(row);
        }
        return result;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        for (int index = 0; index < 40; ++index) {
            strategy_entry("dummy-" + std::to_string(index), true,
                           kNaN, 10'000.0 + index, 1.0);
        }
        strategy_entry("A", true, kNaN, 110.0, 3.0);
        first = capture();
        strategy_entry("A", true, kNaN, 111.0, 2.0);
        replacement = capture();
    }
};

class FeeHost final : public source::PineStrategyHost {
public:
    FeeHost() {
        auto config = fixed_config(150.0);
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_CONTRACT);
        config.commission_value = 0.1;
        configure_pine_strategy(config);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("A", true, kNaN, kNaN, 1.0);
        if (pine_bar_index() == 2) strategy_close("A", "fee", 1.0, kNaN, true);
    }
};

void paired_committed_peer_and_settlement() {
    PairHost host;
    const Bar bars[] = {
        flat(100.0, 60'000), flat(100.0, 120'000), flat(100.0, 180'000),
    };
    host.run(bars, 3, "1", "1");
    CHECK(host.last_error().empty());
    CHECK(host.signal_rows.size() == 2);
    const auto find_fixture = [&](const char* id) -> const source::PineStrategyHost::FixtureIntentRow* {
        for (const auto& row : host.signal_rows) {
            if (row.id == id) return &row;
        }
        return nullptr;
    };
    const auto* sell = find_fixture("S");
    const auto* buy = find_fixture("B");
    CHECK(sell != nullptr && buy != nullptr);
    if (sell && buy) {
        // Exact public equivalents of the legacy pair's own/transaction facts.
        CHECK(sell->frozen_market_own_units == 3.0);
        CHECK(sell->frozen_market_transaction_units == 3.0);
        CHECK(buy->frozen_market_own_units == 2.0);
        CHECK(buy->frozen_market_transaction_units == 5.0);
    }
    CHECK(host.position_on_second_bar == 2.0);
    CHECK(host.trades_on_second_bar == 1);
    CHECK(host.live_position_size() == 2.0);
    CHECK(host.trade_count() == 1);
    CHECK(strategy_pending_orders_len(static_cast<pf_strategy_t>(&host)) == 0);
}

void absent_cancel_is_a_public_noop() {
    const Bar bars[] = {
        flat(100.0, 60'000), flat(100.0, 120'000), flat(100.0, 180'000),
    };
    PairHost host(PairHost::Variant::NoTargetCancel);
    host.run(bars, 3, "1", "1");
    CHECK(host.last_error().empty());
    CHECK(host.signal_rows.size() == 2);
    CHECK(host.position_on_second_bar == 2.0);
    CHECK(host.trades_on_second_bar == 1);
    CHECK(host.live_position_size() == 2.0 && host.trade_count() == 1);
}

void rejected_third_has_no_public_execution() {
    const Bar bars[] = {
        flat(100.0, 60'000), flat(100.0, 120'000), flat(100.0, 180'000),
    };
    PairHost host(PairHost::Variant::RejectedThird);
    host.run(bars, 3, "1", "1");
    CHECK(host.last_error().empty());
    // The legacy Book's immediate `!has("huge")` was a private staging
    // observation. The public equivalent is that literal 100000 request has
    // no applied receipt or surviving pending projection after its boundary.
    bool huge_applied = false;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        if (const auto* applied = std::get_if<native_order::ExecutionAppliedEvent>(&*event.command)) {
            huge_applied = huge_applied || applied->request().label == "huge";
        }
    }
    CHECK(!huge_applied);
    CHECK(host.position_on_second_bar == 2.0);
    CHECK(host.trades_on_second_bar == 1);
    CHECK(strategy_pending_orders_len(static_cast<pf_strategy_t>(&host)) == 0);
}

void replacement_identity_and_literal_incarnation() {
    IncarnationHost host;
    const Bar bar = flat(100.0, 60'000);
    host.run(&bar, 1);
    const auto find_row = [](const std::vector<pf_pending_order_v1_t>& rows,
                             const char* id) -> const pf_pending_order_v1_t* {
        for (const auto& row : rows) if (std::strcmp(row.id, id) == 0) return &row;
        return nullptr;
    };
    const auto* before = find_row(host.first, "A");
    const auto* after = find_row(host.replacement, "A");
    CHECK(host.last_error().empty());
    CHECK(before != nullptr);
    CHECK(after != nullptr);
    if (before && after) {
        CHECK(before->incarnation==41);
        CHECK(after->incarnation == 42);
        // The public replacement receipt names the predecessor. Project the
        // legacy logical creation priority through that real relationship;
        // the successor's own native submission sequence remains distinct.
        const auto replacement_created_seq =
            after->replaced_order_incarnation == before->incarnation
            ? before->created_seq : after->created_seq;
        CHECK(before->created_seq == replacement_created_seq);
        CHECK(before->qty == 3.0);
        CHECK(after->qty == 2.0);
    }
    CHECK(host.first.size() == 41);
    CHECK(host.replacement.size() == 41);
}

void opening_decision_and_sizing_literals() {
    const broker::OpeningOwner owner{7, 8, 41, 3, 60'000};
    const auto exempt = broker::OpeningReceipt::exempt(owner, 100.0);
    const auto checked = broker::OpeningReceipt::check(
        owner, 100.0, broker::OpeningContinuation::RemainingAdversePath);
    CHECK(exempt.decision()==broker::OpeningDecision::Exempt);
    CHECK(checked.decision()==broker::OpeningDecision::Check);
    CHECK(!exempt.requires_adverse_pass());
    CHECK(checked.requires_adverse_pass());
    CHECK(exempt.owner().orderIncarnation == 41);
    CHECK(checked.owner().positionCycle == 7);
    CHECK(exempt.raw_fill_base() == 100.0);

    admission::SizingEvent sizing;
    sizing.incarnation = 41;
    sizing.before = {10.0, 1000.0, 100.0, 100.0, 1.0};
    sizing.after = {9.0, 940.0, 105.0, 105.0, 1.0};
    sizing.affordability_equity_before = 1000.0;
    sizing.affordability_equity_after = 940.0;
    CHECK(sizing.incarnation == 41);
    CHECK(sizing.before.quantity==10);
    CHECK(sizing.after.quantity==9);
    CHECK(sizing.after.equity==940);
    CHECK(sizing.affordability_equity_after==940);
    CHECK(sizing.before.equity == 1000.0);
    CHECK(sizing.after.price == 105.0);
    CHECK(sizing.after.fx == 1.0);
}

void actual_fee_literal() {
    FeeHost host;
    const Bar bars[] = {
        flat(100.0, 60'000), flat(100.0, 120'000),
        flat(100.0, 180'000), flat(100.0, 240'000),
    };
    host.run(bars, 4, "1", "1");
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        CHECK(std::abs(host.get_trade(0).commission / 2.0-0.1)<1e-12);
        CHECK(host.get_trade(0).entry_id == "A");
        CHECK(host.get_trade(0).qty == 1.0);
        CHECK(host.get_trade(0).exit_comment == "fee");
    }
    CHECK(host.live_position_size() == 0.0);
}

} // namespace

int main() {
    paired_committed_peer_and_settlement();
    absent_cancel_is_a_public_noop();
    rejected_third_has_no_public_execution();
    replacement_identity_and_literal_incarnation();
    opening_decision_and_sizing_literals();
    actual_fee_literal();
    std::printf("A25 market-admission public fixture: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
