// Pine-free native example: per-bar broker-state hashes, and the host's own
// state folded into them (hash_host_extension).
//
// With report_policy = KernelRecorded and set_broker_state_hash_recording(true)
// the kernel records one broker-state hash per script bar into the report,
// beside the equity point of that bar:
//
//   broker_state_hash_len == equity_curve_len == script_bars_processed
//
// in a batch and across a stream's warmup and realtime legs alike. A row is
// the run's CONTINUATION IDENTITY at that bar -- the kernel's broker state (the
// lots, the realized sums, the extremes, the closed rows) and, ahead of it,
// the state a resume would continue from -- so the array is a replay check
// within one driving mode:
//   * the same drive twice records the same rows;
//   * a run that ends at bar k recorded, as its last row, row k-1 of the
//     longer run (prefix closure).
// Across driving modes it is deliberately NOT equal: the phase (Batch /
// Warmup / Realtime) is folded into the continuation, so a batch and a stream
// over the same bars share no row, and two streams share exactly the bars
// both are still warming up on, then converge one bar past the later warmup
// boundary. What IS the same across drivings is the length identity, the
// outcome -- every closed row -- and the broker half alone: factor the
// continuation out with broker_state_hash_from_execution_hash(fixed), a
// protected member every host inherits, and the rest of the fold is identical
// at every bar in every driving.
//
// This host's next decision depends on state the kernel does not own: a
// streak of rising closes. It folds that state into every hash through
// hash_host_extension, under its own domain tag, so a replay that diverges
// there diverges in the hash. The extension is input to the hash only: it
// moves every per-bar row and the scalar broker_state_hash(), and no fill and
// no continuation -- the same host with the extension off books the same
// trades under the same native_continuation_hash().
//
//   c++ -std=c++17 native_broker_hash_strategy.cpp -lpineforge_kernel -o broker_hash
//
// Nothing here is PineScript: no codegen, no `src/source`, no `src/compat`.

#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <vector>

namespace {

namespace no = pineforge::native_order;

constexpr std::int64_t kFiveMinutes = 5LL * 60LL * 1000LL;
constexpr int kBarCount = 24;
constexpr int kEarlyWarmup = 6;   // one stream warms up on bars [0, 6) ...
constexpr int kLateWarmup = 12;   // ... the other on bars [0, 12)
constexpr int kPrefix = 12;       // a batch that stops after bar 11
// Any fixed execution hash factors the continuation out of the fold.
constexpr std::uint64_t kFixedExecutionHash = 0;

// Closes chosen so the streak rule below trades four round trips: in after
// bars 2, 7, 12 and 18, out after bars 4, 10, 14 and 20.
const double kCloses[kBarCount] = {
    100.0, 101.0, 102.0, 103.0, 102.0, 101.0, 102.0, 103.0,
    104.0, 105.0, 103.0, 104.0, 105.0, 106.0, 104.0, 103.0,
    102.0, 103.0, 104.0, 105.0, 104.0, 103.0, 104.0, 103.5,
};

std::vector<pineforge::Bar> make_bars() {
    std::vector<pineforge::Bar> bars;
    double previous = kCloses[0];
    for (int i = 0; i < kBarCount; ++i) {
        const double close = kCloses[i];
        const double open = previous;
        bars.push_back(pineforge::Bar{open, std::max(open, close) + 0.5,
                                      std::min(open, close) - 0.5, close, 10.0,
                                      i * kFiveMinutes});
        previous = close;
    }
    return bars;
}

class BrokerHashExample : public pineforge::NativeStrategyHost {
public:
    explicit BrokerHashExample(bool fold) : fold_(fold) {}

    // The broker half of the hash at every calculation, continuation factored out.
    std::vector<std::uint64_t> broker_half;

private:
    bool fold_ = true;
    // The host's own durable state: its next decision reads it.
    std::int64_t streak_ = 0;
    double previous_close_ = 0.0;
    std::int64_t entries_ = 0;

    void on_native_run_begin() override {
        streak_ = 0;
        previous_close_ = 0.0;
        entries_ = 0;
        broker_half.clear();
    }

    void on_native_bar(const pineforge::Bar& bar,
                       const pineforge::NativeDecisionContext&) override {
        streak_ = (previous_close_ > 0.0 && bar.close > previous_close_) ? streak_ + 1 : 0;
        previous_close_ = bar.close;
        const double held = physical_position().signed_units;
        if (held == 0.0 && streak_ >= 2) {
            submit({no::Transact{1.0}, "long", "streak"});
            ++entries_;
        } else if (held > 0.0 && streak_ == 0) {
            submit({no::Flatten{}, "flat", "streak"});
        }
        broker_half.push_back(broker_state_hash_from_execution_hash(kFixedExecutionHash));
    }

    // The fold's tail: a domain tag of this host's own, then each durable
    // value in a fixed order. With the extension off this host folds exactly
    // what a host that overrides nothing folds.
    void hash_host_extension(pineforge::BrokerStateHashSink& sink) const override {
        if (!fold_) {
            pineforge::BacktestEngine::hash_host_extension(sink);
            return;
        }
        sink.s("streak-host/v1");
        sink.i(streak_);
        sink.d(previous_close_);
        sink.i(entries_);
    }
};

pineforge::NativeRunSpec make_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-broker-hash-example";
    spec.identity.run_number = 1;
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = pineforge::NativeFeeKind::Percent;
    spec.fee_value = 0.1;
    // The kernel records the report, one point -- and, with the recording
    // switch on, one broker-state hash -- per script bar.
    spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
    return spec;
}

// What one drive leaves behind.
struct Drive {
    std::vector<std::uint64_t> rows;        // the recorded per-bar hashes
    std::vector<std::uint64_t> broker_half;
    std::vector<pineforge::Trade> trades;
    std::int64_t script_bars = 0;
    std::int64_t curve_points = 0;
    std::uint64_t scalar = 0;               // broker_state_hash() after the run
    std::uint64_t continuation = 0;         // native_continuation_hash()
};

Drive harvest(BrokerHashExample& host) {
    assert(host.native_state().kind == pineforge::NativeLifecycleKind::Completed);
    pineforge::ReportC report{};
    host.fill_report(&report);
    Drive drive;
    drive.rows.assign(report.broker_state_hash,
                      report.broker_state_hash + report.broker_state_hash_len);
    drive.script_bars = report.script_bars_processed;
    drive.curve_points = report.equity_curve_len;
    pineforge::BacktestEngine::free_report(&report);
    drive.broker_half = host.broker_half;
    for (int i = 0; i < host.trade_count(); ++i) drive.trades.push_back(host.get_trade(i));
    drive.scalar = host.broker_state_hash();
    drive.continuation = host.native_continuation_hash();
    // One row per script bar, whatever the driving.
    assert(static_cast<std::int64_t>(drive.rows.size()) == drive.script_bars);
    assert(drive.curve_points == drive.script_bars);
    return drive;
}

// run() over the first `bars` bars.
Drive batch(bool fold, int bars) {
    const auto feed = make_bars();
    BrokerHashExample host(fold);
    host.set_broker_state_hash_recording(true);
    const auto setup = host.configure_native(make_spec());
    assert(setup.status == pineforge::NativeSetupStatus::Applied);
    host.run(feed.data(), bars);
    return harvest(host);
}

// stream_begin over the first `warmup` bars, then every other bar pushed live.
Drive stream(bool fold, int warmup) {
    const auto feed = make_bars();
    BrokerHashExample host(fold);
    // Persistent configuration: set it before stream_begin so the warmup leg
    // records too.
    host.set_broker_state_hash_recording(true);
    const auto setup = host.configure_native(make_spec());
    assert(setup.status == pineforge::NativeSetupStatus::Applied);
    const bool begun = host.stream_begin(feed.data(), warmup, "5", "5");
    assert(begun);
    for (int i = warmup; i < kBarCount; ++i) {
        const bool pushed = host.stream_push_bar(feed[static_cast<std::size_t>(i)]);
        assert(pushed);
    }
    const bool ended = host.stream_end(false);
    assert(ended);
    return harvest(host);
}

std::size_t shared_rows(const std::vector<std::uint64_t>& a, const std::vector<std::uint64_t>& b) {
    assert(a.size() == b.size());
    std::size_t same = 0;
    for (std::size_t i = 0; i < a.size(); ++i) same += a[i] == b[i];
    return same;
}

bool same_trades(const Drive& a, const Drive& b) {
    if (a.trades.size() != b.trades.size()) return false;
    for (std::size_t i = 0; i < a.trades.size(); ++i) {
        const auto& x = a.trades[i];
        const auto& y = b.trades[i];
        if (x.entry_time != y.entry_time || x.exit_time != y.exit_time
            || x.entry_price != y.entry_price || x.exit_price != y.exit_price
            || x.qty != y.qty || x.pnl != y.pnl || x.commission != y.commission
            || x.entry_id != y.entry_id || x.exit_id != y.exit_id) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    const Drive folded = batch(true, kBarCount);
    const Drive plain = batch(false, kBarCount);
    std::printf("batch: %zu rows for %lld script bars, %lld equity points, %zu closed trades\n",
                folded.rows.size(), static_cast<long long>(folded.script_bars),
                static_cast<long long>(folded.curve_points), folded.trades.size());
    assert(folded.script_bars == kBarCount && folded.trades.size() == 4);

    // --- hash_host_extension: every row moves, nothing else does -------------
    std::printf("extension on vs off: %zu of %d rows shared; scalar %016llx vs %016llx; "
                "continuation %s\n",
                shared_rows(folded.rows, plain.rows), kBarCount,
                static_cast<unsigned long long>(folded.scalar),
                static_cast<unsigned long long>(plain.scalar),
                folded.continuation == plain.continuation ? "identical" : "DIFFERS");
    assert(shared_rows(folded.rows, plain.rows) == 0);
    assert(folded.scalar != plain.scalar);
    assert(folded.continuation == plain.continuation);
    assert(same_trades(folded, plain));

    // --- within one driving mode: a replay check -----------------------------
    assert(batch(true, kBarCount).rows == folded.rows);
    const Drive prefix = batch(true, kPrefix);
    std::printf("prefix: a batch of %d bars ends on row %016llx; the full batch's row %d is "
                "%016llx\n",
                kPrefix, static_cast<unsigned long long>(prefix.rows.back()), kPrefix - 1,
                static_cast<unsigned long long>(folded.rows[kPrefix - 1]));
    assert(prefix.rows.size() == static_cast<std::size_t>(kPrefix));
    assert(std::equal(prefix.rows.begin(), prefix.rows.end(), folded.rows.begin()));

    // --- across driving modes ------------------------------------------------
    const Drive early = stream(true, kEarlyWarmup);
    const Drive late = stream(true, kLateWarmup);
    const Drive early_plain = stream(false, kEarlyWarmup);
    for (const Drive* drive : {&early, &late, &early_plain}) {
        assert(drive->script_bars == kBarCount && drive->rows.size() == folded.rows.size());
    }
    // The outcome is the batch's, closed row for closed row...
    assert(same_trades(early, folded) && same_trades(late, folded));
    // ...and so is the broker half, at every bar.
    assert(shared_rows(early.broker_half, folded.broker_half) == kBarCount);
    assert(shared_rows(late.broker_half, folded.broker_half) == kBarCount);
    // The rows are not: a batch shares none with a stream.
    std::printf("batch vs stream (warmup %d): %zu of %d rows shared; broker half %zu of %d\n",
                kEarlyWarmup, shared_rows(folded.rows, early.rows), kBarCount,
                shared_rows(folded.broker_half, early.broker_half), kBarCount);
    assert(shared_rows(folded.rows, early.rows) == 0);
    assert(shared_rows(folded.rows, late.rows) == 0);
    // Two streams: equal on the bars both warm up on, different from the
    // earlier boundary through the later one, equal again past it.
    for (int i = 0; i < kBarCount; ++i) {
        const bool same = early.rows[static_cast<std::size_t>(i)]
                          == late.rows[static_cast<std::size_t>(i)];
        assert(same == (i < kEarlyWarmup || i > kLateWarmup));
    }
    std::printf("stream warmup %d vs %d: rows equal on [0,%d) and [%d,%d), different on [%d,%d]\n",
                kEarlyWarmup, kLateWarmup, kEarlyWarmup, kLateWarmup + 1, kBarCount,
                kEarlyWarmup, kLateWarmup);
    // The extension moves every stream row as well.
    assert(shared_rows(early.rows, early_plain.rows) == 0);
    assert(same_trades(early, early_plain));

    // The summary line, printed only once every check above has passed.
    std::printf("per-bar rows %zu/%lld batch, %zu/%lld stream  extension moved %zu/%zu  "
                "batch vs stream shared %zu/%zu rows, %zu/%zu broker half  closed trades: %zu\n",
                folded.rows.size(), static_cast<long long>(folded.script_bars),
                early.rows.size(), static_cast<long long>(early.script_bars),
                folded.rows.size() - shared_rows(folded.rows, plain.rows), folded.rows.size(),
                shared_rows(folded.rows, early.rows), folded.rows.size(),
                shared_rows(folded.broker_half, early.broker_half), folded.broker_half.size(),
                folded.trades.size());
    return 0;
}
