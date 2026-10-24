// fix-ovf round 2 (Opus F3): the report envelope of a completed fd sink
// generation after its detach. report_sink.h lets NULL through either setter
// detach a completed fd generation "while preserving their receipt, summary
// and borrowed fd", and o1-report-contract.md says a successful setter "does
// not reinterpret a previously frozen report". The generation's rows and
// points went to the sink; its report holds none of them. The run used the
// default FULL mode, and "Explicit FULL through the new envelope requires the
// owning-array route; an installed external sink is a conflict", so the read
// after the detach answers as the read before it: PF_REPORT_MODE_CONFLICT,
// with nothing written to the destination.
// Fail-before: the capture-policy check read enabled alone, so the FULL read
// after the detach was accepted and published trades captured=1, external=0
// with T logical rows over a NULL array the generation never owned.
#include <pineforge/native_host.hpp>
#include <pineforge/native_order.hpp>
#include <pineforge/native_run_spec.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/report_mode.h>
#include <pineforge/report_sink.h>

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
using pineforge::ReportEnvelopeV1;

constexpr std::int64_t start_ms = INT64_C(1704067200000);
constexpr std::int64_t minute = INT64_C(60000);
constexpr int bar_count = 32;
// Seeded into the destination before every read; a refused read keeps them.
constexpr std::uint32_t untouched_complete = 0xA5u;
constexpr int untouched_total = -1;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Report {
    ReportC value{};
    Report() = default;
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
    ~Report() { BacktestEngine::free_report(&value); }
};

struct Envelope {
    ReportEnvelopeV1 value{};
    Envelope() {
        value.metadata.struct_size = sizeof(pf_report_metadata_v1);
        value.metadata.version = PF_REPORT_ENVELOPE_VERSION;
        value.metadata.complete = untouched_complete;
        value.metadata.trades.logical_count = untouched_total;
        value.report.total_trades = untouched_total;
    }
    Envelope(const Envelope&) = delete;
    Envelope& operator=(const Envelope&) = delete;
    ~Envelope() { BacktestEngine::free_report(&value.report); }
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
// and one range-end row, T = 2.
class Fixture final : public pineforge::NativeStrategyHost {
public:
    Fixture() { configure_run(1); }
    // A Completed native host needs a fresh Ready spec before another begin.
    void configure_run(std::uint64_t run_number) {
        pineforge::NativeRunSpec spec;
        spec.identity = {"S4E-fix-ovf-envelope", run_number};
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

pf_report_sink_receipt_v1 receipt(Fixture& host) {
    pf_report_sink_receipt_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    require(strategy_report_sink_receipt_v1(host.handle(), &result) == 0,
            "receipt query refused");
    return result;
}

void run_once(Fixture& host, const std::vector<pineforge::Bar>& bars) {
    host.run(bars.data(), bar_count);
    require(host.last_run_status() == 0 && host.last_error().empty()
            && host.callbacks == bar_count, "engine run failed: " + host.last_error());
}

// A refused read returns before it writes the destination: no payload and no
// capture metadata for a sequence the generation's sink owns.
void require_untouched(const ReportEnvelopeV1& envelope, const std::string& read) {
    const auto& metadata = envelope.metadata;
    require(metadata.complete == untouched_complete
            && metadata.trades.logical_count == untouched_total
            && metadata.trades.captured == 0 && metadata.trades.external == 0
            && metadata.equity_curve.logical_count == 0
            && metadata.equity_curve.captured == 0 && metadata.equity_curve.external == 0,
            read + " wrote envelope metadata");
    const auto& report = envelope.report;
    require(report.total_trades == untouched_total
            && report.trades == nullptr && report.trades_len == 0
            && report.equity_curve == nullptr && report.equity_curve_len == 0
            && report.broker_state_hash == nullptr && report.broker_state_hash_len == 0
            && report.trace == nullptr && report.trace_len == 0,
            read + " published a report payload");
}

void full_envelope_after_detach() {
    const auto bars = feed();
    Pipe pipe;          // outlives the host, which keeps the borrowed fd
    Fixture host;
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, pipe.descriptors[1], "S4E-fix-ovf"};
    require(strategy_set_report_sink_fd_v1(host.handle(), &sink) == 0, "fd sink refused");
    run_once(host, bars);
    require(host.closed_rows() == 1 && host.open_rows() == 1,
            "fixture lacks one closed row and one range-end row");
    const int total = static_cast<int>(host.closed_rows() + host.open_rows());

    // The legacy read completes the sink; the FULL envelope cannot, since it
    // conflicts with the installed sink.
    Report first;
    host.fill_report(&first.value);
    const auto completed = receipt(host);
    require(completed.complete == 1 && completed.count_t == total
            && first.value.total_trades == total,
            "first read did not complete the fd sink with every trade row");
    require(first.value.trades == nullptr && first.value.trades_len == 0
            && first.value.equity_curve == nullptr && first.value.equity_curve_len == 0,
            "first read is not the sink report");
    require(!pipe.drain().empty(), "completed fd sink wrote no bytes");

    // Control: the completed generation's FULL answer before the detach.
    Envelope installed;
    require(host.fill_report_envelope_v1(&installed.value) == PF_REPORT_MODE_CONFLICT,
            "FULL envelope accepted a completed fd generation with its sink installed");
    require_untouched(installed.value, "installed-sink FULL envelope");

    require(strategy_set_report_sink_fd_v1(host.handle(), nullptr) == 0,
            "fd NULL refused a completed fd generation");
    const auto detached = receipt(host);
    require(std::memcmp(&detached, &completed, sizeof completed) == 0,
            "detach changed the completed receipt");

    // The defect: before the fix this read was accepted and called the sink's
    // rows a FULL in-place capture.
    Envelope after;
    const int status = host.fill_report_envelope_v1(&after.value);
    require(status == PF_REPORT_MODE_CONFLICT,
            "FULL envelope after the detach returned " + std::to_string(status)
            + " with trades captured=" + std::to_string(after.value.metadata.trades.captured)
            + " external=" + std::to_string(after.value.metadata.trades.external)
            + " logical_count=" + std::to_string(after.value.metadata.trades.logical_count)
            + " trades_len=" + std::to_string(after.value.report.trades_len));
    require_untouched(after.value, "detached FULL envelope");
    const auto refused = receipt(host);
    require(std::memcmp(&refused, &completed, sizeof completed) == 0,
            "refused envelope read changed the completed receipt");
    require(pipe.drain().empty(), "refused envelope read wrote sink bytes");

    // The legacy read still answers the sink report the envelope will not
    // call FULL.
    Report legacy;
    host.fill_report(&legacy.value);
    require(legacy.value.trades == nullptr && legacy.value.trades_len == 0
            && legacy.value.equity_curve == nullptr && legacy.value.equity_curve_len == 0
            && legacy.value.total_trades == total,
            "detached legacy read is not the completed sink report");

    // The refusal belongs to the detached generation only: the next begin
    // clears the receipt, and that sink-off run owns its arrays again.
    host.configure_run(2);
    run_once(host, bars);
    Envelope next;
    require(host.fill_report_envelope_v1(&next.value) == PF_REPORT_MODE_ACCEPTED,
            "FULL envelope refused the sink-off run after the detach");
    const auto& metadata = next.value.metadata;
    const auto& report = next.value.report;
    require(metadata.mode == PF_REPORT_MODE_FULL && metadata.complete == 1
            && metadata.trades.logical_count == total
            && metadata.trades.captured == 1 && metadata.trades.external == 0
            && report.total_trades == total
            && report.trades != nullptr && report.trades_len == total,
            "sink-off FULL envelope does not own its trade rows");
    require(metadata.equity_curve.captured == 1 && metadata.equity_curve.external == 0
            && report.equity_curve != nullptr && report.equity_curve_len > 0,
            "sink-off FULL envelope does not own its equity curve");
    require(receipt(host).complete == 0, "next run kept the detached generation's receipt");
    require(pipe.drain().empty(), "the detached sink received bytes after its detach");
}
} // namespace

int main() {
    try {
        full_envelope_after_detach();
        std::puts("test_report_envelope_after_fd_detach: the detached generation kept its FULL conflict");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_report_envelope_after_fd_detach: %s\n", error.what());
        return 1;
    }
}
