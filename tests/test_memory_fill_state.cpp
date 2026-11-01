// Fill-state storage follows the submitted orders and settlement schedule;
// the test directly invokes the private closed-origin scan at chosen bars.
// Counts and reference models describe container contents;
// they do not measure resident bytes.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_scheduler.hpp>
#include <pineforge/source/pine_language_state.hpp>
#include "../src/source/pine_position_tail_summary.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using pineforge::Bar;
using pineforge::Series;
using pineforge::source::PineLanguageState;
using pineforge::source::PineStrategyConfig;
using pineforge::source::PineStrategyHost;
using Adapter = pineforge::source::PineExecutionAdapter;
using Tail = pineforge::source::detail::PositionTailSummary;
using AdapterTail = pineforge::source::detail::AdapterPositionTailSummary;
namespace no = pineforge::native_order;

// Same explicit-instantiation seam as test_id_adapter_storage.cpp's CohortFor.
// Observations are const; no target container is seeded or resized here.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Summary { friend auto access(Summary); };
struct Origins { friend auto access(Origins); };
struct LiveCohorts { friend auto access(LiveCohorts); };
struct Margins { friend auto access(Margins); };
struct Placements { friend auto access(Placements); };
struct Handles { friend auto access(Handles); };
struct ReleaseOrigins { friend auto access(ReleaseOrigins); };
struct Frozen { friend auto access(Frozen); };
struct Language { friend auto access(Language); };
template struct Access<Summary, &Adapter::position_tail_summary_>;
template struct Access<Origins, &Tail::origins_>;
template struct Access<LiveCohorts, &Tail::live_cohorts_>;
template struct Access<Margins, &Tail::margins_>;
template struct Access<Placements, &Adapter::placement_>;
template struct Access<Handles, &Adapter::live_handles_>;
template struct Access<ReleaseOrigins, &Adapter::release_closed_cohort_origins>;
template struct Access<Frozen, &PineLanguageState::pos_view_frozen_entry_qty_>;
template struct Access<Language, &pineforge::source::PineScheduler::language_>;

std::size_t checks = 0, failures = 0;
const char* current_row = "startup";

#define MEM_CHECK(expression) do {                                                \
    ++checks;                                                                   \
    if (!(expression)) {                                                        \
        ++failures;                                                             \
        std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n", current_row,              \
                     __FILE__, __LINE__, #expression);                          \
    }                                                                           \
} while (false)

template<class Action>
void run_row(const char* row, Action&& action) {
    current_row = row;
    try { action(); }
    catch (const std::exception& error) {
        ++checks; ++failures;
        std::fprintf(stderr, "FAIL [%s] %s:%d: exception: %s\n", row,
                     __FILE__, __LINE__, error.what());
    } catch (...) {
        ++checks; ++failures;
        std::fprintf(stderr, "FAIL [%s] %s:%d: unknown exception\n", row,
                     __FILE__, __LINE__);
    }
}

constexpr std::int64_t start = INT64_C(1736121600000);
constexpr std::int64_t minute = 60000;
double na() { return std::numeric_limits<double>::quiet_NaN(); }

std::vector<Bar> tape(std::size_t n, bool daily_pairs = false) {
    std::vector<Bar> bars;
    bars.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto offset = daily_pairs
            ? static_cast<std::int64_t>(i / 2) * 86400000
                + static_cast<std::int64_t>(i % 2) * minute
            : static_cast<std::int64_t>(i) * minute;
        bars.push_back({100, 101, 99, 100, 1000, start + offset});
    }
    return bars;
}

void run(PineStrategyHost& host, const std::vector<Bar>& bars) {
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    MEM_CHECK(host.last_error().empty());
}

class SourceHost : public PineStrategyHost {
public:
    explicit SourceHost(int pyramiding, bool pooc = false, bool coof = false) {
        PineStrategyConfig config;
        config.initial_capital = 1e12;
        config.margin_long = config.margin_short = 0;
        config.pyramiding = pyramiding;
        config.process_orders_on_close = pooc;
        config.calc_on_order_fills = coof;
        config.src_series_active = true;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.25);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
    }
    std::size_t fills = 0;
protected:
    void on_native_applied_event(const no::ExecutionAppliedEvent&,
                                 const pineforge::NativeDecisionContext&) override {
        ++fills;
    }
};

class SummaryFillHost final : public SourceHost {
public:
    explicit SummaryFillHost(std::size_t n) : SourceHost(static_cast<int>(n + 1)), n_(n) {}
    std::size_t observations = 0;
    void on_source_bar(const Bar&) override {
        const auto bar = pine_bar_index();
        if (bar == 0) {
            observe({});
            for (std::size_t i = 0; i < n_; ++i)
                strategy_entry(id(i), true, na(), na(), 2);
        } else if (bar == 1) {
            MEM_CHECK(openings_.size() == n_);
            observe(slice(0, n_));
            for (std::size_t i = 0; i < n_ / 2; ++i) strategy_close(id(i));
        } else if (bar == 2) {
            // One two-unit close per oldest named lot removes n/2 origins;
            // n/2 other lots keep the position nonflat, so flat cleanup cannot
            // mask a missing per-origin retirement.
            observe(slice(n_ / 2, n_));
            strategy_close_all();
        } else if (bar == 3) {
            observe({});
            strategy_entry("opposite-new-cycle", false, na(), na(), 2);
        } else if (bar == 4) {
            MEM_CHECK(openings_.size() == n_ + 1);
            observe(slice(n_, n_ + 1));
            strategy_close_all();
        } else if (bar == 5) {
            observe({});
        }
    }
protected:
    void on_native_applied_event(const no::ExecutionAppliedEvent& event,
                                 const pineforge::NativeDecisionContext& context) override {
        SourceHost::on_native_applied_event(event, context);
        if (event.opened_units != 0) {
            MEM_CHECK(std::abs(event.opened_units) == 2);
            openings_.push_back(event.handle().incarnation);
        }
    }
private:
    static std::string id(std::size_t i) { return "filled-" + std::to_string(i); }
    std::vector<std::uint64_t> slice(std::size_t begin, std::size_t end) const {
        if (begin > end || end > openings_.size()) return {};
        return {openings_.begin() + static_cast<std::ptrdiff_t>(begin),
                openings_.begin() + static_cast<std::ptrdiff_t>(end)};
    }
    void observe(std::vector<std::uint64_t> expected) {
        ++observations;
        const auto& summary = adapter_.*access(Summary{});
        MEM_CHECK(summary != nullptr);
        if (!summary) return;
        const auto& origins = (*summary).*access(Origins{});
        const auto& cohorts = (*summary).*access(LiveCohorts{});
        std::vector<std::uint64_t> actual;
        for (const auto& row : origins) actual.push_back(row.first);
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        MEM_CHECK(actual == expected && origins.size() == expected.size());
        // Each planned fill has its own source id/cohort; no two surviving
        // openings can share a cohort in this script.
        MEM_CHECK(cohorts.size() == expected.size());
        MEM_CHECK(summary->resident_origins() == expected.size());
        MEM_CHECK(((*summary).*access(Margins{})).size() == 0); // No margin-call fills in this model.
    }
    std::size_t n_;
    std::vector<std::uint64_t> openings_;
};

class PendingRosterHost final : public SourceHost {
public:
    explicit PendingRosterHost(std::size_t n) : SourceHost(static_cast<int>(n + 1)), n_(n) {}
    std::size_t observations = 0;
    void on_source_bar(const Bar&) override {
        const auto bar = pine_bar_index();
        if (bar == 0) {
            for (std::size_t i = 0; i < n_; ++i)
                strategy_entry(id(i), true, 10, na(), 1);
            adapter_.flush_pending_entries();
            const auto& rows = adapter_.*access(Placements{});
            for (const auto& handle : adapter_.*access(Handles{})) {
                const auto found = rows.find(handle.incarnation);
                if (found != rows.end() && found->second.opening)
                    roots_.push_back(handle.incarnation);
            }
            std::sort(roots_.begin(), roots_.end());
            MEM_CHECK(roots_.size() == n_);
        } else if (bar == 1 || bar == 2) {
            // After these real command endings, the test directly invokes
            // release_closed_cohort_origins without writing its due flag.
            // The scan snapshots BEFORE removing this cancellation's origin;
            // the next scan omits the prior one.
            const auto cancelled = static_cast<std::size_t>(bar - 1);
            strategy_cancel(id(cancelled));
            adapter_.observe_terminal_receipts();
            (adapter_.*access(ReleaseOrigins{}))();
            const auto& summary = adapter_.*access(Summary{});
            MEM_CHECK(summary != nullptr);
            if (!summary) return;
            const auto& roots = static_cast<const AdapterTail&>(*summary).roster_roots;
            MEM_CHECK(cancelled <= roots_.size());
            if (cancelled > roots_.size()) return;
            const std::vector<std::uint64_t> expected(
                roots_.begin() + static_cast<std::ptrdiff_t>(cancelled), roots_.end());
            MEM_CHECK(roots == expected && roots.size() == n_ - cancelled);
            MEM_CHECK(((*summary).*access(Origins{})).empty());
            MEM_CHECK(((*summary).*access(LiveCohorts{})).empty());
            MEM_CHECK(((*summary).*access(Margins{})).size() == 0);
            ++observations;
        } else if (bar == 3) {
            strategy_cancel_all();
        }
    }
private:
    static std::string id(std::size_t i) { return "unfilled-" + std::to_string(i); }
    std::size_t n_;
    std::vector<std::uint64_t> roots_;
};

void summary_rows() {
    for (const std::size_t n : {std::size_t{16}, std::size_t{64}}) {
        run_row("pineexecutionadapter_position_tail_summary", [=] {
            SummaryFillHost host(n);
            run(host, tape(8));
            MEM_CHECK(host.observations == 6);
            // n opens, n/2 individual closes, one remainder close, then one
            // opposite opening and its close = 3*n/2 + 3 actual fills.
            MEM_CHECK(host.fills == 3 * n / 2 + 3);
        });
        run_row("pineexecutionadapter_position_tail_summary", [=] {
            PendingRosterHost host(n);
            run(host, tape(8));
            MEM_CHECK(host.fills == 0 && host.observations == 2);
        });
    }
}

// Pointers into the real protected fields, not a replacement language state.
struct LanguageMembers : PineLanguageState {
    using Slot = Series<double> PineLanguageState::*;
    inline static constexpr std::array<Slot, 9> source{{
        &LanguageMembers::_src_open_, &LanguageMembers::_src_high_,
        &LanguageMembers::_src_low_, &LanguageMembers::_src_close_,
        &LanguageMembers::_src_volume_, &LanguageMembers::_src_hl2_,
        &LanguageMembers::_src_hlc3_, &LanguageMembers::_src_ohlc4_,
        &LanguageMembers::_src_hlcc4_
    }};
    inline static constexpr std::array<Slot, 9> checkpoint{{
        &LanguageMembers::coof_checkpoint_src_open_,
        &LanguageMembers::coof_checkpoint_src_high_,
        &LanguageMembers::coof_checkpoint_src_low_,
        &LanguageMembers::coof_checkpoint_src_close_,
        &LanguageMembers::coof_checkpoint_src_volume_,
        &LanguageMembers::coof_checkpoint_src_hl2_,
        &LanguageMembers::coof_checkpoint_src_hlc3_,
        &LanguageMembers::coof_checkpoint_src_ohlc4_,
        &LanguageMembers::coof_checkpoint_src_hlcc4_
    }};
};

// Series' constructor gives a 500-value history. Our
// reference is a chronological input-bar suffix, never another Series.
constexpr std::size_t history = 500;
double value(const Bar& b, std::size_t column) {
    switch (column) {
    case 0: return b.open;
    case 1: return b.high;
    case 2: return b.low;
    case 3: return b.close;
    case 4: return b.volume;
    case 5: return (b.high + b.low) / 2;
    case 6: return (b.high + b.low + b.close) / 3;
    case 7: return (b.open + b.high + b.low + b.close) / 4;
    default: return (b.high + b.low + b.close + b.close) / 4;
    }
}

class LanguageSeriesHost final : public SourceHost {
public:
    explicit LanguageSeriesHost(bool trade) : SourceHost(1, false, true), trade_(trade) {}
    void on_source_bar(const Bar& bar) override {
        if (history_advances_new_bar()) {
            model_.push_back(bar);
            if (model_.size() > history) model_.erase(model_.begin());
        } else if (!model_.empty()) {
            model_.back() = bar;
        }
        verify(LanguageMembers::source, model_);
        const int index = pine_bar_index();
        if (!trade_ || index == issued_) return;
        if (index % 16 == 0) {
            issued_ = index;
            strategy_entry("ring-fill-" + std::to_string(index), true, na(), na(), 1);
        } else if (index % 16 == 8) {
            issued_ = index;
            strategy_close_all();
        }
    }
    void snapshot_script_state() override {
        saved_ = model_;
        verify(LanguageMembers::checkpoint, saved_);
        ++snapshots;
    }
    void restore_script_state() override {
        model_ = saved_;
        verify(LanguageMembers::source, model_);
        ++restores;
    }
    void commit_script_state() override {
        saved_ = model_;
        verify(LanguageMembers::checkpoint, saved_);
        ++commits;
    }
    std::size_t retained() const {
        std::size_t count = 0;
        const auto& state = scheduler_.*access(Language{});
        for (const auto member : LanguageMembers::source) count += (state.*member).size();
        for (const auto member : LanguageMembers::checkpoint) count += (state.*member).size();
        return count;
    }
    void finished() const {
        verify(LanguageMembers::source, model_);
        verify(LanguageMembers::checkpoint, saved_);
        MEM_CHECK(model_.size() == history && saved_.size() == history);
        MEM_CHECK(retained() == (9 + 9) * history);
    }
    std::size_t snapshots = 0, restores = 0, commits = 0;
private:
    void verify(const std::array<LanguageMembers::Slot, 9>& slots,
                const std::vector<Bar>& model) const {
        const auto& state = scheduler_.*access(Language{});
        for (std::size_t column = 0; column < slots.size(); ++column) {
            const auto& actual = state.*slots[column];
            MEM_CHECK(actual.size() == static_cast<int>(model.size())
                     && actual.size() <= static_cast<int>(history));
            bool same = actual.size() == static_cast<int>(model.size());
            if (same) {
                for (std::size_t i = 0; i < model.size(); ++i)
                    same = same && actual[static_cast<int>(i)]
                        == value(model[model.size() - 1 - i], column);
            }
            MEM_CHECK(same);
        }
    }
    bool trade_;
    int issued_ = -1;
    std::vector<Bar> model_, saved_;
};

class FrozenMapHost final : public SourceHost {
public:
    explicit FrozenMapHost(std::size_t n) : SourceHost(static_cast<int>(n + 1), true), n_(n) {}
    bool trading = true;
    std::size_t generations = 0;
    std::size_t retained() const {
        return ((scheduler_.*access(Language{})).*access(Frozen{})).size();
    }
    void on_source_bar(const Bar&) override {
        if (!trading) {
            MEM_CHECK((scheduler_.language().*access(Frozen{})).empty());
            return;
        }
        const auto bar = pine_bar_index();
        if (bar % 2 == 0) {
            for (std::size_t i = 0; i < n_; ++i)
                strategy_entry(id(bar / 2, i), true, na(), na(), 2);
        } else {
            std::unordered_map<std::string, double> expected;
            for (std::size_t i = 0; i < n_; ++i) expected.emplace(id(bar / 2, i), 2);
            // PineExecutionAdapter::close_all submits without freezing the
            // script view. The public empty-id close freezes the
            // pre-close lots and fills synchronously under ordinary POOC.
            // PineScheduler::freeze_script_position_view sums the
            // N distinct ids' two-unit lots, replacing the prior generation;
            // hence the exact independent map remains {current id: 2}.
            strategy_close("");
            const auto& actual = scheduler_.language().*access(Frozen{});
            MEM_CHECK(actual.size() == n_ && actual == expected);
            ++generations;
        }
    }
private:
    static std::string id(int generation, std::size_t i) {
        return "frozen-" + std::to_string(generation) + "-" + std::to_string(i);
    }
    std::size_t n_;
};

void language_rows() {
    for (const bool trade : {false, true}) {
        std::array<std::size_t, 2> counts{};
        for (std::size_t scale = 0; scale < counts.size(); ++scale) {
            run_row("pinescheduler_language", [&] {
                const std::size_t bars = scale == 0 ? 512 : 2048;
                LanguageSeriesHost host(trade);
                run(host, tape(bars));
                host.finished();
                MEM_CHECK(host.snapshots != 0 && host.restores != 0 && host.commits != 0);
                MEM_CHECK(host.fills == (trade ? bars / 8 : 0));
                counts[scale] = host.retained();
            });
        }
        run_row("pinescheduler_language", [&] {
            MEM_CHECK(counts[0] == counts[1] && counts[0] == (9 + 9) * history);
        });
    }
    for (const std::size_t n : {std::size_t{3}, std::size_t{7}}) {
        std::array<std::size_t, 2> counts{};
        for (std::size_t scale = 0; scale < counts.size(); ++scale) {
            run_row("pinescheduler_language", [&] {
                const std::size_t bars = scale == 0 ? 32 : 128;
                FrozenMapHost host(n);
                run(host, tape(bars));
                MEM_CHECK(host.generations == bars / 2);
                MEM_CHECK(host.fills == (n + 1) * (bars / 2));
                counts[scale] = host.retained();
                host.trading = false;
                run(host, tape(4)); // Real run reset retires the final frozen map.
                MEM_CHECK(host.retained() == 0);
            });
        }
        run_row("pinescheduler_language", [&] {
            MEM_CHECK(counts[0] == n && counts[1] == n && counts[0] == counts[1]);
        });
    }
}

class CapHost final : public SourceHost {
public:
    CapHost() : SourceHost(1, true) {
        adapter_.cap = 1;
        set_syminfo_metadata("intraday_cap_defer_pooc_close", 1);
    }
    bool trading = true;
    std::size_t retired = 0;
    const auto& cap_state() const { return adapter_.cap; }
    void on_source_bar(const Bar&) override {
        const auto& state = adapter_.cap;
        // Both actual optional payloads must be empty at a new ordinary
        // callback: a prior due close ran at the open, and no successor is
        // submitted by this script to inherit a close-quota transfer.
        MEM_CHECK(!state.due_cause().has_value() && !state.budget().transfer().has_value());
        if (pine_bar_index() % 2 == 1) ++retired;
        if (trading && pine_bar_index() % 2 == 0)
            strategy_entry("capped-" + std::to_string(pine_bar_index()), true, na(), na(), 1);
    }
};

void cap_rows() {
    for (const std::size_t cycles : {std::size_t{8}, std::size_t{32}}) {
        run_row("pineexecutionadapter_cap", [=] {
            CapHost host;
            // Each pair is on a different day. K openings and K-1 due closes
            // leave exactly ONE pending cause at the final even-numbered bar.
            run(host, tape(2 * cycles - 1, true));
            const auto& state = host.cap_state();
            MEM_CHECK(host.fills == 2 * cycles - 1 && host.retired == cycles - 1);
            MEM_CHECK(state.due_cause().has_value() && !state.budget().transfer().has_value());
            if (state.due_cause()) {
                MEM_CHECK(state.due_cause()->action_id == cycles);
                MEM_CHECK(state.due_cause()->charged_slots == 1);
            }
            // IntradayCap's retained payloads are the day, quota transfer and
            // due-cause optionals. One filled order/day gives day + cause = 2
            // occupied payloads irrespective of K; transfer is absent.
            const std::size_t occupied = state.budget().day().has_value()
                + state.budget().transfer().has_value() + state.due_cause().has_value();
            MEM_CHECK(occupied == 2);
            host.trading = false;
            const auto before = host.fills;
            run(host, tape(8, true));
            MEM_CHECK(host.fills == before);
            MEM_CHECK(!state.due_cause().has_value() && !state.budget().transfer().has_value());
        });
    }
}
} // namespace

int main() {
    summary_rows();
    language_rows();
    cap_rows();
    std::printf("memory proof fill_state: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
