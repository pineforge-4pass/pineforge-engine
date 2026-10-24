// fix-ovf (ARGON-B2s1-02): a completed fd sink detached between two reads of an
// observed generation, without and with a selected window. report_sink.h lets
// NULL through either setter detach a completed fd generation "while preserving
// their receipt, summary and borrowed fd", so the second read must answer the
// first read's sink report again. Before the fix the selected read took its
// sink-removed rebuild branch: fill_trades_section, which still sees the sink
// generation, allocated the one range-end row, and the captured open row was
// copied at the ledger end (index 1), one TradeC past that block.
// Fail-before: AddressSanitizer heap-buffer-overflow in the second fill_report;
// without a sanitizer, the direct selected read at the end still returns that
// array. The selected read likewise rebuilt its whole metric equity curve
// after the detach (sink_equity on enabled alone), so both rereads pin all
// four sink-shaped arrays.
#include <pineforge/execution_observer.h>
#include <pineforge/native_host.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/report_sink.h>
#include <pineforge/selected_window.h>
#include "../src/native_execution_consumer.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {
using pineforge::BacktestEngine;
using pineforge::ReportC;

constexpr std::int64_t start_ms = INT64_C(1704067200000);
constexpr std::int64_t minute = INT64_C(60000);
constexpr int bar_count = 32;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool same(double left, double right) {
    return std::memcmp(&left, &right, sizeof left) == 0;
}

// report_sink.h: with a sink, trades, equity_curve, broker_state_hash and trace
// are NULL with length 0. One check per array, so a failure names the array.
void require_sink_shape(const ReportC& report, const std::string& read) {
    require(report.trades == nullptr && report.trades_len == 0,
            read + " returned a trade array");
    require(report.equity_curve == nullptr && report.equity_curve_len == 0,
            read + " returned an equity curve");
    require(report.broker_state_hash == nullptr && report.broker_state_hash_len == 0,
            read + " returned a broker state hash array");
    require(report.trace == nullptr && report.trace_len == 0,
            read + " returned a trace array");
}

struct Report {
    ReportC value{};
    Report() = default;
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
    ~Report() { BacktestEngine::free_report(&value); }
};

// No reader thread: a 32-bar run without outputs, trace or hashes writes a few
// kilobytes, which the pipe holds until drain() empties it.
struct Pipe {
    int descriptors[2]{-1, -1};
    Pipe() {
        require(::pipe(descriptors) == 0, "pipe failed");
        const int flags = ::fcntl(descriptors[0], F_GETFL);
        require(flags >= 0 && ::fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) == 0,
                "nonblocking reader setup failed");
    }
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    ~Pipe() {
        for (const int fd : descriptors) if (fd >= 0) ::close(fd);
    }
    std::string drain() const {
        std::string bytes;
        std::array<char, 4096> buffer{};
        for (;;) {
            const auto count = ::read(descriptors[0], buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return bytes;
            require(count >= 0, "pipe read failed");
            if (count == 0) return bytes;
            bytes.append(buffer.data(), static_cast<std::size_t>(count));
        }
    }
};

std::vector<pineforge::Bar> feed() {
    std::vector<pineforge::Bar> bars;
    for (int i = 0; i < bar_count; ++i) {
        const double price = 100.0 + i * 0.25;
        bars.push_back({price, price + 0.5, price - 0.5, price + 0.125, 1.0,
                        start_ms + i * minute});
    }
    return bars;
}

// One round trip and one entry still open after the last bar: one closed row
// and one range-end row.
class Fixture final : public pineforge::NativeStrategyHost {
public:
    Fixture() {
        pineforge::NativeRunSpec spec;
        spec.identity = {"S4E-fix-ovf", 1};
        spec.input_tf = spec.script_tf = "1";
        spec.tickerid = "TEST:S4EOVF";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = 10000.0;
        spec.point_value = spec.account_fx = 1.0;
        spec.price_tick = 0.01;
        spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
        spec.report_open_position_at_end = true;
        require(configure_native(spec).status == pineforge::NativeSetupStatus::Applied,
                "native fixture configure refused");
    }
    pf_strategy_t handle() { return static_cast<BacktestEngine*>(this); }
    // The two lengths the rebuild branch combined: the ledger end it copied
    // at, and the range-end rows a sink generation allocates alone.
    std::size_t closed_rows() const { return trades_.size(); }
    std::size_t open_rows() const { return range_end_trades_.size(); }
    int callbacks = 0;

    void on_native_run_begin() override { callbacks = 0; }
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {
        const int index = callbacks++;
        if (index != 0 && index != 8 && index != 16) return;
        const auto placed = submit({pineforge::native_order::Transact{index == 8 ? -1.0 : 1.0},
                                    index == 8 ? "exit" : "entry", ""});
        require(placed.status == pineforge::native_order::SubmitStatus::Accepted,
                "fixture transaction refused");
    }
};

pf_execution_observation_v1 observation(Fixture& host) {
    pf_execution_observation_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    require(strategy_execution_observation_v1(host.handle(), &result) == 0,
            "observation query refused");
    return result;
}

pf_report_sink_receipt_v1 receipt(Fixture& host) {
    pf_report_sink_receipt_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    require(strategy_report_sink_receipt_v1(host.handle(), &result) == 0,
            "receipt query refused");
    return result;
}

pf_report_sink_summary_v1 summary(Fixture& host) {
    pf_report_sink_summary_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    require(strategy_report_sink_summary_v1(host.handle(), &result) == 0,
            "summary query refused");
    return result;
}

void reread(bool selected) {
    std::fprintf(stderr, "fix-ovf: observer%s, completed fd sink detached\n",
                 selected ? " + selected window" : " only");
    const auto bars = feed();
    Pipe pipe;          // outlives the host, which keeps the borrowed fd
    int observed = 0;   // the observer's borrowed context
    Fixture host;
    const pf_execution_observer_v1 observer{sizeof observer, 1, &observed,
        [](void* context, const pf_execution_boundary_v1*, pf_boundary_receipt_v1* out) -> int {
            ++*static_cast<int*>(context);
            out->frame_bytes = 0;
            return 0;
        }};
    require(strategy_set_execution_observer_v1(host.handle(), &observer) == 0,
            "observer refused");
    if (selected) {
        const pf_selected_window_config_v1 window{sizeof window, 1, start_ms,
            start_ms + (bar_count + 1) * minute};
        require(strategy_set_selected_window_v1(host.handle(), &window) == 0,
                "selected window refused");
    }
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, pipe.descriptors[1], "S4E-fix-ovf"};
    require(strategy_set_report_sink_fd_v1(host.handle(), &sink) == 0, "fd sink refused");
    host.run(bars.data(), bar_count);
    require(host.last_run_status() == 0 && host.last_error().empty()
            && host.callbacks == bar_count, "engine run failed: " + host.last_error());

    // Reach. Results are open on an observed generation (execution_observer.h:
    // phase 4 Results, fault_stage 0 None), so every read below is served from
    // the capture by fill_selected_report. The capture holds the one open row
    // (the sink generation's view); the closed row puts the ledger end past it.
    const auto seen = observation(host);
    require(observed == 1 && seen.phase == 4 && seen.fault_stage == 0
            && seen.boundary_delivered == 1, "generation did not open observed results");
    require(host.closed_rows() == 1 && host.open_rows() == 1,
            "fixture lacks one closed row and one range-end row");
    const int total = static_cast<int>(host.closed_rows() + host.open_rows());

    Report first;
    host.fill_report(&first.value);
    const auto completed = receipt(host);
    const auto completed_summary = summary(host);
    require(completed.complete == 1 && completed.count_t == total
            && completed_summary.complete == 1 && completed_summary.count_t == total,
            "first read did not complete the fd sink with every trade row");
    require(first.value.trades == nullptr && first.value.trades_len == 0
            && first.value.total_trades == total, "first read is not the sink report");
    require(!pipe.drain().empty(), "completed fd sink wrote no bytes");

    require(strategy_set_report_sink_fd_v1(host.handle(), nullptr) == 0,
            "fd NULL refused a completed fd generation");
    const auto detached = receipt(host);
    const auto detached_summary = summary(host);
    require(std::memcmp(&detached, &completed, sizeof completed) == 0
            && std::memcmp(&detached_summary, &completed_summary, sizeof completed_summary) == 0,
            "detach changed the completed receipt or summary");

    // The defect: this read overflowed the heap before the fix.
    Report second;
    host.fill_report(&second.value);
    require(second.value.trades == nullptr && second.value.trades_len == 0,
            "detached reread lost the sink report shape");
    require_sink_shape(second.value, "detached reread");
    require(second.value.total_trades == first.value.total_trades
            && same(second.value.net_profit, first.value.net_profit)
            && std::memcmp(&second.value.metrics, &first.value.metrics,
                           sizeof first.value.metrics) == 0,
            "detached reread changed the report totals or metrics");
    const auto reread_receipt = receipt(host);
    const auto reread_summary = summary(host);
    require(std::memcmp(&reread_receipt, &completed, sizeof completed) == 0
            && std::memcmp(&reread_summary, &completed_summary, sizeof completed_summary) == 0,
            "detached reread changed the completed receipt or summary");
    require(pipe.drain().empty(), "detached reread wrote sink bytes");

    // fill_report's finish_report_sink frees whatever array the selected read
    // returns, so ask the selected read directly as well: a completed sink
    // generation keeps the frozen scalars and builds no trade array and no
    // equity curve.
    Report direct;
    require(pineforge::NativeExecutionConsumer::fill_selected_report(host, &direct.value),
            "selected read did not serve the capture");
    require(direct.value.trades == nullptr && direct.value.trades_len == 0
            && direct.value.total_trades == first.value.total_trades
            && same(direct.value.net_profit, first.value.net_profit),
            "detached selected read rebuilt a trade array");
    require_sink_shape(direct.value, "detached selected read");
}
} // namespace

int main() {
    try {
        for (const bool selected : {false, true}) reread(selected);
        std::puts("test_report_sink_fd_observer_detach_reread: detached rereads kept the sink report");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_report_sink_fd_observer_detach_reread: %s\n", error.what());
        return 1;
    }
}
