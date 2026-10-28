// Measure retirement through ordinary strategy commands and the public run.
// Prices and quantities below have exact integer money oracles. Work counts
// describe the observed loops; this probe imposes no time or scaling verdict.
#include <pineforge/source/pine_strategy_host.hpp>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using Adapter = source::PineExecutionAdapter;
using Work = Adapter::RetirementWork;
using Population = Adapter::RetirementPopulation;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t start_ms = INT64_C(1736121600000);

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

#define RETIREMENT_COUNTS(visit) \
    visit(placement_sweeps) visit(prefix_sweeps) visit(row_visits) \
    visit(pin_rows) visit(gate_rows) visit(candidate_rows) visit(final_rows) \
    visit(pin_comparisons) visit(peer_visits) visit(held_comparisons) visit(k3_pairs) \
    visit(root_visits) visit(family_visits) visit(cohort_rows) visit(certificate_rows) visit(fronts_tested) \
    visit(missing_fronts) visit(missing_fronts_pinned) visit(rows_erased) \
    visit(origins_retired) visit(placement_slots) visit(backing_rows_restored) \
    visit(backing_rows_spilled) visit(spill_rows_visited)

Work difference(const Work& after, const Work& before) {
    Work result;
#define SUBTRACT(field) \
    require(after.field >= before.field, "retirement counter moved backwards"); \
    result.field = after.field - before.field;
    RETIREMENT_COUNTS(SUBTRACT)
#undef SUBTRACT
    return result;
}

enum class Shape { RetiredBrackets, MissingFront, Churn };

const char* name(Shape shape) {
    switch (shape) {
    case Shape::RetiredBrackets: return "retired_brackets";
    case Shape::MissingFront: return "missing_front";
    case Shape::Churn: return "churn";
    }
    throw std::logic_error("unknown retirement probe shape");
}

struct Sample {
    Work work{};
    Population population{};
    double units = 0.0, profit = 0.0;
    int trades = 0;
};

class RetirementProbe final : public source::PineStrategyHost {
public:
    RetirementProbe(Shape shape, int bars, int history, bool recording)
        : shape_(shape), bars_(bars), history_(history) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 2;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(1.0);
        set_broker_state_hash_recording(recording);
    }

    Sample begin_quiet{}, end_quiet{};
    bool saw_begin = false, saw_end = false;

    int quiet_begin() const { return bars_ / 2; }
    int quiet_end() const { return bars_ - 3; }

    Sample sample() const {
        return {adapter_.retirement_work(), adapter_.retirement_population(),
                live_position_size(), net_profit(), trade_count()};
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == quiet_begin()) {
            begin_quiet = sample();
            saw_begin = true;
        }
        if (i == quiet_end()) {
            end_quiet = sample();
            saw_end = true;
            if (shape_ == Shape::Churn) strategy_cancel("moving");
            strategy_close(shape_ == Shape::MissingFront ? "successor" : "position");
            return;
        }
        if (i > quiet_end()) return;

        if (shape_ == Shape::RetiredBrackets) {
            if (i == 0)
                strategy_entry("position", true, missing, missing, history_ + 1.0);
            if (i == 1) {
                for (int exit = 0; exit < history_; ++exit) {
                    const auto id = "target-" + std::to_string(exit);
                    // Each bracket reserves one unit in its own OCA group.
                    strategy_exit(id, "position", 110.0, 90.0, missing, missing,
                                  missing, 100.0, {}, 1.0, id);
                }
            }
        } else if (shape_ == Shape::MissingFront) {
            if (i < 4 * history_) {
                const auto id = "history-" + std::to_string(i / 4);
                if (i % 4 == 0) strategy_entry(id, true, missing, missing, 1.0);
                if (i % 4 == 2) strategy_close(id);
            }
            // The first limit cannot fill. Replacing it creates a successor
            // that fills at 100, while the cohort retains its canonical root.
            if (i == 4 * history_)
                strategy_entry("successor", true, 50.0, missing, 1.0);
            if (i == 4 * history_ + 1)
                strategy_entry("successor", true, 100.0, missing, 1.0);
        } else {
            if (i == 0) strategy_entry("position", true, missing, missing, 1.0);
            if (i > 0) {
                // Neither price is reached. Every publication replaces the
                // previous bracket, with alternating stops to exercise peers.
                strategy_exit("moving", "position", 120.0, 80.0 + (i % 2),
                              missing, missing, missing, 100.0, {}, 1.0);
            }
        }
    }

private:
    Shape shape_;
    int bars_, history_;
};

std::vector<Bar> feed(Shape shape, int count) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double high = shape == Shape::RetiredBrackets && i == 2 ? 110.0 : 100.0;
        bars.push_back({100.0, high, 100.0, 100.0, 1000.0,
                        start_ms + static_cast<std::int64_t>(i) * 60000});
    }
    return bars;
}

void print_phase(Shape shape, const char* phase, int n, int history, bool recording,
                 int bars, const Sample& before, const Sample& after) {
    const auto work = difference(after.work, before.work);
    const auto& p = after.population;
    std::printf("PF_RETIREMENT_WORK shape=%s phase=%s recording=%d N=%d "
                "m=%" PRIu64 " R=%" PRIu64 " C=%" PRIu64
                " row_visits=%" PRIu64 " pin_comparisons=%" PRIu64
                " peer_visits=%" PRIu64 " cohort_rows=%" PRIu64
                " fronts_tested=%" PRIu64 " rows_erased=%" PRIu64
                " bars=%d history=%d m0=%" PRIu64 " R0=%" PRIu64 " C0=%" PRIu64,
                name(shape), phase, recording ? 1 : 0, n, p.m, p.R, p.C,
                work.row_visits, work.pin_comparisons, work.peer_visits,
                work.cohort_rows, work.fronts_tested, work.rows_erased, bars, history,
                before.population.m, before.population.R, before.population.C);
    std::printf(" pin_rows=%" PRIu64 " gate_rows=%" PRIu64
                " candidate_rows=%" PRIu64 " final_rows=%" PRIu64
                " held_comparisons=%" PRIu64 " k3_pairs=%" PRIu64
                " root_visits=%" PRIu64 " family_visits=%" PRIu64 " certificate_rows=%" PRIu64
                " missing_fronts=%" PRIu64 " missing_fronts_pinned=%" PRIu64
                " origins_retired=%" PRIu64 " placement_slots=%" PRIu64
                " backing_rows_restored=%" PRIu64 " backing_rows_spilled=%" PRIu64
                " spill_rows_visited=%" PRIu64
                " placement_sweeps=%" PRIu64 " prefix_sweeps=%" PRIu64
                " live=%" PRIu64 " pending=%" PRIu64 " owners=%" PRIu64 " opened=%" PRIu64
                " retired_exit_rows=%" PRIu64 "\n",
                work.pin_rows, work.gate_rows, work.candidate_rows, work.final_rows,
                work.held_comparisons, work.k3_pairs, work.root_visits, work.family_visits,
                work.certificate_rows,
                work.missing_fronts, work.missing_fronts_pinned, work.origins_retired,
                work.placement_slots, work.backing_rows_restored, work.backing_rows_spilled,
                work.spill_rows_visited, work.placement_sweeps, work.prefix_sweeps,
                p.live, p.pending, p.owners, p.opened, p.retired_exit_rows);
    std::fflush(stdout);
}

struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
};

void run_shape(Shape shape, int n, int history, bool recording) {
    RetirementProbe host(shape, n, history, recording);
    const auto bars = feed(shape, n);
    host.run(bars.data(), n, "1", "1");
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.saw_begin && host.saw_end, "retirement phase boundaries were not reached");
    const Sample initial{};
    const auto terminal = host.sample();
    print_phase(shape, "setup", n, history, recording, host.quiet_begin() + 1,
                initial, host.begin_quiet);
    print_phase(shape, shape == Shape::Churn ? "churn" : "quiet", n, history, recording,
                host.quiet_end() - host.quiet_begin(), host.begin_quiet, host.end_quiet);
    print_phase(shape, "close", n, history, recording, n - host.quiet_end() - 1,
                host.end_quiet, terminal);

    require(host.begin_quiet.units == 1.0 && host.end_quiet.units == 1.0,
            "retirement fixture did not keep its residual unit open");
    require(terminal.units == 0.0, "retirement fixture did not close its residual unit");
    const int closed_before = shape == Shape::Churn ? 0 : history;
    require(host.begin_quiet.trades == closed_before && host.end_quiet.trades == closed_before,
            "retirement fixture did not complete the expected historical exits");
    // Each target closes 1 * (110 - 100) = 10. All history/cleanup closes
    // use 1 * (100 - 100) = 0, with no fees or slippage.
    const double expected_profit = shape == Shape::RetiredBrackets ? 10.0 * history : 0.0;
    require(host.begin_quiet.profit == expected_profit
            && host.end_quiet.profit == expected_profit && terminal.profit == expected_profit,
            "retirement fixture disagrees with the hand money oracle");
    Report report;
    host.fill_report(&report.value);
    require(report.value.trades_len == closed_before + 1
            && report.value.net_profit == expected_profit,
            "retirement report disagrees with the hand money oracle");
    int targets = 0;
    for (int i = 0; i < report.value.trades_len; ++i) {
        const auto& trade = report.value.trades[i];
        require(!trade.open_at_end && trade.qty == 1.0 && trade.entry_price == 100.0
                && trade.commission == 0.0,
                "retirement fixture trade has an unexpected basis");
        if (trade.exit_price == 110.0) ++targets;
        else require(trade.exit_price == 100.0, "retirement fixture filled outside its price path");
    }
    require(targets == (shape == Shape::RetiredBrackets ? history : 0),
            "retirement fixture did not fill exactly its target brackets");

    const auto quiet = difference(host.end_quiet.work, host.begin_quiet.work);
    const auto& begin = host.begin_quiet.population;
    const auto& end = host.end_quiet.population;
    if (shape != Shape::Churn) {
        require(begin.live == 0 && begin.pending == 0 && begin.owners == 0
                && begin.opened == 1 && begin.m == 1,
                "quiet retirement fixture has extra live or pending references");
        require(begin.m == end.m && begin.R == end.R && begin.C == end.C
                && begin.live == end.live && begin.pending == end.pending
                && begin.owners == end.owners && begin.opened == end.opened,
                "quiet retirement population changed");
    }
    if (shape == Shape::RetiredBrackets) {
        require(begin.retired_exit_rows >= static_cast<std::uint64_t>(history)
                && end.retired_exit_rows == begin.retired_exit_rows,
                "retired bracket fixture did not retain nonlive exit rows");
        require(quiet.row_visits > 0 && quiet.pin_rows > 0,
                "retired bracket scan counters are zero");
        require(quiet.gate_rows > 0, "retired bracket gate scan counter is zero");
    } else if (shape == Shape::MissingFront) {
        require(begin.C == static_cast<std::uint64_t>(history + 1)
                && host.begin_quiet.work.origins_retired > 0,
                "cohort fixture did not retire completed cohort prefixes");
        require(quiet.missing_fronts > 0 && quiet.missing_fronts_pinned > 0,
                "cohort fixture did not keep a missing placement front pinned");
        require(quiet.cohort_rows > 0 && quiet.fronts_tested > 0,
                "cohort fallback counters are zero");
    } else {
        require(quiet.row_visits > 0 && quiet.peer_visits > 0 && quiet.k3_pairs > 0
                && quiet.rows_erased > 0,
                "per bar bracket churn did not visit peers and erase retired rows");
    }
    require(terminal.work.rows_erased > 0, "retirement fixture erased no placement rows");
}
#undef RETIREMENT_COUNTS
} // namespace

int main() {
    try {
        for (const int n : {2000, 4000, 8000}) {
            // History and quiet length also vary independently: each size
            // runs both the same history and one proportional to its bars.
            for (const int history : {64, n / 16}) {
                for (const bool recording : {false, true}) {
                    run_shape(Shape::RetiredBrackets, n, history, recording);
                    run_shape(Shape::MissingFront, n, history, recording);
                }
            }
            for (const bool recording : {false, true})
                run_shape(Shape::Churn, n, 0, recording);
        }
        std::puts("retirement work probe: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "retirement work probe: %s\n", error.what());
        return 1;
    }
}
