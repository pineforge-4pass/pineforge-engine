// One native engine per case: OFF -> selected ON -> OFF, and the reverse.
// Real orders leave a populated ledger at each subsequent begin.
#include <pineforge/native_host.hpp>
#include <pineforge/report_sink.h>
#include <pineforge/selected_window.h>
#include <pineforge/trade_ledger.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

// Same native bridges as test_report_sink_selected_l17a / l17b; no private
// consumer header or production test accessor is needed.
namespace pineforge {
inline namespace engine_script_run_v19 {
int native_set_selected_window_v1(
    BacktestEngine*, const pf_selected_window_config_v1*) noexcept;
int native_selected_window_counts_v1(
    const BacktestEngine*, pf_selected_window_counts_v1*) noexcept;
}
}

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

int checks = 0;
int failures = 0;
const char* scenario = "setup";
struct Stop {};
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    std::fprintf(stderr, "FAIL %s %s:%d: %s\n", scenario, __FILE__, __LINE__, #condition); \
} } while (0)
#define REQUIRE(condition) do { const bool ok = bool(condition); CHECK(ok); \
    if (!ok) throw Stop{}; } while (0)

constexpr std::int64_t kStart = 1704067200000LL;
constexpr std::int64_t kMinute = 60000;
constexpr int kClosedTrades = 240;
constexpr int kBars = 2 * kClosedTrades + 3;
static_assert(kClosedTrades >= 200
              && kClosedTrades > TradeLedger::kSinkCacheRows
              && kClosedTrades < TradeLedger::kDefaultCacheRows,
              "the fixture must distinguish the sink and default budgets");

struct Host final : NativeStrategyHost {
    int calculations = 0;
    int begins = 0;

    void on_native_run_begin() override {
        calculations = 0; // Reset only the strategy's own per-run counter.
        ++begins;
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const int index = calculations++;
        if (index >= 2 * kClosedTrades) return;
        // Alternate long/short round trips, using the L17b TradeStorageHost
        // submit/settlement idiom. Three quiet bars let the last exit fill.
        const double direction = (index / 2) % 2 == 0 ? 1.0 : -1.0;
        const double units = index % 2 == 0 ? direction : -direction;
        const auto result = submit({no::Transact{units}, "round-trip", "reuse"});
        CHECK(result.status == no::SubmitStatus::Accepted);
        CHECK(result.handle.has_value());
    }

    // Read-only fixture seam, like p2cx_tledger::Reader::ledger().
    const TradeLedger& ledger() const { return trades_; }
};

struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
};

NativeRunSpec spec(std::uint64_t run_number) {
    NativeRunSpec result;
    result.identity = {"p2cx-ledger-budget-reuse", run_number};
    result.input_tf = result.script_tf = "1";
    result.ticker = "MOCK";
    result.tickerid = "TEST:MOCK";
    result.type = "crypto";
    result.currency = "USD";
    result.timezone = "UTC";
    result.session = "24x7";
    result.initial_capital = 1000.0;
    result.point_value = result.account_fx = 1.0;
    result.price_tick = 0.01;
    result.fee_kind = NativeFeeKind::CashPerExecution;
    result.fee_value = 0.03125;
    result.close_execution = NativeCloseExecution::NextEligiblePoint;
    result.report_policy = NativeReportPolicy::KernelRecorded;
    return result;
}

std::vector<Bar> tape() {
    std::vector<Bar> bars;
    bars.reserve(kBars);
    for (int i = 0; i < kBars; ++i) {
        const double price = 100.0 + ((i / 2) % 7 - 3) * 0.001
            + (i % 2 == 0 ? 0.0 : 0.125);
        bars.push_back({price, price, price, price, 1.0, kStart + i * kMinute});
    }
    return bars;
}

// Encode fields separately, never struct padding or std::string internals.
// Scalars retain their exact object bytes (including floating signed zero);
// strings have explicit lengths and retain all bytes, including embedded NULs.
template<class T>
void append_field(std::string& bytes, const T& value) {
    bytes.append(reinterpret_cast<const char*>(&value), sizeof value);
}

void append_field(std::string& bytes, const std::string& value) {
    append_field(bytes, static_cast<std::uint64_t>(value.size()));
    bytes.append(value);
}

std::string trade_bytes(const Trade& row) {
    std::string bytes;
    append_field(bytes, row.entry_time);
    append_field(bytes, row.exit_time);
    append_field(bytes, row.entry_price);
    append_field(bytes, row.exit_price);
    append_field(bytes, row.qty);
    append_field(bytes, row.pnl);
    append_field(bytes, row.pnl_pct);
    append_field(bytes, row.is_long);
    append_field(bytes, row.entry_bar_index);
    append_field(bytes, row.exit_bar_index);
    append_field(bytes, row.entry_id);
    append_field(bytes, row.entry_comment);
    append_field(bytes, row.exit_comment);
    append_field(bytes, row.exit_id);
    append_field(bytes, row.exit_from_bracket);
    append_field(bytes, row.max_runup);
    append_field(bytes, row.max_drawdown);
    append_field(bytes, row.commission);
    append_field(bytes, row.entry_incarnation);
    append_field(bytes, row.open_at_end);
    append_field(bytes, row.close_cause);
    return bytes;
}

struct Trades {
    std::size_t count = 0;
    std::uint64_t digest = 14695981039346656037ULL; // FNV-1a offset basis.
    std::vector<std::string> rows;
};

Trades snapshot(const Host& host) {
    Trades result;
    result.count = host.closed_trade_count();
    REQUIRE(result.count == static_cast<std::size_t>(kClosedTrades));
    result.rows.reserve(result.count);
    for (std::size_t i = 0; i < result.count; ++i) {
        const auto row = host.closed_trade(i); // Public owning read, all history.
        CHECK(!row.open_at_end);
        CHECK(row.entry_incarnation != 0);
        result.rows.push_back(trade_bytes(row));
        for (unsigned char byte : result.rows.back()) {
            result.digest ^= byte;
            result.digest *= 1099511628211ULL;
        }
    }
    return result;
}

void compare_trades(const Trades& actual, const Trades& expected) {
    REQUIRE(actual.count == expected.count);
    CHECK(actual.digest == expected.digest);
    for (std::size_t i = 0; i < expected.count; ++i) {
        if (actual.rows[i] != expected.rows[i])
            std::fprintf(stderr, "%s: differing closed trade %zu\n", scenario, i);
        CHECK(actual.rows[i] == expected.rows[i]); // Every field, bit for bit.
    }
}

Trades run(Host& host, const std::vector<Bar>& bars, std::uint64_t run_number,
           bool sink_on, const std::string& directory) {
    REQUIRE(host.configure_native(spec(run_number)).status == NativeSetupStatus::Applied);
    const pf_selected_window_config_v1 window{
        sizeof window, 1, bars.front().timestamp, bars.back().timestamp + kMinute};
    REQUIRE(native_set_selected_window_v1(&host, sink_on ? &window : nullptr) == 0);
    if (sink_on) {
        const auto path = directory + "/" + scenario + "-" + std::to_string(run_number) + ".ndjson";
        const pf_report_sink_v1 sink{
            sizeof sink, 1, path.c_str(), scenario, directory.c_str()};
        REQUIRE(host.set_report_sink_v1(&sink) == 0);
    }

    host.run(bars.data(), static_cast<int>(bars.size()));
    if (!host.last_error().empty())
        std::fprintf(stderr, "%s run %llu: %s\n", scenario,
                     static_cast<unsigned long long>(run_number), host.last_error().c_str());
    CHECK(host.last_error().empty());
    REQUIRE(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.last_run_status() == 0);
    CHECK(host.begins == static_cast<int>(run_number));
    CHECK(host.calculations == static_cast<int>(bars.size()));
    const auto budget = sink_on ? TradeLedger::kSinkCacheRows : TradeLedger::kDefaultCacheRows;
    CHECK(host.ledger().cache_rows() == budget);

    Report report;
    host.fill_report(&report.value); // Completes the selected sink's trailer.
    CHECK(host.last_error().empty());
    REQUIRE(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.last_run_status() == 0);
    CHECK(report.value.total_trades == kClosedTrades);
    if (sink_on) {
        CHECK(report.value.trades == nullptr && report.value.trades_len == 0);
        pf_report_sink_receipt_v1 receipt{};
        receipt.struct_size = sizeof receipt;
        receipt.version = 1;
        REQUIRE(host.report_sink_receipt_v1(&receipt) == 0);
        CHECK(receipt.complete == 1);
        CHECK(receipt.count_t == kClosedTrades);
        pf_selected_window_counts_v1 counts{};
        counts.struct_size = sizeof counts;
        counts.version = 1;
        REQUIRE(native_selected_window_counts_v1(&host, &counts) == 0);
        CHECK(counts.preroll_input_bars == 0 && counts.preroll_script_bars == 0);
        CHECK(counts.window_input_bars == bars.size());
        CHECK(counts.window_script_bars == bars.size());
    } else {
        CHECK(report.value.trades_len == kClosedTrades);
    }
    auto trades = snapshot(host);
    std::printf("%s run %llu sink-%s: %zu closed, digest %016llx\n", scenario,
                static_cast<unsigned long long>(run_number), sink_on ? "ON" : "OFF",
                trades.count, static_cast<unsigned long long>(trades.digest));
    return trades;
}

void reuse(bool sink_first, const std::string& directory) {
    Host host; // This exact engine handle serves all three runs in this case.
    const auto bars = tape();
    const auto first = run(host, bars, 1, sink_first, directory);
    if (sink_first) REQUIRE(host.set_report_sink_v1(nullptr) == 0);
    const auto second = run(host, bars, 2, !sink_first, directory);
    compare_trades(second, first);
    if (!sink_first) REQUIRE(host.set_report_sink_v1(nullptr) == 0);
    const auto third = run(host, bars, 3, sink_first, directory);
    compare_trades(third, first);
}

std::string scratch_directory() {
    // CMake compiles the absolute source path. Keep even the sink-OFF
    // ledger's default-directory scratch below the worktree's scratch-s4e.
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path() / "scratch-s4e";
    std::filesystem::create_directories(root);
    auto pattern = (root / "p2cx-reuse-XXXXXX").string();
    const char* created = ::mkdtemp(pattern.data());
    REQUIRE(created != nullptr);
    const std::string directory = created;
    std::filesystem::current_path(directory);
    // Leave output files for review; this test never deletes its artifacts.
    return directory;
}

void test_case(const char* label, bool sink_first, const std::string& directory) {
    scenario = label;
    try {
        reuse(sink_first, directory);
    } catch (const Stop&) {
        // REQUIRE has already counted the failed assertion.
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", scenario, error.what());
    } catch (...) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: unexpected exception\n", scenario);
    }
}
} // namespace

int main() {
    try {
        const auto directory = scratch_directory();
        std::printf("reuse artifacts: %s\n", directory.c_str());
        test_case("off-on-off", false, directory);
        test_case("on-off-on", true, directory);
    } catch (const Stop&) {
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL setup: %s\n", error.what());
    } catch (...) {
        ++failures;
        std::fprintf(stderr, "FAIL setup: unexpected exception\n");
    }
    std::printf("ledger budget handle reuse: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
