// Native-route literals from the A/B/E cells of tests/oracle/test_oracle_frozen_size.cpp.
#include <pineforge/source/pine_native_host.hpp>

#include "oracle_fixture_config_shim.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;
namespace {
int passed = 0;
int failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
Bar bar(int64_t ts, double o, double h, double l, double c) { return {o,h,l,c,1.0,ts}; }

class Probe final : public source::PineNativeHost {
public:
    Probe(QtyType type, double value, bool pooc) {
        initial_capital_ = 10000.0;
        default_qty_type_ = type;
        default_qty_value_ = value;
        commission_value_ = 0.0;
        process_orders_on_close_ = pooc;
        margin_call_enabled_ = false;
    }
    std::string script;
    void on_source_bar(const Bar&) override {
        if (bar_index_ < 0 || bar_index_ >= static_cast<int>(script.size())) return;
        if (script[bar_index_] == 'L') strategy_entry("L", true);
        else if (script[bar_index_] == 'C') strategy_close_all();
    }
};

void flat_gap_down_percent() {
    Probe probe(QtyType::PERCENT_OF_EQUITY, 100.0, false);
    probe.script = "L.C.";
    const std::vector<Bar> bars = {
        bar(1000,100,100,100,100), bar(2000,98,98,98,98),
        bar(3000,98,98,98,98), bar(4000,98,98,98,98)};
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    if (probe.trade_count() == 1) {
        const auto& t = probe.get_trade(0);
        CHECK(std::abs(t.entry_price - 98.0) < 1e-9);
        CHECK(std::abs(t.qty - 100.0) < 1e-9);
    }
}

void cash_gap_down() {
    Probe probe(QtyType::CASH, 1000.0, false);
    probe.script = "L.C.";
    const std::vector<Bar> bars = {
        bar(1000,100,100,100,100), bar(2000,98,98,98,98),
        bar(3000,98,98,98,98), bar(4000,98,98,98,98)};
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    if (probe.trade_count() == 1) {
        const auto& t = probe.get_trade(0);
        CHECK(std::abs(t.entry_price - 98.0) < 1e-9);
        CHECK(std::abs(t.qty - 10.0) < 1e-9);
    }
}

void gap_up_rejected() {
    Probe probe(QtyType::PERCENT_OF_EQUITY, 100.0, false);
    probe.script = "L.C.";
    const std::vector<Bar> bars = {
        bar(1000,100,100,100,100), bar(2000,102,103,101,102),
        bar(3000,102,102,102,102), bar(4000,102,102,102,102)};
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 0);
    CHECK(probe.physical_position().signed_units == 0.0);
}
} // namespace

int main() {
    flat_gap_down_percent();
    cash_gap_down();
    gap_up_rejected();
    std::printf("R4-D native frozen-size twin: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
