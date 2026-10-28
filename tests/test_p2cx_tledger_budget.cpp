#include "p2cx_tledger_fixture.hpp"

#include <cstdint>
#include <limits>
#include <utility>

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;

// A sink run keeps the read cache inside one report_sink::kScratchWindowBytes
// (64 KiB, src/report_scratch.hpp): the sink heap-flatness witness
// (test_selected_metrics_l17b) allows 2 windows for ALL working storage, of
// which the sink's own window takes one. 128 x 312 = 39,936 <= 65,536.
static_assert(TradeLedger::kSinkCacheRows * TradeLedger::kLedgerEntryBytes <= 64U * 1024U,
              "sink-run ledger cache exceeds one scratch window");
static_assert(TradeLedger::kSinkCacheRows >= TradeLedger::kResidentRows
              && TradeLedger::kSinkCacheRows <= TradeLedger::kDefaultCacheRows,
              "sink-run ledger cache rows outside the budget range");

void bounded(const TradeLedger& ledger) {
    CHECK(ledger.cache_size() <= ledger.cache_rows());
    CHECK(ledger.cache_resident_bytes()
          <= TradeLedger::resident_cache_bytes(ledger.cache_rows()));
}

void matches(const TradeLedger& ledger, const std::vector<Trade>& expected) {
    REQUIRE(ledger.size() == expected.size());
    for (Size i = 0; i < expected.size(); ++i) {
        same_trade(ledger.read(i), expected[i]);
        bounded(ledger);
        if (i >= ledger.base()) {
            same_trade(ledger[i], expected[i]);
            same_trade(ledger.at(i), expected[i]);
            same_numeric(ledger.numeric_at(i), expected[i]);
            same_numeric_fields(ledger, i, expected[i]);
            same_string_fields(ledger, i, expected[i]);
        }
    }
}

template<class F>
void refuses(F action, const char* reason) {
    bool caught = false;
    try { action(); }
    catch (const coded<std::invalid_argument>& error) {
        caught = true;
        const auto failure = classify_run_failure(error);
        CHECK(failure.code == RunFailureCode::setting_rejected);
        REQUIRE(failure.args);
        CHECK(*failure.args == std::string("{\"input\":\"resident.ledger_rows\",\"reason\":\"")
                               + reason + "\"}");
    }
    REQUIRE(caught);
}

template<class F>
void sealed_refusal(F action) {
    bool caught = false;
    try { action(); }
    catch (const coded<std::logic_error>& error) {
        caught = true;
        CHECK(classify_run_failure(error).code == RunFailureCode::engine_invariant);
    }
    REQUIRE(caught);
}

void budgets_and_accounting() {
    CHECK(TradeLedger::kLedgerEntryBytes == 312);
    CHECK(TradeLedger::kDefaultCacheRows == 65536);
    CHECK(TradeLedger::kMaxCacheRows == 1048576);
    // Exceed the small/default caps. For the maximum setting, verify real
    // lazy allocation with 129 rows instead of constructing a million-row run.
    for (const Size budget : {Size{4}, Size{7}, TradeLedger::kDefaultCacheRows,
                              TradeLedger::kMaxCacheRows}) {
        TradeLedger ledger;
        CHECK(ledger.cache_rows() == TradeLedger::kDefaultCacheRows);
        ledger.set_scratch_directory(".");
        ledger.set_cache_rows(budget);
        REQUIRE(ledger.cache_rows() == budget);
        CHECK(ledger.cache_size() == 0);
        const auto fixed_bytes = ledger.cache_resident_bytes();
        CHECK(TradeLedger::resident_cache_bytes(budget)
              == fixed_bytes + budget * TradeLedger::kLedgerEntryBytes);
        const Size population = budget == TradeLedger::kMaxCacheRows ? 129 : budget + 3;
        const Size allocated_rows = budget == TradeLedger::kMaxCacheRows ? 256 : budget;
        std::vector<Trade> expected;
        for (Size i = 0; i < population; ++i) {
            expected.push_back(row(static_cast<int>(i)));
            ledger.push_back(expected.back());
            CHECK(ledger.cache_size() == 0); // Appending does not populate it.
            bounded(ledger);
        }
        CHECK(ledger.cache_resident_bytes() == fixed_bytes);
        for (Size i = 0; i < expected.size(); ++i) {
            same_trade(ledger.read(i), expected[i]);
            // A miss below the resident window loads up to kResidentRows
            // consecutive rows, so the cache holds at least the rows read so
            // far and at most one block beyond the row just read.
            CHECK(ledger.cache_size() >= std::min(i + 1, allocated_rows));
            CHECK(ledger.cache_size() <= std::min(allocated_rows, i + TradeLedger::kResidentRows));
            CHECK(ledger.cache_resident_bytes()
                  == fixed_bytes + allocated_rows * TradeLedger::kLedgerEntryBytes);
            bounded(ledger);
        }
        // Reverse traversal reuses/evicts the direct-mapped entries and
        // checks the value of colliding keys against independent rows.
        for (Size i = expected.size(); i != 0; --i) {
            same_trade(ledger.read(i - 1), expected[i - 1]);
            CHECK(ledger.cache_size() == std::min(population, allocated_rows));
            bounded(ledger);
        }
        CHECK(ledger.resident_size()
              == std::min(expected.size(), TradeLedger::kResidentRows));
        ledger.clear();
        CHECK(ledger.cache_rows() == budget);
        CHECK(ledger.cache_size() == 0);
        CHECK(ledger.cache_resident_bytes() == fixed_bytes);
    }
}

void lazy_growth_boundaries() {
    for (const Size budget : {TradeLedger::kDefaultCacheRows, TradeLedger::kMaxCacheRows}) {
        TradeLedger ledger;
        ledger.set_scratch_directory(".");
        ledger.set_cache_rows(budget);
        const auto fixed_bytes = ledger.cache_resident_bytes();
        std::vector<Trade> expected;
        for (int n = 0; n < 64; ++n) {
            expected.push_back(row(n));
            ledger.push_back(expected.back());
        }
        CHECK(ledger.cache_resident_bytes() == fixed_bytes); // No read yet.
        matches(ledger, expected);
        CHECK(ledger.cache_size() == 64);
        CHECK(ledger.cache_resident_bytes() == fixed_bytes + 64 * TradeLedger::kLedgerEntryBytes);
        expected[0] = row(700);
        ledger.replace(0, expected[0]);
        same_numeric_fields(ledger, 0, expected[0]);
        same_string_fields(ledger, 0, expected[0]);
        CHECK(ledger.cache_size() == 64);
        expected.push_back(row(64));
        ledger.push_back(expected.back()); // Crossing 64 grows to 128.
        CHECK(ledger.cache_size() == 0);
        CHECK(ledger.cache_resident_bytes() == fixed_bytes + 128 * TradeLedger::kLedgerEntryBytes);
        matches(ledger, expected);
        for (int n = 65; n < 129; ++n) {
            expected.push_back(row(n));
            ledger.push_back(expected.back());
        }
        CHECK(ledger.cache_size() == 0); // Crossing 128 invalidates again.
        CHECK(ledger.cache_resident_bytes() == fixed_bytes + 256 * TradeLedger::kLedgerEntryBytes);
        matches(ledger, expected);
        ledger.resize(64);
        expected.resize(64);
        expected.push_back(row(900));
        ledger.push_back(expected.back());
        // Reused keys after growth/rewind must return the new oracle values.
        same_numeric_fields(ledger, 64, expected[64]);
        same_string_fields(ledger, 64, expected[64]);
        matches(ledger, expected);
        bounded(ledger);
    }
}

void invalid_and_late_budgets() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    ledger.set_cache_rows(7);
    for (const Size bad : {Size{0}, TradeLedger::kMaxCacheRows + 1,
                           std::numeric_limits<Size>::max()}) {
        const auto* reason = bad == 0 ? "value_below_minimum" : "value_above_maximum";
        refuses([&] { ledger.set_cache_rows(bad); }, reason);
        refuses([&] { (void)TradeLedger::resident_cache_bytes(bad); }, reason);
        CHECK(ledger.cache_rows() == 7);
        CHECK(ledger.cache_size() == 0);
    }
    ledger.push_back(row(0));
    refuses([&] { ledger.set_cache_rows(3); }, "frozen");
    same_trade(ledger.read(0), row(0));
    ledger.resize(0);
    // Rewinding to empty does not make already-open scratch configurable.
    refuses([&] { ledger.set_cache_rows(3); }, "frozen");
    ledger.clear();
    ledger.set_cache_rows(3);
    CHECK(ledger.cache_rows() == 3);
}

void replacements_and_rewinds() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    ledger.set_cache_rows(4);
    std::vector<Trade> expected;
    for (int n = 0; n < 4; ++n) {
        expected.push_back(row(n));
        ledger.push_back(expected.back());
    }
    matches(ledger, expected);
    const Trade retained = ledger.read(0);
    const Trade& borrowed = ledger.materialize(0);
    expected[0] = row(40);
    expected[0].open_at_end = true;
    ledger.replace(0, expected[0]);
    // Read BEFORE an occupancy assertion: removing invalidation must be
    // caught by the oracle comparison itself, not just cache bookkeeping.
    same_trade(ledger.read(0), expected[0]);
    same_trade(borrowed, expected[0]);
    same_trade(retained, row(0));
    expected[1].entry_comment.assign("modified\0id", 11);
    expected[1].pnl = -42.25;
    ledger.modify(1, [](Trade& value) {
        value.entry_comment.assign("modified\0id", 11);
        value.pnl = -42.25;
    });
    matches(ledger, expected);
    ledger.resize(2);
    CHECK(ledger.cache_size() == 0);
    expected.resize(2);
    for (int n = 50; n < 52; ++n) {
        expected.push_back(row(n));
        ledger.push_back(expected.back());
    }
    matches(ledger, expected); // Same global keys, different strings/numbers.
    ledger.resize(6);
    expected.resize(6);
    CHECK(ledger.cache_size() == 0);
    matches(ledger, expected);
    const auto cached_before_seal = ledger.cache_size();
    CHECK(cached_before_seal != 0);
    ledger.seal(2);
    // Publication changes no row content: the cached entries stay valid.
    CHECK(ledger.cache_size() == cached_before_seal);
    matches(ledger, expected); // read() still serves the sealed prefix.
    sealed_refusal([&] { (void)ledger.at(0); });
    sealed_refusal([&] { (void)ledger.numeric_at(0); });
    sealed_refusal([&] { (void)ledger.entry_id_at(0); });
    sealed_refusal([&] { (void)ledger.pnl_at(0); });
    sealed_refusal([&] { (void)ledger.pnl_pct_at(0); });
    sealed_refusal([&] { (void)ledger.commission_at(0); });
    sealed_refusal([&] { (void)ledger.entry_bar_index_at(0); });
    sealed_refusal([&] { (void)ledger.exit_bar_index_at(0); });
    sealed_refusal([&] { (void)ledger.entry_incarnation_at(0); });
    sealed_refusal([&] { (void)ledger.entry_price_at(0); });
    sealed_refusal([&] { (void)ledger.exit_price_at(0); });
    sealed_refusal([&] { (void)ledger.entry_time_at(0); });
    sealed_refusal([&] { (void)ledger.exit_time_at(0); });
    sealed_refusal([&] { (void)ledger.qty_at(0); });
    sealed_refusal([&] { (void)ledger.max_runup_at(0); });
    sealed_refusal([&] { (void)ledger.max_drawdown_at(0); });
    sealed_refusal([&] { (void)ledger.is_long_at(0); });
    sealed_refusal([&] { (void)ledger.entry_comment_at(0); });
    sealed_refusal([&] { (void)ledger.exit_comment_at(0); });
    sealed_refusal([&] { (void)ledger.exit_id_at(0); });
    sealed_refusal([&] { (void)ledger.pnl_at(ledger.size()); });
    sealed_refusal([&] { (void)ledger.exit_id_at(ledger.size()); });
    ledger.clear();
    CHECK(ledger.cache_rows() == 4);
    CHECK(ledger.cache_size() == 0);
    expected = {row(90)};
    ledger.push_back(expected[0]);
    matches(ledger, expected);
}

void copies_and_moves_start_cold() {
    TradeLedger original;
    original.set_scratch_directory(".");
    original.set_cache_rows(4);
    std::vector<Trade> expected;
    for (int n = 0; n < 4; ++n) {
        expected.push_back(row(n));
        original.push_back(expected.back());
    }
    original.seal(2);
    matches(original, expected);
    const auto fixed_bytes = TradeLedger::resident_cache_bytes(4)
        - 4 * TradeLedger::kLedgerEntryBytes;
    TradeLedger copy(original);
    CHECK(copy.cache_rows() == 4 && copy.cache_size() == 0);
    CHECK(copy.cache_resident_bytes() == fixed_bytes);
    CHECK(original.cache_size() == 4); // Copy need not warm/evict its source.
    CHECK(copy.base() == 2);
    original.replace(2, row(80));
    matches(copy, expected);
    TradeLedger assigned;
    assigned.set_scratch_directory(".");
    assigned.set_cache_rows(1);
    assigned.push_back(row(81));
    same_trade(assigned.read(0), row(81));
    assigned = copy;
    CHECK(assigned.cache_rows() == 4 && assigned.cache_size() == 0);
    CHECK(assigned.cache_resident_bytes() == fixed_bytes);
    copy.modify(2, [](Trade& value) { value.exit_id = "copy-only"; });
    matches(assigned, expected);
    TradeLedger moved(std::move(assigned));
    CHECK(moved.cache_rows() == 4 && moved.cache_size() == 0);
    CHECK(assigned.empty() && assigned.cache_size() == 0);
    matches(moved, expected);
    TradeLedger destination;
    destination.set_scratch_directory(".");
    destination.push_back(row(82));
    same_trade(destination.read(0), row(82));
    destination = std::move(moved);
    CHECK(destination.cache_rows() == 4 && destination.cache_size() == 0);
    CHECK(moved.empty() && moved.cache_size() == 0);
    matches(destination, expected);
    for (auto* empty : {&assigned, &moved}) {
        empty->set_scratch_directory(".");
        empty->set_cache_rows(2);
        empty->push_back(row(83));
        same_trade(empty->read(0), row(83));
        bounded(*empty);
    }
}

void exact_string_allowance_and_numeric_bits() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    ledger.set_cache_rows(4);
    Trade value = row(0);
    const Size other_bytes = value.entry_id.size() + value.entry_comment.size()
        + value.exit_id.size();
    value.exit_comment.assign(TradeLedger::kCacheStringBytes - other_bytes, 'C');
    value.exit_comment[10] = '\0';
    const std::uint64_t nan_bits = UINT64_C(0x7ff8000000000a51);
    std::memcpy(&value.entry_price, &nan_bits, sizeof(nan_bits));
    value.exit_price = std::numeric_limits<double>::infinity();
    value.max_drawdown = -std::numeric_limits<double>::infinity();
    value.commission = -0.0;
    value.open_at_end = true;
    ledger.push_back(value);
    same_trade(ledger.read(0), value);
    CHECK(ledger.cache_size() == 1); // Exactly 128 bytes is admitted.
    same_numeric(ledger.numeric_at(0), value);
    same_numeric_fields(ledger, 0, value);
    same_string_fields(ledger, 0, value);
    same_trade(ledger.read(0), value); // Hit preserves the NaN payload/signs.
    Trade wider = value;
    wider.exit_comment.push_back('\0');
    ledger.push_back(wider);
    for (int pass = 0; pass < 3; ++pass) {
        same_trade(ledger.read(1), wider);
        same_numeric_fields(ledger, 1, wider);
        same_string_fields(ledger, 1, wider);
        CHECK(ledger.cache_size() == 2); // 129 bytes: numeric cache, string fallback.
        bounded(ledger);
    }
    ledger.replace(0, wider);
    same_trade(ledger.read(0), wider);
    CHECK(ledger.cache_size() == 2);
    const Trade empty{};
    ledger.push_back(empty);
    same_trade(ledger.read(2), empty);
    CHECK(ledger.cache_size() == 3);
    TradeLedger copied(ledger); // Numeric-only entries cannot decode packed strings.
    CHECK(copied.cache_size() == 0);
    same_trade(copied.read(0), wider);
    same_trade(copied.read(1), wider);
    same_trade(copied.read(2), empty);
    ledger.seal(ledger.size()); // Force external descriptors for the next miss.
    same_trade(ledger.read(0), wider);
    same_trade(ledger.read(2), empty);
    bounded(ledger);
}
} // namespace

int main() {
    test("small/default/maximum budgets and exact allocation accounting", budgets_and_accounting);
    test("lazy 64/128/256 growth and reused-key oracle", lazy_growth_boundaries);
    test("typed invalid and frozen budget refusals", invalid_and_late_budgets);
    test("replace/modify/resize/seal/clear preserve oracle values", replacements_and_rewinds);
    test("copy/move discard cache and preserve independent ledgers", copies_and_moves_start_cold);
    test("inline string boundary and exact numeric representations", exact_string_allowance_and_numeric_bits);
    std::printf("test_p2cx_tledger_budget: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
