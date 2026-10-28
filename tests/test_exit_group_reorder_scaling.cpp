// Drive the same publication path generated Pine strategies use. Work counts
// come from the adapter and host functions; no fixture writes broker state.
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/command_scratch_production.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t epoch = 1704067200000LL;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Key {
    bool open = false;
    std::uint64_t sequence = 0;
    std::size_t source = 0;
    int price_offset = 0;
};

struct Sample {
    std::uint64_t publications = 0, rows = 0, queries = 0, visits = 0, scratch = 0;
    std::uint64_t index_operations = 0, index_pages = 0, resident = 0;
    std::size_t retained_minimum = 0, retained_maximum = 0;
    std::uint64_t total() const {
        return publications + rows + queries + visits + scratch + index_operations + index_pages;
    }
};

class Host final : public source::PineStrategyHost {
public:
    Host(int group, int parked, bool reverse, bool coof, bool churn, bool second)
        : group_(group), parked_(parked), reverse_(reverse), churn_(churn), second_(second) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.initial_capital = 100000000.0;
        config.margin_long = config.margin_short = 0.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.calc_on_order_fills = coof;
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", 1.0);
        syminfo_mintick_ = 0.25;
        for (int index = 0; index < group_; ++index) {
            const int offset = reverse_ ? group_ - index : index + 1;
            oracle_.push_back({false, static_cast<std::uint64_t>(index),
                               static_cast<std::size_t>(offset - 1), offset});
        }
        // The rising fill bar reaches prices in ascending order. Start the
        // reference in that physical order before applying the original keys.
        std::sort(oracle_.begin(), oracle_.end(), [](const Key& a, const Key& b) {
            return a.source < b.source;
        });
        // The unfixed ordered path compares Open first, then command creation
        // sequence, then original row position. No product lookup is the oracle.
        std::stable_sort(oracle_.begin(), oracle_.end(), [](const Key& a, const Key& b) {
            if (a.open != b.open) return a.open;
            if (a.sequence != b.sequence) return a.sequence < b.sequence;
            return a.source < b.source;
        });
    }

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        if (bar == 0 && !entered_) {
            entered_ = true;
            strategy_entry("position", true, na, na, static_cast<double>(group_));
            for (int index = 0; index < parked_; ++index)
                strategy_order("parked-" + std::to_string(index), true, 1.0, 1.0);
        }
        if (bar >= 1 && !exits_ && live_position_size() == group_) {
            exits_ = true;
            for (const auto& key : oracle_)
                strategy_exit("exit-" + std::to_string(key.price_offset), "position",
                              100.0 + key.price_offset, na, na, na, na, 100.0,
                              "one unit", 1.0);
        }
        if (second_ && bar == 14 && !second_entry_) {
            second_entry_ = true;
            strategy_entry("position", true, na, na, static_cast<double>(group_));
        }
        if (second_ && bar >= 15 && !second_exits_ && live_position_size() == group_) {
            second_exits_ = true;
            for (const auto& key : oracle_)
                strategy_exit("exit-" + std::to_string(key.price_offset), "position",
                              100.0 + key.price_offset, na, na, na, na, 100.0,
                              "one unit", 1.0);
        }
        if (bar >= 6 && bar <= 13) {
            check_rows();
            const auto now = adapter_.exit_group_work();
            require(now.retained_placements >= static_cast<std::size_t>(parked_),
                    "setup: parked placements were not retained");
            if (sampled_) {
                Sample delta{now.publications - previous_.publications,
                             now.tail_rows - previous_.tail_rows,
                             now.sequence_queries - previous_.sequence_queries,
                             now.sequence_rows - previous_.sequence_rows,
                             now.scratch_operations - previous_.scratch_operations,
                             now.sequence_index_operations - previous_.sequence_index_operations,
                             now.sequence_index_pages - previous_.sequence_index_pages,
                             now.sequence_index_resident_bytes};
                require(delta.publications > 0, "setup: no exit-group publication observed");
                require(delta.queries <= 4 * static_cast<std::uint64_t>(group_),
                        "sequence lookup calls exceeded linear group bound");
                // A quiet tail has no changed sort key. Its placements may be
                // retained or erased; neither population belongs in this cost.
                require(delta.visits <= 8 * static_cast<std::uint64_t>(group_)
                            && delta.rows <= 8,
                        "same-bar tail rechecked unchanged history / sequence-row bound exceeded");
                require(delta.scratch <= 4 * static_cast<std::uint64_t>(group_),
                        "sorted exit group exceeded linear scratch-operation bound");
                require(delta.index_operations <= 64 * static_cast<std::uint64_t>(group_),
                        "sorted exit group exceeded linear retained-index operation bound");
                maximum_.publications = std::max(maximum_.publications, delta.publications);
                maximum_.rows = std::max(maximum_.rows, delta.rows);
                maximum_.queries = std::max(maximum_.queries, delta.queries);
                maximum_.visits = std::max(maximum_.visits, delta.visits);
                maximum_.scratch = std::max(maximum_.scratch, delta.scratch);
                maximum_.index_operations = std::max(maximum_.index_operations, delta.index_operations);
                maximum_.index_pages = std::max(maximum_.index_pages, delta.index_pages);
                maximum_.resident = delta.resident;
                maximum_.retained_minimum = maximum_.retained_minimum == 0
                    ? now.retained_placements : std::min(maximum_.retained_minimum, now.retained_placements);
                maximum_.retained_maximum = std::max(maximum_.retained_maximum, now.retained_placements);
                ++samples_;
            }
            previous_ = now;
            sampled_ = true;
            if (churn_)
                strategy_order("unrelated", true, 1.0, bar % 2 ? 2.0 : 3.0);
        }
    }

    void check_rows() const {
        const auto count = closed_trade_count();
        require(count == static_cast<std::size_t>(group_)
                    || (second_ && count == 2 * static_cast<std::size_t>(group_)),
                "setup: exits did not produce the complete bracket group");
        const auto start = count - static_cast<std::size_t>(group_);
        double profit = 0.0;
        const auto first = closed_trade(start);
        const auto first_phase = adapter_.exit_phase_value(start);
        for (int index = 0; index < group_; ++index) {
            const auto at = start + static_cast<std::size_t>(index);
            const auto row = closed_trade(at);
            const auto& key = oracle_[static_cast<std::size_t>(index)];
            require(row.exit_from_bracket && row.entry_id == "position"
                        && row.entry_time == first.entry_time && row.exit_time == first.exit_time
                        && adapter_.exit_phase_value(at) == first_phase,
                    "setup: rows do not form one same-phase bracket group");
            require(row.exit_id == "exit-" + std::to_string(key.price_offset)
                        && row.exit_price == 100.0 + key.price_offset,
                    "exit order differs from reference sort of submitted command keys");
            // One unit bought at 100 and sold at 100+j earns j. The sum is
            // 1+...+g = g*(g+1)/2, with no commission or slippage.
            require(row.entry_price == 100.0 && row.qty == 1.0
                        && row.pnl == key.price_offset && row.commission == 0.0,
                    "bracket trade differs from hand arithmetic");
            profit += row.pnl;
        }
        require(profit == static_cast<double>(group_) * (group_ + 1) / 2.0,
                "closed profit differs from triangular-sum oracle");
    }

    Sample finish() const {
        check_rows();
        require(samples_ == 7, "setup: quiet publication window was incomplete");
        require(closed_trade_count() == static_cast<std::size_t>(second_ ? 2 * group_ : group_),
                "setup: replacement tail was incomplete");
        const auto work = adapter_.exit_group_work();
        require(reverse_ ? work.replacements >= static_cast<std::uint64_t>(group_)
                         : work.replacements == 0,
                "setup: initial group did not exercise its expected orderedness path");
        return maximum_;
    }

    void restart() {
        entered_ = exits_ = sampled_ = second_entry_ = second_exits_ = false;
        samples_ = 0;
        previous_ = {};
        maximum_ = {};
    }

private:
    int group_, parked_;
    bool reverse_, churn_, second_, entered_ = false, exits_ = false, sampled_ = false;
    bool second_entry_ = false, second_exits_ = false;
    int samples_ = 0;
    std::vector<Key> oracle_;
    source::PineExecutionAdapter::ExitGroupWork previous_{};
    Sample maximum_{};
};

Sample exercise(int group, int parked, bool reverse, bool coof, bool churn,
                bool second = false, bool reuse = false) {
    Host host(group, parked, reverse, coof, churn, second);
    std::vector<Bar> bars;
    for (int index = 0; index < 20; ++index)
        bars.push_back({100.0, index == 2 || (second && index == 16) ? 100.5 + group : 100.5,
                        99.5, 100.0, 1.0, epoch + 60000LL * index});
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    // A work-bound failure inside a callback ends the run; report that error
    // itself rather than the incomplete-window message that would follow.
    require(host.last_error().empty(), host.last_error().c_str());
    const auto result = host.finish();
    if (reuse) {
        host.restart();
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        require(host.last_error().empty(), host.last_error().c_str());
        host.finish();
    }
    std::printf("exit group=%d retained-at-least=%d reverse=%d fill-recalculation=%d "
                "unrelated-orders=%d publications=%llu rows=%llu queries=%llu "
                "sequence-rows=%llu scratch-operations=%llu index-operations=%llu "
                "index-page-operations=%llu index-resident=%llu retained-minimum=%zu "
                "retained-maximum=%zu\n",
                group, parked, reverse, coof, churn,
                static_cast<unsigned long long>(result.publications),
                static_cast<unsigned long long>(result.rows),
                static_cast<unsigned long long>(result.queries),
                static_cast<unsigned long long>(result.visits),
                static_cast<unsigned long long>(result.scratch),
                static_cast<unsigned long long>(result.index_operations),
                static_cast<unsigned long long>(result.index_pages),
                static_cast<unsigned long long>(result.resident),
                result.retained_minimum, result.retained_maximum);
    return result;
}

void retained_identity_reference() {
    source::PlacementTable table;
    std::map<std::uint64_t, source::PlacementSnapshot> retained;
    std::vector<source::PlacementSnapshot> erased;
    const auto absent = std::numeric_limits<std::uint64_t>::max();
    const std::string binary("id\0tail", 7);
    const auto check = [&] {
        for (const auto& id : {std::string("id"), std::string("changed"), binary})
            for (const auto& from : {std::string(), std::string("a"), std::string("b")}) {
                auto any = absent, paired = absent;
                const auto visit = [&](const source::PlacementSnapshot& row) {
                    if (row.source_id != id) return;
                    any = std::min(any, row.command_sequence);
                    if (row.from_entry == from) paired = std::min(paired, row.command_sequence);
                };
                for (const auto& row : retained) visit(row.second);
                for (const auto& row : erased) visit(row);
                require(table.command_sequence_for(id, from) == (paired != absent ? paired : any),
                        "retained identity lookup differs from independent row-history oracle");
            }
    };
    const auto put = [&](std::uint64_t incarnation, std::string id,
                         std::string from, std::uint64_t sequence) {
        source::PlacementSnapshot row;
        row.source_id = std::move(id);
        row.from_entry = std::move(from);
        row.command_sequence = sequence;
        if (retained.count(incarnation)) {
            auto replacement = row;
            table.replace(incarnation, std::move(replacement));
            retained.at(incarnation) = row;
        } else {
            table.try_emplace(incarnation, row);
            retained.emplace(incarnation, row);
        }
        check();
    };
    const auto erase = [&](std::uint64_t incarnation) {
        require(table.erase(incarnation), "retained oracle row was absent");
        erased.push_back(retained.at(incarnation));
        retained.erase(incarnation);
        table.compact();
        check();
    };
    put(1, "id", "", 9);
    put(2, "id", "a", 7);
    put(3, "id", "a", 7); // Removing one equal minimum preserves the other.
    put(4, "id", "b", absent); // A present maximum still uses the any-entry fallback.
    put(5, binary, "", 3);
    put(2, "changed", "b", 20);
    put(3, "id", "a", 12); // Removing the last minimum must raise its answer.
    put(1, "changed", "", 30); // A same-size wholesale identity replacement.
    erase(3);
    put(6, "id", "a", 50); // Erased evidence wins over a later retained sequence.
    erase(4);
    put(5, "id", "", 2); // The old binary identity must disappear.
    source::PlacementTable moved(std::move(table));
    require(moved.command_sequence_for("id", "a") == 12,
            "move lost retained or erased sequence evidence");
    table.clear();
    table = std::move(moved);
    check();
    table.clear();
    retained.clear();
    erased.clear();
    check();
}

void failed_identity_publication() {
    const auto budget = std::make_shared<detail::CommandScratchByteBudget>(16u * 1024u * 1024u);
    const auto previous = detail::set_command_scratch_byte_cap(budget);
    struct Restore {
        detail::CommandScratchByteCap previous;
        ~Restore() { detail::set_command_scratch_byte_cap(previous); }
    } restore{previous};
    source::PlacementTable table;
    source::PlacementSnapshot row;
    row.source_id = "original";
    row.from_entry = "a";
    row.command_sequence = 7;
    table.try_emplace(1, row);
    const auto digest = table.erased_digest();
    const auto blocked = budget->limit - budget->live_bytes.load();
    require(budget->reserve(blocked), "setup: could not hold the remaining scratch budget");
    auto replacement = row;
    replacement.source_id.assign(1024u * 1024u, 'x');
    bool caught = false;
    try { table.replace(1, std::move(replacement)); }
    catch (const std::exception& error) {
        caught = classify_run_failure(error).code == RunFailureCode::scratch_io_failed;
    }
    budget->release(blocked);
    require(caught, "replacement did not report its injected backing failure");
    require(table.size() == 1 && table.at(1).source_id == row.source_id
                && table.at(1).command_sequence == row.command_sequence
                && table.erased_rows() == 0 && table.erased_digest() == digest
                && table.command_sequence_for("original", "a") == 7,
            "failed retained-index write changed the authoritative placement");
}

class BoundaryAmendmentHost final : public source::PineStrategyHost {
public:
    BoundaryAmendmentHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.margin_long = config.margin_short = 0.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.pyramiding = 2;
        configure_pine_strategy(config);
    }
    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        if (bar == 0) strategy_entry("boundary", true, na, na, 1.0);
        if (bar == 1) {
            strategy_exit("boundary-exit", "boundary", 100.0, na);
            strategy_entry("position", true, na, na, 2.0);
        }
        if (bar == 2) {
            require(live_position_size() == 2.0, "setup: tail position did not open");
            // The path reaches 101 before 102; publication must restore the
            // submitted order, 102 before 101, within the two-row tail.
            strategy_exit("first", "position", 102.0, na, na, na, na, 100.0, "", 1.0);
            strategy_exit("second", "position", 101.0, na, na, na, na, 100.0, "", 1.0);
        }
        if (bar == 3) {
            check_trades();
            const auto boundary_phase = adapter_.exit_phase_value(0);
            const auto tail_phase = adapter_.exit_phase_value(1);
            require(boundary_phase != tail_phase,
                    "setup: predecessor and tail did not exercise different fill phases");
            before_ = adapter_.exit_group_work();
            // Exercise the public phase-amendment interface on actual run
            // output. The predecessor is a certificate dependency even
            // though this amendment leaves all Trade fields unchanged.
            adapter_.permute_exit_phases(0, {1});
            require(adapter_.exit_phase_value(0) == tail_phase,
                    "predecessor phase amendment did not publish its source phase");
            amended_ = true;
        }
        if (bar == 4) {
            require(amended_, "setup: predecessor phase was not amended");
            check_trades();
            const auto after = adapter_.exit_group_work();
            require(after.tail_rows > before_.tail_rows
                        && after.sequence_queries > before_.sequence_queries,
                    "predecessor phase amendment bypassed exit-group publication");
            observed = true;
        }
    }
    bool observed = false;
private:
    void check_trades() const {
        require(closed_trade_count() == 3, "setup: predecessor and bracket tail are incomplete");
        const auto boundary = closed_trade(0);
        const auto first = closed_trade(1);
        const auto second = closed_trade(2);
        require(boundary.entry_id == "boundary" && first.entry_id == "position"
                    && second.entry_id == "position" && boundary.exit_from_bracket
                    && first.exit_from_bracket && second.exit_from_bracket
                    && first.entry_time == second.entry_time && first.exit_time == second.exit_time
                    && boundary.exit_time != first.exit_time,
                "setup: final bracket group does not have exactly one predecessor");
        // All entries cost 100 with no fees. The predecessor exits at 100
        // for zero; the submitted tail exits earn 102-100=2 and 101-100=1.
        require(boundary.entry_price == 100.0 && boundary.exit_price == 100.0
                    && boundary.qty == 1.0 && boundary.pnl == 0.0
                    && first.exit_id == "first" && first.entry_price == 100.0
                    && first.exit_price == 102.0 && first.qty == 1.0 && first.pnl == 2.0
                    && second.exit_id == "second" && second.entry_price == 100.0
                    && second.exit_price == 101.0 && second.qty == 1.0 && second.pnl == 1.0
                    && boundary.commission == 0.0 && first.commission == 0.0
                    && second.commission == 0.0,
                "predecessor amendment changed trades from submitted-order hand arithmetic");
    }
    bool amended_ = false;
    source::PineExecutionAdapter::ExitGroupWork before_{};
};

void boundary_amendment_publication() {
    BoundaryAmendmentHost host;
    std::array<Bar, 6> bars{};
    for (std::size_t index = 0; index < bars.size(); ++index)
        bars[index] = {100.0, index == 3 ? 103.0 : 100.5, 99.5, 100.0,
                       1.0, epoch + 60000LL * static_cast<std::int64_t>(index)};
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.observed, "setup: predecessor amendment publication was not observed");
}
} // namespace

int main() {
    try {
        boundary_amendment_publication();
        std::uint64_t resident = 0;
        for (const int parked : {500, 1000, 2000}) {
            std::uint64_t previous = 0;
            for (const int group : {64, 128, 256, 512}) {
                const auto work = exercise(group, parked, false, false, false);
                require(work.resident > 0, "setup: retained sequence store was not provisioned");
                if (resident)
                    require(work.resident == resident,
                            "retained sequence workspace grew with placement or group population");
                resident = work.resident;
                if (previous)
                    require(10 * work.total() <= 22 * previous,
                            "sorted exit publication work grew more than 2.2 times");
                previous = work.total();
            }
        }
        exercise(128, 1000, true, false, false);
        exercise(128, 1000, true, true, false);
        exercise(128, 1000, true, false, true);
        exercise(64, 500, true, false, true, true, true);
        retained_identity_reference();
        failed_identity_publication();
        std::puts("exit-group publication work and reference order passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
