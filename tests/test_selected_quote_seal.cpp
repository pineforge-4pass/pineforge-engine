// A sealed selected run refuses every writer of the report-only terminal quote
// (the bool setter and the void writers).
//
// The real source::PineStrategyHost, driven only from outside, on the QuoteHost
// conventions of test_report_terminal_quote.cpp: the 61-bar one-minute feed run
// one minute to 15 minutes with the magnifier, long and short sides, two lots
// open at the end, the report-only terminal quote at the last input row's time
// and 112.0. The selected run is made only with the public C export
// strategy_set_selected_window_v1 (the window opens at the first row and ends
// long after the last, so the whole feed is the window); nothing here reaches a
// private flag, a bridge or the consumer.
//
// The three writers and what a sealed selected run owes each of them:
//   set_report_terminal_quote (bool)        returns false, stores nothing;
//   set_syminfo_metadata (the two quote keys) and clear_report_terminal_quote
//                                           throw SelectedWindowQueryAfterSeal
//                                           ("selected_window_query_after_seal")
//                                           before any store;
//   strategy_set_syminfo_metadata (C, void) swallows that exception by contract,
//                                           so its refusal is observed through
//                                           strategy_state_query_status_v1.
// None of them may latch a run failure, change the run's error, status or first
// cause, or reopen the capture. The two quote fields are private and get no
// getter; the witness that nothing was stored is a byte copy of the complete
// host object, taken through const unsigned char* immediately before and after
// the one call under test (no report read in between).
//
// Rows (each prints "ROW <label> (<side>): <n> checks, <m> failures"):
//   PRESET-THEN-READ        window set, invalid presets refuse and a valid one is
//                           accepted before begin; the run seals; the first read
//                           presents the quote; every late bool call returns
//                           false; the next read is the same capture and still
//                           reports the preset quote as applied.
//   LATE-BEFORE-FIRST-READ  the same, with the late bool calls made straight
//                           after the seal and before any read.
//   OFF-RETAINS             the same host without a window: the late valid bool
//                           call returns true and the next read presents the new
//                           quote (105.0); a late invalid call still returns
//                           false and leaves that quote in force.
//   LATE-META-KEYS-REFUSED  eight forced writes of the two quote keys through the
//                           C++ set_syminfo_metadata (valid and different, the
//                           stored value itself, invalid): each throws exactly
//                           SelectedWindowQueryAfterSeal with the fixed literal,
//                           leaves the host object bytes, the error, the run
//                           status and the failure record untouched and leaves
//                           strategy_state_query_status_v1 at
//                           PF_STATE_QUERY_SELECTED_WINDOW_AFTER_SEAL_V1; the
//                           capture read afterwards is the first read again.
//   LATE-CLEAR-REFUSED      the same for clear_report_terminal_quote, called twice.
//   LATE-C-BRIDGE-REFUSED   the same eight writes through the real C export
//                           strategy_set_syminfo_metadata: no exception can cross
//                           it, so the checks are bytes, run state and the
//                           query status.
//   OFF-META-CLEAR-UNCHANGED  a handle without a window keeps the original
//                           behaviour: the C++ key write, the C bridge write and
//                           clear are accepted without an exception, change the
//                           object bytes (so the byte witness has teeth and the
//                           bridge provably reaches the quote branch), leave the
//                           query status ALLOWED, and the next read shows the new
//                           quote (105.0, then 108.0) or, after clear, none.
// Each refusal row presets the quote through its own writer with the window
// already configured and before the first admitted begin (C++ keys; clear then the
// bool setter; the C bridge), the order a selected-window harness uses, and reads
// the preset back from the first report: a preset before begin is not refused.
// Every forced table asserts a literal count of witnessed refusals, so a skipped
// case cannot pass; the row sequence stops at the first failing row; the number of
// rows run must equal the number declared.
//
// It is registered in tests/CMakeLists.txt (source-bound: it includes the Pine
// host, so the kernel-only profile skips it).
#include "native_current_fixture.hpp"

#include <pineforge/query_refusal.hpp>
#include <pineforge/selected_window.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <typeinfo>
#include <vector>

using namespace r4_test;

namespace {

TradeC canonical_trade(TradeC row) {
    auto* bytes = reinterpret_cast<unsigned char*>(&row);
    std::memset(bytes + offsetof(TradeC, is_long) + sizeof(row.is_long), 0,
                offsetof(TradeC, max_runup) - offsetof(TradeC, is_long) - sizeof(row.is_long));
    std::memset(bytes + offsetof(TradeC, open_at_end) + sizeof(row.open_at_end), 0,
                sizeof(row) - offsetof(TradeC, open_at_end) - sizeof(row.open_at_end));
    return row;
}

class QuoteHost final : public pineforge::source::PineStrategyHost {
public:
    explicit QuoteHost(bool long_side) : long_side_(long_side) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_value = 2.0;
        config.pyramiding = 2;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.1;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0 || bar_index_ == 1) strategy_entry("open", long_side_);
        if (bar_index_ == 2) strategy_close("open", {}, 1.0);
    }

private:
    bool long_side_;
};

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (int index = 0; index <= 60; ++index) {
        const double price = 100.0 + index * 0.125;
        bars.push_back({price, price + 0.25, price - 0.25, price + 0.125,
                        1.0, T + index * 60000LL});
    }
    return bars;
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr std::int64_t kJsonSafeMax = 9007199254740991LL;
constexpr std::int64_t kInterval = 15 * 60000LL;

// The two reserved syminfo metadata keys of the report-only quote, and the fixed
// literal of the named refusal (include/pineforge/query_refusal.hpp).
constexpr const char* kTimeKey = "report_terminal_quote_time_ms";
constexpr const char* kCloseKey = "report_terminal_quote_close";
constexpr const char* kRefusalText = "selected_window_query_after_seal";

// The host as the C exports see it.
pf_strategy_t as_pf_strategy(QuoteHost& host) { return static_cast<BacktestEngine*>(&host); }

// The window [first row, long after the last row): the whole feed is the window.
bool select_whole_feed(QuoteHost& host, const std::vector<Bar>& bars) {
    pf_selected_window_config_v1 window{};
    window.struct_size = static_cast<std::uint32_t>(sizeof window);
    window.version = 1;
    window.start_ms = bars.front().timestamp;
    window.end_ms = bars.back().timestamp + 1000 * 60000LL;
    return strategy_set_selected_window_v1(as_pf_strategy(host), &window) == 0;
}

void run_feed(QuoteHost& host, const std::vector<Bar>& bars) {
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "15", true);
    CHECK(host.last_error().empty());
}

// A report read, freed on every exit.
struct ReadReport {
    ReportC raw{};
    ReadReport() = default;
    ReadReport(const ReadReport&) = delete;
    ReadReport& operator=(const ReadReport&) = delete;
    ~ReadReport() { BacktestEngine::free_report(&raw); }
};

// Every open-at-end row is marked at the quote, the curve ends at the quote's
// time, and the host's stored quote is the one the rows carry.
void check_quote_presented(const QuoteHost& host, const ReportC& report,
                           std::int64_t quote_time, double close) {
    int open_rows = 0;
    for (int index = 0; index < report.trades_len; ++index) {
        const TradeC& row = report.trades[index];
        if (!row.open_at_end) continue;
        ++open_rows;
        CHECK(row.exit_time == quote_time);
        CHECK(row.exit_price == close);
    }
    CHECK(open_rows > 0);
    CHECK(report.equity_curve_len > 0);
    if (report.equity_curve_len > 0)
        CHECK(report.equity_curve[report.equity_curve_len - 1].time_ms == quote_time);
    CHECK(host.report_terminal_quote_applied(report));
}

// Two reads of one capture agree on every trade row, every curve point and the
// net profit.
void check_same_capture(const ReportC& left, const ReportC& right) {
    CHECK(left.trades_len == right.trades_len);
    CHECK(left.equity_curve_len == right.equity_curve_len);
    CHECK(left.net_profit == right.net_profit);
    if (left.trades_len != right.trades_len
        || left.equity_curve_len != right.equity_curve_len) return;
    for (int index = 0; index < left.trades_len; ++index) {
        const TradeC a = canonical_trade(left.trades[index]);
        const TradeC b = canonical_trade(right.trades[index]);
        CHECK(std::memcmp(&a, &b, sizeof(TradeC)) == 0);
    }
    for (std::int64_t index = 0; index < left.equity_curve_len; ++index)
        CHECK(std::memcmp(&left.equity_curve[index], &right.equity_curve[index],
                          sizeof(left.equity_curve[index])) == 0);
}

// Before begin: invalid values refuse and never disturb the quote in force, the
// valid preset is accepted.
void preset_before_begin(QuoteHost& host, std::int64_t quote_time) {
    CHECK(host.set_report_terminal_quote(quote_time, 112.0));
    CHECK(!host.set_report_terminal_quote(quote_time, kNaN));
    CHECK(!host.set_report_terminal_quote(quote_time, kInf));
    CHECK(!host.set_report_terminal_quote(quote_time, 0.0));
    CHECK(!host.set_report_terminal_quote(quote_time, -1.0));
    CHECK(!host.set_report_terminal_quote(-1, 112.0));
    CHECK(!host.set_report_terminal_quote(kJsonSafeMax + 1, 112.0));
    CHECK(host.set_report_terminal_quote(quote_time, 112.0));
}

// After the seal every bool call returns false: a different valid quote, a valid
// quote at another valid time, the very values already stored, and the invalid
// values.
void refuse_late_calls(QuoteHost& host, std::int64_t quote_time) {
    CHECK(!host.set_report_terminal_quote(quote_time, 105.0));
    CHECK(!host.set_report_terminal_quote(quote_time - kInterval, 105.0));
    CHECK(!host.set_report_terminal_quote(quote_time, 112.0));
    CHECK(!host.set_report_terminal_quote(quote_time, kNaN));
    CHECK(!host.set_report_terminal_quote(-1, 105.0));
}

// A selected run, sealed: the public evidence is the state-query fence closed and
// a successful counts snapshot (only a selected Results generation owns one).
void check_sealed_selected(QuoteHost& host) {
    CHECK(strategy_state_query_status_v1(as_pf_strategy(host))
          == PF_STATE_QUERY_SELECTED_WINDOW_AFTER_SEAL_V1);
    pf_selected_window_counts_v1 counts{};
    counts.struct_size = static_cast<std::uint32_t>(sizeof counts);
    counts.version = 1;
    CHECK(strategy_selected_window_counts_v1(as_pf_strategy(host), &counts) == 0);
}

// What a rejected call must leave alone: the run's error, status and durable
// failure record, and the representation of the complete host object, which
// holds the two private quote fields. The bytes of an object are read through
// const unsigned char* (the aliasing exception for character types); nothing is
// copied back into an object.
struct Witness {
    std::string last_error;
    int run_status = 0;
    NativeLifecycleKind kind = NativeLifecycleKind::Unconfigured;
    NativeFailureCode failure_code = NativeFailureCode::None;
    NativeFailureOperation failure_operation = NativeFailureOperation::None;
    std::uint64_t failure_ordinal = 0;
    std::uint32_t failure_discriminator = 0;
    std::vector<unsigned char> bytes;
};

std::vector<unsigned char> object_bytes(const QuoteHost& host) {
    const auto* first = reinterpret_cast<const unsigned char*>(std::addressof(host));
    return std::vector<unsigned char>(first, first + sizeof(QuoteHost));
}

void read_run_state(const QuoteHost& host, Witness& seen) {
    seen.last_error = host.last_error();
    seen.run_status = host.last_run_status();
    const NativeStateView state = host.native_state();
    seen.kind = state.kind;
    seen.failure_code = state.failure.code;
    seen.failure_operation = state.failure.operation;
    seen.failure_ordinal = state.failure.ordinal;
    seen.failure_discriminator = state.failure.discriminator;
}

// The bytes are the last thing read before the call and the first thing read
// after it, so nothing but the call stands between the two copies.
Witness taken_before(const QuoteHost& host) {
    Witness seen;
    read_run_state(host, seen);
    seen.bytes = object_bytes(host);
    return seen;
}

Witness taken_after(const QuoteHost& host) {
    Witness seen;
    seen.bytes = object_bytes(host);
    read_run_state(host, seen);
    return seen;
}

void check_untouched(const Witness& before, const Witness& after) {
    CHECK(!before.bytes.empty());
    CHECK(before.bytes == after.bytes);
    CHECK(before.last_error == after.last_error);
    CHECK(before.run_status == after.run_status);
    CHECK(before.kind == after.kind);
    CHECK(before.failure_code == after.failure_code);
    CHECK(before.failure_operation == after.failure_operation);
    CHECK(before.failure_ordinal == after.failure_ordinal);
    CHECK(before.failure_discriminator == after.failure_discriminator);
}

bool refused_by_state_query(QuoteHost& host) {
    return strategy_state_query_status_v1(as_pf_strategy(host))
        == PF_STATE_QUERY_SELECTED_WINDOW_AFTER_SEAL_V1;
}

// Forced cases that passed every one of their checks in the current row; each
// refusal row compares it with the literal size of its own table.
int refusals_witnessed = 0;

// One forced C++ write on a sealed selected host: it must throw exactly the named
// refusal (dynamic type and fixed literal; any other exception, or none, fails),
// store nothing, leave the run's error, status and failure record alone, and the
// state query must still name the seal.
template <class Action>
void forced_named_refusal(QuoteHost& host, Action action) {
    const int failures_before = failures;
    const Witness before = taken_before(host);
    bool thrown = false;
    bool exact = false;
    try {
        action();
    } catch (const std::exception& error) {
        thrown = true;
        exact = typeid(error) == typeid(SelectedWindowQueryAfterSeal)
            && std::strcmp(error.what(), kRefusalText) == 0;
    } catch (...) {
        thrown = true;
    }
    const Witness after = taken_after(host);
    CHECK(thrown);
    CHECK(exact);
    check_untouched(before, after);
    CHECK(refused_by_state_query(host));
    if (failures == failures_before) ++refusals_witnessed;
}

// One forced write through the real C export. It is void and swallows every
// exception, so nothing can be caught here: the witness is the untouched bytes and
// run state, and the state query naming the seal.
void forced_bridge_refusal(QuoteHost& host, const char* key, double value) {
    const int failures_before = failures;
    const Witness before = taken_before(host);
    strategy_set_syminfo_metadata(as_pf_strategy(host), key, value);
    const Witness after = taken_after(host);
    check_untouched(before, after);
    CHECK(refused_by_state_query(host));
    if (failures == failures_before) ++refusals_witnessed;
}

template <class Action>
bool completes_quietly(Action action) {
    try {
        action();
    } catch (...) {
        return false;
    }
    return true;
}

struct KeyedWrite {
    const char* key;
    double value;
};

constexpr std::size_t kLateWrites = 8;

// A valid quote at another time, the stored time itself, two invalid times, a
// valid quote at another close, the stored close itself, two invalid closes: on an
// OFF host the invalid ones would clear the stored side, so a refused one that
// still stored would show.
std::vector<KeyedWrite> late_writes(std::int64_t quote_time) {
    return {
        {kTimeKey, static_cast<double>(quote_time - kInterval)},
        {kTimeKey, static_cast<double>(quote_time)},
        {kTimeKey, kNaN},
        {kTimeKey, -1.0},
        {kCloseKey, 105.0},
        {kCloseKey, 112.0},
        {kCloseKey, kNaN},
        {kCloseKey, 0.0},
    };
}

// The route a refusal row presets the quote through. The window is already
// configured when it is used, so each route also shows that a preset before the
// first admitted begin is not refused (the order a selected-window harness uses:
// window, quote, begin).
enum class PresetRoute { Bool, CppKeys, CBridge, ClearThenBool };

// The window set, the valid preset accepted through `route` while the handle is
// still before its first admitted begin, the feed run to its seal: the state every
// refusal row below starts from. Whether the preset took effect is read back by
// the row's first report (the quote applied at 112.0).
void seal_with_preset(QuoteHost& host, const std::vector<Bar>& bars, std::int64_t quote_time,
                      PresetRoute route) {
    REQUIRE(select_whole_feed(host, bars));
    CHECK(strategy_state_query_status_v1(as_pf_strategy(host)) == PF_STATE_QUERY_ALLOWED_V1);
    if (route == PresetRoute::CppKeys) {
        const bool quiet = completes_quietly([&] {
            host.set_syminfo_metadata(kTimeKey, static_cast<double>(quote_time));
            host.set_syminfo_metadata(kCloseKey, 112.0);
        });
        CHECK(quiet);
    } else if (route == PresetRoute::CBridge) {
        strategy_set_syminfo_metadata(as_pf_strategy(host), kTimeKey,
                                      static_cast<double>(quote_time));
        strategy_set_syminfo_metadata(as_pf_strategy(host), kCloseKey, 112.0);
    } else {
        if (route == PresetRoute::ClearThenBool) {
            const bool quiet = completes_quietly([&] { host.clear_report_terminal_quote(); });
            CHECK(quiet);
        }
        preset_before_begin(host, quote_time);
    }
    run_feed(host, bars);
    CHECK(host.last_run_status() == 0);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    check_sealed_selected(host);
}

void preset_then_read(bool long_side) {
    QuoteHost host(long_side);
    const auto bars = feed();
    const std::int64_t quote_time = bars.back().timestamp;
    REQUIRE(select_whole_feed(host, bars));
    // A configured window is pending state only: the handle still answers as an
    // open one until a run is admitted, so the preset below is not refused.
    CHECK(strategy_state_query_status_v1(as_pf_strategy(host)) == PF_STATE_QUERY_ALLOWED_V1);
    preset_before_begin(host, quote_time);
    run_feed(host, bars);
    check_sealed_selected(host);

    ReadReport first;
    host.fill_report(&first.raw);
    check_quote_presented(host, first.raw, quote_time, 112.0);

    refuse_late_calls(host, quote_time);

    ReadReport second;
    host.fill_report(&second.raw);
    check_same_capture(first.raw, second.raw);
    check_quote_presented(host, second.raw, quote_time, 112.0);
}

void late_before_first_read(bool long_side) {
    QuoteHost host(long_side);
    const auto bars = feed();
    const std::int64_t quote_time = bars.back().timestamp;
    REQUIRE(select_whole_feed(host, bars));
    preset_before_begin(host, quote_time);
    run_feed(host, bars);
    check_sealed_selected(host);

    refuse_late_calls(host, quote_time);

    ReadReport read;
    host.fill_report(&read.raw);
    check_quote_presented(host, read.raw, quote_time, 112.0);
}

void off_retains(bool long_side) {
    QuoteHost host(long_side);
    const auto bars = feed();
    const std::int64_t quote_time = bars.back().timestamp;
    preset_before_begin(host, quote_time);
    run_feed(host, bars);
    CHECK(strategy_state_query_status_v1(as_pf_strategy(host)) == PF_STATE_QUERY_ALLOWED_V1);
    pf_selected_window_counts_v1 counts{};
    counts.struct_size = static_cast<std::uint32_t>(sizeof counts);
    counts.version = 1;
    CHECK(strategy_selected_window_counts_v1(as_pf_strategy(host), &counts) == -2);

    ReadReport first;
    host.fill_report(&first.raw);
    check_quote_presented(host, first.raw, quote_time, 112.0);

    // The old behaviour: a late valid call is accepted and the next read shows it.
    CHECK(host.set_report_terminal_quote(quote_time, 105.0));
    ReadReport second;
    host.fill_report(&second.raw);
    check_quote_presented(host, second.raw, quote_time, 105.0);
    CHECK(second.raw.net_profit != first.raw.net_profit);

    // A late invalid call still refuses and leaves the accepted quote in force.
    CHECK(!host.set_report_terminal_quote(quote_time, kNaN));
    CHECK(!host.set_report_terminal_quote(-1, 105.0));
    ReadReport third;
    host.fill_report(&third.raw);
    check_quote_presented(host, third.raw, quote_time, 105.0);
    check_same_capture(second.raw, third.raw);
}

void late_meta_keys_refused(bool long_side) {
    QuoteHost host(long_side);
    const auto bars = feed();
    const std::int64_t quote_time = bars.back().timestamp;
    seal_with_preset(host, bars, quote_time, PresetRoute::CppKeys);

    ReadReport first;
    host.fill_report(&first.raw);
    check_quote_presented(host, first.raw, quote_time, 112.0);

    const std::vector<KeyedWrite> writes = late_writes(quote_time);
    CHECK(writes.size() == kLateWrites);
    refusals_witnessed = 0;
    for (const KeyedWrite& entry : writes)
        forced_named_refusal(host, [&] { host.set_syminfo_metadata(entry.key, entry.value); });
    CHECK(static_cast<std::size_t>(refusals_witnessed) == kLateWrites);

    ReadReport second;
    host.fill_report(&second.raw);
    check_same_capture(first.raw, second.raw);
    check_quote_presented(host, second.raw, quote_time, 112.0);
    CHECK(refused_by_state_query(host));
}

void late_clear_refused(bool long_side) {
    QuoteHost host(long_side);
    const auto bars = feed();
    const std::int64_t quote_time = bars.back().timestamp;
    seal_with_preset(host, bars, quote_time, PresetRoute::ClearThenBool);

    ReadReport first;
    host.fill_report(&first.raw);
    check_quote_presented(host, first.raw, quote_time, 112.0);

    constexpr int kClearCalls = 2;
    refusals_witnessed = 0;
    for (int call = 0; call < kClearCalls; ++call)
        forced_named_refusal(host, [&] { host.clear_report_terminal_quote(); });
    CHECK(refusals_witnessed == kClearCalls);

    ReadReport second;
    host.fill_report(&second.raw);
    check_same_capture(first.raw, second.raw);
    check_quote_presented(host, second.raw, quote_time, 112.0);
    CHECK(refused_by_state_query(host));
}

void late_c_bridge_refused(bool long_side) {
    QuoteHost host(long_side);
    const auto bars = feed();
    const std::int64_t quote_time = bars.back().timestamp;
    seal_with_preset(host, bars, quote_time, PresetRoute::CBridge);

    ReadReport first;
    host.fill_report(&first.raw);
    check_quote_presented(host, first.raw, quote_time, 112.0);

    const std::vector<KeyedWrite> writes = late_writes(quote_time);
    CHECK(writes.size() == kLateWrites);
    refusals_witnessed = 0;
    for (const KeyedWrite& entry : writes) forced_bridge_refusal(host, entry.key, entry.value);
    CHECK(static_cast<std::size_t>(refusals_witnessed) == kLateWrites);

    ReadReport second;
    host.fill_report(&second.raw);
    check_same_capture(first.raw, second.raw);
    check_quote_presented(host, second.raw, quote_time, 112.0);
    CHECK(refused_by_state_query(host));
}

// A handle without a window: the same writers keep their original behaviour. Each
// accepted write changes the object bytes, which is what gives the byte witness of
// the refusal rows its teeth, and the C bridge write changing them shows that the
// bridge reaches the quote branch the refusal rows go through.
void off_meta_clear_unchanged(bool long_side) {
    QuoteHost host(long_side);
    const auto bars = feed();
    const std::int64_t quote_time = bars.back().timestamp;
    preset_before_begin(host, quote_time);
    run_feed(host, bars);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(strategy_state_query_status_v1(as_pf_strategy(host)) == PF_STATE_QUERY_ALLOWED_V1);

    Witness before = taken_before(host);
    const bool meta_quiet = completes_quietly([&] { host.set_syminfo_metadata(kCloseKey, 105.0); });
    Witness after = taken_after(host);
    CHECK(meta_quiet);
    CHECK(before.bytes != after.bytes);
    CHECK(strategy_state_query_status_v1(as_pf_strategy(host)) == PF_STATE_QUERY_ALLOWED_V1);
    ReadReport by_meta;
    host.fill_report(&by_meta.raw);
    check_quote_presented(host, by_meta.raw, quote_time, 105.0);

    before = taken_before(host);
    strategy_set_syminfo_metadata(as_pf_strategy(host), kCloseKey, 108.0);
    after = taken_after(host);
    CHECK(before.bytes != after.bytes);
    CHECK(strategy_state_query_status_v1(as_pf_strategy(host)) == PF_STATE_QUERY_ALLOWED_V1);
    ReadReport by_bridge;
    host.fill_report(&by_bridge.raw);
    check_quote_presented(host, by_bridge.raw, quote_time, 108.0);

    before = taken_before(host);
    const bool clear_quiet = completes_quietly([&] { host.clear_report_terminal_quote(); });
    after = taken_after(host);
    CHECK(clear_quiet);
    CHECK(before.bytes != after.bytes);
    CHECK(strategy_state_query_status_v1(as_pf_strategy(host)) == PF_STATE_QUERY_ALLOWED_V1);
    ReadReport cleared;
    host.fill_report(&cleared.raw);
    CHECK(!host.report_terminal_quote_applied(cleared.raw));
    for (int index = 0; index < cleared.raw.trades_len; ++index) {
        const TradeC& row = cleared.raw.trades[index];
        if (!row.open_at_end) continue;
        CHECK(row.exit_price != 105.0);
        CHECK(row.exit_price != 108.0);
        CHECK(row.exit_price != 112.0);
    }
}

void run_row(const char* label, void (*body)(bool), bool long_side) {
    const int checks_before = checks;
    const int failures_before = failures;
    scenario = label;
    try {
        body(long_side);
    } catch (const Stop&) {
    } catch (const std::exception& error) {
        ++failures;
        std::printf("FAIL %s: exception %s\n", label, error.what());
    }
    std::printf("ROW %s (%s): %d checks, %d failures\n", label, long_side ? "long" : "short",
                checks - checks_before, failures - failures_before);
}

}  // namespace

int main() {
    struct Row {
        const char* label;
        void (*body)(bool);
    };
    const Row rows[] = {
        {"PRESET-THEN-READ", preset_then_read},
        {"LATE-BEFORE-FIRST-READ", late_before_first_read},
        {"OFF-RETAINS", off_retains},
        {"LATE-META-KEYS-REFUSED", late_meta_keys_refused},
        {"LATE-CLEAR-REFUSED", late_clear_refused},
        {"LATE-C-BRIDGE-REFUSED", late_c_bridge_refused},
        {"OFF-META-CLEAR-UNCHANGED", off_meta_clear_unchanged},
    };
    const int rows_declared = static_cast<int>(sizeof rows / sizeof rows[0]) * 2;
    int rows_run = 0;
    bool stopped = false;
    for (bool long_side : {false, true}) {
        for (const Row& row : rows) {
            if (stopped) break;
            run_row(row.label, row.body, long_side);
            ++rows_run;
            stopped = failures != 0;
        }
    }
    if (stopped) std::printf("STOPPED at the first failing row; later rows did not run\n");
    scenario = "row sequence";
    CHECK(rows_run == rows_declared);
    std::printf("selected quote seal: %d rows, %d checks, %d failures\n", rows_run, checks,
                failures);
    return failures == 0 ? 0 : 1;
}
