// R11: independent old-walk oracle, not a second invocation of the summary.
// Compile first against 583c2217d2's header closure (the helper is absent).
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "../src/source/pine_position_tail_summary.hpp"
#include "../src/source/pine_position_tail_binding.hpp"
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace {
using namespace pineforge;
using source::detail::PositionTailSummary;
using source::detail::PositionTailSpill;
using source::detail::PositionTailBinding;
using Live = std::unordered_map<std::string, std::unordered_map<std::uint64_t, double>>;
using Parts = std::unordered_map<std::uint64_t, double>;

struct Directory {
    char path[64] = "/tmp/pineforge-r11-summary-XXXXXX";
    Directory() { assert(::mkdtemp(path)); }
    ~Directory() { assert(::rmdir(path) == 0); }
};

struct Query {
    bool ancestry = true;
    bool fifo = true;
    bool is_long = true;
    std::string name = "reused";
};

bool included(const Trade& row, const Live& live, const Query& query) {
    if (row.is_long != query.is_long) return false;
    if (!query.ancestry)
        return row.exit_id == source::kMarginCallLabel
            && (!query.fifo || row.entry_id == query.name);
    for (const auto& cohort : live)
        if ((!query.fifo || cohort.first == query.name)
            && cohort.second.count(row.entry_incarnation)) return true;
    return false;
}

double old_walk(const std::vector<Trade>& rows, std::int64_t barrier,
                const Live& live, const Query& query) {
    double result = 0.0;
    for (auto row = rows.rbegin(); row != rows.rend(); ++row) {
        if (row->exit_time < barrier) break;
        if (included(*row, live, query)) result += row->qty;
    }
    return result;
}

Trade row(std::uint64_t origin, double qty, std::int64_t time,
          bool is_long = true, bool margin = false, std::string name = "reused") {
    Trade result{};
    result.entry_incarnation = origin;
    result.qty = qty;
    result.exit_time = time;
    result.is_long = is_long;
    result.entry_id = std::move(name);
    result.exit_id = margin ? source::kMarginCallLabel : "partial";
    return result;
}

struct Fixture {
    Directory directory;
    PositionTailSummary summary;
    std::vector<Trade> reference;
    TradeLedger resident;
    std::shared_ptr<PositionTailSpill> spill;
    Live live;
    std::int64_t barrier = 10;
    std::size_t certified = 0, fallback = 0, queries = 0;

    Fixture() {
        summary.set_directory(directory.path);
        open(10);
    }
    void open(std::int64_t time) {
        barrier = time;
        spill.reset(); // same position-open seam as the adapter
        summary.begin_position(time, resident.base(), resident.size(),
                               [&](std::size_t i) -> Trade { return resident[i]; });
    }
    void add_origin(std::uint64_t origin, bool is_long = true,
                    const std::string& name = "reused") {
        live[name][origin] = 1000000.0;
        summary.open_origin(origin, is_long);
        summary.live_cohort(&*live.find(name), true);
    }
    void append(Trade trade) {
        reference.push_back(trade);
        resident.push_back(std::move(trade));
        sync();
    }
    void sync() {
        summary.observe(resident.base(), resident.size(),
                        [&](std::size_t i) -> Trade { return resident[i]; });
    }
    void retire(std::uint64_t origin) {
        for (auto& cohort : live) {
            cohort.second.erase(origin);
            summary.live_cohort(&cohort, !cohort.second.empty());
        }
        summary.retire_origin(origin);
    }
    void seal(std::size_t end) {
        sync();
        PositionTailBinding::preserve(spill, resident, end, barrier, directory.path);
        resident.seal(end);
    }
    double query(const Query& query, bool expect_fast = false) {
        sync();
        double actual = 0.0;
        const bool fast = query.ancestry
            ? summary.ancestry(query.is_long, [&](const auto& include) {
                summary.for_each_live_cohort([&](const void* pointer) {
                    const auto& cohort = *static_cast<const Live::value_type*>(pointer);
                    if (!query.fifo || cohort.first == query.name)
                        for (const auto& origin : cohort.second) include(origin.first);
                });
              }, actual)
            : summary.margin(query.is_long, query.fifo ? &query.name : nullptr, actual);
        ++queries;
        if (fast) {
            ++certified;
            if (query.ancestry) {
                std::size_t indexed = 0, nonempty = 0;
                for (const auto& cohort : live) nonempty += !cohort.second.empty();
                summary.for_each_live_cohort([&](const void* pointer) {
                    const auto& cohort = *static_cast<const Live::value_type*>(pointer);
                    assert(!cohort.second.empty());
                    ++indexed;
                });
                assert(indexed == nonempty);
            }
        } else {
            ++fallback;
            // Production row source, with a predicate independent of the
            // summary's certificate and incremental bookkeeping.
            actual = source::detail::replay_position_tail(
                resident.size(), resident.base(),
                [&](std::size_t i) -> Trade { return resident[i]; },
                spill.get(), barrier,
                [&](bool is_long, std::uint64_t origin, bool margin, const auto& matches) {
                    if (is_long != query.is_long) return false;
                    if (!query.ancestry) return margin && (!query.fifo || matches());
                    for (const auto& cohort : live)
                        if ((!query.fifo || cohort.first == query.name)
                            && cohort.second.count(origin)) return true;
                    return false;
                }, query.name, summary.work);
        }
        const double expected = old_walk(reference, barrier, live, query);
        assert(std::memcmp(&expected, &actual, sizeof actual) == 0);
        if (expect_fast) assert(fast);
        return actual;
    }
    void all_queries(bool fast = false) {
        for (bool ancestry : {false, true})
            for (bool fifo : {false, true})
                for (bool is_long : {false, true})
                    for (const char* name : {"reused", "other", "absent"})
                        query({ancestry, fifo, is_long, name}, fast);
    }
    Parts parts_query(const Query& query, bool expect_fast) {
        sync();
        Parts parts;
        const auto select = [&](const auto& include) {
            for (const auto& cohort : live)
                if (!query.fifo || cohort.first == query.name)
                    for (const auto& origin : cohort.second)
                        include(origin.first, parts[origin.first]);
        };
        const auto before = summary.work;
        double actual = 0.0;
        using Diagnostics = source::detail::PositionTailDiagnostics;
        const bool fast = query.ancestry
            ? summary.ancestry_parts(query.is_long, select, actual, Diagnostics{})
            : summary.margin_parts(query.is_long, query.fifo ? &query.name : nullptr,
                                   select, actual, Diagnostics{});
        assert(fast == expect_fast);
        assert(summary.work.queries == before.queries + 1);
        assert(summary.work.certified_queries == before.certified_queries + (fast ? 1 : 0));
        assert(summary.work.fallback_queries == before.fallback_queries + (fast ? 0 : 1));
        std::uint64_t verdicts = 0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(
                 source::PineExecutionAdapter::PositionTailGuard::Count); ++i)
            verdicts += summary.work.guard_reasons[i] - before.guard_reasons[i];
        assert(verdicts == 1);
        assert(fast == (summary.work.guard_reason
                        == source::PineExecutionAdapter::PositionTailGuard::None));
        if (!fast) {
            parts.clear();
            select([](std::uint64_t, double& part) { part = 0.0; });
            actual = source::detail::replay_position_tail_split(
                resident.size(), resident.base(),
                [&](std::size_t i) -> Trade { return resident[i]; },
                spill.get(), barrier,
                [&](bool is_long, std::uint64_t origin, bool margin, const auto& matches) {
                    if (is_long != query.is_long) return false;
                    if (!query.ancestry) return margin && (!query.fifo || matches());
                    for (const auto& cohort : live)
                        if ((!query.fifo || cohort.first == query.name)
                            && cohort.second.count(origin)) return true;
                    return false;
                },
                [&](bool is_long, std::uint64_t origin, bool margin, double quantity) {
                    if (is_long != query.is_long || (!query.ancestry && !margin)) return;
                    const auto part = parts.find(origin);
                    if (part != parts.end()) part->second += quantity;
                }, query.name, summary.work, Diagnostics{});
            std::uint64_t reads = 0;
            for (auto trade = reference.rbegin(); trade != reference.rend(); ++trade) {
                ++reads;
                if (trade->exit_time < barrier) break;
            }
            assert(summary.work.replay_rows == before.replay_rows + reads);
            assert(summary.work.replay_rows - before.replay_rows
                   == summary.work.replay_host_rows - before.replay_host_rows
                      + summary.work.spill_rows - before.spill_rows);
        } else {
            assert(summary.work.replay_rows == before.replay_rows);
        }
        const double expected = old_walk(reference, barrier, live, query);
        assert(std::memcmp(&expected, &actual, sizeof actual) == 0);
        for (const auto& cohort : live)
            if (!query.fifo || cohort.first == query.name)
                for (const auto& origin : cohort.second) assert(parts.count(origin.first) == 1);
        for (const auto& part : parts) {
            double expected_part = 0.0;
            for (auto trade = reference.rbegin(); trade != reference.rend(); ++trade) {
                if (trade->exit_time < barrier) break;
                if (trade->is_long == query.is_long
                    && trade->entry_incarnation == part.first
                    && (query.ancestry || trade->exit_id == source::kMarginCallLabel))
                    expected_part += trade->qty;
            }
            assert(std::memcmp(&expected_part, &part.second, sizeof expected_part) == 0);
        }
        return parts;
    }
};

void live_parts_and_historical_margin_totals() {
    for (std::size_t split = 0; split <= 6; ++split) {
        Fixture f;
        f.add_origin(1);
        f.add_origin(2);
        f.add_origin(3, true, "other");
        f.append(row(1, 4, 10, true, true));
        f.append(row(2, 0.5, 10, true, true));
        f.append(row(3, 8, 11, true, true, "other"));
        f.append(row(2, 2, 12)); // ancestry and margin parts must stay distinct
        f.append(row(1, 0.25, 13));
        f.append(row(2, 0.125, 14, true, true));
        const auto updates = f.summary.work.update_rows;
        f.seal(split);
        for (bool ancestry : {false, true})
            for (bool fifo : {false, true})
                for (const char* name : {"reused", "other", "absent"}) {
                    const auto parts = f.parts_query({ancestry, fifo, true, name}, true);
                    if (fifo && std::string(name) == "reused") {
                        assert(parts.at(1) == (ancestry ? 4.25 : 4.0));
                        assert(parts.at(2) == (ancestry ? 2.625 : 0.625));
                    }
                }
        f.retire(1);
        f.add_origin(4); // the same entry name, a fresh incarnation
        f.append(row(4, 0.0625, 15, true, true));
        const auto named = f.parts_query({false, true, true, "reused"}, true);
        assert(named.count(1) == 0 && named.at(4) == 0.0625);
        // The retired origin's 4 units remain in the scalar margin totals.
        assert(f.query({false, true, true, "reused"}, true) == 4.6875);
        assert(f.query({false, false, true, "reused"}, true) == 12.6875);
        f.parts_query({false, false, true, "reused"}, true);
        f.parts_query({true, false, true, "reused"}, true);
        assert(f.summary.work.update_rows == updates + 1);
        assert(f.summary.resident_origins() == 3);
        f.retire(2);
        f.retire(3);
        f.retire(4);
        assert(f.summary.resident_origins() == 0);
        f.parts_query({false, false, true, "reused"}, true);
    }
}

void combined_parts_reject_and_keep_order() {
    for (std::size_t split = 0; split <= 3; ++split) {
        Fixture f;
        f.add_origin(1);
        f.add_origin(2);
        f.append(row(1, 0x1p53, 10, true, true));
        f.append(row(2, 1, 11, true, true));
        f.append(row(2, 1, 12, true, true));
        f.seal(split);
        for (bool ancestry : {false, true}) {
            const auto parts = f.parts_query({ancestry, true, true, "reused"}, false);
            assert(parts.at(1) == 0x1p53 && parts.at(2) == 2);
        }
    }
    Fixture reused;
    reused.add_origin(1);
    reused.append(row(1, 2, 10, true, true));
    reused.retire(1);
    reused.add_origin(1);
    // Scalar margin alone is exact; the reactivated part is uncertified.
    assert(reused.query({false, true, true, "reused"}, true) == 2);
    reused.parts_query({false, true, true, "reused"}, false);
    reused.summary.retire_origin(1); // current membership with a missing old cache row
    reused.parts_query({false, true, true, "reused"}, false);

    Fixture sides;
    sides.add_origin(1);
    sides.append(row(1, 2, 10, false, true));
    sides.parts_query({false, true, false, "reused"}, false);

    Fixture margin_only;
    margin_only.add_origin(1);
    margin_only.append(row(1, 0x1p53, 10));
    margin_only.append(row(1, 0.5, 11, true, true));
    margin_only.parts_query({false, true, true, "reused"}, true);
    margin_only.parts_query({true, true, true, "reused"}, false);

    Fixture decimal;
    decimal.add_origin(1);
    decimal.add_origin(2);
    decimal.append(row(1, 0.0028, 10, true, true));
    decimal.append(row(1, 1.6712, 11, true, true));
    decimal.append(row(2, 0.125, 12, true, true));
    decimal.seal(2);
    decimal.parts_query({false, true, true, "reused"}, false);

    Fixture boundary;
    boundary.add_origin(1);
    boundary.append(row(1, 4, 10, true, true));
    boundary.retire(1);
    boundary.live.clear();
    boundary.open(10);
    boundary.add_origin(2, false);
    boundary.append(row(2, 0.5, 10, false, true));
    boundary.parts_query({false, true, true, "reused"}, true);
    boundary.parts_query({false, true, false, "reused"}, true);
    boundary.retire(2);
    boundary.live.clear();
    boundary.open(11);
    boundary.add_origin(3);
    boundary.append(row(3, 1, 11, true, true));
    boundary.parts_query({false, true, true, "reused"}, true);
    boundary.summary.invalidate();
    boundary.parts_query({false, true, true, "reused"}, false);
}

void combined_parts_publication_guards() {
    Fixture fresh;
    fresh.add_origin(1);
    fresh.summary.retire_origin(1); // missing, but newer than every completed row
    assert(fresh.parts_query({false, true, true, "reused"}, true).at(1) == 0.0);
    fresh.parts_query({true, true, true, "reused"}, true);

    Fixture missed;
    missed.add_origin(1);
    const auto trade = row(1, 0.5, 10, true, true);
    missed.reference.push_back(trade);
    missed.resident.push_back(trade);
    PositionTailBinding::preserve(missed.spill, missed.resident, 1,
                                 missed.barrier, missed.directory.path);
    missed.resident.seal(1); // deliberately no observer before sealing
    missed.parts_query({false, true, true, "reused"}, false);
    missed.parts_query({true, true, true, "reused"}, false);

    Fixture rewind;
    rewind.add_origin(1);
    rewind.append(trade);
    rewind.reference.clear();
    rewind.resident.resize(0);
    rewind.parts_query({false, true, true, "reused"}, false);

    Fixture amended;
    amended.add_origin(1);
    amended.append(trade);
    amended.parts_query({false, true, true, "reused"}, true);
    amended.reference.back().qty = 0.375;
    amended.resident.modify(amended.resident.size() - 1, [](Trade& t) { t.qty = 0.375; });
    amended.summary.invalidate(source::PineExecutionAdapter::PositionTailGuard::Amendment);
    amended.parts_query({false, true, true, "reused"}, false);

    Fixture order;
    order.add_origin(1);
    order.append(row(1, 4, 10, true, true));
    order.append(row(1, 8, 9, true, true)); // strict barrier cuts off older rows
    order.append(row(1, 0.125, 12, true, true));
    order.parts_query({false, true, true, "reused"}, false);
    order.parts_query({true, true, true, "reused"}, false);

    Fixture aliases;
    aliases.add_origin(1);
    aliases.append(trade);
    aliases.live["other"][1] = 1.0;
    aliases.summary.live_cohort(&*aliases.live.find("other"), true);
    aliases.parts_query({false, false, true, "reused"}, true);
    aliases.parts_query({true, false, true, "reused"}, true);
}

void incremental_margin_parts_work() {
    Fixture f;
    f.add_origin(1);
    std::uint64_t rotating = 2;
    f.add_origin(rotating);
    constexpr std::uint64_t count = 1024;
    for (std::uint64_t i = 0; i < count; ++i) {
        f.append(row(1, 0.5, 10 + static_cast<std::int64_t>(i), true, true));
        f.append(row(rotating, i % 2 ? 0.25 : 0.125,
                     10 + static_cast<std::int64_t>(i), true, true));
        f.parts_query({false, true, true, "reused"}, true);
        f.parts_query({false, false, true, "reused"}, true);
        assert(f.summary.resident_origins() == 2);
        f.retire(rotating);
        f.add_origin(++rotating);
        if (i % 64 == 63) f.seal(f.resident.size() - 2);
    }
    assert(f.summary.work.queries == 2 * count);
    assert(f.summary.work.certified_queries == f.summary.work.queries);
    assert(f.summary.work.fallback_queries == 0 && f.summary.work.replay_rows == 0);
    assert(f.summary.work.update_rows == 2 * count);
    assert(f.summary.work.origin_visits == 2 * f.summary.work.queries);
    f.retire(1);
    f.retire(rotating);
    assert(f.summary.resident_origins() == 0);
}

// Native command-boundary regression: the first FIFO stop publishes rows
// before submit_one sizes the added origin. The old predicate is a label
// comparison, so an admitted user exit can itself publish a matching row.
class IntraCallPartsHost final : public source::PineStrategyHost {
public:
    static constexpr std::int64_t start = INT64_C(1704067200000);
    static constexpr std::int64_t interval = 60000;
    IntraCallPartsHost(std::string label, double first, double added)
        : label_(std::move(label)), first_(first), added_(added) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000;
        config.pyramiding = 2;
        config.calc_on_order_fills = true;
        config.close_entries_rule_any = false;
        config.margin_long = config.margin_short = 0;
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_mintick(0.01);
        set_syminfo_metadata("qty_step", 0.125);
    }
    bool checked = false;

    void on_source_bar(const Bar& bar) override {
        const double missing = std::numeric_limits<double>::quiet_NaN();
        if (bar.timestamp == start && !sent_) {
            sent_ = true;
            strategy_entry("entry", false, missing, missing, first_);
            strategy_exit(label_, "entry", missing, 110, missing, missing, missing, 50);
        }
        if (first_origin_ != 0 && !seeded_) {
            seeded_ = true;
            native_order::Request seed;
            seed.intent = native_order::Reduce{native_order::ExplicitUnits{first_ - 1}};
            seed.label = source::kMarginCallLabel;
            seed.trigger = native_order::Market{};
            const auto accepted = submit(seed);
            assert(accepted.handle);
            const auto outcome = execute_current(
                {*accepted.handle, NativeCurrentPriceRule::AsPresented});
            assert(std::holds_alternative<native_order::ExecutionAppliedEvent>(outcome));
        }
        if (bar.timestamp == start + 2 * interval && !add_sent_) {
            add_sent_ = true;
            strategy_entry("entry", false, missing, missing, added_);
        }
        if (added_origin_ == 0 || checked) return;
        checked = true;
        const auto rows_before = closed_trade_count();
        const auto before = adapter_.position_tail_work();
        inside_exit_ = true;
        strategy_exit(label_, "entry", missing, 90, missing, missing, missing, 50);
        inside_exit_ = false;
        assert(closed_trade_count() > rows_before); // execution really occurred within this call
        double part = 0, crossed = 0;
        for (std::size_t i = closed_trade_count(); i != 0;) {
            const auto trade = get_trade(static_cast<int>(--i));
            if (trade.exit_time < barrier_) break;
            if (!trade.is_long && trade.entry_incarnation == added_origin_) {
                if (i >= rows_before) crossed += trade.qty;
                if (trade.exit_id == source::kMarginCallLabel) part += trade.qty;
            }
        }
        assert(crossed > 0); // FIFO crossed from the carried origin into the add
        if (label_ == source::kMarginCallLabel) assert(part > 0);
        else assert(part == 0); // an ordinary stop row is irrelevant to the old predicate
        const double expected_units = (added_ + part) * 50 / 100;
        std::size_t matched = 0;
        for (const auto& pending : adapter_.fixture_pending_snapshots()) {
            const auto& leg = pending.snapshot;
            if (leg.source_id != label_ || leg.per_entry_origin != added_origin_) continue;
            ++matched;
            assert(std::memcmp(&leg.per_entry_units, &expected_units, sizeof expected_units) == 0);
        }
        assert(matched == 1);
        const auto after = adapter_.position_tail_work();
        assert(after.queries == before.queries + 1);
        assert(after.certified_queries == before.certified_queries + 1);
        assert(after.fallback_queries == before.fallback_queries);
        assert(after.replay_rows == before.replay_rows);
        assert(after.update_rows - before.update_rows == closed_trade_count() - rows_before);
    }

protected:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext& context) override {
        assert(!inside_exit_); // notifications/recalculation do not nest into submit_one
        if (event.opened_units == 0) return;
        if (first_origin_ == 0) {
            first_origin_ = event.handle().incarnation;
            barrier_ = context.script_bar_open_ms;
        } else added_origin_ = event.handle().incarnation;
    }

private:
    std::string label_;
    double first_, added_;
    std::uint64_t first_origin_ = 0, added_origin_ = 0;
    std::int64_t barrier_ = 0;
    bool sent_ = false, seeded_ = false, add_sent_ = false, inside_exit_ = false;
};

void intra_call_parts_follow_publication() {
    for (const auto& quantities : {std::pair<double, double>{4, 8}, {8, 12}})
        for (const char* label : {source::kMarginCallLabel, "ordinary-stop"}) {
            IntraCallPartsHost host(label, quantities.first, quantities.second);
            std::array<Bar, 5> bars{};
            for (std::size_t i = 0; i != bars.size(); ++i)
                bars[i] = {100, 100, 99, 100, 1000,
                           IntraCallPartsHost::start + static_cast<std::int64_t>(i)
                               * IntraCallPartsHost::interval};
            host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
            assert(host.last_error().empty());
            assert(host.checked);
        }
}

void refreshed_part_rejection_finishes_once() {
    Fixture f;
    f.add_origin(1);
    f.append(row(1, 1, 10, true, true));
    f.append(row(1, 1, 11, true, true));
    Parts parts;
    const auto select = [&](const auto& include) { include(1, parts[1]); };
    using Diagnostics = source::detail::PositionTailDiagnostics;
    const auto before = f.summary.work;
    double scalar = 0;
    auto query = f.summary.margin_parts_result(true, nullptr, select, scalar, Diagnostics{});
    assert(query.certified && scalar == 2 && parts.at(1) == 2);
    assert(f.summary.work.queries == before.queries);
    f.append(row(1, 0x1p53, 12, true, true));
    const auto refresh = f.summary.parts_result(true, false, select, Diagnostics{});
    assert(!refresh.certified);
    query.merge(refresh);
    parts.at(1) = 0;
    source::detail::replay_position_tail_split(
        f.resident.size(), f.resident.base(),
        [&](std::size_t i) -> Trade { return f.resident[i]; }, nullptr, f.barrier,
        [](bool, std::uint64_t, bool, const auto&) { return false; },
        [&](bool is_long, std::uint64_t origin, bool margin, double qty) {
            if (is_long && origin == 1 && margin) parts.at(1) += qty;
        }, std::string{}, f.summary.work, Diagnostics{});
    const double expected = old_walk(f.reference, f.barrier, f.live,
                                     {false, false, true, "reused"});
    assert(std::memcmp(&parts.at(1), &expected, sizeof expected) == 0);
    assert(scalar == 2); // the scalar keeps its original publication version
    assert(!f.summary.complete_query(query, Diagnostics{}));
    assert(f.summary.work.queries == before.queries + 1);
    assert(f.summary.work.certified_queries == before.certified_queries);
    assert(f.summary.work.fallback_queries == before.fallback_queries + 1);
    assert(f.summary.work.replay_rows == before.replay_rows + f.reference.size());
    assert(f.summary.work.guard_reason != source::PineExecutionAdapter::PositionTailGuard::None);
    std::uint64_t verdicts = 0;
    for (std::size_t i = 0; i != static_cast<std::size_t>(
             source::PineExecutionAdapter::PositionTailGuard::Count); ++i)
        verdicts += f.summary.work.guard_reasons[i] - before.guard_reasons[i];
    assert(verdicts == 1);
}

void exact_and_current_membership() {
    Fixture f;
    f.add_origin(1);
    f.add_origin(2, true, "other");
    f.append(row(1, 4.0, 10));
    f.append(row(2, 2.0, 11, true, true, "other"));
    f.append(row(1, 0.5, 12, true, true));
    f.live["alias"][1] = 1.0; // the same origin in two allowed cohorts
    f.summary.live_cohort(&*f.live.find("alias"), true);
    f.all_queries(true);
    f.live["alias"].erase(1);
    f.retire(1); // selecting the current map must drop the entire subtotal
    f.all_queries(true);
    f.add_origin(3); // reused string id, distinct incarnation
    f.append(row(3, 0.25, 13));
    f.all_queries(true);
    assert(f.certified == f.queries && f.fallback == 0);

    Fixture reactivated;
    reactivated.add_origin(1);
    reactivated.append(row(1, 2, 10));
    reactivated.retire(1);
    reactivated.add_origin(1);
    assert(reactivated.query({true, true, true, "reused"}) == 2);
    assert(reactivated.fallback == 1); // a discarded subtotal cannot be guessed
}

void certified_splits_and_nested_visibility() {
    for (std::size_t split = 0; split <= 48; ++split) {
        Fixture f;
        f.add_origin(1);
        f.add_origin(2, false, "other");
        for (int i = 0; i != 48; ++i) {
            const bool is_long = i % 2 == 0;
            // Query while the source live maps STILL precede a close debit,
            // just as a nested relative reissue does in Applied.
            f.append(row(is_long ? 1 : 2, 0.125, 10 + i, is_long, i % 3 == 0,
                         is_long ? "reused" : "other"));
            f.all_queries(true);
        }
        const auto before = f.summary.work.update_rows;
        f.seal(split);
        f.all_queries(true);
        assert(f.summary.work.update_rows == before); // storage split is not a new row
        assert(f.fallback == 0 && f.reference.size() == 48);
        assert(f.resident.base() == split);
    }
    Fixture unsignalled;
    unsignalled.add_origin(1);
    const auto newly_visible = row(1, 4, 10);
    unsignalled.reference.push_back(newly_visible);
    unsignalled.resident.push_back(newly_visible);
    assert(unsignalled.query({true, true, true, "reused"}, true) == 4);
    assert(unsignalled.summary.work.update_rows == 1);
}

void barriers_reversals_and_amendments() {
    Fixture f;
    f.add_origin(1);
    f.append(row(1, 4, 10, true, true));
    f.retire(1);
    f.live.clear();
    f.open(10); // strict <, so the prior margin row at 10 is still reachable
    f.add_origin(2, false);
    f.append(row(2, 2, 10, false, true));
    f.all_queries(true);
    assert(f.query({false, true, true, "reused"}, true) == 4.0);
    f.retire(2);
    f.live.clear();
    f.open(11); // both equal-time rows are now behind the barrier
    f.add_origin(3);
    f.append(row(3, 1, 11));
    f.all_queries(true);

    // Re-cut a row without changing the final count. Invalidation must not
    // rely on detecting a smaller count or only on the next append.
    f.reference.back().qty = 0.375;
    f.resident.modify(f.resident.size() - 1, [](Trade& t) { t.qty = 0.375; });
    f.summary.invalidate();
    f.all_queries();
    assert(f.fallback != 0);

    // A first too-old row cuts the walk, even with newer times before it.
    f.append(row(3, 8, 9, true, true));
    f.append(row(3, 0.125, 12, true, true));
    f.all_queries();
    assert(f.query({true, true, true, "reused"}) == 0.125);
    f.add_origin(3); // uncertified reactivation is replayed, not reset to zero
    f.all_queries();
}

void uncertain_publication_and_reset() {
    Fixture missed;
    missed.add_origin(1);
    const auto trade = row(1, 4, 10, true, true);
    missed.reference.push_back(trade);
    missed.resident.push_back(trade);
    // Deliberately omit the before-seal observation. The gap must be
    // detected, never certified as a zero completed quantity.
    PositionTailBinding::preserve(missed.spill, missed.resident, 1,
                                 missed.barrier, missed.directory.path);
    missed.resident.seal(1);
    missed.all_queries();
    assert(missed.fallback == missed.queries);

    Fixture rewind;
    rewind.add_origin(1);
    rewind.append(row(1, 4, 10));
    rewind.reference.clear();
    rewind.resident.resize(0);
    rewind.all_queries();
    assert(rewind.fallback == rewind.queries);
}

void arithmetic_counterexamples() {
    for (bool interleaved : {false, true}) {
        Fixture f;
        f.add_origin(1);
        f.add_origin(2);
        f.append(row(1, 0x1p53, 10));
        f.append(row(interleaved ? 2 : 1, 1, 11));
        f.append(row(1, 1, 12));
        assert(f.query({true, true, true, "reused"}) == 0x1.0000000000001p53);
        assert(f.fallback == 1);
        f.all_queries();
    }
    Fixture grouped;
    for (std::uint64_t origin = 1; origin <= 3; ++origin) grouped.add_origin(origin);
    grouped.append(row(1, 0x1p53, 10));
    grouped.append(row(2, 1, 11));
    grouped.append(row(3, 1, 12));
    assert(grouped.query({true, true, true, "reused"}) == 0x1.0000000000001p53);
    assert(grouped.fallback == 1); // individually exact, combined proof fails

    Fixture drained;
    for (std::uint64_t origin = 1; origin <= 3; ++origin) drained.add_origin(origin);
    drained.append(row(1, 1, 10));
    drained.append(row(2, 0x1p53, 11));
    drained.append(row(3, 1, 12));
    drained.query({true, true, true, "reused"});
    drained.retire(2);
    assert(drained.query({true, true, true, "reused"}, true) == 2);
    // Subtracting the drained 2^53 from a rounded all-origin total gives 0.

    Fixture margin_v6;
    margin_v6.add_origin(1);
    margin_v6.append(row(1, 0.0028, 10, true, true));
    margin_v6.append(row(1, 1.6712, 11, true, true));
    margin_v6.query({false, true, true, "reused"});
    assert(margin_v6.fallback == 1); // W5 quantities from the sizing report
    // Different quantum, total integer overflow, non-finite/negative values,
    // signed zero and subnormal quantities all exercise certification.
    for (double quantity : {0.1, 0x1p-100, 0x1p53, -1.0, -0.0,
                            std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::denorm_min()}) {
        Fixture f;
        f.add_origin(1);
        f.append(row(1, 1, 10));
        f.append(row(1, quantity, 11));
        f.all_queries();
    }
    Fixture overflow;
    overflow.add_origin(1);
    overflow.add_origin(2);
    overflow.append(row(1, 0x1.fffffffffffffp52, 10));
    overflow.append(row(2, 1, 11)); // overflow only when combining origins
    overflow.all_queries();
    assert(overflow.fallback != 0);

    Fixture opposite_side;
    opposite_side.add_origin(1);
    opposite_side.append(row(1, 2, 10, false));
    opposite_side.all_queries();
    assert(opposite_side.fallback != 0);

    Fixture finite_overflow;
    finite_overflow.add_origin(1);
    finite_overflow.append(row(1, 0x1p1023, 10));
    finite_overflow.append(row(1, 0x1p1023, 11));
    assert(std::isinf(finite_overflow.query({true, true, true, "reused"})));
    assert(finite_overflow.fallback == 1);
}

std::uint64_t next(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

void random_sequences() {
    // Repeat every RAM/spill split; vector oracle never reads the spill.
    for (int split = 0; split != 13; ++split) {
        Fixture f;
        std::uint64_t random = 0x71f04ad9 + static_cast<std::uint64_t>(split);
        std::uint64_t incarnation = 0;
        std::int64_t time = 10;
        for (int cycle = 0; cycle != 40; ++cycle) {
            f.live.clear();
            f.open(time); // same-time reopening and alternating position side
            const bool is_long = cycle % 2 == 0;
            const auto first = ++incarnation;
            f.add_origin(first, is_long);
            const auto second = ++incarnation;
            f.add_origin(second, is_long, "other");
            for (int i = 0; i != 12; ++i) {
                const auto value = next(random);
                const std::array<double, 7> quantities{{2, 1, 0.5, 0.125, 0.1, 0x1p53, 0x1p-80}};
                f.append(row(value & 1 ? first : second,
                             quantities[(value >> 4) % quantities.size()],
                             time, is_long, (value & 2) != 0,
                             value & 1 ? "reused" : "other"));
                f.all_queries();
                if (i == split && f.resident.size() > f.resident.base() + 2) {
                    f.seal(f.resident.size() - 2);
                    f.all_queries();
                }
                if (i == 5) {
                    f.retire(first);
                    f.all_queries();
                }
                if (i == 7 && cycle % 3 == 0) {
                    f.reference.back().qty = 0.75;
                    f.resident.modify(f.resident.size() - 1, [](Trade& t) { t.qty = 0.75; });
                    f.summary.invalidate();
                    f.all_queries();
                }
                // Include nonmonotone clocks and strict/equal barriers.
                time += cycle % 5 == 0 && i == 8 ? -2 : 1;
            }
            // All spilled rows are behind the next position's barrier.
            time += 10;
        }
        assert(f.queries > 10000 && f.certified != 0 && f.fallback != 0);
    }
}

void resident_bound() {
    PositionTailSummary summary;
    TradeLedger resident;
    summary.begin_position(10, 0, 0, [&](std::size_t i) -> Trade { return resident[i]; });
    global_allocation::recording = true;
    const auto before = global_allocation::live_bytes;
    std::size_t at_1k = 0;
    for (std::uint64_t origin = 1; origin <= 100000; ++origin) {
        summary.open_origin(origin, true);
        summary.live_cohort(&resident, true);
        const auto trade = row(origin, 2, 10);
        summary.observe(0, static_cast<std::size_t>(origin),
                        [&](std::size_t) -> const Trade& { return trade; });
        double value = 0;
        assert(summary.ancestry(true, [&](const auto& include) { include(origin); }, value));
        assert(value == 2);
        assert(summary.resident_origins() == 1);
        summary.retire_origin(origin);
        summary.live_cohort(&resident, false);
        assert(summary.resident_origins() == 0);
        if (origin == 1000) at_1k = global_allocation::live_bytes;
    }
    assert(summary.work.update_rows == 100000 && summary.work.queries == 100000);
    assert(summary.work.replay_rows == 0);
    assert(global_allocation::live_bytes == at_1k);
    assert(global_allocation::live_bytes == before);
    global_allocation::recording = false;
}

void historical_margin_id_bound() {
    Directory directory;
    PositionTailSummary summary;
    summary.set_directory(directory.path);
    Trade empty{};
    summary.begin_position(10, 0, 0, [&](std::size_t) -> const Trade& { return empty; });
    global_allocation::recording = true;
    const auto before = global_allocation::live_bytes;
    std::size_t at_1k = 0;
    for (std::uint64_t origin = 1; origin <= 10000; ++origin) {
        {
            const auto name = "margin-id-" + std::to_string(origin);
            const auto trade = row(origin, 0.5, 10, true, true, name);
            summary.open_origin(origin, true);
            summary.live_cohort(&trade, true);
            summary.observe(0, static_cast<std::size_t>(origin),
                            [&](std::size_t) -> const Trade& { return trade; });
            double value = 0;
            assert(summary.margin(true, &name, value) && value == 0.5);
            summary.retire_origin(origin);
            summary.live_cohort(&trade, false);
        }
        assert(summary.resident_origins() == 0);
        if (origin == 1000) at_1k = global_allocation::live_bytes;
    }
    assert(summary.work.update_rows == 10000 && summary.work.queries == 10000);
    assert(global_allocation::live_bytes == at_1k && at_1k == before);
    double value = 0;
    const std::string retired = "margin-id-1";
    assert(summary.margin(true, &retired, value) && value == 0.5);
    assert(summary.margin(true, nullptr, value) && value == 5000);
    global_allocation::recording = false;
}
} // namespace

int main() {
    live_parts_and_historical_margin_totals();
    combined_parts_reject_and_keep_order();
    combined_parts_publication_guards();
    incremental_margin_parts_work();
    intra_call_parts_follow_publication();
    refreshed_part_rejection_finishes_once();
    exact_and_current_membership();
    certified_splits_and_nested_visibility();
    barriers_reversals_and_amendments();
    uncertain_publication_and_reset();
    arithmetic_counterexamples();
    random_sequences();
    resident_bound();
    historical_margin_id_bound();
    std::puts("R11 position-tail oracle and resident bound: PASS");
}
