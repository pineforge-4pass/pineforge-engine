// Command scratch storage follows the submitted command population.
// Counts below are command arithmetic, not byte measurements or byte caps.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/exit_scratch.hpp>

#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
using Adapter = source::PineExecutionAdapter;
using Handle = no::RequestHandle;
using Family = source::PineOrderFamily;

// Existing repository access pattern: no altered class definition or friend.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
#define ADAPTER_ACCESS(Tag, member) \
    struct Tag { friend auto access(Tag); }; \
    template struct Access<Tag, &Adapter::member>
ADAPTER_ACCESS(ScratchOwner, exit_scratch_);
ADAPTER_ACCESS(OpenEntries, per_entry_open_entries);
ADAPTER_ACCESS(RetiredScratch, retired_row_scratch_);
ADAPTER_ACCESS(Sweep, sweep_retired_rows);
ADAPTER_ACCESS(Relative, pending_relative_exits_);
ADAPTER_ACCESS(Shapes, relative_leg_shapes);
#undef ADAPTER_ACCESS
struct PreOpenBirths { friend auto access(PreOpenBirths); };
struct ActivationBirths { friend auto access(ActivationBirths); };
template struct Access<PreOpenBirths, &NativeExecutionConsumer::pre_open_births_>;
template struct Access<ActivationBirths, &NativeExecutionConsumer::activation_births_>;

constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;
constexpr std::int64_t kMinute = 60000;
int checks = 0;
int failures = 0;
int trial_population = 0;
int trial_bars = 0;

void check(const char* row, const char* name, const char* expression,
           bool passed, int line) {
    ++checks;
    if (passed) return;
    ++failures;
    std::fprintf(stderr, "FAIL [%s] %s:%d: %s -- %s (commands=%d bars=%d)\n",
                 row, __FILE__, line, expression, name, trial_population, trial_bars);
}
#define ROW_CHECK(row, name, ...) \
    check(row, name, #__VA_ARGS__, (__VA_ARGS__), __LINE__)

std::vector<Bar> chart(int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0, kStart + i * kMinute});
    return bars;
}
std::string id(const char* stem, int bar, int index = 0) {
    return std::string(stem) + "/" + std::to_string(bar) + "/" + std::to_string(index);
}

class SourceHost : public source::PineStrategyHost {
public:
    explicit SourceHost(int population) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = population + 4;
        config.commission_value = 0.0;
        config.margin_long = 0.0;
        config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }
    Adapter& adapter() { return adapter_; }
    void run_chart(int count, const char* row) {
        trial_bars = count;
        const auto bars = chart(count);
        run(bars.data(), count, "1", "1", false);
        ROW_CHECK(row, "run completed", last_error().empty());
    }

    // A real default-sized global exit, then its real cancellation, supplies
    // one unpinned row. Unlike a cancelled explicit named leg, this leg has no
    // askable origin to keep it. It makes the full sweep
    // reachable NOW, without ageing fields or manufacturing a placement.
    void full_sweep(const char* row, int serial) {
        const auto sentinel = id("cmd-sweep", pine_bar_index(), serial);
        strategy_exit(sentinel, "", 250.0, kNa);
        adapter_.flush_pending_bracket_legs();
        const auto pending = adapter_.fixture_pending_snapshots();
        const auto found = std::find_if(pending.begin(), pending.end(), [&](const auto& value) {
            return value.snapshot.source_id == sentinel;
        });
        ROW_CHECK(row, "full-sweep stimulus was submitted through strategy.exit",
                  found != pending.end());
        if (found != pending.end()) {
            ROW_CHECK(row, "default global leg has no bound origin",
                      found->snapshot.bracket_origin.incarnation == 0);
        }
        strategy_cancel(sentinel);
        adapter_.observe_terminal_receipts();
        const auto* point = NativeExecutionConsumer::bound(*this).current_point();
        ROW_CHECK(row, "sweep uses the real callback decision", point != nullptr);
        if (!point) return;
        Adapter::RetirementWork work;
        (adapter_.*access(Sweep{}))(point->decision, work);
        ROW_CHECK(row, "full retirement reader was reached", work.placement_sweeps == 1);
    }
};

// One priced raw order generates one CommandEvent, with no market checkpoint.
// Cancelling all N before returning leaves no older pending order on the next
// bar. Therefore the complete in-bar event window is exactly [0, N], and the
// next callback and the completed run both own zero events.
class JournalHost final : public SourceHost {
public:
    explicit JournalHost(int n) : SourceHost(n), n_(n) {}
    void on_source_bar(const Bar&) override {
        ++callbacks;
        const auto& journal = market_admission_journal();
        ROW_CHECK("pineexecutionadapter_admission_journal", "previous bar released every admission event",
                  journal.events().empty());
        std::vector<std::string> model;
        for (int i = 0; i < n_; ++i) {
            const auto key = id("cmd-admission", pine_bar_index(), i);
            strategy_order(key, true, 1.0, 1.0);
            model.push_back(key);
            ROW_CHECK("pineexecutionadapter_admission_journal", "exactly one retained event per submitted command",
                      journal.events().size() == model.size());
            std::vector<std::string> actual;
            for (const auto& event : journal.events()) {
                const auto* command = std::get_if<admission::CommandEvent>(&event);
                ROW_CHECK("pineexecutionadapter_admission_journal", "every retained event is an accepted real command",
                          command && command->observation
                              && command->outcome == admission::Outcome::Admitted
                              && command->admitted_incarnation != 0);
                if (command && command->observation)
                    actual.push_back(command->observation->id);
            }
            ROW_CHECK("pineexecutionadapter_admission_journal", "whole event window equals submitted commands in order",
                      actual == model);
        }
        peak = std::max(peak, journal.events().size());
        strategy_cancel_all();
    }
    int callbacks = 0;
    std::size_t peak = 0;
private:
    int n_;
};

void admission_journal() {
    for (int n : {1, 5, 17}) {
        trial_population = n;
        std::vector<std::size_t> peaks, ends;
        for (int bars : {8, 32}) {
            JournalHost host(n);
            host.run_chart(bars, "pineexecutionadapter_admission_journal");
            ROW_CHECK("pineexecutionadapter_admission_journal", "every bar executed its command window", host.callbacks == bars);
            peaks.push_back(host.peak);
            ends.push_back(host.market_admission_journal().events().size());
            ROW_CHECK("pineexecutionadapter_admission_journal", "bar-end retirement leaves zero live events", ends.back() == 0);
        }
        ROW_CHECK("pineexecutionadapter_admission_journal", "current-window peak is fixed as bars grow fourfold",
                  peaks == std::vector<std::size_t>(2, static_cast<std::size_t>(n)));
        ROW_CHECK("pineexecutionadapter_admission_journal", "completed-window count is fixed at zero for N and 4N bars",
                  ends == std::vector<std::size_t>(2, 0));
    }
}

// Four-bar cycles: submit N one-unit entries; their next-open fills produce
// N lots; select the real lots with the production helper; close the book;
// observe the flat book. Each selected child is ONE record in exit_scratch_'s
// actual store, and its scope must return that store to zero records.
class ScratchHost final : public SourceHost {
public:
    explicit ScratchHost(int n) : SourceHost(n), n_(n) {}
    void on_source_bar(const Bar&) override {
        ++callbacks;
        const int phase = pine_bar_index() % 4;
        if (phase == 0) {
            for (int i = 0; i < n_; ++i)
                strategy_entry(id("cmd-scratch-entry", pine_bar_index(), i), true, kNa, kNa, 1.0);
        }
        if (phase == 1) {
            const auto& owner = adapter().*access(ScratchOwner{});
            ROW_CHECK("pineexecutionadapter_exit_scratch", "run owns a scratch context", owner != nullptr);
            if (!owner) return;
            auto& store = owner->store;
            ROW_CHECK("pineexecutionadapter_exit_scratch", "no earlier command selection remains", store.record_count() == 0);
            ROW_CHECK("pineexecutionadapter_exit_scratch", "N submitted entries really filled", native_open_lots(kNa).size() == static_cast<std::size_t>(n_));
            {
                auto selected = (adapter().*access(OpenEntries{}))(true);
                selected.finish();
                const auto expected = static_cast<std::size_t>(n_); // N commands x one live lot.
                ROW_CHECK("pineexecutionadapter_exit_scratch", "real selection owns one record per submitted and filled entry",
                          selected.size() == expected && store.record_count() == expected);
                std::set<std::uint64_t> origins;
                for (std::size_t i = 0; i < selected.size(); ++i) {
                    const auto child = selected[i];
                    origins.insert(child.origin);
                    ROW_CHECK("pineexecutionadapter_exit_scratch", "selected record contains this cycle's one-unit fill",
                              child.origin != 0 && child.fill_price == 100.0
                                  && child.units == 1.0 && !child.consumed);
                }
                ROW_CHECK("pineexecutionadapter_exit_scratch", "selection has no duplicate or omitted lot",
                          origins.size() == expected);
                peak = std::max(peak, static_cast<std::size_t>(store.record_count()));
                {
                    auto opposite = (adapter().*access(OpenEntries{}))(false);
                    opposite.finish();
                    ROW_CHECK("pineexecutionadapter_exit_scratch", "opposite-side branch contributes zero records",
                              opposite.empty() && store.record_count() == expected);
                }
                ROW_CHECK("pineexecutionadapter_exit_scratch", "nested empty scope preserves only the active selection",
                          store.record_count() == expected);
            }
            ROW_CHECK("pineexecutionadapter_exit_scratch", "selection release retires ALL of its records",
                      store.record_count() == 0);
            ++selections;
            strategy_close_all();
        }
        if (phase == 2) {
            const auto& owner = adapter().*access(ScratchOwner{});
            ROW_CHECK("pineexecutionadapter_exit_scratch", "close fill leaves no old command records",
                      owner && owner->store.record_count() == 0 && native_open_lots(kNa).empty());
        }
    }
    int callbacks = 0;
    int selections = 0;
    std::size_t peak = 0;
private:
    int n_;
};

void exit_scratch() {
    for (int n : {1, 5, 17}) {
        trial_population = n;
        std::vector<std::size_t> peaks, ends;
        for (int bars : {8, 32}) {
            ScratchHost host(n);
            host.run_chart(bars, "pineexecutionadapter_exit_scratch");
            ROW_CHECK("pineexecutionadapter_exit_scratch", "all four-bar cycles reached a positive selection",
                      host.callbacks == bars && host.selections == bars / 4);
            ROW_CHECK("pineexecutionadapter_exit_scratch", "all selected entries used the real close/fill path",
                      host.trade_count() == n * (bars / 4));
            const auto& owner = host.adapter().*access(ScratchOwner{});
            ROW_CHECK("pineexecutionadapter_exit_scratch", "run context remains available for final count", owner != nullptr);
            peaks.push_back(host.peak);
            if (owner) ends.push_back(static_cast<std::size_t>(owner->store.record_count()));
        }
        ROW_CHECK("pineexecutionadapter_exit_scratch", "scratch peak equals N live selections for N and 4N bars",
                  peaks == std::vector<std::size_t>(2, static_cast<std::size_t>(n)));
        ROW_CHECK("pineexecutionadapter_exit_scratch", "released scratch record count stays zero as bars grow",
                  ends == std::vector<std::size_t>(2, 0));
    }
}

class SourceBarsHost final : public SourceHost {
public:
    SourceBarsHost(int n, int bars) : SourceHost(n + 1), n_(n), bars_(bars), model_(n, 0) {}
    void on_source_bar(const Bar&) override {
        ++callbacks;
        if (pine_bar_index() == 0) {
            strategy_entry("cmd-bars-held", true, kNa, kNa, 8.0);
            for (int i = 0; i < n_; ++i)
                strategy_entry(id("cmd-pinned-entry", 0, i), true, 1.0, kNa, 1.0);
        }
        if (pine_bar_index() >= 1 && pine_bar_index() < bars_ - 3) {
            full_sweep("retiredrowscratch_source_bars", 0);
            const auto& values = (adapter().*access(RetiredScratch{})).source_bars;
            // N still-working entries, all submitted on source bar zero.
            // The held entry filled and supplies no live Entry request here.
            ROW_CHECK("retiredrowscratch_source_bars", "whole source-bar scratch equals the N retained command pins",
                      values == model_ && values.size() == static_cast<std::size_t>(n_));
            peak = std::max(peak, values.size());
            ++positive_windows;
        }
        if (pine_bar_index() == bars_ - 3) {
            for (int i = 0; i < n_; ++i) strategy_cancel(id("cmd-pinned-entry", 0, i));
            full_sweep("retiredrowscratch_source_bars", 1);
            ROW_CHECK("retiredrowscratch_source_bars", "next full sweep releases all cancelled source-bar pins",
                      (adapter().*access(RetiredScratch{})).source_bars.empty());
            released = true;
            strategy_close_all();
        }
    }
    int callbacks = 0, positive_windows = 0;
    bool released = false;
    std::size_t peak = 0;
private:
    int n_, bars_;
    const std::vector<std::int32_t> model_;
};

void source_bars() {
    for (int n : {1, 5, 17}) {
        trial_population = n;
        std::vector<std::size_t> peaks, ends;
        for (int bars : {8, 32}) {
            SourceBarsHost host(n, bars);
            host.run_chart(bars, "retiredrowscratch_source_bars");
            ROW_CHECK("retiredrowscratch_source_bars", "all full-sweep windows and release ran",
                      host.callbacks == bars && host.positive_windows == bars - 4 && host.released);
            ROW_CHECK("retiredrowscratch_source_bars", "held lot closed through the engine", host.trade_count() == 1);
            peaks.push_back(host.peak);
            ends.push_back((host.adapter().*access(RetiredScratch{})).source_bars.size());
        }
        ROW_CHECK("retiredrowscratch_source_bars", "source-bar workset holds exactly N commands at both bar counts",
                  peaks == std::vector<std::size_t>(2, static_cast<std::size_t>(n)));
        ROW_CHECK("retiredrowscratch_source_bars", "released pin count is fixed at zero for N and 4N bars",
                  ends == std::vector<std::size_t>(2, 0));
    }
}

class CandidateNamesHost final : public SourceHost {
public:
    CandidateNamesHost(int n, int bars) : SourceHost(n), n_(n), bars_(bars) {}
    void on_source_bar(const Bar&) override {
        ++callbacks;
        if (pine_bar_index() == 0)
            strategy_entry("cmd-candidates-held", true, kNa, kNa, 8.0 * n_ + 8.0);
        if (pine_bar_index() == 1) {
            for (int i = 0; i < n_; ++i)
                strategy_exit(id("cmd-candidate", 1, i), "cmd-candidates-held", 200.0, 80.0,
                              kNa, kNa, kNa, 100.0, "", 1.0);
            adapter().flush_pending_bracket_legs();
            // N commands each declare a limit AND a stop. The kernel's
            // accepted handles provide identities, independently of the
            // retirement reader. Each new leg's target is itself, so the two
            // candidate names are the same accepted incarnation twice.
            const auto working = native_working_requests();
            ROW_CHECK("retiredrowscratch_candidate_names", "two real working legs per submitted bracket",
                      working.size() == 2U * static_cast<std::size_t>(n_));
            for (const auto& request : working) {
                model_.push_back(request.definition->handle.incarnation);
                model_.push_back(request.definition->handle.incarnation);
            }
            std::sort(model_.begin(), model_.end());
        }
        if (pine_bar_index() >= 1 && pine_bar_index() < bars_ - 3) {
            full_sweep("retiredrowscratch_candidate_names", 0);
            const auto& names = (adapter().*access(RetiredScratch{})).candidate_names;
            // N calls x 2 price legs x (candidate handle + lifecycle target).
            const auto expected = 4U * static_cast<std::size_t>(n_);
            ROW_CHECK("retiredrowscratch_candidate_names", "exactly four names per two-leg command, no prior sweep's names",
                      names.size() == expected);
            ROW_CHECK("retiredrowscratch_candidate_names", "whole candidate-name multiset equals accepted leg identities",
                      names == model_ && model_.size() == expected);
            peak = std::max(peak, names.size());
            ++positive_windows;
        }
        if (pine_bar_index() == bars_ - 3) {
            for (int i = 0; i < n_; ++i) strategy_cancel(id("cmd-candidate", 1, i));
            full_sweep("retiredrowscratch_candidate_names", 1);
            ROW_CHECK("retiredrowscratch_candidate_names", "withdrawn brackets contribute no names to the next sweep",
                      (adapter().*access(RetiredScratch{})).candidate_names.empty());
            released = true;
            strategy_close_all();
        }
    }
    int callbacks = 0, positive_windows = 0;
    bool released = false;
    std::size_t peak = 0;
private:
    int n_, bars_;
    std::vector<std::uint64_t> model_;
};

void candidate_names() {
    for (int n : {1, 5, 17}) {
        trial_population = n;
        std::vector<std::size_t> peaks, ends;
        for (int bars : {8, 32}) {
            CandidateNamesHost host(n, bars);
            host.run_chart(bars, "retiredrowscratch_candidate_names");
            ROW_CHECK("retiredrowscratch_candidate_names", "all candidate windows and withdrawal boundary ran",
                      host.callbacks == bars && host.positive_windows == bars - 4 && host.released);
            ROW_CHECK("retiredrowscratch_candidate_names", "held position really closed", host.trade_count() == 1);
            peaks.push_back(host.peak);
            ends.push_back((host.adapter().*access(RetiredScratch{})).candidate_names.size());
        }
        ROW_CHECK("retiredrowscratch_candidate_names", "four names per command remain fixed while bars grow fourfold",
                  peaks == std::vector<std::size_t>(2, 4U * static_cast<std::size_t>(n)));
        ROW_CHECK("retiredrowscratch_candidate_names", "next-window name count remains zero after release",
                  ends == std::vector<std::size_t>(2, 0));
    }
}

enum class TrailInput { Absent, Following, ZeroOffset, OmittedOffset, AbsolutePrice };
struct ShapeInput {
    bool profit, loss;
    TrailInput trail;
};
const ShapeInput shape_inputs[] = {
    {true, false, TrailInput::Absent}, {false, true, TrailInput::Absent},
    {true, true, TrailInput::Absent}, {false, false, TrailInput::Following},
    {true, true, TrailInput::Following}, {true, true, TrailInput::ZeroOffset},
    {true, true, TrailInput::OmittedOffset}, {true, false, TrailInput::ZeroOffset},
    {false, true, TrailInput::OmittedOffset}, {true, true, TrailInput::AbsolutePrice},
};
// Model one explicitly requested numeric leg of each kind. Absolute trail
// prices are already resolved and have no relative leg. The two numeric tick
// legs and the ONE trailing alternative give at most 1 + 1 + 1 = 3 values.
using ShapeModel = std::vector<std::tuple<Family, bool, double, double>>;
ShapeModel shape_model(const ShapeInput& input, bool long_side) {
    ShapeModel result;
    const double sign = long_side ? 1.0 : -1.0;
    if (input.profit) result.emplace_back(Family::ExitLimit, false, sign * 8.0, 8.0);
    if (input.loss) result.emplace_back(Family::ExitStop, false, -sign * 12.0, 12.0);
    if (input.trail == TrailInput::Following || input.trail == TrailInput::ZeroOffset
        || input.trail == TrailInput::OmittedOffset)
        result.emplace_back(Family::ExitTrail, input.trail == TrailInput::Following,
                            sign * 16.0, 16.0);
    return result;
}

class ShapesHost final : public SourceHost {
public:
    explicit ShapesHost(bool long_side) : SourceHost(64), long_(long_side) {}
    void on_source_bar(const Bar&) override {
        ++callbacks;
        const auto parent = id("cmd-shape-parent", pine_bar_index());
        // A stop parent outside the tape has no known fill price. exit()
        // therefore stages the actual unresolved operands for this parent.
        strategy_entry(parent, long_, kNa, long_ ? 150.0 : 50.0, 64.0);
        adapter().flush_pending_entries();
        int index = 0;
        for (const auto& input : shape_inputs) {
            const auto exit = id("cmd-shape", pine_bar_index(), index++);
            const bool has_trail = input.trail != TrailInput::Absent;
            const double offset = input.trail == TrailInput::ZeroOffset ? 0.0
                : (input.trail == TrailInput::OmittedOffset || !has_trail ? kNa : 2.0);
            strategy_exit(exit, parent, kNa, kNa, has_trail ? 16.0 : kNa, offset,
                          input.trail == TrailInput::AbsolutePrice ? 200.0 : kNa,
                          100.0, "", 1.0, "", input.profit ? 8.0 : kNa, input.loss ? 12.0 : kNa);
            const auto& pending = adapter().*access(Relative{});
            const auto found = std::find_if(pending.begin(), pending.end(), [&](const auto& value) {
                return value.exit_id == exit && value.from_entry == parent;
            });
            ROW_CHECK("relative_leg_shapes_shapes", "real strategy.exit staged the requested relative definition",
                      found != pending.end());
            if (found == pending.end()) continue;
            const auto shapes = (adapter().*access(Shapes{}))(*found, long_);
            const auto model = shape_model(input, long_);
            ROW_CHECK("relative_leg_shapes_shapes", "returned shape count equals independently requested numeric legs",
                      shapes.size() == model.size() && shapes.size() <= 3);
            ShapeModel actual;
            for (const auto& shape : shapes)
                actual.emplace_back(shape.family, std::holds_alternative<no::Trail>(shape.trigger),
                                    shape.signed_ticks, shape.operand_ticks);
            ROW_CHECK("relative_leg_shapes_shapes", "whole returned vector equals the limit-stop-one-trail model",
                      actual == model);
            peak = std::max(peak, shapes.size());
            ++definitions;
        }
        strategy_cancel_all();
    }
    int callbacks = 0, definitions = 0;
    std::size_t peak = 0;
private:
    bool long_;
};

void relative_shapes() {
    for (bool long_side : {true, false}) {
        trial_population = static_cast<int>(sizeof(shape_inputs) / sizeof(shape_inputs[0]));
        std::vector<std::size_t> peaks;
        for (int bars : {8, 32}) {
            ShapesHost host(long_side);
            host.run_chart(bars, "relative_leg_shapes_shapes");
            ROW_CHECK("relative_leg_shapes_shapes", "all shape branches used real command definitions",
                      host.callbacks == bars && host.definitions == trial_population * bars);
            peaks.push_back(host.peak);
        }
        ROW_CHECK("relative_leg_shapes_shapes", "two numeric legs plus one trailing alternative stay fixed across bar counts",
                  peaks == std::vector<std::size_t>(2, 1U + 1U + 1U));
    }
}

NativeRunSpec native_spec() {
    NativeRunSpec spec;
    spec.identity = {"command-births", 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:BIRTHS";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 1000000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.25;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    return spec;
}

// Every Open is one window. Even bars submit N one-unit openings; odd bars
// submit N one-unit reductions. The accepted handles themselves are the
// independent ordered membership model. Negative controls are priced requests
// outside the activation scope, or non-Market requests.
class BirthHost final : public NativeStrategyHost {
public:
    BirthHost(int n, bool activation) : n_(n), activation_(activation) {}
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    const char* row() const { return activation_ ? "nativeexecutionconsumer_activation_births" : "nativeexecutionconsumer_pre_open_births"; }
    const std::vector<Handle>& births() {
        if (activation_) return consumer().*access(ActivationBirths{});
        return consumer().*access(PreOpenBirths{});
    }
    void membership(const std::vector<Handle>& model) {
        if (activation_) {
            ROW_CHECK("nativeexecutionconsumer_activation_births", "activation births equal exactly the current scope's accepted commands",
                      (consumer().*access(ActivationBirths{})) == model);
        } else {
            ROW_CHECK("nativeexecutionconsumer_pre_open_births", "pre-open births equal exactly the current Open's accepted market commands",
                      (consumer().*access(PreOpenBirths{})) == model);
        }
    }
    void outside_control(int index, const std::vector<Handle>& model) {
        no::Request control;
        control.intent = no::Transact{1.0};
        control.label = id("cmd-outside", opens, index);
        control.trigger = no::Limit{1.0}; // Tape low is 99: this never fills.
        const auto result = submit(control);
        ROW_CHECK(row(), "negative-control command was accepted", result.handle.has_value());
        membership(model); // Observe the false branch BEFORE cancel can revoke a grant.
        if (result.handle) {
            ROW_CHECK(row(), "negative control really retired via cancel",
                      cancel(*result.handle).status == no::CancelStatus::Cancelled);
        }
    }
    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override {
        const bool closing = opens % 2 == 1;
        ++opens;
        std::vector<Handle> model;
        membership(model);
        outside_control(0, model);
        membership(model);
        if (activation_) {
            const bool began = consumer().begin_open_activation(*this);
            ROW_CHECK("nativeexecutionconsumer_activation_births", "real PreOpen callback opened its activation scope", began);
        }
        for (int i = 0; i < n_; ++i) {
            no::Request request;
            if (closing) request.intent = no::Reduce{no::ExplicitUnits{1.0}};
            else request.intent = no::Transact{1.0};
            request.label = id("cmd-born", opens, i);
            if (activation_) request.trigger = no::Limit{closing ? 99.0 : 101.0};
            const auto result = submit(request);
            ROW_CHECK(row(), "real command accepted", result.handle.has_value());
            if (result.handle) model.push_back(*result.handle);
            membership(model);
        }
        ROW_CHECK(row(), "N submitted commands give exactly N current births",
                  births().size() == static_cast<std::size_t>(n_));
        peak = std::max(peak, births().size());
        if (activation_) {
            // One additional real request enters the scope and is cancelled.
            // Its grant retires, while all N earlier commands remain granted.
            no::Request control;
            control.intent = no::Transact{1.0};
            control.label = id("cmd-revoked", opens);
            control.trigger = no::Limit{1.0};
            const auto extra = submit(control);
            ROW_CHECK("nativeexecutionconsumer_activation_births", "cancellable scope command was accepted", extra.handle.has_value());
            if (extra.handle) model.push_back(*extra.handle);
            membership(model);
            ROW_CHECK("nativeexecutionconsumer_activation_births", "one extra accepted command adds exactly one grant",
                      births().size() == static_cast<std::size_t>(n_ + 1));
            peak = std::max(peak, births().size());
            if (extra.handle) {
                ROW_CHECK("nativeexecutionconsumer_activation_births", "scope command cancelled through the real core",
                          cancel(*extra.handle).status == no::CancelStatus::Cancelled);
                model.pop_back();
            }
            membership(model);
            consumer().end_open_activation();
            membership(model); // Scope exit alone keeps the Open match's grants.
            outside_control(1, model);
            membership(model);
        }
    }
    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        ++fills;
        ROW_CHECK(row(), "current-window commands filled at the intended Open boundary",
                  event.cursor.point.path_phase == NativePathPhase::Open);
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++closes;
        if (activation_) {
            ROW_CHECK("nativeexecutionconsumer_activation_births", "Open match retires ALL activation grants before bar close",
                      (consumer().*access(ActivationBirths{})).empty());
        } else {
            ROW_CHECK("nativeexecutionconsumer_pre_open_births", "Open match retires ALL pre-open births before bar close",
                      (consumer().*access(PreOpenBirths{})).empty());
        }
    }
    int opens = 0, closes = 0, fills = 0;
    std::size_t peak = 0;
private:
    int n_;
    bool activation_;
};

void native_births(bool activation) {
    const char* row = activation ? "nativeexecutionconsumer_activation_births" : "nativeexecutionconsumer_pre_open_births";
    for (int n : {1, 5, 17}) {
        trial_population = n;
        std::vector<std::size_t> peaks, ends;
        for (int bars : {8, 32}) {
            trial_bars = bars;
            BirthHost host(n, activation);
            ROW_CHECK(row, "native configuration accepted",
                      host.configure_native(native_spec()).status == NativeSetupStatus::Applied);
            const auto input = chart(bars);
            host.run(input.data(), bars, "1", "1", false);
            ROW_CHECK(row, "native run completed", host.native_state().kind == NativeLifecycleKind::Completed
                      && host.last_error().empty());
            ROW_CHECK(row, "every window submitted and filled all N real commands",
                      host.opens == bars && host.closes == bars && host.fills == bars * n);
            ROW_CHECK(row, "odd windows closed each even window's N fills",
                      host.trade_count() == (bars / 2) * n && host.native_open_lots(kNa).empty());
            peaks.push_back(host.peak);
            ends.push_back(host.births().size());
        }
        // The activation fixture briefly owns the N primary grants AND one
        // cancellable grant; the pre-open fixture owns only its N markets.
        const auto expected_peak = static_cast<std::size_t>(n + (activation ? 1 : 0));
        ROW_CHECK(row, "whole birth-list peak stays fixed as bars grow fourfold",
                  peaks == std::vector<std::size_t>(2, expected_peak));
        ROW_CHECK(row, "completed-Open count stays fixed at zero for N and 4N bars",
                  ends == std::vector<std::size_t>(2, 0));
    }
}
void pre_open_births() { native_births(false); }
void activation_births() { native_births(true); }

struct Row {
    const char* id;
    void (*run)();
    const char* listed;
};
const Row rows[] = {
    {"pineexecutionadapter_admission_journal", admission_journal, nullptr},
    {"pineexecutionadapter_exit_scratch", exit_scratch, nullptr},
    {"retiredrowscratch_source_bars", source_bars, nullptr},
    {"retiredrowscratch_candidate_names", candidate_names, nullptr},
    {"droppedclosereceipt_source_id", nullptr, "source_id belongs only to the braced DroppedCloseReceipt temporary folded by record_dropped_close"},
    {"droppedclosereceipt_comment", nullptr, "comment belongs only to that full-expression DroppedCloseReceipt temporary"},
    {"placementaudit_tombstones", nullptr, "PlacementAudit is a .cpp anonymous-namespace class; tombstones has no test-TU member name"},
    {"placementaudit_view", nullptr, "PlacementAudit is a .cpp anonymous-namespace class; view has no test-TU member name"},
    {"adapterlookupindex_immediate_closes_by_bar", nullptr, "AdapterLookupIndex is a .cpp anonymous-namespace class; immediate_closes_by_bar has no test-TU member name"},
    {"placementtable_erase_node", nullptr, "PlacementTable::erase::node is a function-local unique_ptr, transferred to spare storage or audit storage before return"},
    {"droppedclosereceipt_transient_command_result", nullptr, "the complete DroppedCloseReceipt is an unnamed full-expression temporary, not retained storage"},
    {"relative_leg_shapes_shapes", relative_shapes, nullptr},
    {"nativeexecutionconsumer_pre_open_births", pre_open_births, nullptr},
    {"nativeexecutionconsumer_activation_births", activation_births, nullptr},
    {"workingrequestcore_validate_request_canonical", nullptr, "WorkingRequestCore::validate_request::canonical is function-local"},
    {"pineexecutionadapter_order_replaced_close_all", nullptr, "PineExecutionAdapter::order::replaced_close_all is function-local on the __close__ branch"},
    {"pineexecutionadapter_order_same_bar_defaults", nullptr, "PineExecutionAdapter::order::same_bar_defaults is function-local on the Market branch"},
    {"pineexecutionadapter_defer_declined_reversal_exits_at_adverse_dependent_closes", nullptr, "defer_declined_reversal_exits_at_adverse::dependent_closes is function-local"},
    {"pineexecutionadapter_record_market_review_selected", nullptr, "record_market_review::selected is function-local"},
    {"pineexecutionadapter_apply_open_market_admission_market", nullptr, "apply_open_market_admission::market is function-local"},
    {"pineexecutionadapter_apply_open_market_admission_cancellations", nullptr, "apply_open_market_admission::cancellations is function-local; cancel_later captures it locally"},
    {"pineexecutionadapter_apply_open_market_admission_review_handles", nullptr, "apply_open_market_admission::review_handles is function-local"},
    {"pineexecutionadapter_apply_terminal_explicit_market_policy_candidates", nullptr, "apply_terminal_explicit_market_policy::candidates is function-local"},
    {"pineexecutionadapter_apply_terminal_explicit_market_policy_seen", nullptr, "apply_terminal_explicit_market_policy::seen is function-local in an audit-only block"},
};
} // namespace

int main() {
    // Every row is dispatched even when an earlier row's assertion failed.
    // Unsupported observations contribute no passing checks.
    for (const auto& row : rows) {
        if (!row.run) {
            continue;
        }
        try {
            row.run();
        } catch (const std::exception& error) {
            check(row.id, error.what(), "unexpected exception in this row", false, __LINE__);
        } catch (...) {
            check(row.id, "unknown exception", "unexpected exception in this row", false, __LINE__);
        }
    }
    std::printf("memory proof command_scratch: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
