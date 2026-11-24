#include "run_results_release_fixture.hpp"

#if defined(__linux__)

#include "../src/retired_trade_history_read.hpp"
#include <pineforge/cache_policy.hpp>
#include <pineforge/detail/scratch_directory.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/report_sink.h>
#include <pineforge/run_failure.hpp>

#include <cstdlib>
#include <variant>

namespace {

namespace pf = pineforge;
namespace no = pineforge::native_order;
using namespace run_results_test;
using Report = OwnedReport<pf::ReportC, &pf::BacktestEngine::free_report>;

class RoundTripHost final : public pf::NativeStrategyHost {
public:
    explicit RoundTripHost(bool batch_sink = false) : replace_entries_(batch_sink) {
        pf::CacheDefaults cache;
        cache.ledger_cache_max_bytes = 0;
        if (batch_sink) {
            cache.equity_curve_pages = {};
            cache.successor_root_pages = {};
        }
        set_cache_policy(cache);
    }

    void configure(int run, pf::NativeEventRetention retention = pf::NativeEventRetention::Window) {
        pf::NativeRunSpec spec;
        spec.identity = {"results-release-native", static_cast<std::uint64_t>(run)};
        spec.input_tf = spec.script_tf = "5";
        spec.ticker = "MOCK";
        spec.tickerid = "TEST:MOCK";
        spec.type = "crypto";
        spec.currency = "USDT";
        spec.basecurrency = "ETH";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = kCapital;
        spec.point_value = spec.account_fx = 1.0;
        spec.price_tick = kTick;
        spec.fee_kind = pf::NativeFeeKind::CashPerUnit;
        spec.report_policy = pf::NativeReportPolicy::KernelRecorded;
        spec.event_retention = retention;
        const auto result = configure_native(spec);
        require(result.status == pf::NativeSetupStatus::Applied,
                "fixture: configure_native: " + last_error());
        run_ = run;
    }

    using pf::BacktestEngine::closed_trade_entry_comment;

private:
    void on_native_run_begin() override { bars_ = 0; }

    void on_native_bar(const pf::Bar&, const pf::NativeDecisionContext&) override {
        const int i = bars_++;
        // Bar 0 is idle. Submit at 1 and 3 of each group, fill at 2 and 4.
        if (i % 4 == 1) {
            const no::Request request{no::Transact{1.0}, order_label("entry", run_, i / 4),
                                      entry_comment(run_, i / 4)};
            const auto handle = place(request);
            if (replace_entries_) {
                const auto replaced = replace_market(handle, request);
                require(replaced.status == no::ReplaceStatus::Replaced && replaced.successor,
                        "fixture: market replacement refused");
            }
        } else if (i % 4 == 3) {
            place({no::Flatten{}, order_label("exit", run_, i / 4), "close"});
        }
    }

    no::RequestHandle place(const no::Request& request) {
        auto result = submit_market(request);
        require(result.status == no::SubmitStatus::Accepted && result.handle,
                "fixture: market request refused");
        return std::move(*result.handle);
    }

    bool replace_entries_;
    int run_ = 0;
    int bars_ = 0;
};

void begin(RoundTripHost& host, int run) {
    host.configure(run);
    const auto warmup = bar<pf::Bar>(0);
    const auto ok = host.stream_begin(&warmup, 1, "5", "5");
    require(ok, "fixture: stream_begin: " + host.last_error());
}

void feed(RoundTripHost& host, int first, int last) {
    for (int i = first; i <= last; ++i) {
        if (!host.stream_push_bar(bar<pf::Bar>(i)))
            throw std::runtime_error("fixture: stream_push_bar: " + host.last_error());
    }
}

void complete(RoundTripHost& host) {
    const auto ok = host.stream_end();
    require(ok, "fixture: stream_end: " + host.last_error());
    require(host.native_state().kind == pf::NativeLifecycleKind::Completed,
            "fixture: native stream did not complete");
}

void check_trade(const pf::Trade& row, int run, int i) {
    require(matches_booked(row, i) && row.entry_id == order_label("entry", run, i)
                && row.entry_comment == entry_comment(run, i)
                && row.exit_id == order_label("exit", run, i) && row.exit_comment == "close",
            "fixture: closed trade differs from the booked trade");
}

bool same_lifecycle(const pf::NativeStateView& a, const pf::NativeStateView& b) {
    return b.kind == pf::NativeLifecycleKind::Running && a.kind == b.kind
        && a.phase == b.phase && a.failure.code == b.failure.code
        && a.decision_floor_ms == b.decision_floor_ms && a.consumed_high_water == b.consumed_high_water;
}

void read_trade_results(const RoundTripHost& host, int run, int n) {
    const auto count = static_cast<std::size_t>(n);
    require(host.closed_trade_count() == count && host.report_trade_count() == n,
            "fixture: wrong number of closed trades");
    for (int i = 0; i < n; ++i) {
        check_trade(host.closed_trade(static_cast<std::size_t>(i)), run, i);
        check_trade(host.get_report_trade(i), run, i);
        require(host.closed_trade_entry_comment(i) == entry_comment(run, i),
                "fixture: entry comment accessor differs from the booked trade");
    }
    auto cursor = count;
    pf::Trade row{};
    for (int i = n - 1; i >= 0; --i) {
        require(host.previous_closed_trade(cursor, row) && cursor == static_cast<std::size_t>(i),
                "fixture: backward traversal lost a closed trade");
        check_trade(row, run, i);
    }
    require(!host.previous_closed_trade(cursor, row), "fixture: backward traversal passed row zero");
}

struct ReleaseCall {
    bool success;
    pf::RunFailureCode code;
    std::string message;

    bool refused_with(std::string_view text) const {
        return !success && code == pf::RunFailureCode::setting_rejected && message == text;
    }
};

ReleaseCall attempt_release(pf::BacktestEngine& host) {
    try {
        host.release_run_results();
        return {true, pf::RunFailureCode::none, {}};
    } catch (const std::logic_error& error) {
        const auto* info = dynamic_cast<const pf::RunFailureInfo*>(&error);
        return {false, info ? info->run_failure_code() : pf::RunFailureCode::none, error.what()};
    }
}

template <class Error, class Read>
bool refuses_read(Measurements& out, std::string_view scenario, const char* name, Read read,
                  std::string_view expected = kReleasedMessage, std::string_view stage = "R") {
    try {
        read();
    } catch (const Error& error) {
        const auto* info = dynamic_cast<const pf::RunFailureInfo*>(&error);
        const std::string_view message(error.what());
        const bool correct = info && info->run_failure_code() == pf::RunFailureCode::engine_invariant
            && message == expected;
        if (!correct)
            out.line(stage, scenario) << " read=" << name << " unexpected_error="
                << std::quoted(error.what()) << '\n';
        return correct;
    } catch (const std::exception& error) {
        out.line(stage, scenario) << " read=" << name << " unexpected_type="
            << std::quoted(error.what()) << '\n';
        return false;
    }
    out.line(stage, scenario) << " read=" << name << " returned without refusal\n";
    return false;
}

bool check_released_reads(Measurements& out, std::string_view scenario, const RoundTripHost& host,
                          std::size_t count, bool journal) {
    using TradeRefusal = pf::detail::RetiredTradeHistoryRead;
    // Check both the disk head and the formerly resident tail of each path.
    bool indexed_reads = true;
    for (const auto i : {std::size_t{0}, count - 1}) {
        const auto closed = refuses_read<TradeRefusal>(out, scenario, "closed_trade", [&] {
            (void)host.closed_trade(i);
        });
        const auto indexed = refuses_read<TradeRefusal>(out, scenario, "get_report_trade", [&] {
            (void)host.get_report_trade(static_cast<int>(i));
        });
        const auto field = refuses_read<TradeRefusal>(out, scenario, "closed_trade_entry_comment", [&] {
            (void)host.closed_trade_entry_comment(static_cast<int>(i));
        });
        indexed_reads = closed && indexed && field && indexed_reads;
    }
    auto cursor = count;
    pf::Trade row{};
    row.entry_comment = "caller-owned";
    const auto previous = refuses_read<TradeRefusal>(out, scenario, "previous_closed_trade", [&] {
        (void)host.previous_closed_trade(cursor, row); // newest, formerly resident
    });
    Report report;
    const auto full = refuses_read<TradeRefusal>(out, scenario, "fill_report", [&] {
        host.fill_report(&report.value);
    });
    const bool events = !journal || refuses_read<std::logic_error>(out, scenario, "native_events",
        [&] { (void)host.native_events(0); }, kJournalReleasedMessage);
    return indexed_reads && previous && cursor == count && row.entry_comment == "caller-owned"
        && full && !report.value.trades && report.value.trades_len == 0 && events;
}

ReleaseResult check_release(Measurements& out, std::string_view scenario, RoundTripHost& host,
                            std::size_t count, bool journal = false) {
    const auto release = attempt_release(host);
    const auto r = out.capture("R", scenario);
    const bool count_kept = host.closed_trade_count() == count;
    const bool released = release.success && r.empty() && count_kept;
    out.line("R", scenario) << " success=" << release.success << " code="
        << static_cast<unsigned>(release.code) << " message=" << std::quoted(release.message)
        << " closed_trades=" << host.closed_trade_count() << " expected_closed_trades=" << count << '\n';
    const auto reads_refused = check_released_reads(out, scenario, host, count, journal);

    const auto again = attempt_release(host);
    const auto r2 = out.capture("R2", scenario);
    const bool stub = release.refused_with(kStubMessage);
    const bool stable = same_files(r, r2) && host.closed_trade_count() == count;
    const bool repeated = again.success && stable && r2.empty()
        && refuses_read<pf::detail::RetiredTradeHistoryRead>(
            out, scenario, "closed_trade", [&] { (void)host.closed_trade(0); }, kReleasedMessage, "R2");
    out.line("R2", scenario) << " success=" << again.success << " unchanged=" << stable
        << " message=" << std::quoted(again.message) << '\n';
    if (released && reads_refused && !repeated)
        out.line("R2", scenario) << " second release changed the results or refused\n";
    return {released && reads_refused, repeated, stub};
}

ReleaseResult stream_case(Measurements& out) {
    constexpr char scenario[] = "stream";
    ReleaseResult result{};
    {
        RoundTripHost host;
        begin(host, 1);
        feed(host, 1, 4 * kRoundTrips);
        const auto a = out.capture("A", scenario);
        require_ledger(a);

        complete(host);
        const auto ended = out.capture("B", scenario, "end");
        require_ledger(ended);
        Report full;
        host.fill_report(&full.value);
        check_full_report(full.value, kRoundTrips);
        read_trade_results(host, 1, kRoundTrips);
        const auto b = out.capture("B", scenario);
        require_ledger(b);
        require(keeps_files(a, ended, kCommandScratch) && keeps_files(ended, b) && keeps_files(b, ended),
                "stage B: completion or report read closed or replaced a results file");
        result = check_release(out, scenario, host, kRoundTrips);

        begin(host, 2);
        check_next_begin(b, out.capture("C", scenario, "begin"));
        require(host.closed_trade_count() == 0, "stage C: stream begin retained the previous count");
        feed(host, 1, 4);
        const auto next_files = out.capture("C", scenario, "first_close");
        require_ledger(next_files);
        require(retired_files(b, next_files), "stage C: first close reused the previous run's files");
        complete(host);
        read_trade_results(host, 2, 1);
        Report next;
        host.fill_report(&next.value);
        check_full_report(next.value, 1);
    }
    require(out.capture("D", scenario).empty(), "stage D: destruction left scratch files open");
    return result;
}

void active_refusal(Measurements& out, bool stub) {
    constexpr char scenario[] = "active_stream";
    Report control;
    {
        RoundTripHost host;
        begin(host, 1);
        feed(host, 1, 4 * kRoundTrips);
        complete(host);
        host.fill_report(&control.value);
        check_full_report(control.value, kRoundTrips);
    }
    {
        RoundTripHost host;
        begin(host, 1);
        feed(host, 1, 4);
        const auto before = out.capture("REFUSAL", scenario, "before");
        require_ledger(before);
        const auto state = host.native_state();
        const auto refusal = attempt_release(host);
        const auto after = out.capture("REFUSAL", scenario, "after");
        out.line("REFUSAL", scenario) << " success=" << refusal.success << " message="
            << std::quoted(refusal.message) << '\n';
        require(refusal.refused_with(stub ? kStubMessage : kBusyMessage),
                "stage REFUSAL: expected a coded refusal of the active stream");
        require(same_files(before, after) && same_lifecycle(state, host.native_state()),
                "stage REFUSAL: refusal changed open files or lifecycle");
        read_trade_results(host, 1, 1);
        feed(host, 5, 4 * kRoundTrips);
        complete(host);
        Report actual;
        host.fill_report(&actual.value);
        check_full_report(actual.value, kRoundTrips);
        read_trade_results(host, 1, kRoundTrips);
        require(same_report(control.value, actual.value), "stage REFUSAL: refusal changed the report");
    }
    require(out.capture("D", scenario).empty(), "stage D: refusal host left scratch files open");
}

// A sink is supported for batch runs only. Its output is named and still
// caller-owned; the private equity file is unlinked and measured.
class SinkDirectory {
public:
    SinkDirectory() {
        std::string refusal;
        int error = 0;
        path_ = pf::detail::resolve_scratch_directory("", refusal, error);
        require(!path_.empty(), "fixture: report sink needs a disk directory: " + refusal);
        path_ += "/run-results-sink-XXXXXX";
        output_.reserve(path_.size() + sizeof("/report.jsonl"));
        if (!::mkdtemp(path_.data()))
            throw std::system_error(errno, std::generic_category(), "mkdtemp for report sink");
        output_.append(path_).append("/report.jsonl");
    }
    ~SinkDirectory() {
        ::unlink(output_.c_str());
        ::rmdir(path_.c_str());
    }
    const char* path() const { return path_.c_str(); }
    const char* output() const { return output_.c_str(); }
private:
    std::string path_;
    std::string output_;
};

ReleaseResult batch_sink_case(Measurements& out) {
    constexpr char scenario[] = "batch_sink";
    // Two accepts and one replacement per round trip alone exceed the
    // journal's resident limit (src/native_order.cpp:1806). The replacements
    // also outgrow the fixed successor root tail.
    constexpr int round_trips = 2048 / 3 + 1;
    SinkDirectory sink_dir;
    ReleaseResult result{};
    {
        RoundTripHost host(true);
        host.configure(1, pf::NativeEventRetention::Commands);
        const pf_report_sink_v1 sink{sizeof(sink), 1, sink_dir.output(), "results", sink_dir.path()};
        require(host.set_report_sink_v1(&sink) == 0, "fixture: batch report sink refused");
        std::vector<pf::Bar> bars;
        bars.reserve(4 * round_trips + 1);
        for (int i = 0; i <= 4 * round_trips; ++i) bars.push_back(bar<pf::Bar>(i));
        host.run(bars.data(), static_cast<int>(bars.size()));
        require(host.last_run_status() == 0 && host.native_state().kind == pf::NativeLifecycleKind::Completed,
                "fixture: batch run failed: " + host.last_error());
        const auto ended = out.capture("B", scenario, "end");
        require_ledger(ended);
        require(contains(ended, kReportScratch) && contains(ended, kOrderScratch) && contains(ended, kRoots),
                "instrument: batch equity curve, command journal or successor roots not observed");
        Report report;
        host.fill_report(&report.value);
        check_summary(report.value, round_trips);
        read_trade_results(host, 1, round_trips);
        pf_report_sink_receipt_v1 receipt{};
        receipt.struct_size = sizeof(receipt);
        receipt.version = 1;
        require(host.report_sink_receipt_v1(&receipt) == 0 && receipt.complete
                    && receipt.count_e == static_cast<std::int64_t>(bars.size())
                    && receipt.count_t == round_trips,
                "fixture: batch report sink did not publish the booked trades and equity marks");
        int accepted = 0;
        int replaced = 0;
        for (const auto& event : host.native_events(0)) {
            if (!event.command) continue;
            if (const auto* row = std::get_if<no::AcceptedEvent>(&*event.command)) {
                require(row->request().label == order_label(accepted % 2 == 0 ? "entry" : "exit",
                                                            1, accepted / 2),
                        "fixture: retained command journal differs from submitted commands");
                ++accepted;
            } else if (const auto* row = std::get_if<no::ReplacedEvent>(&*event.command)) {
                require(row->successor_request().label == order_label("entry", 1, replaced),
                        "fixture: retained command journal differs from replacement commands");
                ++replaced;
            }
        }
        require(accepted == 2 * round_trips && replaced == round_trips,
                "fixture: missing accepted commands in retained journal");

        const auto b = out.capture("B", scenario);
        require_ledger(b);
        require(keeps_files(ended, b) && keeps_files(b, ended),
                "stage B: report reads changed which batch scratch files were open");
        result = check_release(out, scenario, host, round_trips, true);
    }
    require(out.capture("D", scenario).empty(), "stage D: batch host left scratch files open");
    return result;
}

}  // namespace

int main() {
    return run_results_test::measured_main("native", [](auto& out) -> std::string_view {
        const auto stream = stream_case(out);
        active_refusal(out, stream.stub);
        const auto batch = batch_sink_case(out);
        if (!stream.released || !batch.released) return "stage R: completed results were not released correctly";
        if (!stream.repeated || !batch.repeated) return "stage R2: repeated release was not a no-op";
        return {};
    });
}

#else
int main() {
    std::cout << "RELEASE summary=SKIP arm=native reason=\"Linux /proc/self/fd is required\"\n";
    return 77;
}
#endif
