#pragma once

#include <pineforge/report_sink_writer.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/native_execution_consumer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sealed_exit_phase_fixture {
using namespace pineforge;

constexpr std::int64_t start_ms = INT64_C(1736121600000);
constexpr std::int64_t minute_ms = 60000;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();

inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Keep the lab artifacts under the source worktree's named scratch directory.
// No test cleanup removes them: a failed row leaves its actual sink readable.
struct Directory {
    std::string path;
    Directory() {
        const auto root = std::filesystem::path(__FILE__).parent_path().parent_path()
            / "scratch-s4e";
        std::filesystem::create_directories(root);
        const auto pattern = (root / "exit-phase-seal-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const char* made = ::mkdtemp(buffer.data());
        require(made != nullptr, "cannot create exit-phase scratch directory");
        path = made;
        std::fprintf(stderr, "exit-phase artifacts: %s\n", path.c_str());
    }
    std::string file(const char* name) const { return path + '/' + name; }
};

struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
};

struct Row {
    Trade trade;
    std::uint8_t phase = 0;
};

struct Observation {
    int bar = -1;
    std::size_t base = 0, size = 0;
    std::uint64_t broker = 0, extension = 0;
    std::vector<Row> rows;
};

enum class Shape { SameBar, GlobalRecut };

class Host final : public source::PineStrategyHost {
public:
    explicit Host(Shape shape) : shape_(shape) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 3;
        config.commission_value = 0.0;
        config.calc_on_order_fills = false;
        config.process_orders_on_close = false;
        config.close_entries_rule_any = false;
        configure_pine_strategy(config);
        set_trade_history_streamable(true);
        set_broker_state_hash_recording(true);
    }

    const TradeLedger& ledger() const { return trades_; }
    const NativeExecutionConsumer& consumer() const {
        return NativeExecutionConsumer::bound(*this);
    }
    std::uint8_t phase(std::size_t index) const { return adapter_.exit_phase_value(index); }

    std::vector<Observation> observations;
    std::vector<Row> mixed_fills;
    std::size_t base_before_entry = 0;
    std::size_t base_at_fill = 0;
    std::size_t affected_end = 0;

    void on_source_bar(const Bar&) override {
        Observation observation;
        observation.bar = pine_bar_index();
        observation.base = trades_.base();
        observation.size = trades_.size();
        observation.broker = broker_state_hash();
        BrokerStateHashSink extension;
        hash_host_extension(extension);
        observation.extension = extension.h;
        for (std::size_t i = observation.base; i < observation.size; ++i)
            observation.rows.push_back({trades_[i], phase(i)});
        observations.push_back(std::move(observation));

        const int index = pine_bar_index();
        // Six real closes at distinct times let the production seal retire
        // at least four rows before the affected entry. Three later closes
        // retire the affected group too, so the final T comparison reads it.
        if (index < 24 || (index >= 32 && index < 44)) {
            if (index % 4 == 0)
                strategy_entry("warmup", true, missing, missing, 1.0);
            if (index % 4 == 1) strategy_close("warmup");
        }
        if (index == 24) {
            base_before_entry = consumer().closed_trade_prefix().base;
            if (shape_ == Shape::SameBar) {
                strategy_entry("mixed", true, missing, missing, 2.0);
            } else {
                // Z's bucket follows A's. A's two-unit child consumes two
                // of the oldest Z lot's three units under FIFO, then Z's
                // three-unit child consumes Z's last unit and A's two.
                strategy_entry("Z", true, missing, missing, 3.0);
            }
        }
        if (index == 25) {
            if (shape_ == Shape::SameBar) {
                strategy_exit("intrabar", "mixed", 101.0, 50.0,
                              missing, missing, missing, 100.0, "high first", 1.0);
                strategy_exit("open", "mixed", 103.0, 50.0,
                              missing, missing, missing, 100.0, "gap second", 1.0);
            } else {
                // Distinct entry bars make Z the oldest lot independently
                // of any same-point market-entry ordering policy.
                strategy_entry("A", true, missing, missing, 2.0);
            }
        }
        if (index == 26 && shape_ == Shape::GlobalRecut)
            strategy_exit("global", "", 101.0, missing);
    }

protected:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        for (std::size_t i = 0; i < event.closed_trade_count; ++i) {
            const auto index = event.first_trade_index + i;
            const Trade row = trades_[index];
            if (row.entry_id == "mixed") {
                mixed_fills.push_back({row, phase(index)});
                base_at_fill = consumer().closed_trade_prefix().base;
                affected_end = trades_.size();
            } else if (row.exit_id == "global") {
                base_at_fill = consumer().closed_trade_prefix().base;
                affected_end = trades_.size();
            }
        }
    }

private:
    Shape shape_;
};

inline std::vector<Bar> tape(Shape shape) {
    const int inputs_per_chart_bar = shape == Shape::SameBar ? 5 : 1;
    const int touch_bar = shape == Shape::SameBar ? 26 : 27;
    std::vector<Bar> bars;
    for (int i = 0; i < 46 * inputs_per_chart_bar; ++i) {
        Bar bar{100.0, 100.25, 99.75, 100.0, 1.0, start_ms + i * minute_ms};
        if (i == touch_bar * inputs_per_chart_bar) {
            // The first limit is crossed on the High phase, away from Open.
            bar.high = 102.0;
        } else if (shape == Shape::SameBar && i > 26 * inputs_per_chart_bar
                   && i < 27 * inputs_per_chart_bar) {
            // The second limit is gapped at the next intrabar Open, after
            // the High fill but with the same chart-bar exit timestamp.
            bar.open = bar.close = 104.0;
            bar.high = 104.25;
            bar.low = 103.75;
        }
        bars.push_back(bar);
    }
    return bars;
}

inline std::string trade_records(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot read sink: " + path);
    std::string result, line;
    while (std::getline(input, line))
        if (line.compare(0, 5, "[\"T\",") == 0) result += line + '\n';
    require(input.eof(), "sink read failed: " + path);
    return result;
}

inline pf_report_sink_receipt_v1 receipt(const Host& host) {
    pf_report_sink_receipt_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    require(host.report_sink_receipt_v1(&result) == 0, "sink receipt query failed");
    return result;
}

inline void write_reference(const Host& host, const ReportC& report,
                            const std::string& path) {
    report_sink::LineWriter writer({sizeof(pf_report_sink_v1), 1, path.c_str(),
                                   "sealed-exit-phase", nullptr});
    writer.write_header({false, false, false, 0, 0});
    for (int i = 0; i < report.trades_len; ++i) {
        static_assert(sizeof(TradeC) == sizeof(pf_trade_t), "public trade projection");
        pf_trade_t wire{};
        std::memcpy(&wire, &report.trades[i], sizeof wire);
        const auto& identity = host.get_report_trade(i);
        writer.write_t(i + INT64_C(1), wire, identity.entry_incarnation,
                       host.closed_trade_close_cause(i), identity.entry_id,
                       identity.exit_id, identity.exit_comment);
    }
    (void)writer.write_trailer();
}

inline void compare(const Host& reference, const Host& streamed) {
    require(reference.observations.size() == streamed.observations.size(),
            "sink changed callback count");
    std::size_t affected_publications = 0;
    for (std::size_t callback = 0; callback < reference.observations.size(); ++callback) {
        const auto& a = reference.observations[callback];
        const auto& b = streamed.observations[callback];
        const auto at = " at callback " + std::to_string(callback);
        require(a.bar == b.bar && a.size == b.size, "sink changed logical rows" + at);
        require(a.base == 0 && b.base <= b.size, "invalid seal fixture" + at);
        bool saw_affected = false;
        for (std::size_t offset = 0; offset < b.rows.size(); ++offset) {
            const auto& expected = a.rows[b.base + offset];
            const auto& actual = b.rows[offset];
            require(expected.trade.entry_id == actual.trade.entry_id
                    && expected.trade.exit_id == actual.trade.exit_id
                    && expected.trade.entry_time == actual.trade.entry_time
                    && expected.trade.exit_time == actual.trade.exit_time
                    && expected.trade.qty == actual.trade.qty,
                    "sink changed row order/identity" + at);
            require(expected.phase == actual.phase, "exit phase detached from its row" + at);
            if (actual.trade.entry_id == "mixed" || actual.trade.exit_id == "global") {
                const auto wanted = actual.trade.exit_id == "open"
                    ? NativePathPhase::Open : NativePathPhase::High;
                require(actual.phase == static_cast<std::uint8_t>(wanted),
                        "affected row has the wrong fill phase" + at);
                saw_affected = true;
            }
        }
        if (saw_affected) ++affected_publications;
        require(a.broker == b.broker, "sink changed per-callback broker hash" + at);
        require(a.extension == b.extension, "sink changed per-callback extension hash" + at);
    }
    require(affected_publications >= 2, "affected tail was not revisited on later publications");
}

inline void exercise(Shape shape) {
    Directory directory;
    Host reference(shape), streamed(shape);
    const auto sink_path = directory.file("stream.ndjson");
    const pf_report_sink_v1 sink{sizeof sink, 1, sink_path.c_str(),
                                 "sealed-exit-phase", directory.path.c_str()};
    require(streamed.set_report_sink_v1(&sink) == 0, "sink installation failed");
    const auto bars = tape(shape);
    for (Host* host : {&reference, &streamed}) {
        host->run(bars.data(), static_cast<int>(bars.size()), "1",
                  shape == Shape::SameBar ? "5" : "1", shape == Shape::SameBar);
        require(host->last_error().empty(), "run failed: " + host->last_error());
    }
    require(reference.base_before_entry == 0 && reference.ledger().base() == 0,
            "sink-off reference unexpectedly sealed rows");
    require(streamed.base_before_entry > 0 && streamed.base_at_fill > 0,
            "affected entry/fill did not follow a production seal");
    require(streamed.affected_end > streamed.base_at_fill
            && streamed.ledger().base() >= streamed.affected_end,
            "affected rows did not reach the sealed sink prefix");
    require(receipt(streamed).count_t == static_cast<std::int64_t>(streamed.ledger().base()),
            "sealed rows were not emitted by the streaming sink");

    if (shape == Shape::SameBar) {
        for (const Host* host : {&reference, &streamed}) {
            require(host->mixed_fills.size() == 2, "expected two mixed-phase bracket fills");
            const auto& first = host->mixed_fills[0];
            const auto& second = host->mixed_fills[1];
            require(first.trade.exit_id == "intrabar" && second.trade.exit_id == "open"
                    && first.phase == static_cast<std::uint8_t>(NativePathPhase::High)
                    && second.phase == static_cast<std::uint8_t>(NativePathPhase::Open),
                    "fixture did not append the intrabar fill before the Open fill");
            require(first.trade.exit_time == second.trade.exit_time
                    && first.trade.entry_time == second.trade.entry_time,
                    "mixed fills did not share one chart bar and entry");
        }
        require(reference.ledger()[6].exit_id == "open"
                && reference.ledger()[7].exit_id == "intrabar",
                "Open exit must sort before the earlier-appended intrabar exit");
    } else {
        require(reference.ledger()[6].entry_id == "Z" && reference.ledger()[6].qty == 2.0
                && reference.ledger()[7].entry_id == "Z" && reference.ledger()[7].qty == 1.0
                && reference.ledger()[8].entry_id == "A" && reference.ledger()[8].qty == 2.0,
                "global child transactions did not re-cut the FIFO lots into three rows");
        for (std::size_t i = 6; i < 9; ++i)
            require(reference.phase(i) == static_cast<std::uint8_t>(NativePathPhase::High),
                    "re-cut reference lost the fill phase");
    }
    compare(reference, streamed);

    Report expected, actual;
    reference.fill_report(&expected.value);
    streamed.fill_report(&actual.value);
    require(receipt(streamed).complete == 1, "sink did not complete");
    require(receipt(streamed).count_t == expected.value.total_trades
            && expected.value.total_trades == expected.value.trades_len
            && actual.value.total_trades == expected.value.total_trades,
            "sink omitted or duplicated final trades");
    const auto expected_path = directory.file("reference.ndjson");
    write_reference(reference, expected.value, expected_path);
    require(trade_records(sink_path) == trade_records(expected_path),
            "sealed T rows differ from the sink-off report");
}
} // namespace sealed_exit_phase_fixture
