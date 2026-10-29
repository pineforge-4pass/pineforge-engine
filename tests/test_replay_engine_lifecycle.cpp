#include "report_modes_fix_fixture.hpp"
#include "../src/report_replay.hpp"
#include <cerrno>

using namespace report_modes_fix;

namespace {
void same_report(const ReportC& got, const ReportC& want) {
    require(got.total_trades == want.total_trades && got.trades_len == want.trades_len,
            "public equity mode changed the trade count");
    require(same_bits(got.net_profit, want.net_profit)
                && std::memcmp(&got.metrics, &want.metrics, sizeof got.metrics) == 0,
            "public equity mode changed the report scalar bytes");
    require(got.equity_curve_len == want.equity_curve_len, "equity count changed");
    require(got.equity_curve_len > 1, "fixture did not record its curve");
    require(std::memcmp(got.equity_curve, want.equity_curve,
                static_cast<std::size_t>(got.equity_curve_len) * sizeof(pf_equity_point_t)) == 0,
            "public equity mode changed the equity bytes");
    for (int i = 0; i < got.trades_len; ++i) {
        const auto& a = got.trades[i];
        const auto& b = want.trades[i];
        require(a.entry_time == b.entry_time && a.exit_time == b.exit_time
                    && a.entry_bar_index == b.entry_bar_index
                    && a.exit_bar_index == b.exit_bar_index && a.is_long == b.is_long
                    && a.open_at_end == b.open_at_end, "trade coordinates changed");
        const double x[]{a.entry_price, a.exit_price, a.qty, a.pnl, a.pnl_pct,
                         a.commission, a.max_runup, a.max_drawdown};
        const double y[]{b.entry_price, b.exit_price, b.qty, b.pnl, b.pnl_pct,
                         b.commission, b.max_runup, b.max_drawdown};
        require(std::memcmp(x, y, sizeof x) == 0, "trade value bytes changed");
    }
}

void public_streamed_without_replay(bool sink_enabled) {
    Host full, streamed;
    full.close_first = streamed.close_first = true;
    full.hold_lots = streamed.hold_lots = 1;
    configure(full);
    configure(streamed);
    require(full.set_equity_report_mode(ReportMode::FULL) == 0, "FULL refused");
    require(streamed.set_equity_report_mode(ReportMode::STREAMED) == 0, "STREAMED refused");
    const auto full_path = sink_path("equity-full");
    const auto streamed_path = sink_path("equity-streamed");
    if (sink_enabled) {
        install_sink(full, full_path);
        install_sink(streamed, streamed_path);
    }
    const auto bars = tape(12);
    run(full, bars);
    run(streamed, bars);
    require(streamed.equity_report_mode() == ReportMode::STREAMED,
            "accepted public STREAMED mode did not round-trip");
    for (int read = 0; read < 2; ++read) {
        Report want, got;
        full.fill_report(&want.value);
        streamed.fill_report(&got.value);
        if (!sink_enabled) same_report(got.value, want.value);
        else {
            require_trailer(read_sink(full_path));
            require(read_sink(streamed_path) == read_sink(full_path),
                    "STREAMED without replay changed primary report bytes");
        }
    }
}

class FailingHost final : public Host {
public:
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        if (calculations == 4500) throw std::runtime_error("deliberate callback failure");
        Host::on_native_bar(bar, context);
    }
};

class ReplayHost final : public Host {
public:
    report::ReportReplay* replay() const { return report_replay_state(); }
};

// NativeStrategyHost itself is deliberately noncopyable. This snapshot
// exercises the existing BacktestEngine copy policy without running a copy
// as a native host or bypassing native host construction.
class EngineSnapshot final : public BacktestEngine {
public:
    explicit EngineSnapshot(const BacktestEngine& engine) : BacktestEngine(engine) {}
    void on_bar(const Bar&) override {}
    report::ReportReplay* replay() const { return report_replay_state(); }
};

void configuration_lifetime() {
    const auto companion = sink_path("reconfigured-replay");
    ReplayHost host;
    host.close_first = true;
    configure(host);
    install_sink(host, sink_path("reconfigured-first"));
    require(host.set_report_replay_v1(companion.c_str(), "lifetime") == 0, "replay refused");
    require(host.set_equity_report_mode(ReportMode::STREAMED) == 0, "STREAMED refused");
    require(host.set_equity_report_mode(static_cast<ReportMode>(UINT32_C(0x80000002))) == -1,
            "public setter admitted a private configuration tag");
    const auto bars = tape(8);
    run(host, bars);
    Report first;
    host.fill_report(&first.value);
    require(host.equity_report_mode() == ReportMode::STREAMED,
            "replay hid the accepted public equity mode");
    auto* original = host.replay();
    require(original && original->complete(), "first replay never completed");
    const auto original_bytes = read_sink(companion);
    require_trailer(original_bytes);

    EngineSnapshot copied(host);
    require(copied.replay() && copied.replay() != original
                && !copied.replay()->begun() && !copied.replay()->complete(),
            "engine copy retained replay run state or shared the owner");
    require(copied.replay()->path() == companion, "engine copy lost replay configuration");
    require(copied.set_report_replay_v1(nullptr, nullptr) == 0 && !copied.replay(),
            "clearing a copy did not release its replay owner");
    require(host.replay() == original && original->complete()
                && read_sink(companion) == original_bytes,
            "clearing a copy changed the original replay");

    // Eight equity rows and one trade fit in the pipe, as in the existing
    // small fd fixtures. A borrowed primary admits streams, not regular files.
    struct Pipe {
        int descriptors[2]{-1, -1};
        ~Pipe() {
            for (int fd : descriptors) if (fd >= 0) ::close(fd);
        }
    } pipe;
    require(::pipe(pipe.descriptors) == 0, "cannot create borrowed primary");
    const pf_report_sink_fd_v1 descriptor{
        sizeof descriptor, 1, pipe.descriptors[1], "s4e-fix-rep"};
    require(host.set_report_sink_fd_v1(&descriptor) == 0 && host.replay() == original,
            "fd reconfiguration lost the replay owner");
    require(original->complete() && read_sink(companion) == original_bytes,
            "sink setter reset the old run prematurely");
    require(::unlink(companion.c_str()) == 0, "cannot retire the first replay file");
    configure(host, 2);
    run(host, bars);
    Report second;
    host.fill_report(&second.value);
    require(::close(pipe.descriptors[1]) == 0, "cannot close primary writer");
    pipe.descriptors[1] = -1;
    std::string primary_bytes;
    char buffer[4096];
    for (;;) {
        const auto count = ::read(pipe.descriptors[0], buffer, sizeof buffer);
        if (count < 0 && errno == EINTR) continue;
        require(count >= 0, "cannot read borrowed primary");
        if (count == 0) break;
        primary_bytes.append(buffer, static_cast<std::size_t>(count));
    }
    require_trailer(primary_bytes);
    require_trailer(read_sink(companion));
    require(original->complete() && read_sink(companion) == original_bytes,
            "begin failed to reset the moved replay sequences and trailer");
    install_sink(host, sink_path("reconfigured-last"));
    require(host.replay() == original && read_sink(companion) == original_bytes,
            "path reconfiguration lost the replay owner");
    require(::unlink(companion.c_str()) == 0, "cannot retire the second replay file");
    configure(host, 3);
    run(host, bars);
    Report third;
    host.fill_report(&third.value);
    require_trailer(read_sink(companion));
    require(read_sink(companion) == original_bytes, "path reconfiguration changed replay bytes");
}

void failed_run_has_no_companion_trailer() {
    const auto companion = sink_path("failed-bound-replay");
    {
        FailingHost host;
        configure(host);
        install_sink(host, sink_path("failed-primary"));
        require(host.set_report_replay_v1(companion.c_str(), "failed-run") == 0,
                "replay configuration refused");
        const auto bars = tape(4600);
        host.run(bars.data(), static_cast<int>(bars.size()));
        require(host.calculations == 4500 && !host.last_error().empty(),
                "fixture did not fail after recording equity");
        const auto before = read_sink(companion);
        // Several IO windows must already be visible: an empty file cannot
        // satisfy this failure-shape witness.
        require(before.find("{\"schema\":\"pf-report-replay\"") == 0,
                "failed run did not create a companion header");
        require(before.find("[\"Q\",") != std::string::npos, "failed run wrote no Q records");
        require(before.find("\"trailer\":") == std::string::npos,
                "failed run published a replay trailer");
        require(receipt(host).complete == 0, "failed run published a primary receipt");
        const auto error = host.last_error();
        try {
            Report out;
            host.fill_report(&out.value);
        } catch (const std::exception&) {
            // A report may refuse a failed run; it still cannot certify it.
        }
        require(host.last_error() == error, "report retrieval erased the run failure");
        require(read_sink(companion).find("\"trailer\":") == std::string::npos,
                "report retrieval certified the failed replay");
    }
    require(read_sink(companion).find("\"trailer\":") == std::string::npos,
            "destruction certified the failed replay");
}
}

int main() {
    return test_main("test_replay_engine_lifecycle", [] {
        public_streamed_without_replay(false);
        public_streamed_without_replay(true);
        configuration_lifetime();
        failed_run_has_no_companion_trailer();
    });
}
