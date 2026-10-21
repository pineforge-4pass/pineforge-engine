// CR-150 L18 / acceptance (j). Real PineStrategyHost execution: an open
// position receives additions and named 50% limit exits. The sink-off run is
// the reference for every T/H/E record and the entire metric struct.
// No manual ledger sealing, production-private access, or literal outcomes.
#include "../src/source/pine_position_tail.hpp"

#include <pineforge/report_sink.h>
#include <pineforge/report_sink_writer.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using namespace pineforge;
using report_sink::escaped_string;
using report_sink::f64;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t start_ms = INT64_C(1704067200000);
constexpr std::int64_t step_ms = 60000;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Directory {
    char path[64] = "/tmp/pineforge-l18-scale-out-XXXXXX";
    Directory() { require(::mkdtemp(path) != nullptr, "cannot create fixture directory"); }
    ~Directory() { ::rmdir(path); }
};

struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
    Report() = default;
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
};

class ScaleOut final : public source::PineStrategyHost {
public:
    explicit ScaleOut(bool long_side, bool restart = false, int retained_closes = 0)
        : long_side_(long_side), restart_(restart), retained_closes_(retained_closes) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_value = 4.0;
        config.pyramiding = 2;
        config.margin_long = config.margin_short = 0.0;
        config.commission_value = 0.00125;
        config.close_entries_rule_any = false;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        syminfo_mintick_ = 0.01;
        set_broker_state_hash_recording(true);
    }

    std::size_t peak_resident = 0;
    std::size_t peak_sealed = 0;
    int scaled_while_open = 0;

    void tail_work_phase(const char* phase) const {
#ifdef PINEFORGE_POSITION_TAIL_WORK_PHASE
        PINEFORGE_POSITION_TAIL_WORK_PHASE(phase, adapter_.position_tail_work(), &adapter_);
#else
        (void)phase;
#endif
    }

    void on_source_bar(const Bar& bar) override {
        if (bar_index_ == 0) {
            peak_resident = peak_sealed = 0;
            scaled_while_open = 0;
            position_seen_ = false;
        }
        peak_resident = std::max(peak_resident, trades_.size() - trades_.base());
        peak_sealed = std::max(peak_sealed, trades_.base());
        const double units = signed_position_size();
        if (!restart_ && position_seen_) {
            require(long_side_ ? units > 0.0 : units < 0.0,
                    "scale-out fixture did not keep its original position open");
        }
        position_seen_ = position_seen_ || units != 0.0;
        if (retained_closes_ != 0) {
            // Keep the SAME opening incarnation live across every seal.
            // Its small immediate closes accumulate in the spill; the final
            // 50% target needs their quantities to reconstruct its basis.
            // Without the production spill reader, this target is smaller
            // than the sink-off reference even if sealing itself works.
            if (bar_index_ == 0)
                strategy_entry(entry_id(), long_side_, missing, missing,
                               4.0 * (retained_closes_ + 1));
            if (bar_index_ < retained_closes_ * 4 && bar_index_ % 4 == 2) {
                ++scaled_while_open;
                strategy_close(entry_id(), {}, 1.0, missing, true);
            }
            if (bar_index_ == retained_closes_ * 4) {
                ++scaled_while_open;
                strategy_exit("half", entry_id(), bar.close + (long_side_ ? 0.25 : -0.25),
                              missing, missing, missing, missing, 50.0);
            }
            return;
        }
        // The short fixture flattens, reuses the id and reverses on the same
        // script bar, then returns to its original side. The long fixture
        // keeps one position open through every scale-out.
        if (restart_ && (bar_index_ == 41 || bar_index_ == 81)) {
            strategy_close(entry_id(), {}, missing, missing, true);
            strategy_entry(entry_id(), bar_index_ != 41 ? long_side_ : !long_side_,
                           missing, missing, 8.0);
            return;
        }
        const bool side = restart_ && bar_index_ > 41 && bar_index_ <= 81
            ? !long_side_ : long_side_;
        if (bar_index_ % 4 == 0)
            strategy_entry(entry_id(), side, missing, missing, bar_index_ == 0 ? 8.0 : 4.0);
        if (bar_index_ % 4 == 2 && signed_position_size() != 0.0) {
            ++scaled_while_open;
            // One exit id per cycle: an id that already exited (partly) is
            // consumed for this entry, so a repeated "half" never fires again.
            strategy_exit("half" + std::to_string(bar_index_), entry_id(),
                          bar.close + (side ? 0.25 : -0.25), missing,
                          missing, missing, missing, 50.0);
        }
    }

    static const std::string& entry_id() {
        static const std::string id = "scale-out-" + std::string(86, 'L');
        return id;
    }
private:
    bool long_side_;
    bool restart_;
    int retained_closes_;
    bool position_seen_ = false;
};

std::vector<Bar> feed(int exits) {
    std::vector<Bar> result;
    result.reserve(static_cast<std::size_t>(exits) * 4 + 4);
    for (int i = 0; i < exits * 4 + 4; ++i) {
        // Each target reaches the next bar without a margin close. Prices
        // vary to make PnL, percent sums and excursion metrics substantive.
        const double price = 100.0 + (i % 29) * 0.015625;
        result.push_back({price, price + 2.0, price - 2.0, price + 0.0625,
                          1000.0, start_ms + i * step_ms});
    }
    return result;
}

std::string expected_trade(const ScaleOut& reference, int index, const ReportC& report) {
    const auto& row = report.trades[index];
    const auto& identity = reference.get_report_trade(index);
    std::ostringstream out;
    out << "[\"T\"," << index + 1 << (row.is_long ? ",\"long\"," : ",\"short\",")
        << row.entry_time << ',' << row.exit_time;
    for (double value : {row.entry_price, row.exit_price, row.qty, row.pnl, row.pnl_pct,
                         row.max_runup, row.max_drawdown, row.commission})
        out << ',' << f64(value);
    out << ',' << row.entry_bar_index << ',' << row.exit_bar_index
        << (row.open_at_end ? ",true," : ",false,") << identity.entry_incarnation
        << ',' << reference.closed_trade_close_cause(index) << ','
        << escaped_string(identity.entry_id) << ',' << escaped_string(identity.exit_id)
        << ',' << escaped_string(identity.exit_comment) << ']';
    return out.str();
}

std::string expected_hash(std::uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(value));
    return "[\"H\"," + escaped_string(text) + ']';
}

void equal_sink(const ScaleOut& reference, const ReportC& report, const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "cannot read completed scale-out sink");
    int trades = 0;
    std::int64_t hashes = 0, points = 0;
    std::string line;
    while (std::getline(file, line)) {
        if (line.compare(0, 5, "[\"T\",") == 0) {
            require(trades < report.trades_len, "sink has extra trade rows");
            require(line == expected_trade(reference, trades++, report), "scale-out trade differs");
        } else if (line.compare(0, 5, "[\"H\",") == 0) {
            require(hashes < report.broker_state_hash_len, "sink has extra hash rows");
            require(line == expected_hash(report.broker_state_hash[hashes++]), "scale-out hash differs");
        } else if (line.compare(0, 5, "[\"E\",") == 0) {
            require(points < report.equity_curve_len, "sink has extra equity rows");
            const auto& row = report.equity_curve[points++];
            const std::string expected = "[\"E\"," + std::to_string(row.time_ms) + ','
                + f64(row.equity) + ',' + f64(row.open_profit) + ']';
            require(line == expected, "scale-out equity differs");
        }
    }
    require(trades == report.trades_len && hashes == report.broker_state_hash_len
            && points == report.equity_curve_len, "sink omitted reference rows");
}

void run_streamed(const std::vector<Bar>& bars, const ScaleOut& reference,
                  const ReportC& expected, bool long_side, bool restart,
                  bool streamable, int retained_closes, const std::string& path) {
    ScaleOut streamed(long_side, restart, retained_closes);
    streamed.set_trade_history_streamable(streamable);
    const std::string directory = path.substr(0, path.find_last_of('/'));
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "L18", directory.c_str()};
    require(streamed.set_report_sink_v1(&sink) == 0, "scale-out sink refused");
    // Reuse the same host to exercise run reset after the prior spill has
    // been populated and read; the second begin must open a fresh file.
    for (int pass = 0; pass < (restart ? 2 : 1); ++pass) {
        streamed.tail_work_phase("streamed-run-begin");
        streamed.run(bars.data(), static_cast<int>(bars.size()));
        streamed.tail_work_phase("streamed-run-end");
        require(streamed.last_error().empty(), streamed.last_error().c_str());
        Report actual;
        streamed.tail_work_phase("streamed-report-begin");
        streamed.fill_report(&actual.value);
        streamed.tail_work_phase("streamed-report-end");
        require(std::memcmp(&expected.metrics, &actual.value.metrics, sizeof expected.metrics) == 0,
                "scale-out metrics differ");
        require(expected.net_profit == actual.value.net_profit
                && expected.total_trades == actual.value.total_trades,
                "scale-out report totals differ");
        require(reference.broker_state_hash() == streamed.broker_state_hash(),
                "scale-out terminal broker hash differs");
        streamed.tail_work_phase("sink-compare-begin");
        equal_sink(reference, expected, path);
        streamed.tail_work_phase("sink-compare-end");
        pf_report_sink_receipt_v1 receipt{};
        receipt.struct_size = sizeof receipt;
        receipt.version = 1;
        require(streamed.report_sink_receipt_v1(&receipt) == 0 && receipt.complete == 1,
                "scale-out sink did not complete");
        require(receipt.count_t == expected.trades_len
                && receipt.count_h == expected.broker_state_hash_len
                && receipt.count_e == expected.equity_curve_len
                && receipt.trades_retained == (streamable ? 0U : 1U),
                "scale-out receipt counts differ");
        require(streamed.scaled_while_open == reference.scaled_while_open,
                "scale-out command count differs");
        if (streamable) {
            require(streamed.peak_sealed != 0, "fixture did not exercise ledger sealing");
            require(streamed.peak_resident <= 32, "scale-out resident trade tail grew");
            if (!restart) {
                // This fixture has one unbroken position, increasing exit
                // times and one entry id. Every sealed row must therefore
                // still be reachable in its spill at this observation.
                using source::detail::PositionTailSpill;
                const std::size_t record_bytes = ScaleOut::entry_id().size()
                    + PositionTailSpill::kFooterBytes;
                require(streamed.peak_sealed
                            > PositionTailSpill::kWindowBytes / record_bytes,
                        "scale-out fixture did not exceed the spill RAM window");
            }
        } else {
            require(streamed.peak_sealed == 0, "retaining policy dropped trades");
        }
    }
    require(::unlink(path.c_str()) == 0, "cannot remove fixture sink");
}

void differential(int exits, bool long_side, bool restart, bool retaining_control,
                  bool retained_origin = false) {
    Directory directory;
    const auto bars = feed(exits);
    const int retained_closes = retained_origin ? exits : 0;
    ScaleOut reference(long_side, restart, retained_closes);
    reference.tail_work_phase("reference-run-begin");
    reference.run(bars.data(), static_cast<int>(bars.size()));
    reference.tail_work_phase("reference-run-end");
    require(reference.last_error().empty(), reference.last_error().c_str());
    Report expected;
    reference.tail_work_phase("reference-report-begin");
    reference.fill_report(&expected.value);
    reference.tail_work_phase("reference-report-end");
    require(reference.scaled_while_open >= exits && expected.value.trades_len >= exits,
            "scale-out reference did not generate the requested exit population");
    if (retained_origin) {
        bool half_filled = false;
        for (int i = 0; i < reference.trade_count(); ++i)
            half_filled = half_filled || reference.get_trade(i).exit_id == "half";
        require(half_filled, "retained-origin reference did not fill its final percentage exit");
    }
    const std::string root = directory.path;
    run_streamed(bars, reference, expected.value, long_side, restart, true, retained_closes,
                 root + "/spill.ndjson");
    if (retaining_control)
        run_streamed(bars, reference, expected.value, long_side, restart, false, retained_closes,
                     root + "/retain.ndjson");
    std::printf("L18 scale-out differential: %s, %s, %d exits, %d trades, %lld hashes\n",
                long_side ? "long" : "short", retained_origin ? "retained origin" : "adds",
                exits, expected.value.trades_len,
                static_cast<long long>(expected.value.broker_state_hash_len));
}

} // namespace

int main(int argc, char** argv) {
    try {
#ifdef PINEFORGE_L18_SCALE_OUT_100K
        const bool large = true;
        require(argc == 1, "usage: test_position_tail_scale_out_100k");
        (void)argv;
#else
        const bool large = argc == 2 && std::strcmp(argv[1], "--100k") == 0;
        require(argc == 1 || large, "usage: test_position_tail_scale_out [--100k]");
#endif
        differential(large ? 100000 : 1000, true, false, !large);
        if (!large) {
            differential(1000, false, false, true);
            differential(32, true, true, false);
        }
        differential(1024, true, false, true, true);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "L18 scale-out failure: %s\n", error.what());
        return 1;
    }
    return 0; // also permits the R11 fixture to include this entry point by name
}
