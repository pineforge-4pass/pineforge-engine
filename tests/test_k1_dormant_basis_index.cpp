// Run the real retirement sweep twice: once with the identity index and
// once with a test-owned ordered scan at BOTH dormant-basis call sites.
#include "k1_dormant_retirement_fixture.hpp"

#include <array>
#include <exception>
#include <tuple>
#include <utility>

namespace a5d_witness {
using namespace dormant_retirement;
using Basis = ps::PlacementTable::ReissueBasis;

inline bool scan_mode = false;
inline std::uint64_t retirement_queries = 0;

// First principles: walk the retained placement rows ourselves, in their
// incarnation order. In particular, equal stops do NOT overwrite basis[1].
// No production index, comparison helper or release memo supplies an answer.
Basis scan_basis(const ps::PlacementTable& rows, std::int64_t cycle,
                 const ps::PlacementSnapshot& own) {
    Basis result{};
    for (const auto row : rows) {
        const auto& peer = row.second;
        if (peer.placement_cycle != cycle || peer.family != own.family
            || peer.source_id != own.source_id || peer.from_entry != own.from_entry) continue;
        if (result[0].second
            && bits(result[0].second->exit_levels.stop) != bits(peer.exit_levels.stop))
            result[1] = result[0];
        result[0] = {row.first, &peer};
    }
    return result;
}

Basis differential_basis(const ps::PlacementTable& rows, std::int64_t cycle,
                         const ps::PlacementSnapshot& own) {
    const auto expected = scan_basis(rows, cycle, own);
    const auto indexed = rows.reissue_basis(cycle, own);
    require(indexed == expected, "identity index differs from independent ordered scan");
    ++retirement_queries;
    return scan_mode ? expected : indexed;
}
} // namespace a5d_witness

// As in the scan-work fixture, compile the adapter/hash TUs and link the
// ordinary library. Only this TU substitutes a mechanism; production builds
// have neither the selector nor the scan.
#define PINEFORGE_REISSUE_BASIS(rows, cycle, own) \
    ::a5d_witness::differential_basis(rows, cycle, own)
#include "../src/source/pine_adapter.cpp"
#include "../src/source/pine_state_hash.cpp"
#undef PINEFORGE_REISSUE_BASIS

namespace a5d_witness {
template<class Tag, auto Member>
struct DeducedAccess {
    friend auto access(Tag) { return Member; }
};
struct ScratchTag { friend auto access(ScratchTag); };
struct IndexTag { friend auto access(IndexTag); };
struct CycleTag { friend auto access(CycleTag); };
struct CohortsTag { friend auto access(CohortsTag); };
template struct DeducedAccess<ScratchTag, &ps::PineExecutionAdapter::retired_row_scratch_>;
template struct DeducedAccess<IndexTag, &ps::PlacementTable::reissue_rows_>;
template struct DeducedAccess<CycleTag, &ps::PineExecutionAdapter::current_position_cycle_>;
template struct DeducedAccess<CohortsTag, &ps::PineExecutionAdapter::cohorts_by_id_>;
struct SweepTag { friend auto access(SweepTag); };
template struct DeducedAccess<SweepTag, &ps::PineExecutionAdapter::erase_retired_rows>;

bool exit_family(ps::PineOrderFamily family) {
    return family == ps::PineOrderFamily::ExitLimit || family == ps::PineOrderFamily::ExitStop
        || family == ps::PineOrderFamily::ExitTrail;
}

// Inspect ownership as well as answers: no stale pointer, duplicate member,
// empty identity, old-cycle leak, or member missing from a retained row.
void audit(const ps::PlacementTable& rows) {
    std::size_t expected_members = 0, actual_members = 0;
    for (const auto row : rows) {
        if (!exit_family(row.second.family)) {
            const auto& links = row.second.reissue_links;
            require(!links.prev && !links.next && links.incarnation == 0,
                    "ordinary row has exit membership");
            continue;
        }
        ++expected_members;
        require(rows.reissue_basis(row.second.placement_cycle, row.second)
                    == scan_basis(rows, row.second.placement_cycle, row.second),
                "retained-row basis audit differs");
        require(rows.reissue_basis(row.second.placement_cycle + 1, row.second)
                    == scan_basis(rows, row.second.placement_cycle + 1, row.second),
                "cycle-isolation basis audit differs");
    }
    const auto& groups = rows.*access(IndexTag{});
    for (const auto& group : groups) {
        require(group.second.head && group.second.tail, "empty identity retained in index");
        const ps::PlacementSnapshot* previous = nullptr;
        std::size_t forward = 0;
        for (const auto* member = group.second.head; member; member = member->reissue_links.next) {
            require(++forward <= rows.size(), "identity list contains a cycle");
            ++actual_members;
            const auto& links = member->reissue_links;
            require(rows.contains(links.incarnation), "index outlived its placement row");
            const auto& row = rows.at(links.incarnation);
            require(member == &row, "index does not name the live placement node");
            require(links.prev == previous, "identity predecessor link is stale");
            require(!previous || previous->reissue_links.incarnation < links.incarnation,
                    "identity list is not strictly incarnation ordered");
            require(std::get<0>(group.first) == row.placement_cycle
                    && std::get<1>(group.first) == row.family
                    && std::get<2>(group.first) == row.source_id
                    && std::get<3>(group.first) == row.from_entry,
                    "index membership has a stale identity");
            previous = member;
        }
        require(previous == group.second.tail, "identity tail is stale");
        const ps::PlacementSnapshot* next = nullptr;
        std::size_t backward = 0;
        for (const auto* member = group.second.tail; member; member = member->reissue_links.prev) {
            require(++backward <= forward, "reverse identity list contains a cycle");
            require(member->reissue_links.next == next, "identity successor link is stale");
            next = member;
        }
        require(next == group.second.head && backward == forward,
                "forward and reverse identity memberships differ");
    }
    require(expected_members == actual_members, "index population differs from retained exits");
}

// Store lifecycle words exactly, including double bits, rather than relying
// on a digest to compare which legs were withdrawn/retired/suspended.
struct Words {
    std::vector<std::uint64_t> values;
    void u(std::uint64_t value) { values.push_back(value); }
    void i(std::int64_t value) { u(static_cast<std::uint64_t>(value)); }
    void b(bool value) { u(value ? 1 : 0); }
    void d(double value) { u(bits(value)); }
};
using Row = std::tuple<std::uint64_t, ps::PineOrderFamily, std::string, std::string,
                       std::int64_t, std::int64_t, std::vector<std::uint64_t>>;
using Checkpoint = std::pair<std::vector<Row>, std::vector<std::uint64_t>>;
struct Result {
    std::vector<Checkpoint> checkpoints;
    std::vector<Trade> trades;
    std::vector<std::int64_t> cycles;
    std::uint64_t queries = 0;
    std::size_t dormant = 0, released = 0, erased = 0, explicit_cancels = 0;
    int callbacks = 0;
    double equity = 0.0, position = 0.0;
};

enum class Scenario { Alternating, Equal, Cancel, FillCycle };
constexpr int kBars = 24;

class WitnessHost final : public ps::PineStrategyHost {
public:
    explicit WitnessHost(Scenario scenario) : scenario_(scenario) {
        // Same real touched-stop / declined-reversal setup as the host
        // fixture. No synthetic lifecycle is used to manufacture dormancy.
        ps::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.pyramiding = 1;
        config.margin_long = config.margin_short = 100.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.01);
        set_margin_call_enabled(true);
        set_broker_state_hash_recording(false);
    }

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        require(bar == out_.callbacks++, "witness callbacks are consecutive");
        capture();
        if (scenario_ == Scenario::FillCycle && bar == 10) {
            check_fill();
            sweep(); // Second sweep, after a host-observed fill and before cancel/close.
        }
        if (bar == 0) strategy_entry("S", false, kNa, kNa, 40.0);
        if (bar == 1)
            strategy_exit("X", "S", kNa, 150.0, kNa, kNa, kNa, 100.0, "Y");
        // Cancel an already observed dormant issue before this bar's new
        // commands. strategy_cancel withdraws lifecycle legs; it does not
        // stamp the separate PineCancellationReceipt::cause field.
        if (scenario_ == Scenario::Cancel && bar == 6) cancel_x();
        const bool held_bar = bar >= 2 && bar <= 21
            && (scenario_ != Scenario::FillCycle || bar <= 7 || bar >= 15);
        if (held_bar) {
            const double stop = scenario_ == Scenario::Equal ? 120.0 : stop_for(bar - 2);
            strategy_exit("X", "S", kNa, stop, kNa, kNa, kNa, 100.0, "held", 20.0);
            strategy_entry("L", true);
            strategy_close("S", "Reverse to Long");
        }
        if (scenario_ == Scenario::FillCycle) {
            // S sold 40 at 100 on b1. A BUY limit at 95 takes profit on one
            // unit. Issue on b8; by b9 the real leg must be working, and
            // that bar's low 114 leaves it resting. Then b10 opens at 117 and
            // crosses 95 down to 94: a fill at 95, leaving -39, before any
            // cancellation or market close.
            if (bar == 8) {
                require(live_position_size() == -40.0 && position_avg_price() == 100.0,
                        "limit experiment requires the unchanged short origin");
                strategy_exit("F", "S", 95.0, kNa, kNa, kNa, kNa, 100.0, "fill", 1.0);
            }
            if (bar == 10) cancel_x();
            if (bar == 11) strategy_close_all();
            if (bar == 13) strategy_entry("S", false, kNa, kNa, 40.0);
        }
        capture();
        if (scenario_ == Scenario::FillCycle && bar == 9) {
            check_pending_fill();
            sweep(); // First sweep, with the real F limit still working.
            check_pending_fill();
        }
        if (bar == kBars - 1) mutation_sweeps();
    }

    Result finish() {
        require(last_error().empty(), "witness host completed without error");
        require(out_.callbacks == kBars && out_.dormant > 0,
                "witness never reached real dormant askable-origin legs");
        capture();
        out_.queries = retirement_queries;
        out_.equity = current_equity();
        out_.position = live_position_size();
        for (int i = 0; i < trade_count(); ++i) out_.trades.push_back(get_trade(i));
        require(out_.queries > 0 && out_.released > 0 && out_.erased > 0,
                "differential retirement outcomes were not exercised");
        if (scenario_ == Scenario::Cancel)
            require(out_.explicit_cancels > 0, "explicit cancellation was not observed");
        if (scenario_ == Scenario::FillCycle) {
            require(out_.cycles.size() >= 2, "close/reopen did not advance the cycle");
            require(fill_observed_, "limit fill was not observed between the two sweeps");
            require(std::any_of(out_.trades.begin(), out_.trades.end(), [](const Trade& trade) {
                return !trade.open_at_end && trade.exit_id == "F"
                    && trade.entry_id == "S" && !trade.is_long && trade.qty == 1.0
                    && trade.entry_price == 100.0 && trade.exit_price == 95.0
                    && trade.exit_bar_index == 10 && trade.exit_from_bracket;
            }), "the between-sweeps limit never filled");
            const auto& rows = adapter_.*access(PlacementTag{});
            require(fill_incarnation_ != 0 && !rows.contains(fill_incarnation_),
                    "filled placement was not erased after its origin closed");
            require(!cancelled_incarnations_.empty(), "cancel did not withdraw an X lifecycle");
            for (const auto id : cancelled_incarnations_)
                require(!rows.contains(id), "cancelled placement outlived its closed origin");
        }
        return out_;
    }

    void hash_host_extension(BrokerStateHashSink& f) const override {
        ps::PineStrategyHost::hash_host_extension(f);
        f.i(static_cast<int>(scenario_));
        // out_ and the incarnation lists only observe the experiment.
    }

private:
    void check_pending_fill() const {
        const auto& rows = adapter_.*access(PlacementTag{});
        require(fill_incarnation_ != 0 && rows.contains(fill_incarnation_),
                "F must have a real placement before the fill sweep");
        const auto& row = rows.at(fill_incarnation_);
        require(row.family == ps::PineOrderFamily::ExitLimit && row.from_entry == "S"
                && row.exit_levels.limit == 95.0 && row.requested_qty == 1.0
                && !row.filled_exit && !row.legs.dormant(),
                "first sweep must retain the unfilled, active one-unit F limit");
        bool working = false;
        for (const auto& request : native_working_requests())
            if (request.definition->handle.incarnation == fill_incarnation_)
                working = true;
        require(working && live_position_size() == -40.0,
                "first sweep lost the working F limit or changed the origin");
    }

    void check_fill() {
        int fills = 0;
        for (int i = 0; i < trade_count(); ++i) {
            const auto trade = get_trade(i);
            if (trade.open_at_end || trade.exit_id != "F") continue;
            require(trade.entry_id == "S" && !trade.is_long && trade.qty == 1.0
                    && trade.entry_price == 100.0 && trade.exit_price == 95.0
                    && trade.exit_bar_index == 10 && trade.exit_from_bracket
                    && trade.exit_comment == "fill",
                    "F must close one short unit at the crossed limit on b10");
            ++fills;
        }
        require(fills == 1 && live_position_size() == -39.0
                && adapter_.source_unclosed_qty_for("S") == 39.0,
                "the between-sweeps limit must fill once and leave 39 short units");
        fill_observed_ = true;
    }

    void cancel_x() {
        const auto& rows = adapter_.*access(PlacementTag{});
        std::vector<std::uint64_t> dormant;
        for (const auto row : rows) {
            const auto& value = row.second;
            if (value.source_id == "X" && value.from_entry == "S"
                && value.family == ps::PineOrderFamily::ExitStop
                && value.bracket_origin.incarnation != 0 && value.legs.dormant()
                && !value.legs.retired(exit_legs::Leg::Stop))
                dormant.push_back(row.first);
        }
        require(!dormant.empty(), "explicit cancel requires an observed unwithdrawn dormant X");
        strategy_cancel("X");
        for (const auto id : dormant) {
            require(rows.contains(id), "cancelled X disappeared before its withdrawal was observed");
            const auto& legs = rows.at(id).legs;
            require(legs.retired(exit_legs::Leg::Stop) && legs.retired(exit_legs::Leg::Limit)
                    && legs.retired(exit_legs::Leg::Trail),
                    "strategy_cancel did not withdraw the observed dormant X");
            ++out_.explicit_cancels;
            cancelled_incarnations_.push_back(id);
        }
        for (const auto& request : native_working_requests())
            require(request.definition->request.label != "X",
                    "explicitly cancelled X remains in the native working book");
        capture(); // Compare the actual withdrawn lifecycle words in both runs.
    }

    void capture() {
        const auto& rows = adapter_.*access(PlacementTag{});
        audit(rows);
        const auto cycle = adapter_.*access(CycleTag{});
        if (cycle > 0 && (out_.cycles.empty() || out_.cycles.back() != cycle))
            out_.cycles.push_back(cycle);
        Checkpoint checkpoint;
        for (const auto row : rows) {
            const auto& value = row.second;
            Words words;
            value.legs.visit(words);
            words.u(value.bracket_origin.incarnation);
            words.u(value.projection_predecessor);
            words.d(value.exit_levels.stop);
            words.d(value.exit_levels.limit);
            words.d(value.requested_qty);
            words.i(static_cast<std::int32_t>(value.cancellation.cause));
            checkpoint.first.emplace_back(row.first, value.family, value.source_id,
                value.from_entry, value.placement_cycle, value.placement_script_open_ms,
                std::move(words.values));
            if (value.legs.dormant() && value.bracket_origin.incarnation != 0
                && !value.legs.retired(exit_legs::Leg::Stop)) ++out_.dormant;
            if (value.source_id == "F") fill_incarnation_ = row.first;
            if (scenario_ == Scenario::FillCycle && value.source_id == "X"
                && value.legs.retired(exit_legs::Leg::Stop)
                && value.legs.retired(exit_legs::Leg::Limit)
                && value.legs.retired(exit_legs::Leg::Trail)
                && std::find(cancelled_incarnations_.begin(), cancelled_incarnations_.end(),
                             row.first) == cancelled_incarnations_.end())
                cancelled_incarnations_.push_back(row.first);
        }
        checkpoint.second = (adapter_.*access(ScratchTag{})).released;
        out_.released += checkpoint.second.size();
        out_.erased = static_cast<std::size_t>(rows.erased_rows());
        out_.checkpoints.push_back(std::move(checkpoint));
    }

    void sweep() {
        const auto* point = ps::detail::callback_point(*this);
        require(point != nullptr, "mutation sweep requires the real host callback point");
        (adapter_.*access(SweepTag{}))(point->decision);
        capture();
    }

    void mutation_sweeps() {
        auto& rows = adapter_.*access(PlacementTag{});
        const auto cycle = adapter_.*access(CycleTag{});
        std::optional<ps::PlacementSnapshot> seed;
        for (const auto row : rows) {
            if (row.second.family == ps::PineOrderFamily::ExitStop
                && row.second.source_id == "X" && row.second.placement_cycle == cycle
                && row.second.legs.dormant()
                && !row.second.legs.retired(exit_legs::Leg::Stop)
                && row.second.bracket_origin.incarnation != 0) seed = row.second;
        }
        require(seed.has_value(), "mutation states require an actual held exit from the fixture");
        const auto& cohorts = adapter_.*access(CohortsTag{});
        const auto cohort = cohorts.find("S");
        require(cohort != cohorts.end()
                && std::any_of(cohort->second.opened.begin(), cohort->second.opened.end(),
                    [&](const native_order::RequestHandle& origin) {
                        return origin == seed->bracket_origin;
                    }), "fixture seed must have a currently askable opening");
        for (const auto& request : native_working_requests())
            require(request.definition->handle.incarnation != seed->legs.target().incarnation,
                    "fixture seed must have left the native working book");
        // Only after the final source commands: reserve holes for delayed
        // adoption without colliding with a future native submission. These
        // rows borrow a REAL dormant lifecycle; only immutable placement
        // evidence is varied to test identity, chronology, and table erasure.
        const auto base = rows.high_water() + 1;
        std::vector<std::uint64_t> probes;
        const auto add = [&](std::uint64_t id, ps::PlacementSnapshot row) {
            row.projection_predecessor = 0;
            row.legs.fork(id, row.legs.target().owner);
            require(rows.try_emplace(id, std::move(row)).second, "probe incarnation reused");
            probes.push_back(id);
            audit(rows);
        };
        std::array<ps::PlacementSnapshot, 4> identities{*seed, *seed, *seed, *seed};
        for (auto& row : identities) row.source_id = "basis-probe";
        identities[1].source_id += "-other";
        identities[2].from_entry += "-other";
        identities[3].family = ps::PineOrderFamily::ExitLimit;
        for (std::size_t group = 0; group < identities.size(); ++group) {
            for (int ordinal = 0; ordinal < 3; ++ordinal) {
                auto row = identities[group];
                row.exit_levels.stop = stop_for(ordinal); // A/B/A
                row.placement_script_open_ms = kStart + (16 + ordinal) * kMinute;
                add(base + group * 8 + static_cast<std::uint64_t>(ordinal * 2), std::move(row));
            }
        }
        sweep(); // Full production retirement outcome with isolated identities.
        for (std::size_t group = 0; group < identities.size(); ++group) {
            const auto middle = base + group * 8 + 2;
            require(rows.contains(middle), "transition witness was not retained before erasure");
            require(rows.erase(middle), "erase the live differing-stop witness");
        }
        capture();
        sweep(); // Rebuild each basis from survivors, not from the deleted B.

        // Add rows AFTER their leg was held, including adoption below the
        // high water, equal stops, and same-bar / backwards placement ages.
        for (std::size_t group = 0; group < identities.size(); ++group) {
            auto row = identities[group];
            row.exit_levels.stop = 125.0;
            row.placement_script_open_ms = kStart + 18 * kMinute; // same bar as latest A
            add(base + group * 8 + 1, row); // delayed adoption
            row.exit_levels.stop = 120.0;
            row.placement_script_open_ms = kStart + 15 * kMinute; // newer id, older age
            add(base + group * 8 + 6, row);
            row.placement_script_open_ms = kStart + 19 * kMinute;
            add(base + group * 8 + 7, row); // equal-stop newest must keep prior transition
        }
        sweep();

        // A rewrite moves all four key components, then crosses an epoch.
        const auto rewritten = base + 7;
        require(rows.contains(rewritten), "newest certificate missing before rewrite");
        auto replacement = rows.at(rewritten);
        replacement.source_id += "-rewritten";
        replacement.from_entry += "-rewritten";
        replacement.family = ps::PineOrderFamily::ExitTrail;
        replacement.placement_cycle = cycle - 1;
        replacement.bracket_origin = {}; // The old origin is no longer askable.
        rows.replace(rewritten, std::move(replacement));
        audit(rows);
        sweep();
        require(!rows.contains(rewritten), "old-cycle unrooted row survived its sweep");

        for (const auto id : probes) {
            if (rows.contains(id)) require(rows.erase(id), "probe cleanup erase failed");
            audit(rows);
        }
        sweep(); // Every now-empty probe identity must have disappeared.

        // Reset and move storage with an index populated from the same real
        // fixture row. No adapter/kernel lifecycle is changed by this check.
        ps::PlacementTable temporary;
        temporary.fixture_use_memory_auxiliary_tables();
        temporary.try_emplace(9, *seed);
        temporary.try_emplace(3, *seed);
        audit(temporary);
        temporary.compact();
        ps::PlacementTable moved(std::move(temporary));
        audit(moved);
        moved.clear();
        require((moved.*access(IndexTag{})).empty(), "clear leaked reissue identities");
        const std::array<std::uint64_t, 6> stop_bits{
            0x0000000000000000ULL, 0x8000000000000000ULL,
            0x7ff8000000000001ULL, 0x7ff8000000000002ULL,
            0x7ff8000000000002ULL, 0x0000000000000000ULL};
        std::uint64_t bit_id = 1;
        for (const auto raw : stop_bits) {
            auto row = *seed;
            std::memcpy(&row.exit_levels.stop, &raw, sizeof(raw));
            moved.try_emplace(bit_id++, std::move(row));
            audit(moved);
        }
        moved.clear();
        moved.try_emplace(1, *seed);
        moved.try_emplace(2, *seed);
        auto same_identity = moved.at(1);
        require(!same_identity.reissue_links.prev && !same_identity.reissue_links.next
                && same_identity.reissue_links.incarnation == 0,
                "snapshot copy inherited table membership");
        same_identity.exit_levels.stop = -0.0;
        moved.replace(1, std::move(same_identity));
        audit(moved);
        // Move assignment destroys a populated destination's old identity
        // while transferring stable nodes and their links from the source.
        ps::PlacementTable assigned;
        assigned.fixture_use_memory_auxiliary_tables();
        auto other_identity = *seed;
        other_identity.source_id += "-move-destination";
        assigned.try_emplace(7, std::move(other_identity));
        assigned = std::move(moved);
        audit(assigned);
        moved = std::move(assigned);
        audit(moved);
        require(moved.erase(2), "same-key rewrite peer cleanup failed");
        auto ordinary = moved.at(1);
        ordinary.family = ps::PineOrderFamily::Order;
        moved.replace(1, std::move(ordinary));
        audit(moved);
        require((moved.*access(IndexTag{})).empty(), "exit-to-order rewrite leaked its identity");
        moved.replace(1, ps::PlacementSnapshot(*seed));
        audit(moved);
        require(moved.erase(1), "erase last member failed");
        require((moved.*access(IndexTag{})).empty(), "last-row erase leaked its identity");

        // Move a row into an existing identity, including insertion between
        // its peers on replacement; then recycle the erased middle node.
        moved.try_emplace(1, *seed);
        moved.try_emplace(3, *seed);
        auto separate = *seed;
        separate.source_id += "-separate";
        moved.try_emplace(2, separate);
        moved.replace(2, ps::PlacementSnapshot(*seed));
        audit(moved);
        moved.replace(2, std::move(separate));
        audit(moved);
        moved.replace(2, ps::PlacementSnapshot(*seed));
        audit(moved);
        require(moved.erase(2), "intrusive middle erase failed");
        audit(moved);
        moved.try_emplace(2, *seed);
        moved.compact();
        audit(moved);
        moved.clear();
        audit(moved);
    }

    const Scenario scenario_;
    Result out_;
    std::uint64_t fill_incarnation_ = 0;
    bool fill_observed_ = false;
    std::vector<std::uint64_t> cancelled_incarnations_;
};

Result run(Scenario scenario, bool oracle) {
    RetentionSwitch retirement(false);
    scan_mode = oracle;
    retirement_queries = 0;
    auto bars = tape(kBars, false);
    if (scenario == Scenario::FillCycle)
        bars[10] = {117.0, 118.0, 94.0, 115.0, 1.0, kStart + 10 * kMinute};
    WitnessHost host(scenario);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1",
             false, 4, MagnifierDistribution::ENDPOINTS);
    return host.finish();
}
} // namespace a5d_witness

int main() {
    try {
        using namespace a5d_witness;
        for (const auto scenario : {Scenario::Alternating, Scenario::Equal,
                                    Scenario::Cancel, Scenario::FillCycle}) {
            const auto indexed = run(scenario, false);
            const auto scanned = run(scenario, true);
            require(indexed.checkpoints == scanned.checkpoints,
                    "indexed/scanned released legs or surviving placement rows differ");
            require(indexed.cycles == scanned.cycles && indexed.queries == scanned.queries,
                    "indexed/scanned retirement schedules differ");
            require(indexed.explicit_cancels == scanned.explicit_cancels,
                    "indexed/scanned explicit cancellation transitions differ");
            require(same_number(indexed.equity, scanned.equity)
                    && same_number(indexed.position, scanned.position),
                    "indexed/scanned public account differs");
            require(indexed.trades.size() == scanned.trades.size(), "trade counts differ");
            for (std::size_t i = 0; i < indexed.trades.size(); ++i)
                require(same_trade(indexed.trades[i], scanned.trades[i]), "trade fields differ");
            std::printf("A5D WITNESS scenario=%d checkpoints=%zu queries=%llu "
                        "released=%zu erased=%zu cycles=%zu equality=exact\n",
                        static_cast<int>(scenario), indexed.checkpoints.size(),
                        static_cast<unsigned long long>(indexed.queries), indexed.released,
                        indexed.erased, indexed.cycles.size());
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_k1_dormant_basis_index: %s\n", error.what());
        return 1;
    }
}
