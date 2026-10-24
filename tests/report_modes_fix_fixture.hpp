#pragma once

#include <pineforge/native_host.hpp>
#include <pineforge/report_mode.h>
#include <pineforge/report_sink.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace report_modes_fix {
using namespace pineforge;
namespace no = pineforge::native_order;

constexpr std::int64_t start_ms = INT64_C(1704067200000);
constexpr std::int64_t minute_ms = 60000;

inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

inline bool same_bits(double a, double b) {
    return std::memcmp(&a, &b, sizeof a) == 0;
}

inline void require_distinct_closed_prefix(const ReportC& report, int closed_count) {
    require(report.trades_len >= closed_count, "FULL fixture lacks its closed prefix");
    for (int i = 0; i < closed_count; ++i) {
        const auto& row = report.trades[i];
        require(!row.open_at_end, "warmup row did not really close");
        if (i > 0)
            require(report.trades[i - 1].exit_time < row.exit_time,
                    "warmup closes do not have distinct increasing timestamps");
    }
    if (closed_count > 0) {
        for (int i = closed_count; i < report.trades_len; ++i)
            require(report.trades[closed_count - 1].exit_time < report.trades[i].entry_time,
                    "terminal lot entered before the warmup closes finished");
    }
}

// Each test process owns one mkdtemp directory outside the source checkout.
// Successful rows remove only that directory; failed rows retain evidence.
class FixtureDirectory {
public:
    const std::string& path() {
        if (path_.empty()) {
            const auto* configured = std::getenv("TMPDIR");
            const std::filesystem::path parent =
                configured && *configured ? configured : "/tmp";
            path_ = (parent / "pineforge-s4e-XXXXXX").string();
            require(::mkdtemp(path_.data()) != nullptr, "cannot create fixture directory");
        }
        return path_;
    }
    void remove_on_success() {
        if (path_.empty()) return;
        std::filesystem::remove_all(path_);
        path_.clear();
    }
private:
    std::string path_;
};

inline FixtureDirectory& fixture_directory() {
    static FixtureDirectory directory;
    return directory;
}

inline std::string scratch_directory() {
    return fixture_directory().path();
}

inline std::string sink_path(const char* label) {
    return scratch_directory() + "/" + label + "-"
        + std::to_string(static_cast<long long>(::getpid())) + ".ndjson";
}

inline void install_sink(BacktestEngine& host, const std::string& path) {
    const auto directory = scratch_directory();
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "s4e-fix-rep",
                                 directory.c_str()};
    require(host.set_report_sink_v1(&sink) == 0, "path sink install refused");
}

inline std::string read_sink(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "cannot read fixture sink");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

inline void require_trailer(const std::string& bytes) {
    const auto trailer = bytes.find("{\"trailer\":");
    require(trailer != std::string::npos, "sink has no trailer");
    require(bytes.find('\n', trailer) == bytes.size() - 1,
            "trailer is not the final complete line");
    require(bytes.find("{\"trailer\":", trailer + 1) == std::string::npos,
            "sink has more than one trailer");
}

inline pf_report_sink_receipt_v1 receipt(const BacktestEngine& host) {
    pf_report_sink_receipt_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    require(host.report_sink_receipt_v1(&result) == 0, "receipt query refused");
    return result;
}

inline pf_report_sink_summary_v1 summary(const BacktestEngine& host) {
    pf_report_sink_summary_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    require(host.report_sink_summary_v1(&result) == 0, "summary query refused");
    return result;
}

inline pf_report_sink_diagnostics_v1 diagnostics(const BacktestEngine& host) {
    pf_report_sink_diagnostics_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    require(host.report_sink_diagnostics_v1(&result) == 0, "diagnostics query refused");
    return result;
}

struct Envelope {
    ReportEnvelopeV1 value{};
    Envelope() {
        value.metadata.struct_size = sizeof value.metadata;
        value.metadata.version = PF_REPORT_ENVELOPE_VERSION;
    }
    ~Envelope() { BacktestEngine::free_report(&value.report); }
    Envelope(const Envelope&) = delete;
    Envelope& operator=(const Envelope&) = delete;
};

struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
};

inline NativeRunSpec spec(std::uint64_t run_number = 1) {
    NativeRunSpec result;
    result.identity = {"s4e-fix-rep", run_number};
    result.input_tf = result.script_tf = "1";
    result.tickerid = "TEST:S4E";
    result.timezone = "UTC";
    result.session = "24x7";
    result.initial_capital = 10000.0;
    result.point_value = result.account_fx = 1.0;
    result.price_tick = 0.01;
    result.report_policy = NativeReportPolicy::KernelRecorded;
    result.report_open_position_at_end = true;
    return result;
}

inline std::vector<Bar> tape(int count = 12) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i) {
        const double price = 100.0 + i * 0.25;
        bars.push_back({price, price + 0.5, price - 0.5, price + 0.125,
                        1.0, start_ms + i * minute_ms});
    }
    return bars;
}

class Host : public NativeStrategyHost {
public:
    int hold_lots = 0;
    bool close_first = false;
    bool emit_outputs = false;
    int calculations = 0;

    Host() { declare_outputs(1, 1, 1); }

    void on_native_run_begin() override {
        calculations = 0;
        if (emit_outputs) {
            output_run_begin();
            output_constant(0, 42.0);
        }
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        const int index = calculations++;
        if (emit_outputs) {
            output_bar(context.script_bar_open_ms, context.script_bar_open_ms + minute_ms);
            output_value(0, bar.close);
            output_event(0, bar.close, "s4e event");
        }
        if (close_first && index == 0) put({no::Transact{1.0}, "closed entry", ""});
        if (close_first && index == 2) put({no::Flatten{}, "closed exit", ""});
        const int first_open = close_first ? 4 : 0;
        if (index >= first_open && index < first_open + hold_lots)
            put({no::Transact{1.0}, "held entry", ""});
    }

    std::size_t open_lots() const { return pyramid_entries_.size(); }
    std::size_t sealed_base() const { return trades_.base(); }
    pf_equity_point_t terminal_point() const {
        const auto* point = last_equity_point();
        require(point != nullptr, "fixture has no terminal equity point");
        return *point;
    }

private:
    void put(const no::Request& request) {
        const auto result = submit(request);
        require(result.status == no::SubmitStatus::Accepted && result.handle.has_value(),
                "fixture order refused");
    }
};

inline void configure(Host& host, std::uint64_t run_number = 1) {
    require(host.configure_native(spec(run_number)).status == NativeSetupStatus::Applied,
            "native configure refused");
}

template<class RunHost>
inline void require_completed_run(const RunHost& host, const std::vector<Bar>& bars) {
    require(host.last_run_status() == 0 && host.last_error().empty(),
            "fixture run failed (status " + std::to_string(host.last_run_status())
                + "): " + host.last_error());
    require(host.native_state().kind == NativeLifecycleKind::Completed,
            "fixture did not complete");
    require(host.calculations == static_cast<int>(bars.size()),
            "fixture did not calculate all bars");
}

inline void run(Host& host, const std::vector<Bar>& bars) {
    host.run(bars.data(), static_cast<int>(bars.size()));
    require_completed_run(host, bars);
}

// The presenter refusal must leave native hosts without a presenter eligible.
// Compare their omitted/external rows with the owning FULL view, including
// three nonzero range-end rows and, in the second case, six real closes first.
// ParityHost uses source publication for the production seal and explicitly
// supplies no presentation; the ordinary native Host above stays source-free.
template<class ParityHost>
inline void require_non_presenter_mode_parity(ReportMode mode, const char* label) {
    require(mode == ReportMode::SUMMARY_ONLY || mode == ReportMode::STREAMED,
            "parity fixture needs a non-FULL mode");
    const bool streamed = mode == ReportMode::STREAMED;
    for (bool closed_prefix : {false, true}) {
        const auto bars = tape(closed_prefix ? ParityHost::closed_tape_bars : 12);
        ParityHost full, candidate;
        full.hold_lots = candidate.hold_lots = 3;
        full.close_first = candidate.close_first = closed_prefix;
        require(full.set_report_mode_v1(ReportMode::FULL) == PF_REPORT_MODE_ACCEPTED,
                "non-presenter FULL mode refused");
        const std::string name = std::string(label) + (closed_prefix ? "-closed" : "-open");
        const auto path = sink_path(name.c_str());
        if (streamed) {
            candidate.set_trade_history_streamable(closed_prefix);
            install_sink(candidate, path);
        }
        require(candidate.set_report_mode_v1(mode) == PF_REPORT_MODE_ACCEPTED,
                "non-presenter non-FULL mode refused");
        run(full, bars);
        run(candidate, bars);
        require(full.open_lots() == 3 && candidate.open_lots() == 3,
                "non-presenter fixture did not leave three open lots");
        require(full.closed_rows() == (closed_prefix ? ParityHost::warmup_closes : 0)
                    && candidate.closed_rows() == full.closed_rows(),
                "non-presenter fixture did not produce its real closes");
        require(candidate.sealed_base() == candidate.prefix_base(),
                "non-presenter ledger and report fold disagree on the sealed base");
        if (streamed && closed_prefix) {
            require(candidate.sealed_base() > 0, "non-presenter closed prefix never sealed");
            require(candidate.base_before_open > 0,
                    "non-presenter terminal entries preceded the production seal");
        } else {
            require(candidate.sealed_base() == 0, "retained fixture unexpectedly sealed");
        }

        Report reference;
        full.fill_report(&reference.value);
        const int expected_trades = (closed_prefix ? ParityHost::warmup_closes : 0) + 3;
        require(reference.value.total_trades == expected_trades
                    && reference.value.trades_len == expected_trades,
                "FULL fixture lost closed or range-end rows");
        require_distinct_closed_prefix(reference.value,
                                       closed_prefix ? ParityHost::warmup_closes : 0);
        int open_rows = 0;
        for (int i = 0; i < reference.value.trades_len; ++i) {
            if (!reference.value.trades[i].open_at_end) continue;
            ++open_rows;
            require(reference.value.trades[i].pnl > 0.0, "terminal row has no open PnL");
        }
        require(open_rows == 3, "FULL fixture lacks three terminal rows");
        const auto expected_equity = static_cast<std::int64_t>(bars.size());
        require(reference.value.equity_curve_len == expected_equity,
                "FULL fixture lacks one equity point per bar");
        const auto& last = reference.value.equity_curve[expected_equity - 1];

        for (int read = 0; read < 2; ++read) {
            Envelope result;
            require(candidate.fill_report_envelope_v1(&result.value) == PF_REPORT_MODE_ACCEPTED,
                    "non-presenter envelope refused");
            require(result.value.metadata.complete == 1
                        && result.value.metadata.mode == static_cast<pf_report_mode_v1_t>(mode),
                    "non-presenter envelope has the wrong mode or completion");
            require(result.value.report.total_trades == expected_trades
                        && result.value.metadata.trades.logical_count == expected_trades,
                    "non-FULL report lost closed or terminal trades");
            require(result.value.metadata.equity_curve.logical_count == expected_equity,
                    "non-FULL report lost logical equity points");
            require(result.value.report.trades == nullptr && result.value.report.trades_len == 0
                        && result.value.report.equity_curve == nullptr
                        && result.value.report.equity_curve_len == 0,
                    "non-FULL report owns sequence arrays");
            require(same_bits(result.value.report.net_profit, reference.value.net_profit),
                    "non-presenter non-FULL net-profit bits differ from FULL");
            require(std::memcmp(&result.value.report.metrics, &reference.value.metrics,
                                sizeof(pf_metrics_t)) == 0,
                    "non-presenter non-FULL metric bits differ from FULL");
            if (streamed) {
                const auto got = receipt(candidate);
                const auto folded = summary(candidate);
                const auto provenance = diagnostics(candidate);
                require(got.complete == 1 && folded.complete == 1
                            && got.count_t == expected_trades && folded.count_t == expected_trades,
                        "non-presenter trailer or summary lost terminal trades");
                require(provenance.complete == 1
                            && std::memcmp(&provenance.last, &last, sizeof last) == 0,
                        "non-presenter streamed terminal equity bits differ from FULL");
                require_trailer(read_sink(path));
            } else {
                const auto terminal = candidate.terminal_point();
                require(std::memcmp(&terminal, &last, sizeof last) == 0,
                        "non-presenter summary terminal equity bits differ from FULL");
            }
        }
    }
}

template<class Test>
int test_main(const char* name, Test test) {
    try {
        test();
        fixture_directory().remove_on_success();
        std::printf("%s: PASS\n", name);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: FAIL: %s\n", name, error.what());
        return 1;
    }
}
} // namespace report_modes_fix
