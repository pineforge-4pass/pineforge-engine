// X1: observer admission order must not turn omitted trades into empty metrics.
// Public APIs only; the independent FULL run supplies every parity expectation.
#include <pineforge/execution_observer.h>
#include <pineforge/native_host.hpp>
#include <pineforge/report_mode.h>
#include <pineforge/report_sink.h>
#include <pineforge/run_failure.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

constexpr int round_trips = 3;
constexpr int bar_count = 4 * round_trips;
constexpr std::int64_t start_ms = INT64_C(1704067200000);
constexpr std::int64_t minute_ms = 60000;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    static_assert(sizeof result == sizeof value, "binary64 report values required");
    std::memcpy(&result, &value, sizeof result);
    return result;
}

struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
    Report() = default;
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
};

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

class Host final : public NativeStrategyHost {
public:
    int calculations = 0;
    void on_native_run_begin() override { calculations = 0; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const int index = calculations++;
        const int cycle = index / 4;
        if (index % 4 == 0) {
            // Long, short, long; differing units keep the total P&L nonzero.
            const double units = (cycle == 1 ? -1.0 : 1.0) * (cycle + 1);
            put({no::Transact{units}, "entry", ""});
        }
        if (index % 4 == 2) put({no::Flatten{}, "exit", ""});
    }
private:
    void put(const no::Request& request) {
        const auto result = submit(request);
        require(result.status == no::SubmitStatus::Accepted && result.handle.has_value(),
                "fixture order refused");
    }
};

std::array<Bar, bar_count> tape() {
    std::array<Bar, bar_count> bars{};
    for (int index = 0; index < bar_count; ++index) {
        const double price = 100.0 + index;
        bars[static_cast<std::size_t>(index)] =
            {price, price + 0.5, price - 0.5, price + 0.25,
             1.0, start_ms + index * minute_ms};
    }
    return bars;
}

void configure(Host& host) {
    NativeRunSpec spec;
    spec.identity = {"x1-observer-summary-only", 1};
    spec.input_tf = spec.script_tf = "1";
    spec.tickerid = "TEST:X1";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    spec.report_open_position_at_end = true;
    const auto result = host.configure_native(spec);
    require(result.status == NativeSetupStatus::Applied,
            "configure refused: field=" + std::to_string(static_cast<int>(result.validation.field))
                + " error=" + std::to_string(static_cast<int>(result.validation.error)));
}

pf_strategy_t handle(BacktestEngine& host) { return &host; }

struct Observer {
    int calls = 0;
    std::uint64_t generation = 0;
};

int before_results(void* context, const pf_execution_boundary_v1* boundary,
                   pf_boundary_receipt_v1* receipt) {
    auto& observer = *static_cast<Observer*>(context);
    ++observer.calls;
    if (!boundary || !receipt || boundary->struct_size != sizeof *boundary
        || boundary->version != 1 || boundary->run_generation == 0)
        return 1;
    observer.generation = boundary->run_generation;
    receipt->frame_bytes = 0;
    receipt->handed_bytes = 0;
    receipt->export_requested = 0;
    receipt->reserved = 0;
    return 0;
}

void attach(Host& host, Observer& observer) {
    const pf_execution_observer_v1 descriptor{
        sizeof descriptor, 1, &observer, before_results};
    const int status = strategy_set_execution_observer_v1(handle(host), &descriptor);
    require(status == 0, "observer registration status=" + std::to_string(status));
}

void run(Host& host, const Observer* observer = nullptr) {
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    require(host.last_run_status() == 0 && host.last_error().empty(),
            "run status=" + std::to_string(host.last_run_status())
                + " code=" + run_failure_code_of(host) + " error=" + host.last_error());
    require(host.native_state().kind == NativeLifecycleKind::Completed,
            "fixture did not complete");
    require(host.calculations == bar_count,
            "calculation count=" + std::to_string(host.calculations)
                + " expected=" + std::to_string(bar_count));
    const auto position = host.physical_position();
    require(position.lot_count == 0 && position.signed_units == 0.0,
            "fixture did not finish flat: lots=" + std::to_string(position.lot_count)
                + " units=" + std::to_string(position.signed_units));
    if (observer) {
        pf_execution_observation_v1 observation{};
        observation.struct_size = sizeof observation;
        observation.version = 1;
        const int status = strategy_execution_observation_v1(handle(host), &observation);
        require(status == 0 && observer->calls == 1 && observer->generation != 0
                    && observation.run_generation == observer->generation
                    && observation.boundary_delivered == 1 && observation.phase == 4
                    && observation.fault_stage == 0 && observation.attempt_outcome == 4,
                "observer capture missing: query=" + std::to_string(status)
                    + " calls=" + std::to_string(observer->calls)
                    + " callback_generation=" + std::to_string(observer->generation)
                    + " generation=" + std::to_string(observation.run_generation)
                    + " boundary=" + std::to_string(observation.boundary_delivered)
                    + " phase=" + std::to_string(observation.phase)
                    + " fault=" + std::to_string(observation.fault_stage)
                    + " outcome=" + std::to_string(observation.attempt_outcome));
    }
}

void print_report(const char* label, const ReportC& report) {
    std::fprintf(stderr,
        "%s: N=%d total=%d trades_len=%d trades=%p all=%d longs=%d shorts=%d "
        "equity_len=%lld equity=%p\n",
        label, round_trips, report.total_trades, report.trades_len,
        static_cast<const void*>(report.trades), report.metrics.all.num_trades,
        report.metrics.longs.num_trades, report.metrics.shorts.num_trades,
        static_cast<long long>(report.equity_curve_len),
        static_cast<const void*>(report.equity_curve));
    const std::array<double, 4> values{{report.net_profit, report.metrics.all.net_profit,
        report.metrics.longs.net_profit, report.metrics.shorts.net_profit}};
    const std::array<const char*, 4> names{{"net_profit", "all.net_profit",
        "longs.net_profit", "shorts.net_profit"}};
    for (std::size_t index = 0; index < values.size(); ++index)
        std::fprintf(stderr, "%s: %s=%.17g bits=0x%016llx\n",
            label, names[index], values[index],
            static_cast<unsigned long long>(bits(values[index])));
}

struct Checks {
    const char* row;
    bool good = true;
    void check(bool condition, const char* message) {
        if (condition) return;
        good = false;
        std::fprintf(stderr, "%s: %s\n", row, message);
    }
    bool finish(const ReportC& actual, const ReportC& oracle,
                const pf_report_metadata_v1* metadata = nullptr) const {
        if (!good) {
            print_report(row, actual);
            print_report("FULL oracle", oracle);
            if (metadata)
                std::fprintf(stderr,
                    "%s: mode=%u complete=%u equity.logical_count=%lld "
                    "equity.captured=%u equity.external=%u resident=%u\n",
                    row, metadata->mode, metadata->complete,
                    static_cast<long long>(metadata->equity_curve.logical_count),
                    metadata->equity_curve.captured, metadata->equity_curve.external,
                    actual.equity_curve_len != 0 ? 1u : 0u);
        }
        return good;
    }
};

void compare(Checks& checks, const ReportC& actual, const ReportC& oracle) {
    checks.check(actual.total_trades == round_trips, "total_trades != N");
    checks.check(actual.total_trades == oracle.total_trades, "total differs from FULL");
    checks.check(actual.metrics.all.num_trades == actual.total_trades,
                 "metrics.all.num_trades != total_trades");
    checks.check(actual.metrics.all.num_trades == oracle.metrics.all.num_trades,
                 "all-trade count differs from FULL");
    checks.check(actual.metrics.longs.num_trades == oracle.metrics.longs.num_trades,
                 "long-trade count differs from FULL");
    checks.check(actual.metrics.shorts.num_trades == oracle.metrics.shorts.num_trades,
                 "short-trade count differs from FULL");
    checks.check(static_cast<std::int64_t>(actual.metrics.longs.num_trades)
                     + actual.metrics.shorts.num_trades == actual.metrics.all.num_trades,
                 "long + short count differs from all");
    checks.check(bits(actual.net_profit) == bits(oracle.net_profit),
                 "net_profit bits differ from FULL");
    checks.check(bits(actual.metrics.all.net_profit) == bits(oracle.metrics.all.net_profit),
                 "all.net_profit bits differ from FULL");
    checks.check(bits(actual.metrics.longs.net_profit) == bits(oracle.metrics.longs.net_profit),
                 "longs.net_profit bits differ from FULL");
    checks.check(bits(actual.metrics.shorts.net_profit) == bits(oracle.metrics.shorts.net_profit),
                 "shorts.net_profit bits differ from FULL");
    checks.check(actual.equity_curve_len >= 0
                     && (actual.equity_curve != nullptr) == (actual.equity_curve_len != 0),
                 "equity pointer and resident length disagree");
}

bool typed(RunFailureCode code) {
    return code != RunFailureCode::none && code != RunFailureCode::engine_unclassified_error;
}

bool typed_exception(const char* row, const std::exception& error) {
    // Require a carried code; the classifier's fallback for std::logic_error
    // must not turn an ordinary fixture/assertion error into an accepted refusal.
    const auto* info = dynamic_cast<const RunFailureInfo*>(&error);
    if (!info || !typed(info->run_failure_code())) return false;
    std::printf("%s: typed fill refusal=%s error=%s\n",
                row, run_failure_code_name(info->run_failure_code()), error.what());
    return true;
}

bool withheld(const ReportC& report) {
    return report.total_trades == 0 && report.net_profit == 0.0
        && report.metrics.all.num_trades == 0 && report.metrics.longs.num_trades == 0
        && report.metrics.shorts.num_trades == 0
        && !report.trades && report.trades_len == 0
        && !report.equity_curve && report.equity_curve_len == 0
        && !report.security_diag && report.security_diag_len == 0
        && !report.trace && report.trace_len == 0
        && !report.trace_names && report.trace_names_len == 0
        && !report.broker_state_hash && report.broker_state_hash_len == 0;
}

bool plain(const char* row, Host& host, const ReportC& oracle, bool allow_refusal) {
    Report result;
    try {
        host.fill_report(&result.value);
    } catch (const std::exception& error) {
        if (allow_refusal && typed_exception(row, error)) return true;
        print_report(row, result.value);
        print_report("FULL oracle", oracle);
        throw;
    }
    if (allow_refusal && host.last_run_status() != 0
        && typed(run_failure_value_of(host).code) && withheld(result.value)) {
        std::printf("%s: typed failed-closed fill status=%d code=%s\n",
                    row, host.last_run_status(), run_failure_code_of(host));
        return true;
    }
    Checks checks{row};
    checks.check(host.last_run_status() == 0 && host.last_error().empty(),
                 "plain fill did not preserve successful run status");
    compare(checks, result.value, oracle);
    return checks.finish(result.value, oracle);
}

bool envelope(const char* row, Host& host, const ReportC& oracle,
              ReportMode expected_mode, bool allow_refusal) {
    Envelope result;
    int status = PF_REPORT_MODE_INVALID;
    try {
        status = host.fill_report_envelope_v1(&result.value);
    } catch (const std::exception& error) {
        if (allow_refusal && result.value.metadata.complete == 0
            && withheld(result.value.report) && typed_exception(row, error))
            return true;
        print_report(row, result.value.report);
        print_report("FULL oracle", oracle);
        throw;
    }
    std::printf("%s: envelope status=%d mode=%u expected_mode=%u complete=%u\n",
                row, status, result.value.metadata.mode,
                static_cast<pf_report_mode_v1_t>(expected_mode), result.value.metadata.complete);
    const bool refusal = status == PF_REPORT_MODE_REPLAY_UNAVAILABLE
        || status == PF_REPORT_MODE_UNSUPPORTED || status == PF_REPORT_MODE_CONFLICT;
    if (allow_refusal && refusal && result.value.metadata.complete == 0
        && withheld(result.value.report))
        return true;
    Checks checks{row};
    checks.check(status == PF_REPORT_MODE_ACCEPTED, "envelope was not accepted");
    checks.check(host.last_run_status() == 0 && host.last_error().empty(),
                 "envelope did not preserve successful run status");
    checks.check(result.value.metadata.mode == static_cast<pf_report_mode_v1_t>(expected_mode),
                 "envelope mode differs from expected mode");
    checks.check(result.value.metadata.complete == 1, "completed run has incomplete envelope");
    compare(checks, result.value.report, oracle);
    if (result.value.metadata.complete == 1)
        checks.check(result.value.metadata.equity_curve.captured
                         == (result.value.report.equity_curve_len != 0 ? 1u : 0u),
                     "complete envelope equity capture flag disagrees with resident curve");
    return checks.finish(result.value.report, oracle, &result.value.metadata);
}

struct Suite {
    int failures = 0;
    template<class Row>
    bool row(const char* name, Row body) {
        bool good = false;
        try {
            good = body();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s: exception: %s\n", name, error.what());
        } catch (...) {
            std::fprintf(stderr, "%s: unknown exception\n", name);
        }
        if (!good) ++failures;
        std::printf("%s: %s\n", name, good ? "PASS" : "FAIL");
        return good;
    }
};

bool oracle_run(Report& report) {
    Host host;
    configure(host);
    require(host.set_report_mode_v1(ReportMode::FULL) == PF_REPORT_MODE_ACCEPTED,
            "oracle FULL refused");
    run(host);
    host.fill_report(&report.value);
    const auto& value = report.value;
    print_report("FULL oracle", value);
    require(host.last_run_status() == 0 && host.last_error().empty(), "oracle fill failed");
    require(value.total_trades == round_trips && value.trades_len == round_trips
                && value.trades != nullptr, "oracle must own exactly N trades");
    int longs = 0;
    int shorts = 0;
    double net = 0.0;
    for (int index = 0; index < value.trades_len; ++index) {
        const auto& trade = value.trades[index];
        std::fprintf(stderr, "FULL oracle trade[%d]: open_at_end=%d is_long=%d pnl=%.17g\n",
                     index, trade.open_at_end, trade.is_long, trade.pnl);
        require(trade.open_at_end == 0, "oracle contains a range-end mark, not a closed trip");
        if (trade.is_long) ++longs;
        else ++shorts;
        net += trade.pnl;
    }
    std::fprintf(stderr, "FULL oracle row fold: longs=%d shorts=%d net=%.17g bits=0x%016llx\n",
                 longs, shorts, net, static_cast<unsigned long long>(bits(net)));
    require(longs > 0 && shorts > 0 && value.metrics.all.num_trades == round_trips
                && value.metrics.longs.num_trades == longs
                && value.metrics.shorts.num_trades == shorts,
            "oracle trade metrics do not describe both sides of the closed fixture");
    require(std::isfinite(net) && net != 0.0 && bits(net) == bits(value.net_profit)
                && bits(net) == bits(value.metrics.all.net_profit), "oracle P&L fold differs");
    require(value.equity_curve && value.equity_curve_len > 0, "oracle has no resident curve");
    return true;
}

std::string sink_directory() {
    const std::filesystem::path source(__FILE__);
    require(source.is_absolute() && source.parent_path().filename() == "tests",
            "test requires the CMake source path to locate worktree scratch");
    const auto scratch = source.parent_path().parent_path() / "scratch-s4e";
    std::filesystem::create_directories(scratch);
    std::string directory = (scratch / "x1-observer-XXXXXX").string();
    require(::mkdtemp(directory.data()) != nullptr, "cannot create X1 scratch directory");
    // Preserve the sink evidence; never use the system temp directory or delete it.
    return directory;
}
} // namespace

int main() {
    Suite suite;
    Report oracle;
    // An invalid FULL oracle is a failed control, not evidence about X1.
    if (!suite.row("FULL oracle control", [&] { return oracle_run(oracle); })) return 1;

    suite.row("A", [&] {
        Observer observer;
        Host host;
        configure(host);
        attach(host, observer);
        const int status = host.set_report_mode_v1(ReportMode::SUMMARY_ONLY);
        std::printf("A: setter status=%d expected=%d\n",
                    status, PF_REPORT_MODE_REPLAY_UNAVAILABLE);
        run(host, &observer);
        const bool full = envelope("A", host, oracle.value, ReportMode::FULL, false);
        if (status != PF_REPORT_MODE_REPLAY_UNAVAILABLE)
            std::fprintf(stderr, "A: observer-first setter must refuse with REPLAY_UNAVAILABLE\n");
        return status == PF_REPORT_MODE_REPLAY_UNAVAILABLE && full;
    });

    // B1 and B2 read the SAME completed generation, even when B1 is inconsistent.
    {
        Observer observer;
        Host host;
        bool ready = false;
        try {
            configure(host);
            const int status = host.set_report_mode_v1(ReportMode::SUMMARY_ONLY);
            require(status == PF_REPORT_MODE_ACCEPTED,
                    "B: observer-free SUMMARY_ONLY setter status=" + std::to_string(status));
            attach(host, observer);
            run(host, &observer);
            ready = true;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "B setup: %s\n", error.what());
        } catch (...) {
            std::fprintf(stderr, "B setup: unknown exception\n");
        }
        suite.row("B1", [&] { return ready && plain("B1", host, oracle.value, true); });
        suite.row("B2", [&] {
            return ready && envelope("B2", host, oracle.value, ReportMode::SUMMARY_ONLY, true);
        });
    }

    for (const bool observer_first : std::array<bool, 2>{{true, false}}) {
        const char* row = observer_first ? "C/observer-first" : "C/observer-last";
        suite.row(row, [&] {
            Observer observer;
            Host host;
            configure(host);
            if (observer_first) attach(host, observer);
            const int status = host.set_equity_report_mode(ReportMode::SUMMARY_ONLY);
            require(status == 0, std::string(row) + ": legacy setter status=" + std::to_string(status));
            if (!observer_first) attach(host, observer);
            run(host, &observer);
            const auto active = host.equity_report_mode();
            require(active == ReportMode::SUMMARY_ONLY,
                    "legacy equity mode=" + std::to_string(static_cast<unsigned>(active))
                        + " expected=" + std::to_string(PF_REPORT_MODE_SUMMARY_ONLY));
            return plain(row, host, oracle.value, true);
        });
    }

    suite.row("D", [&] {
        Observer observer;
        Host host;
        configure(host);
        require(host.set_report_mode_v1(ReportMode::FULL) == PF_REPORT_MODE_ACCEPTED,
                "D: FULL setter refused");
        const auto directory = sink_directory();
        const auto path = directory + "/full-observed.ndjson";
        const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "p2cx-x1", directory.c_str()};
        const int status = host.set_report_sink_v1(&sink);
        require(status == 0, "D: path sink setter status=" + std::to_string(status));
        attach(host, observer);
        run(host, &observer);
        const bool parity = plain("D", host, oracle.value, false);
        pf_report_sink_receipt_v1 receipt{};
        receipt.struct_size = sizeof receipt;
        receipt.version = 1;
        const int receipt_status = host.report_sink_receipt_v1(&receipt);
        std::printf("D: receipt status=%d complete=%u count_t=%lld expected=%d sink=%s\n",
                    receipt_status, receipt.complete, static_cast<long long>(receipt.count_t),
                    oracle.value.total_trades, path.c_str());
        require(receipt_status == 0 && receipt.complete == 1
                    && receipt.count_t == oracle.value.total_trades,
                "D: sink control did not publish exactly the oracle's closed trades");
        return parity;
    });

    std::printf("test_x1_observer_summary_only_counts: %d failed rows\n", suite.failures);
    return suite.failures == 0 ? 0 : 1;
}
