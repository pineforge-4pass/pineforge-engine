// R11: independent old-walk oracle, not a second invocation of the summary.
// Compile first against 583c2217d2's header closure (the helper is absent).
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "../src/source/pine_position_tail_summary.hpp"
#include "../src/source/pine_position_tail_binding.hpp"

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
                               [&](std::size_t i) -> const Trade& { return resident[i]; });
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
                        [&](std::size_t i) -> const Trade& { return resident[i]; });
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
                [&](std::size_t i) -> const Trade& { return resident[i]; },
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
};

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
    f.reference.back().qty = f.resident.back().qty = 0.375;
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
                    f.reference.back().qty = f.resident.back().qty = 0.75;
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
    summary.begin_position(10, 0, 0, [&](std::size_t i) -> const Trade& { return resident[i]; });
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
