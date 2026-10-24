// F2 RED: a report-time abort keeps security_diag, just like normal sink
// cleanup. HEAD frees it even though its scalar diagnostic totals survive.
#include "report_modes_fix_fixture.hpp"

#include <cerrno>
#include <csignal>
#include <sys/socket.h>

using namespace report_modes_fix;

namespace {

struct Sigterm {
    sigset_t term{}, previous{};
    Sigterm() {
        require(::sigemptyset(&term) == 0 && ::sigaddset(&term, SIGTERM) == 0,
                "cannot prepare SIGTERM mask");
        require(::sigprocmask(SIG_BLOCK, &term, &previous) == 0,
                "cannot block SIGTERM for installation");
        require(pf_run_abort_install_sigterm_v1() == 0,
                "cannot install SIGTERM abort handler");
        require(pf_run_abort_requested_v1() == 0, "stale SIGTERM request");
        require(::sigprocmask(SIG_UNBLOCK, &term, nullptr) == 0,
                "cannot unblock SIGTERM");
    }
    ~Sigterm() {
        ::sigprocmask(SIG_BLOCK, &term, nullptr);
        pf_run_abort_restore_sigterm_v1();
        ::sigprocmask(SIG_SETMASK, &previous, nullptr);
    }
};

struct Socket {
    int fd[2]{-1, -1};
    Socket() {
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0,
                "cannot create report socket");
    }
    ~Socket() {
        for (int descriptor : fd)
            if (descriptor >= 0) ::close(descriptor);
    }
    std::string finish() const {
        require(::shutdown(fd[1], SHUT_WR) == 0, "cannot finish report socket");
        std::string bytes;
        char buffer[4096];
        for (;;) {
            const auto count = ::read(fd[0], buffer, sizeof buffer);
            if (count < 0 && errno == EINTR) continue;
            require(count >= 0, "cannot read report socket");
            if (count == 0) return bytes;
            bytes.append(buffer, static_cast<std::size_t>(count));
        }
    }
};

struct DiagnosticHost : Host {
    int deliveries = 0;
    void on_native_timeframe_bar(const Bar& bar,
                                const NativeTimeframeBarContext& context) override {
        require(context.subscription == 0, "unexpected subscription");
        require(bar.timestamp == start_ms + deliveries * minute_ms,
                "subscription did not deliver each input bar in order");
        ++deliveries;
    }
};

void check_report(bool abort) {
    Sigterm signals;
    Socket socket;
    DiagnosticHost host;
    auto configuration = spec();
    NativeTimeframeSubscription subscription;
    subscription.tf = "1";
    configuration.subscriptions.push_back(subscription);
    const auto setup = host.configure_native(configuration);
    require(setup.status == NativeSetupStatus::Applied,
            "native configure refused: " + host.last_error());
    host.hold_lots = 3;
    host.set_broker_state_hash_recording(true);
    const pf_report_sink_fd_v1 sink{
        sizeof sink, 1, socket.fd[1], "p2-abort-security-diag"};
    require(host.set_report_sink_fd_v1(&sink) == 0, "fd sink install refused");
    run(host, tape(8));
    require(host.deliveries == 8, "fixture did not evaluate its subscription");
    require(receipt(host).complete == 0, "fixture finalized before report capture");
    if (abort) {
        require(::raise(SIGTERM) == 0, "cannot request report-time abort");
        require(pf_run_abort_requested_v1() == 1, "SIGTERM was not observed");
    }

    Report report;
    host.fill_report(&report.value);
    if (abort) {
        require(host.last_run_status() == 1
                    && run_failure_value_of(host).code == RunFailureCode::run_aborted,
                "report did not take the typed abort-before-trailer path");
    } else {
        require(host.last_run_status() == 0 && host.last_error().empty(),
                "normal report control failed");
    }
    require(receipt(host).complete == (abort ? 0 : 1), "incorrect sink completion");
    require(report.value.trades == nullptr && report.value.trades_len == 0
                && report.value.equity_curve == nullptr && report.value.equity_curve_len == 0
                && report.value.broker_state_hash == nullptr
                && report.value.broker_state_hash_len == 0
                && report.value.trace == nullptr && report.value.trace_len == 0
                && report.value.trace_names == nullptr && report.value.trace_names_len == 0,
            "report lost the sink array shape");
    require(report.value.total_trades == 3
                && report.value.input_bars_processed == 8
                && report.value.script_bars_processed == 8,
            "report lost completed-run scalars");
    // One same-timeframe subscription consumes and completes all eight bars;
    // it has no partial buckets. These expectations come from the input tape.
    require(report.value.security_feeds_total == 8
                && report.value.security_eval_complete_total == 8
                && report.value.security_eval_partial_total == 0,
            "report lost scalar security diagnostics");
    require(report.value.security_diag != nullptr && report.value.security_diag_len == 1,
            "report-time abort freed security_diag");
    const auto& diagnostic = report.value.security_diag[0];
    require(diagnostic.sec_id == 0 && diagnostic.feed_count == 8
                && diagnostic.eval_complete_count == 8 && diagnostic.eval_partial_count == 0,
            "report changed security diagnostic fields");
    const auto bytes = socket.finish();
    require(!bytes.empty() && bytes.back() == '\n', "sink lacks a complete record prefix");
    if (abort) {
        require(bytes.find("{\"trailer\":") == std::string::npos,
                "aborted report wrote a success trailer");
    } else {
        require_trailer(bytes);
    }
}

} // namespace

int main() {
    try {
        check_report(false);
        check_report(true);
        std::puts("F2 report-time abort retains security diagnostics: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "F2: %s\n", error.what());
        return 1;
    }
}
