// A live Pine host, not a model of its id tables. The profile
// checkpoint is BEFORE host destruction and BEFORE allocating a report copy.
// No closedtrades/opentrades reads occur in on_source_bar. Normal ctest checks
// the fixture's fills and ids; heap_census.py checks its retained-byte slope.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/report_sink.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <unistd.h>
#include <vector>

namespace {
using namespace pineforge;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;

void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

std::string entry_id(int ordinal, bool long_id) {
    char digits[32];
    // Equal byte lengths in the unique and reused controls, including when
    // ordinal crosses a power of ten. Short ids fit libstdc++'s SSO buffer;
    // the second population deliberately crosses it.
    std::snprintf(digits, sizeof digits, "%014d", ordinal);
    return std::string(long_id ? "census-entry-" : "E") + digits;
}

// Kept out of line so only this caller-owned OHLCV allocation is admissible
// in the Massif reader. An engine copy must have another allocation stack.
[[gnu::noinline]] std::vector<Bar> census_chart_ohlcv(int n, bool daily) {
    std::vector<Bar> bars(static_cast<std::size_t>(n));
    const std::int64_t step = daily ? 86400000LL : 60000LL;
    for (int i = 0; i < n; ++i)
        bars[static_cast<std::size_t>(i)] =
            {100.0, 101.0, 99.0, 100.0, 1000.0, kStart + i * step};
    return bars;
}

[[gnu::noinline]] std::vector<std::int64_t> census_feed_close_ms(const std::vector<Bar>& bars) {
    std::vector<std::int64_t> closes(bars.size());
    for (std::size_t i = 0; i < bars.size(); ++i) closes[i] = bars[i].timestamp + 60000;
    return closes;
}

class CensusHost final : public source::PineStrategyHost {
public:
    CensusHost(std::string mode, int trades, int stride, int period, bool long_id)
        : mode_(std::move(mode)), limit_(trades), stride_(stride), period_(period),
          long_id_(long_id) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = mode_ == "pyramiding" ? 4 : 1;
        config.process_orders_on_close = true;
        config.commission_value = 0.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        // Keep recording OFF: eager admission folding must also work on the
        // production path, which never asks for a broker hash array.
        if (mode_ == "routed_security") register_security_eval(0, "60", "1", false, false);
        if (mode_ == "nday_security") register_security_eval(0, "3D", "1D", false, false);
        if (mode_ == "foreign_security") {
            set_syminfo_string("tickerid", "TEST:CHART");
            register_security_eval(0, "TEST:OTHER", "1", std::string("1"), false, false, false);
        }
    }

    void evaluate_security(int, const Bar&, bool) override { ++evaluations_; }

    void on_source_bar(const Bar& bar) override {
        ++callbacks_;
        const int i = pine_bar_index();
        if (mode_ == "moved_exits") {
            if (i == 0) strategy_entry("L", true, kNa, kNa, 1.0);
            if (live_position_size() > 0.0) {
                const double distance = 10.0 + static_cast<double>(i % 7);
                strategy_exit("guard", "L", bar.close + distance, bar.close - distance);
                ++exit_calls_;
            }
            return; // Deliberately hold the origin for the whole K1 run.
        }
        if (mode_ == "oca_brackets") {
            if (i % 4 == 0) strategy_entry("L", true, kNa, kNa, 2.0);
            if (live_position_size() > 0.0) {
                strategy_exit("pair", "L", 100.5, 99.5);
                ++exit_calls_;
            }
            return;
        }
        if (mode_ == "pyramiding") {
            if (i % 8 < 4) strategy_entry("L", true, kNa, kNa, 1.0);
            if (i % 8 == 6) strategy_close_all();
            return;
        }
        if (mode_ == "partial_exits") {
            if (i % 6 == 0) strategy_entry("L", true, kNa, kNa, 4.0);
            if (i % 6 == 1) {
                strategy_exit("half", "L", 100.5, 99.5, kNa, kNa, kNa, 50.0);
                ++exit_calls_;
            }
            if (i % 6 == 4) strategy_close_all();
            return;
        }
        // This path backs both Q9 populations. Only period_ changes:
        // 1 reuses one id; 0 uses a new id; 31 tests an old id recurring.
        if (i % stride_ == 0 && opened_ < limit_) {
            active_ = entry_id(period_ == 0 ? opened_ : opened_ % period_, long_id_);
            strategy_entry(active_, true, kNa, kNa, 1.0);
            ++opened_;
        }
        if (i % stride_ == 1 && live_position_size() > 0.0) strategy_close(active_);
    }

    void hash_host_extension(BrokerStateHashSink& sink) const override {
        source::PineStrategyHost::hash_host_extension(sink);
        sink.s("cr150-l0b/v1");
        sink.s(mode_); sink.i(limit_); sink.i(stride_); sink.i(period_);
        sink.i(long_id_ ? 1 : 0); sink.s(active_); sink.i(opened_);
        sink.i(callbacks_); sink.i(evaluations_); sink.i(exit_calls_);
    }

    int opened() const { return opened_; }
    int evaluations() const { return evaluations_; }
    int exit_calls() const { return exit_calls_; }
    int callbacks() const { return callbacks_; }

    // The P00 base has declarations only. Detect the later policy method
    // through substitution, so the same fixture runs the old retaining path
    // and the implemented streaming path without a build-only test macro.
    bool make_streamable() { return streamable(*this, 0); }

private:
    template<class Host>
    static auto streamable(Host& host, int)
        -> decltype(host.set_trade_history_streamable(true), bool()) {
        host.set_trade_history_streamable(true);
        return true;
    }
    template<class Host> static bool streamable(Host&, long) { return false; }
    std::string mode_, active_;
    int limit_, stride_, period_;
    bool long_id_;
    int opened_ = 0, callbacks_ = 0, evaluations_ = 0, exit_calls_ = 0;
};

void run_case(const std::string& mode, int bars_count, int trades, int period,
              bool long_id, bool wait) {
    require(bars_count >= 16 && trades > 0, "invalid census population");
    const bool split = mode == "split_magnifier" || mode == "split_no_magnifier";
    const bool daily = mode == "nday_security";
    const int script_bars = split ? bars_count / 15 : bars_count;
    require(script_bars >= trades * 2, "population needs two script bars per trade");
    const int stride = std::max(2, script_bars / trades);
    const auto bars = census_chart_ohlcv(bars_count, daily);
    const auto closes = mode == "foreign_security" ? census_feed_close_ms(bars) : std::vector<std::int64_t>{};
    CensusHost host(mode, trades, stride, period, long_id);
    bool sink_on = false;
    bool policy = false;
    const char* sink_path = std::getenv("PF_HEAP_CENSUS_SINK_PATH");
    // The strong reference links c_abi.cpp from the static archive. CMake's
    // ENABLE_EXPORTS makes its implemented symbols visible to dlsym. P00's
    // declarations alone do not make a feature present.
    require(pf_abi_version() == PF_ABI_VERSION, "fixture/runtime ABI mismatch");
    auto set_sink = reinterpret_cast<decltype(&strategy_set_report_sink_v1)>(
        dlsym(RTLD_DEFAULT, "strategy_set_report_sink_v1"));
    auto get_receipt = reinterpret_cast<decltype(&strategy_report_sink_receipt_v1)>(
        dlsym(RTLD_DEFAULT, "strategy_report_sink_receipt_v1"));
    if (mode == "foreign_security") {
        using BorrowedFeed = int (*)(pf_strategy_t, const char*, const char*, const pf_bar_t*, const int64_t*, int);
        auto borrowed = reinterpret_cast<BorrowedFeed>(dlsym(RTLD_DEFAULT, "strategy_set_symbol_feed_borrowed_v1"));
        if (borrowed) {
            require(borrowed(static_cast<BacktestEngine*>(&host), "TEST:OTHER", "1",
                            reinterpret_cast<const pf_bar_t*>(bars.data()), closes.data(), bars_count) == 0,
                    "borrowed foreign feed refused");
        } else {
            require(host.set_symbol_feed("TEST:OTHER", "1", bars.data(), closes.data(), bars_count),
                    "foreign feed refused");
        }
    }
    if (wait && sink_path && set_sink) {
        require(get_receipt != nullptr, "sink implementation lacks receipt query");
        policy = host.make_streamable();
        const pf_report_sink_v1 descriptor{sizeof(pf_report_sink_v1), 1, sink_path, "L0B", nullptr};
        require(set_sink(static_cast<BacktestEngine*>(&host), &descriptor) == 0,
                "sink installation refused");
        sink_on = true;
    }
    host.run(bars.data(), bars_count, daily ? "1D" : "1", split ? "15" : (daily ? "1D" : "1"),
             mode == "split_magnifier", 4, MagnifierDistribution::ENDPOINTS);
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.callbacks() > 0, "fixture never calculated");
    if (mode == "routed_security" || mode == "nday_security" || mode == "foreign_security")
        require(host.evaluations() > 0, "security fixture never evaluated");
    if (mode == "moved_exits" || mode == "oca_brackets" || mode == "partial_exits")
        require(host.exit_calls() > 1, "exit fixture did not reissue");
    // Validation reads are outside script execution. They make no growing
    // fixture-owned collection and allocate no report/trade copy.
    const bool exact = mode == "unique_ids" || mode == "reused_id";
    if (exact) {
        require(host.opened() == trades, "entry population was not reached");
        require(host.trade_count() == trades, "closed-trade population was not reached");
        for (int i = 0; !sink_on && i < trades; ++i) {
            const Trade& row = host.get_trade(i);
            require(row.entry_id == entry_id(period == 0 ? i : i % period, long_id),
                    "entry identity changed");
            require(row.qty == 1.0 && !row.open_at_end, "fixture did not close a whole lot");
        }
    } else if (mode != "moved_exits") {
        require(host.trade_count() > 0, "battery fixture produced no closed rows");
    }
    if (sink_on) {
        pf_report_sink_receipt_v1 receipt{};
        receipt.struct_size = sizeof receipt;
        receipt.version = 1;
        require(get_receipt(static_cast<BacktestEngine*>(&host), &receipt) == 0 && receipt.complete == 1,
                "sink run did not complete");
        require(!policy || receipt.trades_retained == 0, "streamable fixture retained trade history");
    }
    const int ids = host.opened() == 0 ? 1 :
        (period == 0 ? host.opened() : std::min(host.opened(), period));
    std::printf("PF_HEAP_CENSUS {\"bars\":%d,\"trades\":%d,\"ids\":%d,"
                "\"id_bytes\":%zu,\"callbacks\":%d,\"security_evaluations\":%d,"
                "\"exit_calls\":%d,\"sink\":%s,\"streamable_policy\":%s,"
                "\"phase\":\"native_return\"}\n",
                bars_count, host.trade_count(), ids, entry_id(0, long_id).size(),
                host.callbacks(), host.evaluations(), host.exit_calls(),
                sink_on ? "true" : "false", policy ? "true" : "false");
    std::fflush(stdout);
    if (wait) {
        char acknowledgement = 0;
        require(::read(STDIN_FILENO, &acknowledgement, 1) == 1 && acknowledgement == '\n',
                "profiler did not release checkpoint");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            for (const bool long_id : {false, true})
                for (const int period : {0, 1, 31})
                    run_case(period == 1 ? "reused_id" : "unique_ids", 256, 40,
                             period, long_id, false);
        } else {
            require(argc == 8, "usage: test_heap_census_unique_ids MODE BARS TRADES PERIOD LONG_ID --wait yes");
            require(std::string(argv[6]) == "--wait" && std::string(argv[7]) == "yes",
                    "profiling requires an acknowledged live checkpoint");
            run_case(argv[1], std::stoi(argv[2]), std::stoi(argv[3]), std::stoi(argv[4]),
                     std::stoi(argv[5]) != 0, true);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "heap census fixture: %s\n", error.what());
        return 1;
    }
}
