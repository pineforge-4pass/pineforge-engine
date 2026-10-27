// CR-398: public reversing runs, with the pre-gate loop retained as the oracle.
// Like test_adapter_reversal_storage, this TU supplies the real adapter before
// the static archive is searched. The observation also works with the frozen
// base adapter: it counts that adapter's actual sorted-id visits, excluding the
// incoming market entry (the only preserved id in the growth fixture).
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cr398_fixture {
struct Probe {
    bool oracle = false;
    bool gate_seen = false;
    std::uint64_t calls = 0;
    std::string incoming;
};
inline Probe* active = nullptr;

template<class Visit> auto observe_old_walk(Visit visit) {
    return [visit = std::move(visit)](const std::string& id) mutable {
        // A fixed Debug build also walks skipped ids to assert the store
        // predicates. That verification is not a cancellation call. On the
        // frozen base no gate runs, so this still counts its actual old loop.
        if (active && (!active->gate_seen || active->oracle)
            && id != active->incoming) ++active->calls;
        visit(id);
    };
}
inline bool use_old_loop() {
    if (!active) return false;
    active->gate_seen = true;
    return active->oracle;
}
inline void observe_call() {
    if (active) ++active->calls;
}
} // namespace cr398_fixture

#define for_each_sorted_id(visit) \
    for_each_sorted_id(::cr398_fixture::observe_old_walk(visit))
#define PINEFORGE_FULL_CLOSE_EXIT_ORACLE() ::cr398_fixture::use_old_loop()
#define PINEFORGE_FULL_CLOSE_EXIT_CALL() ::cr398_fixture::observe_call()
#include "../src/source/pine_adapter.cpp"
#undef PINEFORGE_FULL_CLOSE_EXIT_CALL
#undef PINEFORGE_FULL_CLOSE_EXIT_ORACLE
#undef for_each_sorted_id
#include "../src/source/pine_state_hash.cpp"

namespace cr398_fixture {
using namespace pineforge;
namespace no = pineforge::native_order;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr double kCapital = 1000000.0;
constexpr std::int64_t kStart = 1736121600000LL;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> void bits(std::string& out, const T& value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof value);
}
void text(std::string& out, const std::string& value) {
    bits(out, value.size());
    out += value;
}

// Serialize values, never addresses or the padding of a Trade/string.
// This is the report_image contract used by test_id_adapter_storage.
std::string report_image(const BacktestEngine& host) {
    ReportC report{};
    host.fill_report(&report);
    struct Release {
        ReportC& report;
        ~Release() { BacktestEngine::free_report(&report); }
    } release{report};
    std::string out;
    bits(out, report.total_trades); bits(out, report.trades_len);
    bits(out, report.net_profit);
    bits(out, report.input_bars_processed); bits(out, report.script_bars_processed);
    bits(out, report.security_feeds_total); bits(out, report.security_eval_complete_total);
    bits(out, report.security_eval_partial_total); bits(out, report.magnifier_sub_bars_total);
    bits(out, report.magnifier_sample_ticks_total); bits(out, report.input_tf_seconds);
    bits(out, report.script_tf_seconds); bits(out, report.script_tf_ratio);
    bits(out, report.needs_aggregation); bits(out, report.bar_magnifier_enabled);
    bits(out, report.metrics);
    for (int i = 0; i < report.trades_len; ++i) {
        const auto& t = report.trades[i];
        bits(out, t.entry_time); bits(out, t.exit_time);
        bits(out, t.entry_price); bits(out, t.exit_price); bits(out, t.pnl); bits(out, t.pnl_pct);
        bits(out, t.is_long); bits(out, t.max_runup); bits(out, t.max_drawdown);
        bits(out, t.qty); bits(out, t.commission);
        bits(out, t.entry_bar_index); bits(out, t.exit_bar_index); bits(out, t.open_at_end);
    }
    bits(out, report.security_diag_len);
    for (int i = 0; i < report.security_diag_len; ++i) {
        const auto& row = report.security_diag[i];
        bits(out, row.sec_id); bits(out, row.feed_count);
        bits(out, row.eval_complete_count); bits(out, row.eval_partial_count);
    }
    bits(out, report.trace_names_len);
    for (int i = 0; i < report.trace_names_len; ++i) text(out, report.trace_names[i]);
    bits(out, report.trace_len);
    for (int i = 0; i < report.trace_len; ++i) {
        const auto& row = report.trace[i];
        bits(out, row.timestamp); bits(out, row.bar_index);
        bits(out, row.name_id); bits(out, row.value);
    }
    bits(out, report.equity_curve_len);
    for (std::int64_t i = 0; i < report.equity_curve_len; ++i) bits(out, report.equity_curve[i]);
    bits(out, report.broker_state_hash_len);
    for (std::int64_t i = 0; i < report.broker_state_hash_len; ++i)
        bits(out, report.broker_state_hash[i]);
    bits(out, host.report_trade_count());
    for (int i = 0; i < host.report_trade_count(); ++i) {
        const auto row = host.get_report_trade(i);
        text(out, row.entry_id); text(out, row.exit_id);
        text(out, row.entry_comment); text(out, row.exit_comment);
        bits(out, row.entry_incarnation); bits(out, row.exit_from_bracket); bits(out, row.close_cause);
    }
    return out;
}

std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1000.0, kStart + i * 60000LL});
    return bars;
}
std::string id_for(int i) { return "reversal-owner-" + std::to_string(i); }

class Host : public source::PineStrategyHost {
public:
    explicit Host(bool coof = false) {
        source::PineStrategyConfig config;
        config.initial_capital = kCapital;
        config.process_orders_on_close = true;
        config.calc_on_order_fills = coof;
        config.margin_long = config.margin_short = 0.0;
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        set_broker_state_hash_recording(true);
    }
    // The existing recording-hash witness's portability seam: compare the
    // native broker and source extension while fixing the tzdata execution hash.
    std::uint64_t broker_state_hash_projection() const override {
        return broker_state_hash_from_execution_hash(0x5eed1234abcd0398ULL);
    }
    void enter(const std::string& id, bool is_long) {
        require(active != nullptr, "the public-run observer is installed");
        active->incoming = id;
        strategy_entry(id, is_long, kNa, kNa, 1.0);
        reads.push_back(broker_state_hash_projection());
    }
    bool pending(const char* id) const {
        const auto& view = pending_intent_view();
        for (int i = 0; i < view.size(); ++i) {
            pf_pending_order_v1_t row{};
            require(view.copy_v1(i, &row) == 0, "copy the public pending projection");
            if (std::string(row.id) == id) return true;
        }
        return false;
    }
    bool working(const char* id) const {
        for (const auto& row : native_working_requests())
            if (row.definition->request.label == id) return true;
        return false;
    }
    std::vector<std::uint64_t> reads;
    std::uint64_t reversals = 0;
protected:
    void on_native_applied_event(const no::ExecutionAppliedEvent& event,
                                const NativeDecisionContext&) override {
        if (event.closed_units > 0.0 && event.opened_units != 0.0) ++reversals;
        reads.push_back(broker_state_hash_projection());
    }
};

class DenseHost final : public Host {
public:
    explicit DenseHost(bool fixed_ids) : fixed_ids_(fixed_ids) {}
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        enter(id_for(fixed_ids_ ? i % 2 : i), i % 2 == 0);
    }
private:
    bool fixed_ids_;
};

struct Result {
    std::string report;
    std::vector<std::uint64_t> reads;
    std::uint64_t final_hash = 0, calls = 0, reversals = 0;
};

template<class H> Result run(H& host, const std::vector<Bar>& bars, bool oracle) {
    Probe probe;
    probe.oracle = oracle;
    struct Restore {
        Probe* previous = active;
        ~Restore() { active = previous; }
    } restore;
    active = &probe;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(host.last_run_status() == 0 && host.last_error().empty(), host.last_error().c_str());
    return {report_image(host), host.reads, host.broker_state_hash_projection(),
            probe.calls, host.reversals};
}

void same_result(const Result& actual, const Result& old) {
    require(actual.report == old.report && actual.reads == old.reads
            && actual.final_hash == old.final_hash && actual.reversals == old.reversals,
            "reversal exit owners: every trade, report, equity and broker hash equals the old loop");
}

void check_dense_trades(const DenseHost& host, int n, bool fixed_ids) {
    // One unit opens at each 100 close. Each following opposite unit closes
    // its predecessor at 100 and opens one new unit: N-1 closed trades,
    // zero realized P&L, zero fees, and unchanged equity.
    require(host.trade_count() == n - 1 && host.reversals == static_cast<std::uint64_t>(n - 1),
            "the public market tape reverses once per bar after the first");
    for (int i = 0; i < host.trade_count(); ++i) {
        const auto row = host.get_trade(i);
        require(row.entry_id == id_for(fixed_ids ? i % 2 : i)
                && row.exit_id == id_for(fixed_ids ? (i + 1) % 2 : i + 1)
                && row.entry_bar_index == i && row.exit_bar_index == i + 1
                && row.entry_time == kStart + i * 60000LL
                && row.exit_time == kStart + (i + 1) * 60000LL
                && row.entry_price == 100.0 && row.exit_price == 100.0 && row.qty == 1.0
                && row.pnl == 0.0 && row.commission == 0.0 && !row.open_at_end
                && row.is_long == (i % 2 == 0) && !row.exit_from_bracket,
                "the hand-worked reversing trade rows are exact");
    }
    ReportC report{};
    host.fill_report(&report);
    struct Release {
        ReportC& report;
        ~Release() { BacktestEngine::free_report(&report); }
    } release{report};
    require(report.net_profit == 0.0 && report.equity_curve_len > 0
            && report.broker_state_hash_len > 0, "record equity and hashes through the public report");
    for (std::int64_t i = 0; i < report.equity_curve_len; ++i)
        require(report.equity_curve[i].equity == kCapital
                && report.equity_curve[i].open_profit == 0.0,
                "zero-P&L tape keeps every equity sample unchanged");
}

void growth() {
    std::uint64_t calls[2]{}, old_calls[2]{};
    for (int sample = 0; sample < 2; ++sample) {
        const int n = sample == 0 ? 128 : 256;
        const auto bars = tape(n);
        for (bool fixed_ids : {false, true}) {
            DenseHost candidate(fixed_ids), oracle(fixed_ids);
            const auto actual = run(candidate, bars, false);
            const auto old = run(oracle, bars, true);
            check_dense_trades(candidate, n, fixed_ids);
            check_dense_trades(oracle, n, fixed_ids);
            same_result(actual, old);
            // Count the base's real visits, not a guessed charge per bar.
            // The only exemption is the incoming id; every other visit calls
            // the old full-close canceler. Unique ids cost 1+...+(N-1);
            // the two fixed ids cost one call per reversal.
            const auto expected = fixed_ids ? static_cast<std::uint64_t>(n - 1)
                : static_cast<std::uint64_t>(n) * static_cast<std::uint64_t>(n - 1) / 2;
            require(old.calls == expected, "the retained oracle counts actual full-close calls");
            std::printf("CR398 N=%d fixed_ids=%d calls=%llu old_calls=%llu reversals=%llu\n",
                n, fixed_ids, static_cast<unsigned long long>(actual.calls),
                static_cast<unsigned long long>(old.calls),
                static_cast<unsigned long long>(actual.reversals));
            if (!fixed_ids) { calls[sample] = actual.calls; old_calls[sample] = old.calls; }
        }
    }
    std::fflush(stdout);
    require(2 * calls[1] <= 5 * calls[0],
            "reversal exit owners: cancellation calls grow faster than bars");
    require(old_calls[1] > 3 * old_calls[0], "the old-loop control retains the quadratic defect");
}

enum class OwnerCase { Live, Empty, Coof, Anchored, PendingBound };
constexpr int kPrefix = 64;

class OwnerHost final : public Host {
public:
    explicit OwnerHost(OwnerCase which) : Host(which == OwnerCase::Coof), which_(which) {}
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == previous_) {
            // The close-fill recalculation queues a far stop. No later
            // reissue is allowed to conceal losing that COOF-owned exit.
            if (which_ == OwnerCase::Coof && i == kPrefix - 1 && !coof_queued) {
                strategy_exit("owner-exit", id_for(i), kNa, 150.0);
                coof_queued = pending("owner-exit");
                reads.push_back(broker_state_hash_projection());
            }
            return;
        }
        previous_ = i;
        if (i < kPrefix) {
            const auto id = which_ == OwnerCase::Empty && i == kPrefix - 1
                ? std::string{} : id_for(i);
            enter(id, i % 2 == 0);
            if (i == kPrefix - 1 && which_ == OwnerCase::Anchored) {
                // A short STOP parent at 50 never fills on the 99..101 tape.
                // Its unresolved loss is anchored to that future fill.
                strategy_entry("anchored-parent", false, kNa, 50.0, 1.0);
                strategy_exit("owner-exit", "anchored-parent", kNa, kNa, kNa, kNa, kNa,
                              100.0, {}, kNa, {}, kNa, 400.0);
            }
            if (i == kPrefix - 1 && which_ == OwnerCase::PendingBound) {
                // The short limit at 150 waits through a reversal to long.
                // Whole absolute exits bind to this pending id and survive
                // flat/reversal until that id really fills.
                strategy_entry("bound-parent", false, 150.0, kNa, 1.0);
                strategy_exit("owner-exit", "bound-parent", 50.0, 200.0);
            }
        } else if (i == kPrefix) {
            if (which_ == OwnerCase::Live || which_ == OwnerCase::Empty)
                strategy_exit("owner-exit",
                    which_ == OwnerCase::Empty ? std::string{} : id_for(kPrefix - 1),
                    kNa, 150.0);
            if (which_ == OwnerCase::Anchored) {
                const auto& anchors = adapter_.anchored_relative_stats();
                anchored_before = anchors.anchored;
                // FromOwnerFill children are PendingUntilArmed: the public
                // working enumeration hides this child. One parent and one
                // loss operand produce one anchor; that parent never filled.
                require(anchored_before == 1 && anchors.adopted == 0
                        && anchors.withdrawn == 0 && !working("owner-exit"),
                        "a real anchored child waits without a working projection");
            } else {
                require((pending("owner-exit") || working("owner-exit")), "a real public exit exists before the reversal");
            }
            owner_seen = true;
            enter("after-owner", true);
        } else if (i == kPrefix + 2 && which_ == OwnerCase::PendingBound) {
            // Bar 65's gap to 150 fills bound-parent and binds its exits.
            // A second reversal must now cancel those same exits.
            require(bound_filled && (pending("owner-exit") || working("owner-exit")), "the formerly pending-bound exit bound to a fill");
            enter("after-bound-fill", true);
        }
        reads.push_back(broker_state_hash_projection());
    }
    bool owner_seen = false, cancelled = false, survived = false;
    bool coof_queued = false, bound_filled = false;
    std::uint64_t anchored_before = 0;
protected:
    void on_native_applied_event(const no::ExecutionAppliedEvent& event,
                                const NativeDecisionContext& context) override {
        Host::on_native_applied_event(event, context);
        if (event.request().label == "bound-parent" && event.opened_units != 0.0)
            bound_filled = true;
        if (event.request().label == "after-owner" && event.closed_units > 0.0) {
            require(owner_seen, "the reversal follows the observed exit");
            if (which_ == OwnerCase::PendingBound) {
                survived = (pending("owner-exit") || working("owner-exit"));
                require(survived, "pending-bound exits survive the first full close");
            } else {
                cancelled = !pending("owner-exit") && !working("owner-exit");
                require(cancelled, "the reversal cancels the old owner's queued exit");
                if (which_ == OwnerCase::Anchored)
                    require(adapter_.anchored_relative_stats().withdrawn >= anchored_before,
                            "the anchored child is withdrawn at full close");
            }
        }
        if (event.request().label == "after-bound-fill" && event.closed_units > 0.0) {
            cancelled = !pending("owner-exit") && !working("owner-exit");
            require(cancelled, "the second full close cancels the exit after its binding fill");
        }
    }
private:
    OwnerCase which_;
    int previous_ = -1;
};

void owners() {
    for (const auto which : {OwnerCase::Live, OwnerCase::Empty, OwnerCase::Coof,
                            OwnerCase::Anchored, OwnerCase::PendingBound}) {
        auto bars = tape(kPrefix + 4);
        if (which == OwnerCase::PendingBound) {
            for (int i = kPrefix + 1; i < static_cast<int>(bars.size()); ++i) {
                bars[i].open = bars[i].close = 150.0;
                bars[i].high = 151.0; bars[i].low = 149.0;
            }
        }
        OwnerHost candidate(which), oracle(which);
        const auto actual = run(candidate, bars, false);
        const auto old = run(oracle, bars, true);
        same_result(actual, old);
        require(actual.calls > 0,
                "a queued owner reaches the full-close canceler as well as matching its outputs");
        for (const auto* host : {&candidate, &oracle}) {
            require(host->owner_seen && host->cancelled, "the owner cancellation witness completed");
            require(which != OwnerCase::Coof || host->coof_queued,
                    "the COOF callback really issued the queued exit");
            require(which != OwnerCase::PendingBound || (host->survived && host->bound_filled),
                    "the pending-bound survival and binding transitions both ran");
            // 64 prefix entries give 63 closes; after-owner gives the 64th.
            // PendingBound adds two reversals (the 150 fill, then its close).
            require(host->trade_count() == kPrefix + (which == OwnerCase::PendingBound ? 2 : 0),
                    "owner fixtures keep the hand-worked public closed-trade count");
            for (int i = 0; i < host->trade_count(); ++i)
                require(!host->get_trade(i).exit_from_bracket,
                        "no cancelled far exit becomes a later bracket trade");
        }
    }
}
} // namespace cr398_fixture

int main(int argc, char** argv) {
    try {
        const bool all = argc == 1;
        const bool only_growth = argc == 2 && std::string(argv[1]) == "--growth";
        const bool only_owners = argc == 2 && std::string(argv[1]) == "--owners";
        if (!all && !only_growth && !only_owners) {
            std::fputs("expected --growth, --owners, or no arguments\n", stderr);
            return 2;
        }
        if (all || only_growth) cr398_fixture::growth();
        if (all || only_owners) cr398_fixture::owners();
        std::puts("reversal exit owner gate: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
