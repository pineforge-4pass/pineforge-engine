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
#include <stdexcept>
#include <string>
#include <type_traits>
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

// Test-only access to the v4 cache, using the same explicit-instantiation
// access pattern as the existing admission mutation controls.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
#define MEMBER(Tag, field) \
    struct Tag { friend auto access(Tag); }; \
    template struct Access<Tag, &source::PineExecutionAdapter::field>
MEMBER(Fold, fold_admission_events);
MEMBER(Count, admission_events_folded_);
MEMBER(Digest, admission_events_digest_);
MEMBER(Last, admission_events_last_);
#undef MEMBER

std::uint64_t reference_fold(std::uint64_t digest, const admission::Event& event) {
    BrokerStateHashSink sink;
    sink.h = digest;
    admission::reflect(event, "event", [&](const admission::Field& field) {
        sink.s(field.path);
        sink.u(field.value.index());
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::uint64_t>) sink.u(value);
            else if constexpr (std::is_same_v<T, std::int64_t>) sink.i(value);
            else if constexpr (std::is_same_v<T, double>) sink.d(value);
            else sink.s(value);
        }, field.value);
    });
    return sink.h;
}

void fold(source::PineExecutionAdapter& adapter) {
    (adapter.*access(Fold{}))();
}
void retire(source::PineExecutionAdapter& adapter) {
    adapter.admission_journal.retire_prefix(adapter.*access(Count{}),
        adapter.*access(Digest{}), adapter.*access(Last{}));
}
template<class Fn> void invariant(Fn&& call) {
    bool rejected = false;
    try { call(); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);
}

admission::CommandEvent command(admission::Journal& journal, int bar,
                                admission::Outcome outcome = admission::Outcome::Admitted) {
    auto observation = std::make_shared<admission::CommandObservation>();
    observation->command = journal.next_sequence();
    observation->bar = bar;
    observation->id = std::string(128, 'a') + std::to_string(bar);
    observation->requested_quantity = 2.5;
    admission::CommandEvent event;
    event.observation = std::move(observation);
    event.outcome = outcome;
    event.removed = {13, 17};
    return event;
}

void variants_fold_once_and_release_payloads() {
    source::PineExecutionAdapter adapter;
    auto& journal = adapter.admission_journal;
    std::uint64_t expected = 1469598103934665603ULL;
    for (int bar = 0; bar < 128; ++bar) {
        std::weak_ptr<const admission::CommandObservation> lifetime;
        {
            const auto gap = journal.next_sequence();
            journal.abandon(gap); // Sequence frontier is not the event count.
            auto opening = command(journal, bar, admission::Outcome::RejectedAffordability);
            lifetime = opening.observation;
            expected = reference_fold(expected, opening);
            const auto origin = opening.observation;
            journal.append(std::move(opening));
            admission::ReviewEvent review;
            review.receipt = {journal.next_sequence(), admission::Checkpoint::DefaultGross, bar, 0};
            admission::BookObservation book;
            book.draft.bind(origin);
            book.bar = bar;
            book.id = origin->id;
            review.book.push_back(book);
            review.reviewed.push_back(book);
            review.causes = {origin->command};
            expected = reference_fold(expected, review);
            journal.append(std::move(review));
            admission::SizingEvent sizing;
            sizing.receipt = {journal.next_sequence(), 21, bar, origin->command};
            sizing.before.quantity = 2.5;
            sizing.after.quantity = 1.25;
            expected = reference_fold(expected, sizing);
            journal.append(std::move(sizing));
        }
        CHECK(!lifetime.expired());
        fold(adapter);
        fold(adapter);
        CHECK(adapter.*access(Digest{}) == expected);
        CHECK(adapter.*access(Count{}) == static_cast<std::size_t>((bar + 1) * 3));
        retire(adapter);
        CHECK(journal.events().empty());
        CHECK(lifetime.expired());
        CHECK(journal.retired_prefix().digest == expected);
        CHECK(journal.retired_prefix().count == static_cast<std::size_t>((bar + 1) * 3));
        CHECK(compat::pine::last_rejected_command_bar(journal) == bar);
        fold(adapter);
        CHECK(adapter.*access(Digest{}) == expected);
    }
    const auto& prefix = journal.retired_prefix();
    CHECK(prefix.last_sequence == journal.sequence_frontier() - 1);
    bool count = false, digest = false, bar = false;
    journal.reflect("journal", [&](const admission::Field& field) {
        if (field.path == "journal.base.count")
            count = std::get<std::uint64_t>(field.value) == prefix.count;
        if (field.path == "journal.base.digest")
            digest = std::get<std::uint64_t>(field.value) == prefix.digest;
        if (field.path == "journal.base.last_rejected_command_bar")
            bar = std::get<std::int64_t>(field.value) == 127;
    });
    CHECK(count && digest && bar);
}

void unfinished_captures_and_window_restart() {
    source::PineExecutionAdapter adapter;
    auto& journal = adapter.admission_journal;
    auto first = command(journal, 0);
    std::uint64_t expected = reference_fold(1469598103934665603ULL, first);
    journal.append(std::move(first));
    fold(adapter);
    retire(adapter);
    const auto base = journal.retired_prefix();
    {
        auto allocation = journal.reserve();
        auto observation = std::make_shared<admission::CommandObservation>();
        observation->command = allocation.sequence();
        admission::CommandEvent event;
        event.observation = std::move(observation);
        expected = reference_fold(expected, event);
        journal.append(std::move(event));
        fold(adapter);
        invariant([&] { retire(adapter); });
        CHECK(journal.retired_prefix().count == base.count);
    }
    retire(adapter);
    CHECK(journal.retired_prefix().digest == expected);
    auto outer = command(journal, 1);
    auto inner = command(journal, 1);
    journal.append(inner);
    fold(adapter);
    invariant([&] { retire(adapter); }); // The earlier allocation is unfinished.
    journal.append(outer);
    expected = reference_fold(reference_fold(expected, outer), inner);
    fold(adapter);
    CHECK(adapter.*access(Digest{}) == expected);
    retire(adapter);

    const auto stable = journal.retired_prefix();
    auto removed = command(journal, 2);
    auto kept = command(journal, 2);
    journal.append(removed);
    journal.append(kept);
    fold(adapter);
    journal.retain({kept.observation->command});
    fold(adapter); // Restart from the nonzero retired base, not the FNV seed.
    CHECK(adapter.*access(Count{}) == stable.count + 1);
    CHECK(adapter.*access(Digest{}) == reference_fold(stable.digest, kept));
    retire(adapter);
    invariant([&] { journal.retire_prefix(0, 0, 0); });
    CHECK(journal.retired_prefix().count == stable.count + 1);
    const admission::Journal replacement = journal;
    journal.reset();
    fold(adapter);
    CHECK(adapter.*access(Count{}) == 0);
    CHECK(adapter.*access(Digest{}) == 1469598103934665603ULL);
    journal = replacement;
    fold(adapter);
    CHECK(adapter.*access(Digest{}) == replacement.retired_prefix().digest);
}

void retired_prefix_value_semantics() {
    source::PineExecutionAdapter adapter;
    auto& journal = adapter.admission_journal;
    journal.append(command(journal, 4, admission::Outcome::RejectedPricedCap));
    fold(adapter);
    retire(adapter);
    const auto check_prefix = [&](const admission::Journal& value) {
        CHECK(value.events().empty());
        CHECK(value.retired_prefix().count == 1);
        CHECK(value.retired_prefix().digest == journal.retired_prefix().digest);
        CHECK(value.retired_prefix().last_sequence == journal.retired_prefix().last_sequence);
        CHECK(compat::pine::last_rejected_command_bar(value) == 4);
    };
    admission::Journal copied(journal);
    check_prefix(copied);
    admission::Journal assigned;
    assigned = journal;
    check_prefix(assigned);
    admission::Journal moved(std::move(copied));
    check_prefix(moved);
    CHECK(copied.retired_prefix().count == 0 && copied.sequence_frontier() == 1);
    admission::Journal move_assigned;
    move_assigned = std::move(assigned);
    check_prefix(move_assigned);
    CHECK(assigned.retired_prefix().count == 0 && assigned.sequence_frontier() == 1);
    moved.reset();
    CHECK(moved.retired_prefix().count == 0);
    CHECK(moved.retired_prefix().digest == 1469598103934665603ULL);
    CHECK(moved.retired_prefix().last_sequence == 0);
    CHECK(compat::pine::last_rejected_command_bar(moved) == -1);
    source::PineExecutionAdapter other;
    other.admission_journal.append(command(other.admission_journal, 9));
    fold(other);
    retire(other);
    CHECK(other.admission_journal.retired_prefix().count == journal.retired_prefix().count);
    CHECK(other.admission_journal.retired_prefix().last_sequence
          == journal.retired_prefix().last_sequence);
    journal = other.admission_journal;
    fold(adapter);
    CHECK(adapter.*access(Digest{}) == other.admission_journal.retired_prefix().digest);
}

void full_observation_preserves_execution() {
    const auto bars = chart(64);
    TradingHost window;
    TradingHost full;
    full.market_admission_journal().retain_full_observation_history();
    window.set_broker_state_hash_recording(true);
    full.set_broker_state_hash_recording(true);
    window.run(bars.data(), 64, "1", "1");
    full.run(bars.data(), 64, "1", "1");
    CHECK(window.last_error().empty() && full.last_error().empty());
    CHECK(window.market_admission_journal().events().empty());
    CHECK(!full.market_admission_journal().events().empty());
    CHECK(window.market_admission_journal().event_count()
          == full.market_admission_journal().event_count());
    CHECK(window.broker_state_hash() == full.broker_state_hash());
    ReportC a{}, b{};
    window.fill_report(&a);
    full.fill_report(&b);
    CHECK(a.broker_state_hash_len == 64 && b.broker_state_hash_len == 64);
    if (a.broker_state_hash_len == 64 && b.broker_state_hash_len == 64)
        for (int i = 0; i < 64; ++i) CHECK(a.broker_state_hash[i] == b.broker_state_hash[i]);
    CHECK(a.total_trades == b.total_trades && a.total_trades == 32);
    BacktestEngine::free_report(&a);
    BacktestEngine::free_report(&b);
    invariant([&] { window.market_admission_journal().retain_full_observation_history(); });
}
} // namespace

int main() {
    const struct Case { const char* name; void (*run)(); } cases[] = {
        {"recording off without a query", recording_off_retires_without_a_query},
        {"recording and prefix identity", recording_and_prefix_identity},
        {"split script boundaries", split_script_boundaries},
        {"rejection reader and reset", rejection_reader_survives_retirement_and_reset},
        {"v4 variants and payload release", variants_fold_once_and_release_payloads},
        {"unfinished captures and restart", unfinished_captures_and_window_restart},
        {"prefix value semantics", retired_prefix_value_semantics},
        {"full observation execution identity", full_observation_preserves_execution},
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
