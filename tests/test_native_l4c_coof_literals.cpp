#include <pineforge/source/pine_native_host.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace pineforge;
namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (0)
bool near(double left, double right) { return std::abs(left - right) <= 1e-9; }

class CoofProbe : public source::PineNativeHost {
public:
    CoofProbe() {
        source::PineStrategyConfig config;
        config.calc_on_order_fills = true;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 10;
        config.commission_value = 0.0;
        configure_pine_strategy(config);
    }
    std::string lot_id(int index) const { return open_trade_entry_id(index); }
    double lot_price(int index) const { return open_trade_entry_price(index); }
};

class RefillProbe final : public CoofProbe {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ <= 1 && std::abs(physical_position().signed_units) < 6.0)
            strategy_entry("L" + std::to_string(physical_position().lot_count), true);
    }
};

class ChronologyProbe final : public CoofProbe {
public:
    void on_source_bar(const Bar&) override {
        if (!submitted_) {
            submitted_ = true;
            strategy_entry("Far", true, std::numeric_limits<double>::quiet_NaN(), 108.0);
            strategy_entry("Near", true, std::numeric_limits<double>::quiet_NaN(), 105.0);
        }
    }
private:
    bool submitted_ = false;
};

class StopLimitProbe final : public CoofProbe {
public:
    void on_source_bar(const Bar&) override {
        if (!submitted_) {
            submitted_ = true;
            strategy_entry("M0", true);
            strategy_entry("M1", true);
            strategy_entry("A", true, 95.0, 108.0);
            strategy_entry("B103", true, std::numeric_limits<double>::quiet_NaN(), 103.0);
            strategy_entry("B105", true, std::numeric_limits<double>::quiet_NaN(), 105.0);
        }
    }
private:
    bool submitted_ = false;
};

void first_open_literals() {
    RefillProbe magnified;
    std::vector<Bar> lower;
    for (int i = 0; i < 30; ++i) {
        const double open = i < 15 ? 100.0 : 100.0 + (i - 15) * 0.1;
        lower.push_back({open, open + 1.0, open - 1.0, open + 0.25, 500.0,
                         static_cast<std::int64_t>(i) * 60'000});
    }
    magnified.run(lower.data(), static_cast<int>(lower.size()), "1", "15", true, 4,
                  MagnifierDistribution::ENDPOINTS);
    CHECK(magnified.last_error().empty());
    CHECK(magnified.physical_position().lot_count == 6); // L0: open_lot_count()==6

    ChronologyProbe chronology;
    const Bar chronology_bars[] = {{100,101,99,100,1,900000}, {100,110,99,100,1,1800000}};
    chronology.run(chronology_bars, 2);
    CHECK(chronology.last_error().empty());
    CHECK(chronology.physical_position().lot_count == 2);
    CHECK(chronology.lot_id(0) == "Near");
    CHECK(chronology.lot_id(1) == "Far");
    CHECK(near(chronology.lot_price(1), 108.0)); // L0: near(px[1],108.0)

    StopLimitProbe stops;
    const Bar stop_bars[] = {{100,101,99,100,1,900000}, {100,110,85,100,1,1800000},
                             {100,104,90,95,1,2700000}};
    stops.run(stop_bars, 3);
    CHECK(stops.last_error().empty());
    CHECK(stops.physical_position().lot_count == 5); // L0: ids.size()==5
    CHECK(stops.lot_id(4) == "A");
    CHECK(near(stops.lot_price(4), 95.0));
}
} // namespace

int main() {
    first_open_literals();
    std::printf("L4c public COOF literals: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
