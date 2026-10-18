// CR-150 L19E: exercise retirement on the real Pine host with recording OFF.
// The first checkpoint uses only base-tree APIs, before any hash/report query:
// an append-only journal fails because completed events still own their payloads.
#include <pineforge/compat/pine/market_admission.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;
int checks = 0;
int failures = 0;

#define CHECK(condition) do {                                                   \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        ++failures;                                                            \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    }                                                                          \
} while (false)

std::vector<Bar> chart(int count) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0, kStart + i * 60000LL});
    return bars;
}

class TradingHost final : public source::PineStrategyHost {
public:
    TradingHost() {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 1;
        config.process_orders_on_close = true;
        config.commission_value = 0.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ % 2 == 0) strategy_entry("E", true, kNa, kNa, 1.0);
        else strategy_close_all();
    }
};

class RejectionHost final : public source::PineStrategyHost {
public:
    RejectionHost() {
        source::PineStrategyConfig config;
        config.initial_capital = 1000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 1;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.commission_value = 0.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0 || bar_index_ == 2) {
            strategy_entry("unaffordable", true, kNa, kNa, 100000.0);
            // The native route refuses this quantity before submit() appends
            // an Admitted observation. Exercise the rejection reader with an
            // explicit Journal API input, independent of that producer path.
            auto& journal = market_admission_journal();
            auto observation = std::make_shared<admission::CommandObservation>();
            observation->command = journal.next_sequence();
            observation->bar = bar_index_;
            observation->id = "reader-rejection";
            observation->requested_quantity = 100000.0;
            admission::CommandEvent event;
            event.observation = std::move(observation);
            event.outcome = admission::Outcome::RejectedAffordability;
            journal.append(std::move(event));
            CHECK(compat::pine::last_rejected_command_bar(journal) == bar_index_);
        }
    }
};

void recording_off_retires_without_a_query() {
    for (const int count : {64, 256}) {
        const auto bars = chart(count);
        TradingHost host;
        host.run(bars.data(), count, "1", "1");
        CHECK(host.last_error().empty());
        // Keep this before fill_report, broker_state_hash and reflection. A
        // lazy hash fold would hide the production recording-off regression.
        CHECK(host.market_admission_journal().events().empty());
        CHECK(host.market_admission_journal().events().capacity() <= 64);
        CHECK(host.market_admission_journal().sequence_frontier() > 1);
        CHECK(host.trade_count() == count / 2);
        ReportC report{};
        host.fill_report(&report);
        CHECK(report.broker_state_hash_len == 0);
        BacktestEngine::free_report(&report);
    }
}

void recording_and_prefix_identity() {
    const auto bars = chart(64);
    TradingHost off;
    TradingHost on;
    TradingHost prefix;
    on.set_broker_state_hash_recording(true);
    prefix.set_broker_state_hash_recording(true);
    off.run(bars.data(), 64, "1", "1");
    on.run(bars.data(), 64, "1", "1");
    prefix.run(bars.data(), 32, "1", "1");
    CHECK(off.last_error().empty() && on.last_error().empty() && prefix.last_error().empty());
    CHECK(off.market_admission_journal().events().empty());
    CHECK(on.market_admission_journal().events().empty());
    CHECK(prefix.market_admission_journal().events().empty());
    CHECK(off.broker_state_hash() == on.broker_state_hash());
    CHECK(off.trade_count() == on.trade_count());
    const auto before_report = on.broker_state_hash();
    ReportC whole{};
    ReportC part{};
    on.fill_report(&whole);
    prefix.fill_report(&part);
    CHECK(whole.broker_state_hash_len == 64);
    CHECK(part.broker_state_hash_len == 32);
    if (whole.broker_state_hash_len == 64 && part.broker_state_hash_len == 32) {
        for (int i = 0; i < 32; ++i)
            CHECK(part.broker_state_hash[i] == whole.broker_state_hash[i]);
        if (whole.broker_state_hash[63] != on.broker_state_hash())
            std::fprintf(stderr, "hash boundary: recorded=%016" PRIx64
                " scalar_after_run=%016" PRIx64 " scalar_after_report=%016" PRIx64 "\n",
                whole.broker_state_hash[63], before_report, on.broker_state_hash());
        CHECK(whole.broker_state_hash[63] == on.broker_state_hash());
    }
    CHECK(on.broker_state_hash() == before_report);
    const auto digest = on.broker_state_hash();
    CHECK(on.broker_state_hash() == digest);
    BacktestEngine::free_report(&whole);
    BacktestEngine::free_report(&part);
}

void split_script_boundaries() {
    const auto bars = chart(64 * 5);
    for (const bool magnifier : {false, true}) {
        TradingHost host;
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "5", magnifier,
                 4, MagnifierDistribution::ENDPOINTS);
        CHECK(host.last_error().empty());
        CHECK(host.market_admission_journal().events().empty());
        CHECK(host.market_admission_journal().events().capacity() <= 64);
        CHECK(host.trade_count() > 0);
    }
}

void rejection_reader_survives_retirement_and_reset() {
    const auto bars = chart(64);
    RejectionHost host;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    CHECK(host.last_error().empty());
    CHECK(host.market_admission_journal().events().empty());
    CHECK(compat::pine::last_rejected_command_bar(host.market_admission_journal()) == 2);
    CHECK(host.trade_count() == 0);
    // A fresh run must not inherit the prior run's remembered rejection.
    host.run(bars.data(), 1, "1", "1");
    CHECK(host.last_error().empty());
    CHECK(host.market_admission_journal().events().empty());
    CHECK(compat::pine::last_rejected_command_bar(host.market_admission_journal()) == 0);
}
} // namespace

int main() {
    const struct Case { const char* name; void (*run)(); } cases[] = {
        {"recording off without a query", recording_off_retires_without_a_query},
        {"recording and prefix identity", recording_and_prefix_identity},
        {"split script boundaries", split_script_boundaries},
        {"rejection reader and reset", rejection_reader_survives_retirement_and_reset},
    };
    for (const auto& test : cases) {
        std::printf("CASE %s\n", test.name);
        try { test.run(); }
        catch (const std::exception& error) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s\n", test.name, error.what());
        }
    }
    std::printf("admission journal window: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
