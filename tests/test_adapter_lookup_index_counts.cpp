// Counts of real adapter lookup populations, before and after release.
#include "../src/source/pine_adapter_probe.hpp"
#include "../src/native_execution_consumer.hpp"

#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr double kParentUnits = 100.0;
constexpr double kExplicitLegUnits = 1.0;
constexpr std::int64_t kStart = 1736121600000LL;
// Mirrors the adapter's kIndexedFrom: at or below this roster size, lookup
// walks the roster instead of using the index.
constexpr std::size_t kIndexedFrom = 16;

int checks = 0;
int failures = 0;

void check(bool condition, const char* message, const char* phase, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL %s:%d [%s] %s\n", __FILE__, line, phase, message);
    }
}

void equal_count(std::size_t actual, std::size_t expected, const char* message,
                 const char* phase, int line, std::size_t kind = 0) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::fprintf(stderr, "FAIL %s:%d [%s] %s slot=%zu: actual=%zu expected=%zu\n",
                     __FILE__, line, phase, message, kind, actual, expected);
    }
}

// The same access pattern as test_origin_roster_retirement. These tags only
// call real lookup methods or find a cohort to query; they never write state.
template<class Tag, auto Member>
struct Access { friend auto access(Tag) { return Member; } };
struct CohortsTag { friend auto access(CohortsTag); };
template struct Access<CohortsTag, &source::PineExecutionAdapter::cohorts_by_id_>;
struct SidesTag { friend auto access(SidesTag); };
template struct Access<SidesTag, &source::PineExecutionAdapter::cohort_opened_on_side>;
struct ImmediateTag { friend auto access(ImmediateTag); };
template struct Access<ImmediateTag, &source::PineExecutionAdapter::immediate_close_placed_on>;
struct PlacementTag { friend auto access(PlacementTag); };
template struct Access<PlacementTag, &source::PineExecutionAdapter::placement_>;
struct CycleTag { friend auto access(CycleTag); };
template struct Access<CycleTag, &source::PineExecutionAdapter::current_position_cycle_>;

// Independent command ledger. No placement table, handle, probe result or
// engine count enters this model. A leg is one issued single-kind exit call;
// its parent name is sufficient to count distinct origins in these streams
// because each named parent in the leg streams opens exactly once.
struct Model {
    struct Cohort {
        std::string name;
        std::size_t issued = 0;
        bool queried = false;
        bool retained = true;
    };
    struct Leg {
        std::string parent;
        std::size_t kind;
        bool bound;
    };
    struct Counts {
        std::size_t cohorts = 0;
        std::size_t waiting = 0;
        std::array<std::size_t, 3> keys{};
        std::array<std::size_t, 3> ids{};
        std::size_t close_keys = 0;
        std::size_t close_ids = 0;
    };
    std::vector<Cohort> cohorts;
    std::vector<Leg> legs;
    std::vector<int> close_bars;

    Counts counts() const {
        Counts result;
        for (const auto& cohort : cohorts)
            result.cohorts += cohort.queried && cohort.retained && cohort.issued > kIndexedFrom;
        std::array<std::set<std::string>, 3> origins;
        for (const auto& leg : legs) {
            if (!leg.bound) {
                ++result.waiting;
            } else {
                origins[leg.kind].insert(leg.parent);
                ++result.ids[leg.kind];
            }
        }
        for (std::size_t kind = 0; kind < origins.size(); ++kind)
            result.keys[kind] = origins[kind].size();
        const std::set<int> bars(close_bars.begin(), close_bars.end());
        result.close_keys = bars.size();
        result.close_ids = close_bars.size();
        return result;
    }

    void bind(const std::string& parent) {
        for (auto& leg : legs) if (leg.parent == parent) leg.bound = true;
    }
    void retire(const std::string& parent) {
        legs.erase(std::remove_if(legs.begin(), legs.end(), [&](const Leg& leg) {
            return leg.parent == parent;
        }), legs.end());
    }
    void begin_bar(int bar) {
        close_bars.erase(std::remove_if(close_bars.begin(), close_bars.end(),
            [bar](int placed) { return placed < bar; }), close_bars.end());
    }
};

void compare_counts(const source::detail::LookupIndexCounts& actual,
                    const Model& model, const char* phase) {
    const auto expected = model.counts();
    check(actual.present, "present == true", phase, __LINE__);
    equal_count(actual.cohort_sides, expected.cohorts,
                "cohort_sides exact", phase, __LINE__);
    equal_count(actual.unbound, expected.waiting,
                "unbound exact (waiting or filed on bind)", phase, __LINE__);
    for (std::size_t kind = 0; kind < expected.keys.size(); ++kind) {
        equal_count(actual.legs_by_origin_keys[kind], expected.keys[kind],
                    "legs_by_origin keys exact", phase, __LINE__, kind);
        equal_count(actual.legs_by_origin_ids[kind], expected.ids[kind],
                    "legs_by_origin id sum exact", phase, __LINE__, kind);
    }
    equal_count(actual.immediate_closes_keys, expected.close_keys,
                "immediate_closes_by_bar keys exact", phase, __LINE__);
    equal_count(actual.immediate_closes_ids, expected.close_ids,
                "immediate_closes_by_bar id sum exact", phase, __LINE__);
}

source::PineStrategyConfig config(int pyramiding, bool on_close, bool any = false) {
    source::PineStrategyConfig value;
    value.initial_capital = 1000000.0;
    value.default_qty_type = static_cast<int>(QtyType::FIXED);
    value.default_qty_value = 1.0;
    value.pyramiding = pyramiding;
    value.process_orders_on_close = on_close;
    value.close_entries_rule_any = any;
    value.commission_value = 0.0;
    value.margin_long = 100.0;
    value.margin_short = 100.0;
    return value;
}

class ProbeHost : public source::PineStrategyHost {
public:
    explicit ProbeHost(const source::PineStrategyConfig& settings) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        configure_pine_strategy(settings);
        NativeExecutionConsumer::bound(*this).set_host_cache(true);
    }

protected:
    Model model_;

    void padding(int count) {
        // Working raw limits keep the real placement table above kIndexedFrom, so
        // immediate_close_placed_on takes its indexed path even after release.
        for (int i = 0; i < count; ++i)
            strategy_order("padding-" + std::to_string(i), true, 1.0, 25.0);
    }

    static std::string leg_id(const std::string& parent, std::size_t kind, int ordinal) {
        return parent + "-leg-" + std::to_string(kind) + "-" + std::to_string(ordinal);
    }

    void place_legs(const std::string& parent, int base, bool bound) {
        // Limit, stop, trail respectively. Different multiplicities make the
        // three id sums distinguishable: base, base+1 and base+2 per parent.
        for (std::size_t kind = 0; kind < 3; ++kind) {
            for (int i = 0; i < base + static_cast<int>(kind); ++i) {
                const auto id = leg_id(parent, kind, i);
                strategy_exit(id, parent, kind == 0 ? 250.0 : kNa,
                              kind == 1 ? 50.0 : kNa, kNa,
                              kind == 2 ? 4.0 : kNa, kind == 2 ? 300.0 : kNa,
                              100.0, {}, kExplicitLegUnits, id);
                model_.legs.push_back({parent, kind, bound});
            }
        }
        adapter_.flush_pending_entries();
        adapter_.flush_pending_bracket_legs();
    }

    void cancel_legs(const std::string& parent, int base) {
        for (std::size_t kind = 0; kind < 3; ++kind)
            for (int i = 0; i < base + static_cast<int>(kind); ++i)
                strategy_exit_cancel_bracket(leg_id(parent, kind, i), parent);
    }

    void read_counts(const char* phase, bool row_query = true) {
        if (row_query) {
            const int bar = pine_bar_index();
            const bool expected_close = std::find(model_.close_bars.begin(),
                model_.close_bars.end(), bar) != model_.close_bars.end();
            const bool actual_close = (adapter_.*access(ImmediateTag{}))(bar);
            check(actual_close == expected_close, "real same-bar lookup answer", phase, __LINE__);
        }
        // Read point: inside this run's source callback, after the real lookup
        // above (or the cohort queries below). No next run begin or index clear
        // has occurred; the consumer still owns this run's populated index.
        const auto* cache = NativeExecutionConsumer::bound(*this).host_cache();
        compare_counts(source::detail::probe_lookup_index(cache), model_, phase);
    }
};

class CohortHost final : public ProbeHost {
public:
    CohortHost(int groups, int origins)
        : ProbeHost(config(groups * (origins + groups), true)) {
        for (int i = 0; i < groups; ++i) {
            model_.cohorts.push_back({"cohort-" + std::to_string(i)});
            plan_.push_back(origins + i);
            total_ += origins + i;
        }
    }
    int bars() const { return total_ + 7; }
    bool witnessed() const { return populated_ && retired_; }

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        if (bar < total_) {
            int offset = bar;
            for (std::size_t i = 0; i < plan_.size(); ++i) {
                if (offset < plan_[i]) {
                    strategy_entry(model_.cohorts[i].name, true);
                    ++model_.cohorts[i].issued;
                    break;
                }
                offset -= plan_[i];
            }
        }
        if (bar == total_) {
            check(live_position_size() == static_cast<double>(total_),
                  "all planned cohort openings filled", "cohort populated", __LINE__);
            const auto& cohorts = adapter_.*access(CohortsTag{});
            for (auto& planned : model_.cohorts) {
                const auto found = cohorts.find(planned.name);
                check(found != cohorts.end(), "planned cohort exists", "cohort populated", __LINE__);
                if (found != cohorts.end()) {
                    check((adapter_.*access(SidesTag{}))(found->second, true),
                          "real cohort side lookup sees long opening", "cohort populated", __LINE__);
                }
                planned.queried = true;
            }
            check(model_.counts().cohorts > 0, "nonzero independent population",
                  "cohort populated", __LINE__);
            read_counts("cohort populated", false);
            populated_ = true;
            strategy_close_all();
        }
        if (bar == total_ + 5) {
            check(live_position_size() == 0.0, "cohort lots all closed", "cohort retired", __LINE__);
            for (auto& cohort : model_.cohorts) cohort.retained = false;
            // Five later bar opens have released the rows and origin prefixes.
            // Do not start a new run: the same cache must lose its old entries.
            read_counts("cohort retired", false);
            retired_ = true;
        }
    }

private:
    std::vector<int> plan_;
    int total_ = 0;
    bool populated_ = false;
    bool retired_ = false;
};

class BindingHost final : public ProbeHost {
public:
    explicit BindingHost(int base) : ProbeHost(config(1, false)), base_(base) {}
    int bars() const { return 11; }
    bool witnessed() const { return waiting_ && bound_ && retired_ && released_; }

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        if (bar == 0) padding(17 + base_);
        if (bar == 1) {
            // With pyramiding=1 and no costs, this market entry waits in the
            // same-bar batch. Queue explicit children with origin zero, then
            // submit parent and children without matching; the fill is next bar.
            strategy_entry("waiting-parent", true, kNa, kNa, kParentUnits);
            place_legs("waiting-parent", base_, false);
            check(model_.counts().waiting > 0, "nonzero independent waiting population",
                  "unbound waiting", __LINE__);
            read_counts("unbound waiting");
            waiting_ = true;
        }
        if (bar == 2) {
            check(live_position_size() == kParentUnits, "waiting parent filled",
                  "unbound filed on bind", __LINE__);
            lifecycle_.parent_filled = true;
            lifecycle_.parent_fill_cycle = ++lifecycle_.swept_cycle;
            check((adapter_.*access(CycleTag{})) == lifecycle_.parent_fill_cycle,
                  "parent fill advanced the cycle once", "unbound filed on bind", __LINE__);
            model_.bind("waiting-parent");
            read_counts("unbound filed on bind");
            bound_ = true;
            cancel_legs("waiting-parent", base_);
            strategy_close_all();
        }
        if (bar == 7) {
            check(live_position_size() == 0.0, "bound parent closed", "bound rows retired", __LINE__);
            check((adapter_.*access(CycleTag{})) == lifecycle_.swept_cycle,
                  "flat did not advance the cycle", "bound rows retired", __LINE__);
            // At the parent fill the delay block (gated by partial_prearmed_parent)
            // moves the priced children to the delayed queue. They are still queued
            // after exit_cancel_bracket and release_delayed_orders submits them at
            // the next bar open (bar 3).
            // submit_or_replace stamps the opened cycle; the K1 clause in
            // sweep_retired_rows retains their new rows at flat. The original
            // trails still belong to the pre-opening cycle.
            read_retained_binding_counts("bound rows retired");
            retired_ = true;
        }
        if (bar == 8)
            strategy_entry("binding-next-parent", true, kNa, kNa, kParentUnits);
        if (bar == 10) {
            // No exits for the new parent. With orders-on-close disabled, its
            // bar-9 opening follows that bar's sweep; bar 10 is the first
            // erase_retired_rows sweep after the position cycle advances.
            ++lifecycle_.swept_cycle;
            check(live_position_size() == kParentUnits, "fresh parent filled from flat",
                  "binding rows released after cycle", __LINE__);
            check((adapter_.*access(CycleTag{})) == lifecycle_.swept_cycle,
                  "fresh opening advanced the cycle once",
                  "binding rows released after cycle", __LINE__);
            read_retained_binding_counts("binding rows released after cycle");
            released_ = true;
        }
    }

private:
    struct BindingLifecycle {
        std::int64_t parent_fill_cycle = 0;
        std::int64_t swept_cycle = 0;
        bool parent_filled = false;
    };

    static source::PineOrderFamily leg_family(std::size_t kind) {
        constexpr std::array<source::PineOrderFamily, 3> families{
            source::PineOrderFamily::ExitLimit, source::PineOrderFamily::ExitStop,
            source::PineOrderFamily::ExitTrail};
        return families[kind];
    }

    // Models uniform child quantities for these same-bar prearmed commands,
    // with no child fills, script withdrawal or superseding exit calls.
    // The engine decides per parent: any same-bar finite limit/stop child
    // below the opened units sets partial_prearmed_parent, and the delay block
    // then moves every same-bar limit/stop child of that parent to the queue. Trailing activation is not
    // delayed. The uniform quantities here let each child use the same test,
    // independent of child multiplicity or observed rows.
    static bool retained_prearmed_leg(source::PineOrderFamily family,
                                     double requested_units, double parent_units,
                                     const BindingLifecycle& state) {
        const bool fixed_price_leg = family == source::PineOrderFamily::ExitLimit
            || family == source::PineOrderFamily::ExitStop;
        const bool delayed_at_fill = state.parent_filled && fixed_price_leg
            && std::isfinite(requested_units) && requested_units < parent_units;
        // These commands start in cycle zero. submit_or_replace stamps the
        // fill-delayed copies with the parent-fill cycle; the original trails
        // keep cycle zero. The copies remain queued after exit_cancel_bracket
        // and release_delayed_orders submits them at the next bar open.
        const auto placement_cycle = delayed_at_fill ? state.parent_fill_cycle : 0;
        // At both read points the old parent is flat, with no remaining roots
        // or supersessors. The K1 cycle clause in sweep_retired_rows decides
        // retention; its K2/K3 clauses cannot hold an ended cycle.
        return placement_cycle >= state.swept_cycle;
    }

    void read_retained_binding_counts(const char* phase) {
        // Filter only the independently issued ledger. Each retained child
        // contributes one id; Model::counts obtains keys from distinct parent
        // names, so both expectations follow from the issued commands.
        model_.legs.erase(std::remove_if(model_.legs.begin(), model_.legs.end(),
            [&](const Model::Leg& leg) {
                return !retained_prearmed_leg(leg_family(leg.kind),
                    kExplicitLegUnits, kParentUnits, lifecycle_);
            }), model_.legs.end());
        read_counts(phase);
        std::array<std::size_t, 3> table_rows{};
        for (const auto& row : adapter_.*access(PlacementTag{})) {
            if (row.second.from_entry != "waiting-parent") continue;
            for (std::size_t kind = 0; kind < table_rows.size(); ++kind)
                table_rows[kind] += row.second.family == leg_family(kind);
        }
        const auto expected = model_.counts();
        for (std::size_t kind = 0; kind < table_rows.size(); ++kind)
            equal_count(table_rows[kind], expected.ids[kind],
                        "binding retained table rows exact", phase, __LINE__, kind);
    }

    int base_;
    BindingLifecycle lifecycle_;
    bool waiting_ = false;
    bool bound_ = false;
    bool retired_ = false;
    bool released_ = false;
};

class LegsHost final : public ProbeHost {
public:
    LegsHost(int parents, int base)
        : ProbeHost(config(parents + 1, true, true)), parents_(parents), base_(base) {}
    int bars() const { return parents_ + 12; }
    bool witnessed() const { return populated_ && refolded_ && flat_ && released_; }

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        if (bar == 0) padding(17 + parents_ + base_);
        if (bar > 0 && bar <= parents_)
            strategy_entry(parent(bar - 1), true, kNa, kNa, kParentUnits);
        if (bar == parents_ + 1) {
            check(live_position_size() == parents_ * kParentUnits,
                  "each distinct parent filled once", "legs populated", __LINE__);
            for (int i = 0; i < parents_; ++i) place_legs(parent(i), base_, true);
            const auto expected = model_.counts();
            for (std::size_t kind = 0; kind < expected.keys.size(); ++kind)
                check(expected.keys[kind] > 0 && expected.ids[kind] > expected.keys[kind],
                      "nonzero keys and multiple ids per origin", "legs populated", __LINE__);
            // The finite-quantity path in exit binds every issued leg to its
            // opened origin. Changing an explicit level retains one row per
            // live leg rather than one per historical reissue.
            read_counts("legs populated");
            cycle_ = adapter_.*access(CycleTag{});
            populated_ = true;
            // Close alternating named parents under close_entries_rule=ANY;
            // the other parents and their original legs remain live.
            for (int i = 0; i < parents_; i += 2) {
                cancel_legs(parent(i), base_);
                strategy_close(parent(i), {}, kParentUnits);
            }
        }
        if (bar == parents_ + 7) {
            const int survivors = parents_ / 2;
            check(live_position_size() == survivors * kParentUnits,
                  "only the planned surviving parents hold units", "legs refolded", __LINE__);
            check((adapter_.*access(CycleTag{})) == cycle_,
                  "partial close did not advance the position cycle", "legs refolded", __LINE__);
            const auto expected = model_.counts();
            for (std::size_t kind = 0; kind < expected.keys.size(); ++kind)
                check(expected.keys[kind] > 0 && expected.ids[kind] > 0,
                      "nonzero retained population", "legs refolded", __LINE__);
            // Explicit exits with all levels unset retain retired rows until
            // the next cycle. This direct bracket cancel follows the same
            // exit_cancel_bracket route as an exit call with all levels missing.
            // The K1 cycle clause in sweep_retired_rows applies to all three
            // exit families: every leg here was placed after its parent filled,
            // in cycle_.
            // Thus each kind still has parents_ keys and
            // parents_ * (base_ + kind) ids, including the closed parents.
            read_counts("legs refolded");
            refolded_ = true;
        }
        if (bar == parents_ + 8) strategy_close_all();
        if (bar == parents_ + 9) {
            check(live_position_size() == 0.0, "remaining parents reached flat",
                  "legs flat before new cycle", __LINE__);
            check((adapter_.*access(CycleTag{})) == cycle_,
                  "flat alone did not advance the position cycle",
                  "legs flat before new cycle", __LINE__);
            // Current-cycle rows remain held at flat. retire_in_position_exits_at_flat
            // retires the closed parents' live exits with lifecycle intact.
            read_counts("legs flat before new cycle");
            flat_ = true;
        }
        if (bar == parents_ + 10)
            strategy_entry("legs-next-cycle-parent", true, kNa, kNa, kParentUnits);
        if (bar == parents_ + 11) {
            check(live_position_size() == kParentUnits, "fresh parent filled from flat",
                  "legs released after cycle", __LINE__);
            check((adapter_.*access(CycleTag{})) == cycle_ + 1,
                  "fresh opening advanced the position cycle once",
                  "legs released after cycle", __LINE__);
            for (int i = 0; i < parents_; ++i) model_.retire(parent(i));
            // After a position cycle change, no old origin is askable and no
            // exit is issued anew. In sweep_retired_rows, K1 fails and K2/K3
            // require the current cycle, leaving zero rows and ids.
            read_counts("legs released after cycle");
            released_ = true;
        }
    }

private:
    static std::string parent(int i) { return "bound-parent-" + std::to_string(i); }
    int parents_;
    int base_;
    std::int64_t cycle_ = 0;
    bool populated_ = false;
    bool refolded_ = false;
    bool flat_ = false;
    bool released_ = false;
};

enum class ExitShape { AllMissing, OneMissing, ChangedLevel, ScriptCancel };
enum class QuantityKind { Explicit, Default };

// Additional retention streams use one parent and one exit.
// Expectations come only from the issued commands.
class LegsRetentionHost final : public ProbeHost {
public:
    LegsRetentionHost(ExitShape shape, QuantityKind quantity, bool moving_stop, int length)
        : ProbeHost(config(2, true, true)), shape_(shape), quantity_(quantity),
          moving_stop_(moving_stop), length_(length) {}
    int bars() const { return measurement_bar() + 4; }
    bool witnessed() const {
        return observed_ && flat_ && released_
            && reads_ == static_cast<std::size_t>(length_) + 1;
    }
    std::string description() const {
        const char* shape = shape_ == ExitShape::AllMissing ? "all levels unset between value calls"
            : shape_ == ExitShape::OneMissing ? "one level toggled"
            : shape_ == ExitShape::ChangedLevel ? "level changed" : "strategy.cancel";
        return std::string(shape)
            + (quantity_ == QuantityKind::Explicit ? " explicit" : " default")
            + (shape_ == ExitShape::OneMissing
                ? (moving_stop_ ? " moving-stop" : " constant-stop") : "")
            + " L=" + std::to_string(length_);
    }

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        const std::string phase = description() + " callback=" + std::to_string(bar);
        if (bar == 0) padding(17);
        if (bar == 1) {
            strategy_entry("retention-parent", true, kNa, kNa, kParentUnits);
            ++openings_;
        }
        if (bar == kFirstShapeBar) {
            origin_ = opening("retention-parent", phase.c_str());
            cycle_ = adapter_.*access(CycleTag{});
            check(origin_ != 0 && cycle_ > 0, "parent opened before the first exit call",
                  phase.c_str(), __LINE__);
        }
        if (bar >= kFirstShapeBar && bar <= measurement_bar()) {
            check(live_position_size() == kParentUnits, "position stays fully open",
                  phase.c_str(), __LINE__);
            check(opening("retention-parent", phase.c_str()) == origin_,
                  "same origin throughout the exit stream", phase.c_str(), __LINE__);
            check((adapter_.*access(CycleTag{})) == cycle_, "same position cycle",
                  phase.c_str(), __LINE__);
            // Read after the previous command's next bar-open sweep, before
            // sending any new exit, close or entry command.
            if (bar > kFirstShapeBar) observe(false, phase.c_str());
            if (bar < measurement_bar()) {
                drive(bar - kFirstShapeBar);
            } else {
                equal_count(commands_, static_cast<std::size_t>(length_),
                            "all driving commands issued", phase.c_str(), __LINE__);
                equal_count(values_ + transitions_ + cancels_, commands_,
                            "values, missing transitions and cancels account for commands",
                            phase.c_str(), __LINE__);
                check(values_ > 1, "repeated value calls reached the intended exit branch",
                      phase.c_str(), __LINE__);
                observed_ = true;
                strategy_close_all();
            }
        }
        if (bar == measurement_bar() + 1) {
            check(live_position_size() == 0.0, "full close reached flat",
                  phase.c_str(), __LINE__);
            check((adapter_.*access(CycleTag{})) == cycle_, "flat did not advance cycle",
                  phase.c_str(), __LINE__);
            check_old_origin_closed(phase.c_str());
            flat_ = true;
        }
        if (bar == measurement_bar() + 2) {
            check(live_position_size() == 0.0, "fresh entry issued from flat",
                  phase.c_str(), __LINE__);
            strategy_entry("retention-next-parent", true, kNa, kNa, kParentUnits);
        }
        if (bar == measurement_bar() + 3) {
            check(live_position_size() == kParentUnits, "fresh entry filled",
                  phase.c_str(), __LINE__);
            check((adapter_.*access(CycleTag{})) == cycle_ + 1,
                  "fresh opening advanced the cycle once", phase.c_str(), __LINE__);
            const auto next = opening("retention-next-parent", phase.c_str());
            check(next != 0 && next != origin_, "fresh opening has a distinct origin",
                  phase.c_str(), __LINE__);
            check_old_origin_closed(phase.c_str());
            observe(true, phase.c_str());
            released_ = true;
        }
    }

private:
    static constexpr int kFirstShapeBar = 2;
    int measurement_bar() const { return kFirstShapeBar + length_; }

    std::uint64_t opening(const char* name, const char* phase) const {
        const auto& cohorts = adapter_.*access(CohortsTag{});
        const auto found = cohorts.find(name);
        const bool single = found != cohorts.end() && found->second.opened.size() == 1;
        check(single, "named parent has exactly one opened origin", phase, __LINE__);
        return single ? found->second.opened.front().incarnation : 0;
    }
    void check_old_origin_closed(const char* phase) const {
        const auto& cohorts = adapter_.*access(CohortsTag{});
        const auto found = cohorts.find("retention-parent");
        check(found == cohorts.end() || found->second.opened.empty(),
              "old origin is no longer opened", phase, __LINE__);
    }

    void drive(int position_bar) {
        const bool value_bar = position_bar % 2 == 0;
        double limit = kNa;
        double stop = kNa;
        const bool cancel = shape_ == ExitShape::ScriptCancel && !value_bar;
        if (shape_ == ExitShape::OneMissing) {
            limit = 250.0; // The finite sibling selects the one-level route.
            if (value_bar)
                stop = moving_stop_ ? 50.0 - 0.25 * position_bar : 50.0;
        } else if (shape_ == ExitShape::ChangedLevel) {
            limit = 250.0 + position_bar; // Replace, with no missing level transition.
        } else if (value_bar) {
            limit = 250.0;
        }
        if (cancel) {
            strategy_cancel("retention-exit"); // Script cancel, not bracket cancel.
            ++cancels_;
            has_value_ = false;
        } else {
            // In exit, finite qty follows the opened-origin path; qty=na
            // follows the dynamic origin-zero path.
            const double qty = quantity_ == QuantityKind::Explicit ? 1.0 : kNa;
            strategy_exit("retention-exit", "retention-parent", limit, stop,
                          kNa, kNa, kNa, 100.0, {}, qty, "retention-exit");
            const bool has_value = shape_ == ExitShape::OneMissing ? value_bar
                : shape_ == ExitShape::ChangedLevel || value_bar;
            if (has_value) {
                if (shape_ == ExitShape::OneMissing && quantity_ == QuantityKind::Default) {
                    // For default qty with a moving stop, a later stop
                    // releases older differing stops through the
                    // release_superseded clause in sweep_retired_rows.
                    for (auto prior = retired_stops_.begin(); prior != retired_stops_.end();) {
                        if (*prior != stop) {
                            ++superseded_;
                            prior = retired_stops_.erase(prior);
                        } else {
                            ++prior;
                        }
                    }
                }
                ++values_;
                last_stop_ = stop;
            } else if (has_value_) {
                ++transitions_;
                if (shape_ == ExitShape::OneMissing) retired_stops_.push_back(last_stop_);
            }
            has_value_ = has_value;
        }
        ++commands_;
        adapter_.flush_pending_entries();
        adapter_.flush_pending_bracket_legs();
    }

    std::array<std::size_t, 3> expected_rows(bool after_cycle) const {
        std::array<std::size_t, 3> rows{};
        // After a position cycle change, sweep_retired_rows retains no exit
        // rows: K1 loses the old cycle/origin; K2/K3 require the current cycle.
        // No exit was issued for the new origin.
        if (after_cycle) return rows;
        const auto first = values_ != 0 ? openings_ : 0;
        const auto current = has_value_ ? openings_ : 0;
        switch (shape_) {
        case ExitShape::AllMissing:
            // With all levels unset between value calls, both qty kinds keep
            // one row per completed value-to-na transition, plus a working
            // value. exit_cancel_bracket removes the family, permitting a
            // fresh row through origin_leg_consumed; the K1 clause in
            // sweep_retired_rows keeps its intact lifecycle.
            rows[0] = transitions_ * openings_ + current;
            break;
        case ExitShape::OneMissing:
            // With one level toggled and explicit qty, constant and moving
            // stops keep one limit plus the first stop. The retained family
            // makes exit's consumed_origin_leg skip apply, and the K1 clause
            // in sweep_retired_rows keeps the row. Default qty keeps one limit
            // plus transitions less differing-stop releases plus any current
            // stop, through release_superseded. Constant stops release none.
            rows[0] = first;
            rows[1] = quantity_ == QuantityKind::Explicit ? first
                : (transitions_ - superseded_) * openings_ + current;
            break;
        case ExitShape::ChangedLevel:
            // With a changed level, both qty kinds keep one limit row: the
            // replacement clause in submit_or_replace carries the lifecycle
            // into the successor and leaves its predecessor empty.
            rows[0] = first;
            break;
        case ExitShape::ScriptCancel:
            // After strategy.cancel, explicit qty retains its first withdrawn
            // row via the askable-origin K1 clause in sweep_retired_rows and
            // skips later value calls through exit's consumed_origin_leg
            // clause; default qty releases the row after cancel.
            rows[0] = quantity_ == QuantityKind::Explicit ? first : current;
            break;
        }
        // No stream issues a trailing leg: its exact expected row count is 0.
        return rows;
    }

    void observe(bool after_cycle, const char* phase) {
        check(!(adapter_.*access(ImmediateTag{}))(pine_bar_index()),
              "real lookup sees no immediate close", phase, __LINE__);
        const auto* cache = NativeExecutionConsumer::bound(*this).host_cache();
        const auto actual = source::detail::probe_lookup_index(cache);
        check(actual.present, "probe present", phase, __LINE__);
        std::array<std::size_t, 3> table{};
        const auto& placement = adapter_.*access(PlacementTag{});
        for (const auto& row : placement) {
            const auto family = row.second.family;
            if (family == source::PineOrderFamily::ExitLimit) ++table[0];
            else if (family == source::PineOrderFamily::ExitStop) ++table[1];
            else if (family == source::PineOrderFamily::ExitTrail) ++table[2];
        }
        const auto expected = expected_rows(after_cycle);
        for (std::size_t kind = 0; kind < expected.size(); ++kind) {
            // The derivation for each shape is beside expected_rows above.
            // Check the table too: zero default-qty
            // index ids alone cannot distinguish retained and released rows.
            equal_count(table[kind], expected[kind], "retained table rows exact",
                        phase, __LINE__, kind);
            // Only the explicit-qty path in exit assigns an origin.
            // fold indexes every bound row, working or retired.
            // Thus ids equal the row count for explicit, zero for default;
            // each nonempty kind has one key per issued opening.
            const auto ids = quantity_ == QuantityKind::Explicit ? expected[kind] : 0;
            const auto keys = ids != 0 ? openings_ : 0;
            equal_count(actual.legs_by_origin_ids[kind], ids,
                        "shape id sum exact", phase, __LINE__, kind);
            equal_count(actual.legs_by_origin_keys[kind], keys,
                        "shape keys exact", phase, __LINE__, kind);
        }
        ++reads_;
    }

    ExitShape shape_;
    QuantityKind quantity_;
    bool moving_stop_;
    int length_;
    std::size_t openings_ = 0;
    std::size_t commands_ = 0;
    std::size_t values_ = 0;
    std::size_t transitions_ = 0;
    std::size_t cancels_ = 0;
    std::size_t superseded_ = 0;
    std::size_t reads_ = 0;
    bool has_value_ = false;
    double last_stop_ = kNa;
    std::vector<double> retired_stops_;
    std::uint64_t origin_ = 0;
    std::int64_t cycle_ = 0;
    bool observed_ = false;
    bool flat_ = false;
    bool released_ = false;
};

class ImmediateHost final : public ProbeHost {
public:
    explicit ImmediateHost(int parents)
        : ProbeHost(config(parents + 1, true, true)), parents_(parents) {}
    int bars() const { return parents_ + 8; }
    bool witnessed() const { return first_ && second_ && retired_; }

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        model_.begin_bar(bar);
        if (bar == 0) padding(17 + parents_);
        if (bar > 0 && bar <= parents_)
            strategy_entry("close-parent-" + std::to_string(bar - 1), true, kNa, kNa, kParentUnits);
        if (bar == parents_ + 1 || bar == parents_ + 2) {
            const int calls = parents_ - (bar - parents_ - 1);
            for (int i = 0; i < calls; ++i) {
                strategy_close("close-parent-" + std::to_string(i), {}, 1.0, kNa, true);
                model_.close_bars.push_back(bar);
                ++closed_units_;
            }
            check(live_position_size() == parents_ * kParentUnits - closed_units_,
                  "each immediate close moved one unit", "immediate closes current bar", __LINE__);
            const auto expected = model_.counts();
            check(expected.close_keys > 0 && expected.close_ids > expected.close_keys,
                  "nonzero placement bar and multiple closes", "immediate closes current bar", __LINE__);
            read_counts(bar == parents_ + 1 ? "immediate first bar" : "immediate next bar");
            if (bar == parents_ + 1) first_ = true;
            else second_ = true;
        }
        if (bar == parents_ + 6) {
            read_counts("immediate rows retired");
            retired_ = true;
        }
    }

private:
    int parents_;
    int closed_units_ = 0;
    bool first_ = false;
    bool second_ = false;
    bool retired_ = false;
};

template<class Host>
void run(Host& host, const char* name) {
    std::vector<Bar> bars;
    for (int i = 0; i < host.bars(); ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0, kStart + i * 60000LL});
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    if (!host.last_error().empty())
        std::fprintf(stderr, "%s: %s\n", name, host.last_error().c_str());
    check(host.last_error().empty(), "adapter stream completed without error", name, __LINE__);
    check(host.native_state().kind == NativeLifecycleKind::Completed,
          "native run completed", name, __LINE__);
    check(host.witnessed(), "every populated and retirement read point ran", name, __LINE__);
}

} // namespace

int main() {
    // Two different input populations exercise the same model and all four
    // streams; expected counts follow from the issued commands.
    for (int scale : {2, 3}) {
        CohortHost cohorts(scale, 16 + scale);
        run(cohorts, "cohort sides");
        BindingHost binding(scale);
        run(binding, "unbound legs");
        LegsHost legs(scale + 1, scale);
        run(legs, "legs by origin");
        for (auto quantity : {QuantityKind::Explicit, QuantityKind::Default}) {
            for (auto shape : {ExitShape::AllMissing, ExitShape::OneMissing,
                               ExitShape::ChangedLevel, ExitShape::ScriptCancel}) {
                // Two command-stream lengths exercise the same derivation
                // of expected populations from issued commands.
                LegsRetentionHost retention(shape, quantity, false, 16 * (scale - 1));
                const auto name = retention.description();
                run(retention, name.c_str());
            }
            LegsRetentionHost moving(ExitShape::OneMissing, quantity, true, 16 * (scale - 1));
            const auto name = moving.description();
            run(moving, name.c_str());
        }
        ImmediateHost closes(scale + 1);
        run(closes, "immediate closes");
    }
    std::printf("test_adapter_lookup_index_counts: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
