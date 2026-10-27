#include "report_event_trace_fixture.hpp"

#include <pineforge/run_failure.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>

namespace {
using namespace event_trace_fixture;

enum class Fault { EventPeer, TracePeer, EventLine, TraceLine, FinishPeer };

struct Socket {
    int writer = -1, reader = -1;
    Socket() {
        int descriptors[2]{-1, -1};
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0,
                "create event/trace failure socket");
        writer = descriptors[0];
        reader = descriptors[1];
    }
    void close_peer() {
        require(reader >= 0 && ::close(reader) == 0, "close event/trace peer");
        reader = -1;
    }
    ~Socket() {
        if (reader >= 0) ::close(reader);
        if (writer >= 0) ::close(writer);
    }
};

class FailureHost final : public Host {
public:
    Fault fault;
    Socket* socket;
    explicit FailureHost(Fault selected, Socket* stream) : fault(selected), socket(stream) {}
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        Host::on_native_bar(bar, context);
        if (calculations != 2 || fault == Fault::FinishPeer) return;
        const bool line = fault == Fault::EventLine || fault == Fault::TraceLine;
        const std::string text(line ? 1024 * 1024 : 64 * 1024 + 1, 'x');
        if (!line) socket->close_peer();
        if (fault == Fault::TracePeer || fault == Fault::TraceLine) trace(text, 1.0);
        else {
            output_event(0, 1.0, text);
            output_events_clear();
        }
        throw std::runtime_error("event/trace fault did not interrupt the callback");
    }
};

void empty(const ReportC& report) {
    const pf_metrics_t zero{};
    require(report.total_trades == 0 && report.net_profit == 0.0
                && report.trades == nullptr && report.trades_len == 0
                && report.security_diag == nullptr && report.security_diag_len == 0
                && report.trace == nullptr && report.trace_len == 0
                && report.trace_names == nullptr && report.trace_names_len == 0
                && report.equity_curve == nullptr && report.equity_curve_len == 0
                && report.broker_state_hash == nullptr && report.broker_state_hash_len == 0
                && report.input_bars_processed == 0 && report.script_bars_processed == 0
                && report.security_feeds_total == 0 && report.security_eval_complete_total == 0
                && report.security_eval_partial_total == 0
                && report.magnifier_sub_bars_total == 0 && report.magnifier_sample_ticks_total == 0
                && report.input_tf_seconds == 0 && report.script_tf_seconds == 0
                && report.script_tf_ratio == 0 && report.needs_aggregation == 0
                && report.bar_magnifier_enabled == 0
                && std::memcmp(&report.metrics, &zero, sizeof zero) == 0,
            "a public reader published a partial report after sink failure");
}

void check(Fault fault, const Directory& directory) {
    const bool line = fault == Fault::EventLine || fault == Fault::TraceLine;
    Socket socket;
    FailureHost host(fault, &socket);
    const auto path = directory.path + "/failure.ndjson";
    // Configure the public native host first; this helper installs a path
    // sink only for the wire-limit cases, a borrowed socket for I/O faults.
    configure(host, line ? Route::Sink : Route::Full, true, path, directory.path);
    if (!line) {
        const pf_report_sink_fd_v1 sink{sizeof sink, 1, socket.writer, "event-trace"};
        require(host.set_report_sink_fd_v1(&sink) == 0, "install failure fd sink");
        require(host.set_report_mode_v1(ReportMode::STREAMED) == PF_REPORT_MODE_ACCEPTED,
                "select failure streamed route");
    }
    const auto bars = feed(fault == Fault::FinishPeer ? 2 : 4);
    try { host.run(bars.data(), static_cast<int>(bars.size())); }
    catch (const std::exception& error) { note_run_failure(host, error.what(), error); }
    require(host.calculations == 2, "fault fixture did not execute its nonempty prefix");
    if (fault == Fault::FinishPeer) {
        require(host.last_error().empty(), "finish fixture failed before capture");
        socket.close_peer();
        bool caught = false;
        Envelope report;
        try { host.fill_report(&report.value.report); }
        catch (const std::exception&) { caught = true; }
        require(caught, "closed peer did not fail final report publication");
        empty(report.value.report);
    }
    const auto wanted_code = line ? RunFailureCode::engine_invariant
                                 : RunFailureCode::report_sink_failed;
    const auto failure = run_failure_value_of(host);
    const std::string arguments = run_failure_args_of(host);
    const auto error = host.last_error();
    require(host.last_run_status() != 0 && failure.code == wanted_code
                && run_failure_code_class(failure.code) == RunFailureClass::engine_fault
                && !run_failure_code_retryable(failure.code),
            "streaming fault changed the existing typed failure class");
    require(arguments == (line ? "{}" : "{\"reason\":\"sink_peer_closed\"}"),
            "streaming fault changed its canonical arguments");
    auto* handle = static_cast<BacktestEngine*>(&host);
    for (int read_index = 0; read_index < 2; ++read_index) {
        Envelope direct;
        host.fill_report(&direct.value.report);
        empty(direct.value.report);
        Envelope snapshot;
        require(strategy_stream_fill_report(handle,
                    reinterpret_cast<pf_report_t*>(&snapshot.value.report)) == 0,
                "read failed stream snapshot");
        empty(snapshot.value.report);
        Envelope envelope;
        require(host.fill_report_envelope_v1(&envelope.value) == PF_REPORT_MODE_ACCEPTED,
                "read failed report envelope");
        empty(envelope.value.report);
        const auto& metadata = envelope.value.metadata;
        require(metadata.complete == 0 && metadata.trades.logical_count == 0
                    && metadata.equity_curve.logical_count == 0
                    && metadata.broker_state_hash.logical_count == 0
                    && metadata.trace.logical_count == 0 && metadata.trace_names.logical_count == 0
                    && metadata.output_rows.logical_count == 0
                    && metadata.output_events.logical_count == 0,
                "failed envelope exposed prefix counts as a report");
        const auto got = receipt(host);
        require(got.complete == 0 && got.sha256_hex[0] == '\0',
                "streaming fault completed its receipt");
        pf_report_sink_summary_v1 summary{sizeof summary, 1};
        pf_report_sink_diagnostics_v1 diagnostics{sizeof diagnostics, 1};
        require(host.report_sink_summary_v1(&summary) == 0 && summary.complete == 0
                    && summary.count_t == 0
                    && host.report_sink_diagnostics_v1(&diagnostics) == 0
                    && diagnostics.complete == 0 && diagnostics.count_e == 0,
                "failed sink published completed summary or provenance");
        const auto witness = host.optional_sequence_summary();
        require(witness.rows.count == 0 && witness.rows.digest == 0
                    && witness.events.count == 0 && witness.events.digest == 0
                    && witness.traces.count == 0 && witness.traces.digest == 0,
                "failed public summary exposed a provisional witness");
        require(run_failure_value_of(host).code == wanted_code
                    && run_failure_args_of(host) == arguments && host.last_error() == error,
                "later reads replaced the original failure");
    }
    // Replacing next-run sink configuration must not unpoison this report.
    if (line) {
        const pf_report_sink_v1 replacement{sizeof replacement, 1, path.c_str(),
                                           "event-trace", directory.path.c_str()};
        require(host.set_report_sink_v1(&replacement) == 0, "replace failed path sink");
    } else {
        const pf_report_sink_fd_v1 replacement{sizeof replacement, 1, socket.writer, "event-trace"};
        require(host.set_report_sink_fd_v1(&replacement) == 0, "replace failed fd sink");
    }
    Envelope replaced;
    host.fill_report(&replaced.value.report);
    empty(replaced.value.report);
    require(run_failure_value_of(host).code == wanted_code
                && run_failure_args_of(host) == arguments && host.last_error() == error,
            "sink replacement changed the latched failure");
    if (line)
        require(event_trace_fixture::read(path).find("{\"trailer\":") == std::string::npos,
                "wire-limit failure left a valid trailer");
    else require(::fcntl(socket.writer, F_GETFD) >= 0, "failure closed the borrowed sink fd");
}
} // namespace

int main() {
    try {
        Directory directory;
        int cases = 0;
        for (const auto fault : {Fault::EventPeer, Fault::TracePeer, Fault::EventLine,
                                 Fault::TraceLine, Fault::FinishPeer}) {
            check(fault, directory);
            ++cases;
        }
        require(cases == 5, "all event/trace failure cases must run");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
