// CR-150 TREE. Full bodies, no harvested pins. Predicted first red on
// d91c96d47d is the missing DiskResidentIdTable::bind_scratch below; with that
// primitive supplied alone the real adapter arm still hits the refusing tree
// binder. The supervisor records the executed first diagnostic on the lab.
#include <pineforge/detail/disk_id_table.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace tree_ram_fixture {
struct Probe { std::size_t spills = 0, orphan_spills = 0; };
inline Probe* active = nullptr;
inline bool observe_spill(const std::string& id) {
    if (active) {
        ++active->spills;
        if (id == "old") ++active->orphan_spills;
    }
    return true;
}
} // namespace tree_ram_fixture

// As in test_adapter_reversal_storage, observe only the storage predicate
// after the real shared-row pin/spillable checks. No predicate result, row
// layout, access level, ordering, source command or hash fold is replaced.
#define spillable() spillable() && ::tree_ram_fixture::observe_spill(at->second->first)
#include <pineforge/detail/disk_resident_id_table.hpp>
#undef spillable
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/source/pine_adapter.cpp"
#include "../src/source/pine_state_hash.cpp"

namespace tree_ram_fixture {
using namespace pineforge;
namespace no = pineforge::native_order;
using detail::ScratchManifest;
using detail::ScratchResource;
using detail::ScratchSlab;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}
bool same_bits(double left, double right) {
    return std::memcmp(&left, &right, sizeof left) == 0;
}
struct ProbeScope {
    explicit ProbeScope(Probe& probe) { active = &probe; }
    ~ProbeScope() { active = nullptr; }
};
std::shared_ptr<ScratchSlab> owner(bool ram, std::uint64_t rows, std::uint64_t bytes,
                                 std::uint64_t ranks, std::uint64_t rank_bytes) {
    if (!ram) return {};
    ScratchManifest manifest;
    manifest.U = rows; manifest.B_U = bytes;
    manifest.V = ranks; manifest.B_V = rank_bytes;
    return std::make_shared<ScratchSlab>(manifest);
}

std::vector<Bar> flat_tape(int n) {
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0, kStart + i * 60000LL});
    return bars;
}
void compare_trade(const Trade& a, const Trade& b) {
    require(a.entry_time == b.entry_time && a.exit_time == b.exit_time
            && a.entry_bar_index == b.entry_bar_index && a.exit_bar_index == b.exit_bar_index
            && a.entry_id == b.entry_id && a.exit_id == b.exit_id
            && a.entry_comment == b.entry_comment && a.exit_comment == b.exit_comment
            && a.entry_incarnation == b.entry_incarnation && a.is_long == b.is_long
            && a.open_at_end == b.open_at_end && a.exit_from_bracket == b.exit_from_bracket
            && a.close_cause == b.close_cause, "disk/RAM trade identity and metadata");
    require(same_bits(a.entry_price, b.entry_price) && same_bits(a.exit_price, b.exit_price)
            && same_bits(a.qty, b.qty) && same_bits(a.pnl, b.pnl)
            && same_bits(a.pnl_pct, b.pnl_pct) && same_bits(a.commission, b.commission)
            && same_bits(a.max_runup, b.max_runup) && same_bits(a.max_drawdown, b.max_drawdown),
            "disk/RAM trade numeric bits");
}

class TreeHost : public source::PineStrategyHost {
public:
    explicit TreeHost(std::shared_ptr<ScratchSlab> slab, bool recording = true,
                      bool process_on_close = false, int pyramiding = 4)
        : slab_(std::move(slab)) {
        source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = pyramiding;
        config.process_orders_on_close = process_on_close;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 0.5;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        set_broker_state_hash_recording(recording);
    }
    void bind_trees() {
        // Tree-only witness: leave q/e/c and roots on their unchanged disk
        // backends so this unit does not depend on the other two lane binders.
        // This is before this run's first command/id, after the host reset.
        detail::bind_tree_scratch_tables(adapter_, slab_);
        reads.clear(); source_reads.clear();
    }
    void sample() {
        reads.push_back(broker_state_hash());
        source_reads.push_back(source_hash());
    }
    std::uint64_t source_hash() const {
        BrokerStateHashSink sink;
        adapter_.hash_state(sink);
        return sink.h;
    }
    std::optional<std::vector<std::uint32_t>> ranks(const std::vector<std::string>& ids) const {
        return adapter_.global_exit_child_ranks(ids);
    }
    bool pending(const char* id, const char* from) const {
        const auto& view = pending_intent_view();
        for (int i = 0; i < view.size(); ++i) {
            pf_pending_order_v1_t row{};
            require(view.copy_v1(i, &row) == 0, "copy a real pending-intent row");
            if (std::strcmp(row.id, id) == 0 && std::strcmp(row.from_entry, from) == 0) return true;
        }
        return false;
    }
    const std::vector<std::uint64_t>& recorded() const { return broker_state_hashes_; }
    const std::shared_ptr<ScratchSlab>& slab() const { return slab_; }
    std::vector<std::uint64_t> reads, source_reads;
    Probe operations;
private:
    std::shared_ptr<ScratchSlab> slab_;
};

void compare_runs(const TreeHost& disk, const TreeHost& ram) {
    require(disk.last_error().empty(), disk.last_error().c_str());
    require(ram.last_error().empty(), ram.last_error().c_str());
    require(disk.trade_count() > 0 && disk.trade_count() == ram.trade_count(), "real trade populations");
    for (int i = 0; i < disk.trade_count(); ++i) compare_trade(disk.get_trade(i), ram.get_trade(i));
    require(!disk.reads.empty() && disk.reads == ram.reads, "every callback broker hash");
    // hash_state contains BOTH cohort replay leaves and the rank digest/peaks
    // at their production sites, not a test-supplied replacement digest.
    require(disk.source_reads == ram.source_reads && disk.source_hash() == ram.source_hash(),
            "complete adapter fold, including cohort and rank leaves");
    require(disk.recorded() == ram.recorded() && disk.broker_state_hash() == ram.broker_state_hash(),
            "recorded and final broker folds");
}

class MultiHost final : public TreeHost {
public:
    explicit MultiHost(bool ram) : TreeHost(owner(ram, 3, 3, 3, 3), true, true) {}
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) { bind_trees(); rank_at_three.reset(); }
        if (i == 0) strategy_entry("A", true);
        if (i == 2) strategy_entry("Q", true);
        if (i == 4) strategy_entry("B", true);
        if (i == 5) rank_at_three = ranks({"A", "Q", "B"});
        if (i == 6) strategy_exit("global", "", 100.5, 99.5);
        if (i == 10) strategy_entry("A", true);
        if (i == 12) strategy_entry("B", false);
        if (i == 15) strategy_close("B");
        sample();
    }
    std::optional<std::vector<std::uint32_t>> rank_at_three;
};

void multi_entry_and_global_child_order() {
    MultiHost disk(false), ram(true);
    const auto bars = flat_tape(20);
    for (int repeat = 0; repeat < 2; ++repeat) {
        { ProbeScope scope(disk.operations); disk.run(bars.data(), static_cast<int>(bars.size())); }
        { ProbeScope scope(ram.operations); ram.run(bars.data(), static_cast<int>(bars.size())); }
        compare_runs(disk, ram);
        // Java hashes A=65,Q=81,B=66 map to buckets 1,1,2 in 16 buckets.
        // Equal-bucket replay is newest first: Q before A, then B.
        const std::vector<std::uint32_t> expected{1, 0, 2};
        require(disk.rank_at_three && ram.rank_at_three
                && *disk.rank_at_three == expected && *ram.rank_at_three == expected,
                "global_exit_child_ranks keeps bucket order and reverse age within a bucket");
        // Three distinct one-byte ids: Coh=1+96*3+3=292; Rank=1+60*3+3=184.
        require(ram.slab()->region(ScratchResource::cohort).capacity == 292
                && ram.slab()->region(ScratchResource::cohort).logical_extent == 292
                && ram.slab()->region(ScratchResource::exit_rank).capacity == 184
                && ram.slab()->region(ScratchResource::exit_rank).logical_extent == 184,
                "both real adapter tables use their exact independent regions");
        require(ram.operations.spills > 0, "real RAM cohorts spill and restore");
    }
}

int triangle(int index, int period) {
    const int phase = index % (2 * period);
    return phase < period ? phase : 2 * period - phase;
}
double reversal_price(int index) {
    return 0.25 * (400 + 2 * triangle(index, 9)
                   + 3 * triangle(index, 23) - triangle(index, 4));
}
std::vector<Bar> reversal_tape() {
    std::vector<Bar> bars;
    for (int i = 0; i < 96; ++i) {
        const double open = reversal_price(i), close = reversal_price(i + 1);
        bars.push_back({open, std::max(open, close) + 0.5, std::min(open, close) - 0.5,
                        close, 1.0, kStart + i * 60000LL});
    }
    return bars;
}
class ReversalHost final : public TreeHost {
public:
    // The original witness has five possible cohort/rank ids: L,S,L2,B,o,
    // six aggregate bytes. Its finite regions are Coh=487 and Rank=307.
    ReversalHost(bool ram, bool recording) : TreeHost(owner(ram, 5, 6, 5, 6), recording, false, 2) {}
    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        if (i == 0) { bind_trees(); reversals = 0; last_side_ = 0; }
        const double position = live_position_size();
        if (position != 0.0) {
            const int side = position > 0.0 ? 1 : -1;
            if (last_side_ != 0 && side != last_side_) ++reversals;
            last_side_ = side;
        }
        // The exact L/S/L2/B/orders/brackets/close witness from
        // test_adapter_reversal_storage.cpp:143-185, now disk versus slab.
        if (i % 6 == 1) strategy_entry("L", true);
        if (i % 6 == 4) strategy_entry("S", false);
        if (i % 12 == 2 && position > 0.0) strategy_entry("L2", true);
        if (i % 18 == 9) strategy_entry("B", true);
        if (i % 18 == 15) strategy_entry("B", false);
        if (position > 0.0) strategy_exit("xl", "L", bar.close + 2.0, bar.close - 2.0);
        if (position < 0.0)
            strategy_exit("xs", "S", bar.close - 2.0, bar.close + 2.0,
                          kNa, kNa, kNa, 100.0, {}, 1.0);
        if (i % 10 == 7) strategy_close("L", {}, 1.0);
        if (i % 14 == 11) strategy_order("o", true, 1.0);
        if (i % 14 == 12) strategy_order("o", false, 1.0);
        sample();
    }
    std::size_t reversals = 0;
private:
    int last_side_ = 0;
};

void reversal_witness_on_the_slab() {
    const auto bars = reversal_tape();
    for (const bool magnifier : {false, true}) {
        for (const bool recording : {false, true}) {
            ReversalHost disk(false, recording), ram(true, recording);
            { ProbeScope scope(disk.operations);
              disk.run(bars.data(), static_cast<int>(bars.size()), "", "", magnifier, 4,
                       MagnifierDistribution::ENDPOINTS); }
            { ProbeScope scope(ram.operations);
              ram.run(bars.data(), static_cast<int>(bars.size()), "", "", magnifier, 4,
                      MagnifierDistribution::ENDPOINTS); }
            compare_runs(disk, ram);
            require(disk.reversals > 1 && disk.reversals == ram.reversals
                    && disk.operations.spills > 0 && ram.operations.spills > 0,
                    "both real backends repeatedly reverse and evict eligible cohorts");
            require(recording ? !ram.recorded().empty() : ram.recorded().empty(),
                    "recording-on and recording-off both execute the witness");
        }
    }
}

class OrphanHost final : public TreeHost {
public:
    // old (3 bytes), L and S: U=3,B_U=5. Only L and S fill: V=2,B_V=2.
    explicit OrphanHost(bool ram) : TreeHost(owner(ram, 3, 5, 2, 2)) {}
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {
            bind_trees();
            // A void named exit still waits in the real queue with an empty
            // logical cohort. Far-away levels prevent a trigger from removing it.
            strategy_exit("orphan-x", "old", 200.0, 0.5);
            strategy_entry("L", true);
        }
        if (i == 1) {
            queued_before_flip = pending("orphan-x", "old");
            spilled_before_flip = operations.orphan_spills > 0;
            strategy_entry("S", false);
        }
        if (i == 2) {
            reversed = live_position_size() < 0.0;
            removed_after_flip = !pending("orphan-x", "old");
        }
        sample();
    }
    bool queued_before_flip = false, spilled_before_flip = false;
    bool reversed = false, removed_after_flip = false;
};

void spilled_empty_cohort_still_cancels_its_queued_exit() {
    OrphanHost disk(false), ram(true);
    const auto bars = flat_tape(5);
    { ProbeScope scope(disk.operations); disk.run(bars.data(), static_cast<int>(bars.size())); }
    { ProbeScope scope(ram.operations); ram.run(bars.data(), static_cast<int>(bars.size())); }
    compare_runs(disk, ram);
    require(disk.queued_before_flip && ram.queued_before_flip
            && disk.spilled_before_flip && ram.spilled_before_flip,
            "witness has an already-spilled empty cohort that still owns a queued exit");
    require(disk.reversed && ram.reversed && disk.removed_after_flip && ram.removed_after_flip,
            "reversal visits every logical id and cancels the spilled cohort's exit");
}

std::string rank_id(int ordinal) {
    if (ordinal == 27) return std::string(300, 'Z');
    return std::string(1, static_cast<char>('A' + ordinal));
}
class CutoffHost final : public TreeHost {
public:
    // A..[ are 27 one-byte rank ids. The 28th id is 300 bytes: it fits
    // Coh (U=28,B_U=327), but Rank admits exactly 27 bytes and must IGNORE it.
    explicit CutoffHost(bool ram) : TreeHost(owner(ram, 28, 327, 27, 27), true, true, 32) {}
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) bind_trees();
        if (i < 28) strategy_entry(rank_id(i), true);
        if (i == 27) extent_at_27 = slab() ? slab()->region(ScratchResource::exit_rank).logical_extent : 0;
        if (i == 28) {
            ignored_key_has_no_rank = !ranks({rank_id(27)}).has_value();
            extent_at_28 = slab() ? slab()->region(ScratchResource::exit_rank).logical_extent : 0;
            strategy_close_all();
        }
        sample();
    }
    bool ignored_key_has_no_rank = false;
    std::uint64_t extent_at_27 = 0, extent_at_28 = 0;
};

void twenty_eighth_rank_key_is_ignored() {
    CutoffHost disk(false), ram(true);
    const auto bars = flat_tape(32);
    { ProbeScope scope(disk.operations); disk.run(bars.data(), static_cast<int>(bars.size())); }
    { ProbeScope scope(ram.operations); ram.run(bars.data(), static_cast<int>(bars.size())); }
    compare_runs(disk, ram);
    // 1+60*27+27=1648; ignoring a 28th id must not spend its 300 bytes.
    require(ram.extent_at_27 == 1648 && ram.extent_at_28 == 1648
            && ram.slab()->region(ScratchResource::exit_rank).capacity == 1648,
            "27/28 cutoff preserves the exact Rank extent");
    require(disk.ignored_key_has_no_rank && ram.ignored_key_has_no_rank,
            "unsupported 28th rank key has no successful child order");
    require(ram.trade_count() == 28, "the ignored rank key still opens and closes a real trade");
}

struct TreeSnapshot {
    std::array<std::uint64_t, 9> extents{};
    std::array<std::vector<std::uint8_t>, 9> bytes;
    bool operator==(const TreeSnapshot& other) const {
        return extents == other.extents && bytes == other.bytes;
    }
};
TreeSnapshot snapshot(const ScratchSlab& slab) {
    TreeSnapshot result;
    for (std::size_t i = 0; i < result.bytes.size(); ++i) {
        const auto& region = slab.regions()[i];
        result.extents[i] = region.logical_extent;
        result.bytes[i].resize(static_cast<std::size_t>(region.logical_extent));
        slab.read_at(region, 0, result.bytes[i].data(), result.bytes[i].size());
    }
    return result;
}
template<class F> void capacity_failure(const char* resource, F operation) {
    bool caught = false;
    try { operation(); }
    catch (const detail::ScratchCapacityError& error) {
        caught = true;
        const auto failure = classify_run_failure(error);
        require(failure.code == RunFailureCode::scratch_capacity_exceeded && failure.args
                && *failure.args == std::string("{\"resource\":\"") + resource + "\"}",
                "adapter capacity carries its closed tree resource");
    }
    require(caught, "adapter operation must refuse before changing state");
}

class CohortRefusalHost final : public TreeHost {
public:
    CohortRefusalHost() : TreeHost(owner(true, 1, 1, 1, 1)) {}
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        bind_trees();
        strategy_order("A", true, 1.0);
        const auto broker_before = broker_state_hash();
        const auto source_before = source_hash();
        const auto bytes_before = snapshot(*slab());
        const auto requests_before = native_working_requests().size();
        capacity_failure("cohort", [&] { strategy_order("B", true, 1.0); });
        require(broker_state_hash() == broker_before && source_hash() == source_before
                && snapshot(*slab()) == bytes_before
                && native_working_requests().size() == requests_before,
                "accepted-opening capacity fails before commands, origins and source publication");
        // A already owns handle 1. A refused B must not consume handle 2.
        require(cohort_open().value == 2, "cohort overflow consumes no kernel cohort handle");
        checked = true;
    }
    bool checked = false;
};

class RankRefusalHost final : public TreeHost {
public:
    explicit RankRefusalHost(bool byte_limit) : TreeHost(owner(true, 1, 1, byte_limit ? 1 : 0, 0)) {}
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        bind_trees();
        strategy_order("A", true, 1.0);
        const auto requests = native_working_requests();
        require(requests.size() == 1 && requests.front().definition, "accepted raw opening is live");
        const auto definition = requests.front().definition;
        const auto broker_before = broker_state_hash();
        const auto source_before = source_hash();
        const auto bytes_before = snapshot(*slab());
        const double equity_before = native_marked_equity(100.0);
        NativePrecommitView view;
        view.target = definition->handle;
        view.definition = definition;
        view.account.would_open = true;
        view.inspected_opened_units = 1.0;
        view.raw_price = view.resolved_price = 100.0;
        // Exercise the real host hook with an Applied-ready view first, so
        // the pure ticket has an observable before/after operation boundary.
        capacity_failure("exit_rank", [&] { (void)validate_execution_precommit(view); });
        require(broker_state_hash() == broker_before && source_hash() == source_before
                && snapshot(*slab()) == bytes_before, "precommit capacity is read-only");
        // Then reach it through actual settlement. A post-on_applied check
        // would already have charged the entry fee and opened a physical lot.
        // execute_current latches the precommit failure and throws its coded
        // cause; catch that boundary exception to inspect the failed run.
        bool execution_threw = false;
        try {
            (void)execute_current({definition->handle, NativeCurrentPriceRule::NearestTick});
        } catch (const std::exception& error) {
            const auto failure = classify_run_failure(error);
            require(failure.code == RunFailureCode::scratch_capacity_exceeded && failure.args
                    && *failure.args == "{\"resource\":\"exit_rank\"}",
                    "current execution throws the latched rank capacity cause");
            execution_threw = true;
        }
        require(execution_threw, "current execution must throw after the precommit capacity failure");
        require(run_failure_value_of(*this).code == RunFailureCode::scratch_capacity_exceeded,
                "kernel transports the thrown capacity code, never a trading Refuse");
        const auto physical = physical_position();
        require(physical.signed_units == 0.0 && physical.lot_count == 0 && trade_count() == 0
                && same_bits(native_marked_equity(100.0), equity_before)
                && source_hash() == source_before && snapshot(*slab()) == bytes_before,
                "rank refusal precedes financial effects, digest updates and table writes");
        checked = true;
    }
    bool checked = false;
};

void adapter_capacity_before_command_and_financial_commit() {
    const auto bars = flat_tape(4);
    CohortRefusalHost cohorts;
    cohorts.run(bars.data(), static_cast<int>(bars.size()));
    require(cohorts.last_error().empty() && cohorts.checked, "cohort ingress ticket witness completed");
    for (const bool bytes : {false, true}) {
        RankRefusalHost ranks(bytes);
        ranks.run(bars.data(), static_cast<int>(bars.size()));
        require(ranks.checked && !ranks.last_error().empty()
                && run_failure_value_of(ranks).code == RunFailureCode::scratch_capacity_exceeded,
                "rank row-cap and byte-cap failures reach the public run boundary");
    }
}

// Exact cohort leaf witness over the real resident-table codec, in addition to
// the complete real-adapter folds above. This carrier has the production five
// scalar retired fields and uses the real OriginRoster, without private access.
struct LeafFacts {
    struct Record {
        std::uint64_t handle; std::int64_t cycle;
        std::uint64_t count, digest, sides;
    };
    no::CohortHandle handle{};
    source::OriginRoster origins;
    std::vector<no::RequestHandle> opened;
    std::map<std::uint64_t, double> units;
    std::int64_t cycle = 0;
    std::uint64_t sides = 0;
    Record retired_record() const noexcept {
        return {handle.value, cycle, origins.size(), origins.digest(), sides};
    }
    static LeafFacts restore_record(const Record& row) {
        LeafFacts facts;
        facts.handle.value = row.handle; facts.cycle = row.cycle; facts.sides = row.sides;
        facts.origins = source::OriginRoster::from_retired(static_cast<std::size_t>(row.count), row.digest);
        return facts;
    }
    bool spillable() const noexcept { return origins.members().empty() && opened.empty() && units.empty(); }
};
static_assert(sizeof(LeafFacts::Record) == 40, "Coh leaf uses the exact retired record");
using LeafTable = detail::DiskResidentIdTable<LeafFacts>;

std::uint64_t cohort_leaf(const LeafTable& table) {
    // pine_state_hash.cpp's lexical cohort leaf then insertion-key leaf,
    // including the source hash's canonical generation 0 for opened handles.
    BrokerStateHashSink sink;
    sink.u(table.size());
    for (const auto& row : table.all_sorted()) {
        const auto& facts = row.second;
        sink.s(row.first); sink.u(facts.handle.value); sink.i(facts.cycle);
        sink.u(facts.origins.size()); sink.u(facts.origins.digest());
        sink.u(facts.opened.size());
        for (const auto& handle : facts.opened) {
            sink.s(handle.run.session_key); sink.u(0); sink.u(handle.incarnation);
        }
        sink.u(facts.sides); sink.u(facts.units.size());
        for (const auto& unit : facts.units) { sink.u(unit.first); sink.d(unit.second); }
    }
    sink.u(table.size());
    table.for_each_id([&](const auto& id) { sink.s(id); });
    return sink.h;
}

void isolated_cohort_leaves_match_with_retired_and_live_facts() {
    auto slab = owner(true, 4, 6, 0, 0);
    LeafTable disk, ram;
    ram.bind_scratch(slab, ScratchResource::cohort);
    ram.set_directory("/dev/null/no-cohort-leaf-fallback");
    const std::array<std::string, 4> ids{{"z", "a", "m", std::string("b\0x", 3)}};
    const auto populate = [&](LeafTable& table) {
        for (std::size_t i = 0; i < ids.size(); ++i) {
            LeafFacts facts;
            facts.handle.value = i + 1;
            auto row = table.emplace(ids[i], std::move(facts)).first;
            row->second.cycle = static_cast<std::int64_t>(i + 7);
            const no::RequestHandle origin{{"tree-leaf", 9}, 101 + i};
            row->second.origins.push_back(origin);
            row->second.sides = i % 2 + 1;
            if (i == 2) {
                row->second.opened.push_back(origin);
                row->second.units.emplace(origin.incarnation, 3.25);
            } else row->second.origins.retire_before(row->second.origins.size());
        }
        table.spill();
    };
    populate(disk); populate(ram);
    require(disk.resident_size() == 1 && ram.resident_size() == 1,
            "leaf witness contains three retired ids and one live pinned-by-exposure id");
    require(cohort_leaf(disk) == cohort_leaf(ram), "isolated lexical and insertion cohort leaves");
    for (auto* table : {&disk, &ram}) {
        const auto old = table->find("a");
        require(old != table->end() && old->second.handle.value == 2, "spilled cohort reuses its handle");
        const no::RequestHandle again{{"tree-leaf", 10}, 500};
        old->second.origins.push_back(again);
        old->second.opened.push_back(again);
        old->second.units.emplace(again.incarnation, 0.5);
    }
    require(cohort_leaf(disk) == cohort_leaf(ram), "recurring old id preserves the same cohort hash leaf");
    // Four ids have 1+1+1+3=6 bytes: 1+96*4+6=391, with no alignment gaps.
    require(slab->region(ScratchResource::cohort).logical_extent == 391,
            "cohort leaf witness exercises the actual packed Coh region");
}

class CorruptCohortHost final : public TreeHost {
public:
    explicit CorruptCohortHost(std::optional<std::uint64_t> reference)
        : TreeHost(owner(true, 1, 3, 0, 0)), reference_(reference) {}
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        bind_trees();
        strategy_exit("orphan-x", "old", 200.0, 0.5);
        // all_sorted's range pins its first row during iterator advancement;
        // the single empty cohort therefore stays resident after this walk.
        (void)source_hash();
        const auto spills_before = operations.orphan_spills;
        // A missing-id close uses openings_for -> find -> spill without
        // restoring "old" or creating another cohort. It also records a
        // dropped close, so the clean control supplies the matching hash.
        strategy_close("missing");
        require(operations.orphan_spills == spills_before + 1,
                "missing-id close spills the resident empty cohort");
        require(slab()->region(ScratchResource::cohort).logical_extent == 100,
                "one old key uses 1+96+3 bytes");
        // First packed node is at 1; its 56-byte Links precede the handle.
        std::uint64_t handle = 0;
        slab()->read_at(ScratchResource::cohort, 57, &handle, sizeof handle);
        require(handle != 0, "real cohort record has a kernel-issued handle");
        if (!reference_) {
            clean_hash = source_hash();
            checked = true;
            return;
        }
        const auto before = *reference_;
        const std::uint64_t invalid = 0;
        slab()->write_at(ScratchResource::cohort, 57, &invalid, sizeof invalid);
        bool io = false;
        try { (void)source_hash(); }
        catch (const detail::ScratchIoError& error) {
            io = error.code().value() == EIO
                && classify_run_failure(error).code == RunFailureCode::scratch_io_failed;
        }
        require(io, "corrupt cohort restore is scratch I/O, with the original errno surface");
        slab()->write_at(ScratchResource::cohort, 57, &handle, sizeof handle);
        require(source_hash() == before, "repair restores the exact cohort fold");
        checked = true;
    }
    bool checked = false;
    std::optional<std::uint64_t> clean_hash;
private:
    const std::optional<std::uint64_t> reference_;
};

void corrupt_cohort_keeps_the_scratch_io_code() {
    const auto bars = flat_tape(2);
    CorruptCohortHost control(std::nullopt);
    { ProbeScope scope(control.operations); control.run(bars.data(), static_cast<int>(bars.size())); }
    require(control.last_error().empty() && control.checked && control.clean_hash.has_value(),
            "clean corrupt-record control completed");
    CorruptCohortHost host(*control.clean_hash);
    { ProbeScope scope(host.operations); host.run(bars.data(), static_cast<int>(bars.size())); }
    require(host.last_error().empty() && host.checked, "corrupt-record witness completed");
}

} // namespace tree_ram_fixture

int main() {
    try {
        tree_ram_fixture::isolated_cohort_leaves_match_with_retired_and_live_facts();
        tree_ram_fixture::multi_entry_and_global_child_order();
        tree_ram_fixture::reversal_witness_on_the_slab();
        tree_ram_fixture::spilled_empty_cohort_still_cancels_its_queued_exit();
        tree_ram_fixture::twenty_eighth_rank_key_is_ignored();
        tree_ram_fixture::adapter_capacity_before_command_and_financial_commit();
        tree_ram_fixture::corrupt_cohort_keeps_the_scratch_io_code();
        std::puts("test_adapter_tree_ram: disk/RAM trades, cohort folds and rank order");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_adapter_tree_ram: %s\n", error.what());
        return 1;
    }
}
