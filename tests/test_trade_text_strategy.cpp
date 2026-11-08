#include <pineforge/trade_text_source.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/trade_text_writer.hpp"
#include "../runner/report.hpp"

#include <array>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
class TextStrategy final : public pineforge::source::PineStrategyHost {
public:
    TextStrategy() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000;
        config.default_qty_value = 1;
        configure_pine_strategy(config);
    }
    void on_source_bar(const pineforge::Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 1) strategy_close("L", "digits 3678.7059999999997");
    }
};
}

#ifdef PINEFORGE_TRADE_TEXT_MODULE
extern "C" {
PF_API void* strategy_create(const char*) {
    try { return static_cast<pineforge::BacktestEngine*>(new TextStrategy); }
    catch (...) { return nullptr; }
}
PF_API void strategy_free(void* state) {
    delete static_cast<pineforge::BacktestEngine*>(state);
}
PF_API void strategy_set_input(void* state, const char* key, const char* value) {
    static_cast<TextStrategy*>(static_cast<pineforge::BacktestEngine*>(state))->set_input(key, value);
}
PF_API void strategy_set_override(void*, const char*, const char*) {}
// Retain the archive object supplying the standard stream ABI.
PF_API int trade_text_fixture_abi_version() { return pf_abi_version(); }
}
#else
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main(int argc, char** argv) {
    try {
        TextStrategy strategy;
        const std::array<pineforge::Bar, 3> bars{{
            {100, 100, 100, 100, 1, INT64_C(1704067200000)},
            {100, 100, 100, 100, 1, INT64_C(1704067260000)},
            {100, 100, 100, 100, 1, INT64_C(1704067320000)}
        }};
        strategy.run(bars.data(), 3, "1", "1");
        require(strategy.last_error().empty(), "strategy execution");
        pineforge::report::EngineReportTradeSourceV1 source(strategy);
        // Entry fills on bar 1, close on bar 2; no open position remains.
        require(source.size() == 1, "one finished strategy trade");
        const auto row = source.at(0);
        std::ostringstream ordinary;
        ordinary << pineforge::live::report_trade(row.value).dump() << '\n';
        // Flat prices and zero costs give zero PnL/excursions; map keys sort lexically.
        const std::string expected =
            "{\"commission\":0,\"entry_bar_index\":1,\"entry_price\":100,"
            "\"entry_time\":1704067260000,\"exit_bar_index\":2,\"exit_price\":100,"
            "\"exit_time\":1704067320000,\"is_long\":1,\"max_drawdown\":0,"
            "\"max_runup\":0,\"open_at_end\":0,\"pnl\":0,\"pnl_pct\":0,\"qty\":1}\n";
        require(ordinary.str() == expected, "ordinary trade writer literal bytes");
        if (argc == 2 && std::string(argv[1]) == "--primary-wire") {
            pineforge::ReportC report{};
            struct Release {
                pineforge::ReportC& report;
                ~Release() { pineforge::BacktestEngine::free_report(&report); }
            } release{report};
            strategy.fill_report(&report);
            const pf_report_sink_fd_v1 descriptor{sizeof(descriptor), 1, 1, "default"};
            pineforge::report_sink::LineWriter writer(descriptor);
            writer.write_header({false, false, false, 0, 0});
            for (int index = 0; index < report.equity_curve_len; ++index) {
                const auto& point = report.equity_curve[index];
                writer.write_e({point.time_ms, point.equity, point.open_profit});
            }
            const auto identity = strategy.get_report_trade(0);
            writer.write_t(1, row.value, row.entry_incarnation, row.close_cause,
                           identity.entry_id, identity.exit_id, identity.exit_comment);
            writer.write_trailer();
            return 0;
        }
        require(argc == 1, "unknown test argument");
        std::ostringstream companion;
        pineforge::report::write_trade_text_companion(companion, "strategy", source, 4);
        // The first entry has incarnation 1; a market close uses the __close__L exit ID.
        require(companion.str().find(
            "[\"T\",\"strategy\",0,1,\"L\",\"__close__L\",\"\",\"digits 3678.7059999999997\"]\n")
                != std::string::npos, "strategy text bytes");
        std::ostringstream after;
        after << pineforge::live::report_trade(source.at(0).value).dump() << '\n';
        // Reading the separate companion does not change the ordinary projection.
        require(after.str() == expected, "ordinary bytes after companion read");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL trade text strategy: %s\n", error.what());
        return 1;
    }
    std::puts("PASS trade text strategy");
    return 0;
}
#endif
