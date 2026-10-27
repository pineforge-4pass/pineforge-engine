// Hold dormant-exit population and bar count fixed; vary only unrelated LIVE
// placement rows. Count the production k3_pairs seam, never elapsed time.
#include "k1_dormant_retirement_fixture.hpp"

#include <array>
#include <exception>

namespace a5d_scan {
using namespace dormant_retirement;

constexpr int kDormant = 16;
constexpr int kGrowBars = 4;
constexpr int kWarmup = 12;
constexpr int kMeasuredBars = 200;
constexpr int kLastBar = kWarmup + kMeasuredBars;

// Same explicit-instantiation private-access idiom as PlacementTag. The
// cohort type is private, so deduce its member-pointer type.
struct CohortsTag {
    friend auto access(CohortsTag);
};
template<class Tag, auto Member>
struct DeducedPrivateAccess {
    friend auto access(Tag) { return Member; }
};
template struct DeducedPrivateAccess<CohortsTag, &ps::PineExecutionAdapter::cohorts_by_id_>;
struct CycleTag {
    using type = std::int64_t ps::PineExecutionAdapter::*;
    friend type access(CycleTag);
};
template struct PrivateAccess<CycleTag, &ps::PineExecutionAdapter::current_position_cycle_>;

struct Work {
    std::uint64_t pairs = 0;
    std::uint64_t sweeps = 0;
    std::size_t min_sweep_rows = std::numeric_limits<std::size_t>::max();
    std::size_t max_sweep_rows = 0;
};
inline Work* counting = nullptr;

inline void work(const char* site, std::uint64_t count) {
    if (counting && std::strcmp(site, "k3_pairs") == 0) counting->pairs += count;
}

inline void sweep(const ps::PlacementTable& rows) {
    if (!counting) return;
    ++counting->sweeps;
    counting->min_sweep_rows = std::min(counting->min_sweep_rows, rows.size());
    counting->max_sweep_rows = std::max(counting->max_sweep_rows, rows.size());
}
} // namespace a5d_scan

// Reproduce position_tail_retirement_fixture.hpp's instrumentation/linkage:
// compile the real adapter and hash TUs here, then link the normal static
// library through pineforge_add_unit_test. No compile definition is needed.
#define PINEFORGE_TAIL_WORK(site, count) ::a5d_scan::work(site, count)
#define PINEFORGE_RETIREMENT_OBSERVE(rows, cycle, askable, roots, candidates, first) \
    ::a5d_scan::sweep(rows)
#include "../src/source/pine_adapter.cpp"
#include "../src/source/pine_state_hash.cpp"
#undef PINEFORGE_RETIREMENT_OBSERVE
#undef PINEFORGE_TAIL_WORK

namespace a5d_scan {
struct Outcome {
    int unrelated = 0;
    int measured_bars = 0;
    Work work;
};

class ScanHost final : public ps::PineStrategyHost {
public:
    explicit ScanHost(int unrelated) : unrelated_(unrelated) {
        ps::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.pyramiding = 1;
        config.commission_value = 0.0;
        config.margin_long = config.margin_short = 100.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.01);
        set_margin_call_enabled(true);
        set_broker_state_hash_recording(false);
    }

    ~ScanHost() override {
        if (counting == &work_) counting = nullptr;
    }

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        check(i == callbacks_, "one callback per consecutive minute");
        ++callbacks_;
        check(bar.timestamp == kStart + static_cast<std::int64_t>(i) * kMinute,
              "synthetic minute timestamp");
        if (i >= 1)
            check(live_position_size() == -40.0
                  && adapter_.source_unclosed_qty_for("S") == 40.0,
                  "one unchanged open short origin; no pending-row fills");

        // The warmup checkpoint precedes counting and precedes the commands
        // whose next-bar sweeps are measured. Recheck every measured bar.
        if (i >= kWarmup) {
            if (i == kLastBar) counting = nullptr;
            check_population(bar, i - 1);
            if (i == kWarmup) {
                check(counting == nullptr, "measurement windows do not overlap");
                work_ = {};
                counting = &work_;
            } else {
                ++measured_bars_;
            }
        }

        if (i == 0) strategy_entry("S", false, kNa, kNa, 40.0);
        // Unique ids, no OCA group, and a buy limit below EVERY tape low.
        // Ordinary orders bypass pyramiding without attempting a reversal.
        if (i < kGrowBars) {
            const int begin = unrelated_ * i / kGrowBars;
            const int end = unrelated_ * (i + 1) / kGrowBars;
            for (int row = begin; row < end; ++row)
                strategy_order("pending-" + std::to_string(row), true, 1.0, 1.0);
        }
        if (i >= 2 && i < kLastBar) {
            const double stop = stop_for(i - 2);
            check(i == 2 || bits(stop) != previous_stop_, "stop changes every bar");
            previous_stop_ = bits(stop);
            for (int leg = 0; leg < kDormant; ++leg) {
                // Sixteen independent identities, total reservation 16 < 40.
                strategy_exit(exit_id(leg), "S", kNa, stop, kNa, kNa, kNa,
                              100.0, "held", 1.0);
                ++exit_calls_;
            }
            // The fixture's real pair hold followed by an adverse-open
            // declined reversal: touched stops retire without withdrawal.
            strategy_entry("L", true);
            strategy_close("S", "Reverse to Long");
        }
    }

    Outcome finish() {
        check(last_error().empty(), "run completed without error");
        check(callbacks_ == kLastBar + 1 && measured_bars_ == kMeasuredBars,
              "exactly 200 measured bars after warmup");
        check(exit_calls_ == kDormant * (kLastBar - 2),
              "all sixteen stops reissued on every scheduled bar");
        check(work_.sweeps == static_cast<std::uint64_t>(kMeasuredBars),
              "exactly one observed retirement sweep per measured bar");
        check(work_.pairs > 0, "production k3_pairs hook was not exercised");
        check(!ps::detail::retain_retired_rows(), "measure normal retirement");
        for (int i = 0; i < trade_count(); ++i)
            check(get_trade(i).open_at_end, "no stop, pending order or margin fill");
        return {unrelated_, measured_bars_, work_};
    }

    void hash_host_extension(BrokerStateHashSink& f) const override {
        ps::PineStrategyHost::hash_host_extension(f);
        f.i(unrelated_);
        // All remaining members are test observations, never order inputs.
    }

private:
    static std::string exit_id(int leg) { return "X-" + std::to_string(leg); }

    void check(bool holds, const char* message) const {
        if (holds) return;
        std::fprintf(stderr, "A5D SCAN CONTROL R=%d D=%d bar=%d: %s error=\"%s\"\n",
                     unrelated_, kDormant, pine_bar_index(), message, last_error().c_str());
        throw std::runtime_error(message);
    }

    void check_population(const Bar& bar, int issue_bar) {
        const auto& rows = adapter_.*access(PlacementTag{});
        std::vector<std::uint64_t> working;
        for (const auto& request : native_working_requests())
            working.push_back(request.definition->handle.incarnation);
        std::sort(working.begin(), working.end());
        const auto is_working = [&](std::uint64_t incarnation) {
            return std::binary_search(working.begin(), working.end(), incarnation);
        };

        // PRECONDITION 1: the R distinct orders exist in placement_, still
        // have native working requests, and keep their original incarnations.
        std::vector<std::uint64_t> pending(static_cast<std::size_t>(unrelated_), 0);
        int live_pending = 0;
        for (const auto row : rows) {
            const auto& value = row.second;
            if (value.source_id.compare(0, 8, "pending-") != 0) continue;
            const int id = std::stoi(value.source_id.substr(8));
            check(id >= 0 && id < unrelated_, "unrelated placement id in range");
            auto& incarnation = pending[static_cast<std::size_t>(id)];
            check(incarnation == 0, "one placement per unrelated pending id");
            incarnation = row.first;
            check(value.family == ps::PineOrderFamily::Order && value.is_long
                  && value.exit_levels.limit == 1.0 && value.requested_qty == 1.0
                  && value.bracket_origin.incarnation == 0 && is_working(row.first),
                  "unrelated row is a live, never-filled native placement");
            ++live_pending;
        }
        check(live_pending == unrelated_, "all R unrelated live placement rows exist");
        if (pending_incarnations_.empty()) pending_incarnations_ = pending;
        check(pending == pending_incarnations_, "pending rows persist without replacement");

        // Askability is checked against the SAME opened roster the production
        // sweep reads, not inferred from a nonzero bracket-origin handle.
        const auto& cohorts = adapter_.*access(CohortsTag{});
        const auto cohort = cohorts.find("S");
        check(cohort != cohorts.end() && cohort->second.opened.size() == 1,
              "S has exactly one opened, askable cohort origin");
        const auto origin = cohort->second.opened.front().incarnation;
        check(origin != 0 && rows.contains(origin), "askable origin has a placement row");
        const auto& opening = rows.at(origin);
        check(opening.family == ps::PineOrderFamily::Entry && opening.source_id == "S",
              "askable origin is the actual S opening");
        const auto cycle = adapter_.*access(CycleTag{});
        check(cycle > 0 && cohort->second.cycle == cycle, "S belongs to the current open cycle");

        // PRECONDITION 2: one actual previous-bar exit for EACH identity is
        // dormant, askable-origin, native-retired, unwithdrawn and current-cycle.
        std::array<int, kDormant> settled{};
        int declined_reversals = 0;
        for (const auto row : rows) {
            const auto& value = row.second;
            if (value.placement_script_open_ms != kStart + issue_bar * kMinute) continue;
            if (value.family == ps::PineOrderFamily::Entry && value.source_id == "L") {
                check(value.reverse_to && value.opening && !value.signal_price_refused
                      && !is_working(row.first) && std::isfinite(value.sizing.frozen_units)
                      && value.sizing.frozen_units > 0.0 && std::isfinite(value.sizing.equity)
                      && value.sizing.frozen_units * bar.open
                          > value.sizing.equity + std::abs(value.sizing.equity) * 1e-12,
                      "public reversal declined at the adverse opening");
                ++declined_reversals;
            }
            if (value.family != ps::PineOrderFamily::ExitStop) continue;
            for (int leg = 0; leg < kDormant; ++leg) {
                if (value.source_id != exit_id(leg)) continue;
                check(value.from_entry == "S" && value.bracket_origin.incarnation == origin
                      && value.placement_cycle == cycle
                      && value.legs.target().incarnation != 0 && value.legs.dormant()
                      && !value.legs.retired(exit_legs::Leg::Stop)
                      && value.cancellation.cause != ps::PineCancellationCause::Explicit
                      && !is_working(row.first) && value.projection_predecessor == 0
                      && value.requested_qty == 1.0
                      && bits(value.exit_levels.stop) == bits(stop_for(issue_bar - 2))
                      && bar.open < value.exit_levels.stop && bar.high >= value.exit_levels.stop,
                      "previous stop is touched, dormant, askable and native-retired");
                ++settled[static_cast<std::size_t>(leg)];
            }
        }
        for (int count : settled) check(count == 1, "all D dormant identities are present");
        check(declined_reversals == 1, "one real declined reversal for the D held exits");
    }

    const int unrelated_;
    int callbacks_ = 0, measured_bars_ = 0, exit_calls_ = 0;
    std::uint64_t previous_stop_ = 0;
    std::vector<std::uint64_t> pending_incarnations_;
    Work work_;
};

Outcome run(int unrelated) {
    RetentionSwitch retirement(false);
    const auto bars = tape(kLastBar + 1, false);
    ScanHost host(unrelated);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1",
             false, 4, MagnifierDistribution::ENDPOINTS);
    return host.finish();
}
} // namespace a5d_scan

int main() {
    try {
        const std::array<a5d_scan::Outcome, 3> outcomes{
            a5d_scan::run(500), a5d_scan::run(1000), a5d_scan::run(2000)};
        for (const auto& out : outcomes) {
            std::printf("A5D SCAN R=%d D=%d B=%d k3_pairs=%llu per_bar=%.3f "
                        "sweeps=%llu sweep_rows=%zu..%zu\n",
                        out.unrelated, a5d_scan::kDormant, out.measured_bars,
                        static_cast<unsigned long long>(out.work.pairs),
                        static_cast<double>(out.work.pairs) / out.measured_bars,
                        static_cast<unsigned long long>(out.work.sweeps),
                        out.work.min_sweep_rows, out.work.max_sweep_rows);
        }
        std::fflush(stdout);
        const auto& small = outcomes.front();
        const auto& large = outcomes.back();
        // B is identical, so cross-multiplying integer totals compares exact
        // per-bar work. No row-count-derived allowance can hide a full scan.
        if (2 * large.work.pairs > 3 * small.work.pairs) {
            std::fprintf(stderr,
                         "A5D SCAN FAILED: per-bar k3_pairs R=%d:%.3f R=%d:%.3f R=%d:%.3f; "
                         "R=%d must be at most 1.5x R=%d (totals=%llu/%llu/%llu B=%d)\n",
                         small.unrelated, static_cast<double>(small.work.pairs) / small.measured_bars,
                         outcomes[1].unrelated,
                         static_cast<double>(outcomes[1].work.pairs) / outcomes[1].measured_bars,
                         large.unrelated, static_cast<double>(large.work.pairs) / large.measured_bars,
                         large.unrelated, small.unrelated,
                         static_cast<unsigned long long>(small.work.pairs),
                         static_cast<unsigned long long>(outcomes[1].work.pairs),
                         static_cast<unsigned long long>(large.work.pairs), a5d_scan::kMeasuredBars);
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_k1_dormant_reissue_scan_work: %s\n", error.what());
        return 1;
    }
}
