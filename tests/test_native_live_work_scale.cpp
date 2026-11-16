// SPDX-License-Identifier: Apache-2.0
// The work for one new input must not grow with the history behind it.
//
//   ledger  Every ledger call the runner makes for one new feed message
//           (runner/main.cpp consume_message and pulse) and one iteration of
//           the delivery scheduler (runner/delivery.cpp DeliveryWorker::run),
//           on ledgers holding two very different numbers of prior inputs.
//           Counted by the link observers below: the SQLite VM steps of every
//           statement, and the rows read by table scans or automatic indexes.
//   report  One cumulative report line after one new input (what
//           --report-jsonl prints), on two histories. Counted as heap
//           allocations: serializing a JSON node allocates, copying text
//           that is already serialized does not.
//
// Both rows compare the same fixed batch of new inputs on the two sizes. No
// time is measured or asserted anywhere in this file.
#include "../runner/report.hpp"
#include "../runner/store.hpp"

#include <sqlite3.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

using namespace pineforge::live;

namespace {

struct Work {
    std::uint64_t statements = 0;
    std::uint64_t vm_steps = 0;     // SQLITE_STMTSTATUS_VM_STEP, summed at finalize
    std::uint64_t scanned_rows = 0; // FULLSCAN_STEP + AUTOINDEX: rows read with no usable index
    std::uint64_t result_rows = 0;
    std::uint64_t worst_steps = 0;  // the single most expensive statement
    std::string worst_sql;
};

std::mutex work_mutex;
Work work;
std::atomic<std::uint64_t> allocations{0};

void reset_work() {
    std::lock_guard<std::mutex> lock(work_mutex);
    work = {};
}

Work take_work() {
    std::lock_guard<std::mutex> lock(work_mutex);
    return work;
}

void* counted_allocation(std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}

} // namespace

// GNU --wrap observers for the real ledger: every sqlite3_step and
// sqlite3_finalize reference in the runner support library lands here first.
// No statement, result or counter is replaced.
extern "C" int __real_sqlite3_step(sqlite3_stmt*);
extern "C" int __real_sqlite3_finalize(sqlite3_stmt*);

extern "C" int __wrap_sqlite3_step(sqlite3_stmt* statement) {
    const int result = __real_sqlite3_step(statement);
    if (result == SQLITE_ROW) {
        std::lock_guard<std::mutex> lock(work_mutex);
        ++work.result_rows;
    }
    return result;
}

extern "C" int __wrap_sqlite3_finalize(sqlite3_stmt* statement) {
    if (statement) {
        const auto steps = static_cast<std::uint64_t>(
            sqlite3_stmt_status(statement, SQLITE_STMTSTATUS_VM_STEP, 0));
        const auto scanned = static_cast<std::uint64_t>(
            sqlite3_stmt_status(statement, SQLITE_STMTSTATUS_FULLSCAN_STEP, 0)) +
            static_cast<std::uint64_t>(sqlite3_stmt_status(statement, SQLITE_STMTSTATUS_AUTOINDEX, 0));
        const char* sql = sqlite3_sql(statement);
        std::lock_guard<std::mutex> lock(work_mutex);
        ++work.statements;
        work.vm_steps += steps;
        work.scanned_rows += scanned;
        if (steps > work.worst_steps) {
            work.worst_steps = steps;
            work.worst_sql = sql ? sql : "";
        }
    }
    return __real_sqlite3_finalize(statement);
}

// Count every allocation of this process; the report row reads the counter
// around the calls it measures.
void* operator new(std::size_t size) { return counted_allocation(size); }
void* operator new[](std::size_t size) { return counted_allocation(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

int failures = 0;

#define REQUIRE(condition) do { if (!(condition)) \
    throw std::runtime_error(std::string("requirement failed: ") + #condition + \
        " (" + __FILE__ + ":" + std::to_string(__LINE__) + ")"); } while (false)

// The allowance for the larger history: a quarter above the smaller one.
// Work that follows history is about large/small times the smaller count
// (100 times at the default sizes), so it cannot fit.
bool within(std::uint64_t small, std::uint64_t large, std::uint64_t slack = 0) {
    return large <= small + small / 4 + slack;
}

void check(const char* row, const char* what, std::uint64_t small_inputs, std::uint64_t small,
           std::uint64_t large_inputs, std::uint64_t large, std::uint64_t slack = 0) {
    const bool passed = within(small, large, slack);
    std::printf("%s %s %s: %llu prior inputs -> %llu, %llu prior inputs -> %llu (allowed %llu)\n",
        passed ? "ok  " : "FAIL", row, what,
        static_cast<unsigned long long>(small_inputs), static_cast<unsigned long long>(small),
        static_cast<unsigned long long>(large_inputs), static_cast<unsigned long long>(large),
        static_cast<unsigned long long>(small + small / 4 + slack));
    if (!passed) ++failures;
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        // Seeding commits every prior input durably. A memory filesystem keeps
        // that quick; the counted statement work does not depend on the medium.
        const auto base = ::access("/dev/shm", W_OK | X_OK) == 0
            ? std::filesystem::path("/dev/shm") : std::filesystem::temp_directory_path();
        auto pattern = (base / "pineforge-work-scale-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        if (!mkdtemp(name.data())) throw std::runtime_error("cannot create a work directory");
        path = name.data();
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

// ---------------------------------------------------------------------------
// ledger
// ---------------------------------------------------------------------------

constexpr const char* deployment = "work-scale";
constexpr std::uint64_t batch = 8;          // new inputs per measured batch
constexpr std::uint64_t routed_every = 500; // one seeded input in 500 sends a webhook action
constexpr std::uint64_t clock_ms = 1700000000000ULL;

std::string feed_message(std::uint64_t index) {
    return "{\"input\":" + std::to_string(index) + "}";
}

std::string report_delta(std::uint64_t cursor) {
    return "{\"arrays\":{},\"fields\":{\"value\":" + std::to_string(cursor) +
           "},\"schema_version\":\"pineforge-native-report-delta/v1\"}";
}

// A journal-only action, or one routed to the webhook target "main".
Event action(std::uint64_t index, bool routed) {
    const auto id = "action-" + std::to_string(index);
    return {id, "{\"action\":" + std::to_string(index) + "}",
            routed ? std::optional<std::string>("main") : std::nullopt, "delivery-" + id};
}

void deliver(Ledger& ledger, const StoredEvent& event, const std::string& request_id) {
    const auto attempt = ledger.start_attempt(event, clock_ms + event.ordinal, request_id);
    ledger.finish_attempt(attempt, clock_ms + event.ordinal + 1, 204, true, "");
}

void seed(const std::string& path, std::uint64_t inputs) {
    Ledger ledger(path, deployment);
    ledger.verify_report(0, "{\"schema_version\":\"pineforge-native-report/v1\",\"value\":0}");
    for (std::uint64_t index = 0; index < inputs; ++index) {
        const bool routed = index % routed_every == routed_every / 2;
        ledger.commit_input(index, feed_message(index), index, {action(index, routed)},
                            report_delta(index + 1));
        if (routed) deliver(ledger, ledger.input(index)->events.front(), "");
    }
    REQUIRE(ledger.input_count() == inputs);
    REQUIRE(ledger.unsent_count() == 0);
    REQUIRE(!ledger.checkpoint_diagnostic());
}

// The delivery scheduler's cursors (delivery.cpp: low, requested_order,
// requested_after), which live as long as the worker thread.
struct Scheduler {
    std::uint64_t low = 0, order = 0, after = 0;
    std::uint64_t redelivered = 0;
};

// One iteration of DeliveryWorker::run: read the unsent actions after the
// cursor and settle them, then read the requested redeliveries.
void scheduler_pass(Ledger& ledger, Scheduler& cursor) {
    while (const auto next = ledger.next_delivery_event(cursor.low)) {
        cursor.low = next->event.ordinal;
        if (next->unsent) deliver(ledger, next->event, "");
    }
    while (const auto next = ledger.next_redelivery_event(cursor.order, cursor.after)) {
        cursor.order = next->request_order;
        cursor.after = next->event.ordinal;
        deliver(ledger, next->event, next->request_id);
        ++cursor.redelivered;
    }
}

// One new feed message as runner/main.cpp handles it with a status file:
// the prefix probe, the contiguity check, the input transaction, the status
// pulse, then one scheduler iteration that delivers the new action.
void live_input(Ledger& ledger, Scheduler& cursor, std::uint64_t index) {
    REQUIRE(!ledger.input(index));
    REQUIRE(ledger.input_count() == index);
    ledger.commit_input(index, feed_message(index), index, {action(index, true)},
                        report_delta(index + 1));
    REQUIRE(ledger.input_count() == index + 1);
    // The new action is still pending here: one target row in both ledgers.
    REQUIRE(ledger.delivery_metrics_json(clock_ms + index).find("\"main\"") != std::string::npos);
    REQUIRE(ledger.input_count() == index + 1);
    REQUIRE(ledger.report_cursor() == index + 1);
    scheduler_pass(ledger, cursor);
    REQUIRE(ledger.unsent_count() == 0);
}

Work live_batch(Ledger& ledger, Scheduler& cursor, std::uint64_t first) {
    reset_work();
    for (std::uint64_t index = first; index < first + batch; ++index) live_input(ledger, cursor, index);
    return take_work();
}

struct LedgerWork {
    std::uint64_t inputs = 0;
    Work idle;    // a batch with no redelivery request on record
    Work drain;   // selecting and settling one request for the batch's actions
    Work served;  // a batch after a restart, with served requests on record
    Work restart; // the first scheduler iteration after that restart (reported only)
};

// Report checkpoints fall on interval * 2^j committed inputs and rebuild the
// cumulative report once each; a measured batch must not contain one.
bool crosses_checkpoint(std::uint64_t first, std::uint64_t last) {
    for (std::uint64_t boundary = kDefaultReportCheckpointInterval; boundary <= last; boundary *= 2)
        if (boundary > first) return true;
    return false;
}

LedgerWork ledger_work(const std::filesystem::path& directory, std::uint64_t inputs) {
    if (crosses_checkpoint(inputs, inputs + 2 * batch))
        throw std::runtime_error("choose sizes whose measured batches cross no report checkpoint boundary");
    const auto path = (directory / ("ledger-" + std::to_string(inputs) + ".sqlite")).string();
    seed(path, inputs);
    LedgerWork result;
    result.inputs = inputs;
    const auto seeded_actions = inputs / routed_every + (inputs % routed_every > routed_every / 2 ? 1 : 0);
    {
        Ledger ledger(path, deployment); // the runner restarts on the seeded ledger
        Scheduler cursor;
        scheduler_pass(ledger, cursor);  // the worker's first iteration
        REQUIRE(cursor.redelivered == 0);
        result.idle = live_batch(ledger, cursor, inputs);

        // An operator asks for the batch's actions again.
        const auto first_action = ledger.input(inputs)->events.front().ordinal;
        REQUIRE(ledger.request_redelivery(std::string(64, 'a'), "main", first_action, false) == batch);
        reset_work();
        scheduler_pass(ledger, cursor);
        result.drain = take_work();
        REQUIRE(cursor.redelivered == batch);

        // And once for every webhook action the ledger holds (not measured:
        // reading a request's own range once is the request's work).
        REQUIRE(ledger.request_redelivery(std::string(64, 'b'), "main", 1, false) == seeded_actions + batch);
        scheduler_pass(ledger, cursor);
        REQUIRE(cursor.redelivered == seeded_actions + 2 * batch);
    }
    {
        Ledger ledger(path, deployment); // restart with both served requests on record
        Scheduler cursor;
        reset_work();
        scheduler_pass(ledger, cursor);
        result.restart = take_work();
        REQUIRE(cursor.redelivered == 0);
        result.served = live_batch(ledger, cursor, inputs + batch);
        REQUIRE(cursor.redelivered == 0);
        REQUIRE(ledger.input_count() == inputs + 2 * batch);
        REQUIRE(!ledger.checkpoint_diagnostic());
    }
    return result;
}

void print_work(const char* name, std::uint64_t inputs, const Work& value) {
    std::printf("work ledger %s: %llu prior inputs: statements=%llu vm_steps=%llu scanned_rows=%llu "
                "result_rows=%llu worst_statement_vm_steps=%llu worst_statement=\"%.96s\"\n",
        name, static_cast<unsigned long long>(inputs),
        static_cast<unsigned long long>(value.statements), static_cast<unsigned long long>(value.vm_steps),
        static_cast<unsigned long long>(value.scanned_rows), static_cast<unsigned long long>(value.result_rows),
        static_cast<unsigned long long>(value.worst_steps), value.worst_sql.c_str());
}

void ledger_row(std::uint64_t small_inputs, std::uint64_t large_inputs) {
    TemporaryDirectory directory;
    const auto small = ledger_work(directory.path, small_inputs);
    const auto large = ledger_work(directory.path, large_inputs);
    for (const auto* measured : {&small, &large}) {
        print_work("idle", measured->inputs, measured->idle);
        print_work("drain", measured->inputs, measured->drain);
        print_work("served", measured->inputs, measured->served);
        print_work("restart (once per start, not asserted)", measured->inputs, measured->restart);
    }
    // The observers must have seen the real statements of the same batch.
    REQUIRE(small.idle.vm_steps > 0 && small.idle.statements == large.idle.statements);
    REQUIRE(small.drain.vm_steps > 0 && small.drain.statements == large.drain.statements);
    REQUIRE(small.served.vm_steps > 0 && small.served.statements == large.served.statements);
    const struct { const char* name; const Work LedgerWork::* member; } rows[] = {
        {"idle", &LedgerWork::idle}, {"drain", &LedgerWork::drain}, {"served", &LedgerWork::served}};
    for (const auto& row : rows) {
        const auto& low = small.*(row.member);
        const auto& high = large.*(row.member);
        check(row.name, "vm_steps of the batch", small.inputs, low.vm_steps, large.inputs, high.vm_steps);
        check(row.name, "rows scanned without an index", small.inputs, low.scanned_rows,
              large.inputs, high.scanned_rows, 8);
        check(row.name, "vm_steps of the costliest statement", small.inputs, low.worst_steps,
              large.inputs, high.worst_steps);
    }
}

// ---------------------------------------------------------------------------
// report
// ---------------------------------------------------------------------------

// A report that grows as a live run's does: one equity point and one broker
// hash per input and one closed trade every fourth input, handed over as the
// carried patch the runner receives from the accumulator (the replaced last
// row and the new rows of each array).
struct History {
    std::vector<pf_trade_t> trades;
    std::vector<pf_equity_point_t> curve;
    std::vector<std::uint64_t> hashes;
    std::size_t reported_trades = 0, reported_points = 0;

    pineforge::report::LiveReportPatch input(std::uint64_t cursor) {
        const auto time = static_cast<std::int64_t>(clock_ms + cursor * 60000);
        curve.push_back({time, 10000.0 + static_cast<double>(cursor % 97) * 0.25, 0.0});
        hashes.push_back(0x9e3779b97f4a7c15ULL * (cursor + 1));
        if (cursor % 4 == 0) {
            pf_trade_t trade{};
            trade.entry_time = time - 60000;
            trade.exit_time = time;
            trade.entry_price = 100.0;
            trade.exit_price = 101.0;
            trade.pnl = trade.pnl_pct = trade.qty = 1.0;
            trade.is_long = 1;
            trade.entry_bar_index = static_cast<std::int32_t>(cursor - 1);
            trade.exit_bar_index = static_cast<std::int32_t>(cursor);
            trades.push_back(trade);
        }
        pineforge::report::LiveReportPatch patch;
        const auto suffix = [](auto& to, const auto& from, std::size_t reported) {
            to.offset = reported ? reported - 1 : 0;
            to.logical_length = from.size();
            to.rows.assign(from.begin() + static_cast<std::ptrdiff_t>(to.offset), from.end());
        };
        suffix(patch.trades, trades, reported_trades);
        suffix(patch.equity, curve, reported_points);
        suffix(patch.hashes, hashes, reported_points);
        patch.closed_trades = trades.size();
        patch.scalars.total_trades = patch.scalars.trades_len = static_cast<int>(trades.size());
        patch.scalars.net_profit = static_cast<double>(trades.size());
        patch.scalars.equity_curve_len = patch.scalars.broker_state_hash_len =
            static_cast<std::int64_t>(curve.size());
        patch.scalars.input_bars_processed = patch.scalars.script_bars_processed =
            static_cast<std::int64_t>(cursor);
        reported_trades = trades.size();
        reported_points = curve.size();
        return patch;
    }
};

std::uint64_t report_work(std::uint64_t inputs) {
    ReportDeltas deltas(true);
    Json oracle = Json::object({}); // the same deltas folded by the ledger's reader
    History history;
    std::uint64_t cursor = 0;
    // Recovered history: replay folds every delta and prints no line.
    while (cursor < inputs) {
        const auto patch = history.input(++cursor);
        apply_report_delta(oracle, parse_json(deltas.update(patch, deployment, cursor, cursor)));
    }
    auto line = deltas.full_json(); // the first cumulative line after the restart
    REQUIRE(line == oracle.dump());
    std::vector<std::string> payloads;
    payloads.reserve(batch);
    const auto before = allocations.load();
    for (std::uint64_t index = 0; index < batch; ++index) {
        const auto patch = history.input(++cursor);
        payloads.push_back(deltas.update(patch, deployment, cursor, cursor));
        line = deltas.full_json();
    }
    const auto counted = allocations.load() - before;
    for (const auto& payload : payloads) apply_report_delta(oracle, parse_json(payload));
    REQUIRE(line == oracle.dump()); // the printed bytes are the canonical document
    REQUIRE(parse_json(payloads.back()).at("arrays").at("equity_curve").at("items").items.size() == 2);
    std::printf("work report: %llu prior inputs: allocations=%llu for %llu cumulative lines of %zu bytes\n",
        static_cast<unsigned long long>(inputs), static_cast<unsigned long long>(counted),
        static_cast<unsigned long long>(batch), line.size());
    return counted;
}

void report_row(std::uint64_t small_inputs, std::uint64_t large_inputs) {
    const auto small = report_work(small_inputs);
    const auto large = report_work(large_inputs);
    REQUIRE(small > 0);
    // 64: the vectors of this fixture and the line buffer grow at different inputs.
    check("report", "allocations of the batch's cumulative lines", small_inputs, small, large_inputs, large, 64);
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2 && argc != 4)
            throw std::runtime_error("usage: test_native_live_work_scale ledger|report [SMALL LARGE]");
        const std::string row = argv[1];
        const std::uint64_t small = argc == 4 ? std::stoull(argv[2]) : 1000;
        const std::uint64_t large = argc == 4 ? std::stoull(argv[3]) : 100000;
        if (small < routed_every || large < 16 * small)
            throw std::runtime_error("sizes must be at least 500 and at least sixteen times apart");
        if (row == "ledger") ledger_row(small, large);
        else if (row == "report") report_row(small, large);
        else throw std::runtime_error("unknown row: " + row);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
    if (failures) {
        std::fprintf(stderr, "FAIL %d measure(s) grew with the prior history\n", failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
