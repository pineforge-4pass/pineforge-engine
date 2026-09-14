// Native-route twin of tests/oracle/test_oracle_pooc_immediate.cpp.
#include <pineforge/source/pine_native_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pineforge;
namespace {
int failures = 0;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
#define CHECK(expression) do { if (!(expression)) { \
    std::printf("FAIL line %d: %s\n", __LINE__, #expression); ++failures; } } while (false)
bool near(double a, double b) { return std::abs(a - b) < 1e-7; }

struct Design {
    bool pooc = true;
    bool different_bars = false;
    bool immediately = false;
    bool any = false;
    bool is_long = true;
    bool explicit_qty = false;
    double quantity = 800000.0;
    double percent = 30.0;
    double step = 0.01;
};

class Probe : public source::PineNativeHost {
public:
    explicit Probe(Design design) : design_(design) {
        initial_capital_ = 10000000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = design.quantity;
        process_orders_on_close_ = design.pooc;
        close_entries_rule_any_ = design.any;
        commission_value_ = 0.0;
        slippage_ = 0;
        margin_long_ = margin_short_ = 0.0;
        qty_step_ = design.step;
        set_syminfo_mintick(0.00001);
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", design_.is_long);
        if (bar_index_ == 2) { close("P1"); if (!design_.different_bars) close("P2"); }
        if (bar_index_ == 4 && design_.different_bars) close("P2");
        if (bar_index_ == 6) strategy_close_all();
    }
private:
    void close(const char* comment) {
        strategy_close("L", comment,
            design_.explicit_qty ? design_.quantity * 0.3 : kNaN,
            design_.explicit_qty ? kNaN : design_.percent,
            design_.immediately);
    }
    Design design_;
};

std::vector<Bar> bars() {
    std::vector<Bar> result;
    for (int i = 0; i < 9; ++i)
        result.push_back({1.1, 1.1, 1.1, 1.1, 1.0, 1747823400000LL + i * 900000LL});
    return result;
}

void check(Design design, double first, double second, double remainder, const char* name) {
    std::printf("%s\n", name);
    Probe probe(design);
    const auto feed = bars();
    for (int run = 0; run < 2; ++run) {
        probe.run(feed.data(), static_cast<int>(feed.size()));
        CHECK(probe.last_error().empty());
        CHECK(probe.trade_count() == (remainder > 0.0 ? 3 : 2));
        if (probe.trade_count() < 2) continue;
        const Trade& a = probe.get_trade(0);
        const Trade& b = probe.get_trade(1);
        CHECK(near(a.qty, first)); CHECK(near(b.qty, second));
        CHECK(a.is_long == design.is_long && b.is_long == design.is_long);
        CHECK(a.exit_comment == "P1" && b.exit_comment == "P2");
        const int delay = design.pooc || design.immediately ? 0 : 1;
        CHECK(a.exit_bar_index == 2 + delay);
        CHECK(b.exit_bar_index == (design.different_bars ? 4 : 2) + delay);
        if (remainder > 0.0 && probe.trade_count() >= 3) CHECK(near(probe.get_trade(2).qty, remainder));
    }
}

void check_entry_id_basis(bool any) {
    class MultiProbe : public source::PineNativeHost {
    public:
        explicit MultiProbe(bool any) {
            initial_capital_ = 10000000; process_orders_on_close_ = true;
            close_entries_rule_any_ = any; pyramiding_ = 2; commission_value_ = 0;
            margin_long_ = margin_short_ = 0;
        }
        void on_source_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("A", true, kNaN, kNaN, 100);
            if (bar_index_ == 1) strategy_entry("B", true, kNaN, kNaN, 200);
            if (bar_index_ == 3) {
                strategy_close("B", "P1", kNaN, 30);
                strategy_close("missing", "invalid", kNaN, 30);
                strategy_close("B", "P2", kNaN, 30);
            }
            if (bar_index_ == 6) strategy_close_all();
        }
    } probe(any);
    const auto feed = bars();
    probe.run(feed.data(), static_cast<int>(feed.size()));
    CHECK(probe.last_error().empty());
    double first = 0, second = 0, rest = 0;
    for (int i = 0; i < probe.trade_count(); ++i) {
        const auto& trade = probe.get_trade(i);
        CHECK(trade.exit_comment != "invalid");
        if (trade.exit_comment == "P1") first += trade.qty;
        else if (trade.exit_comment == "P2") second += trade.qty;
        else rest += trade.qty;
        if (any && (trade.exit_comment == "P1" || trade.exit_comment == "P2"))
            CHECK(trade.entry_id == "B");
    }
    CHECK(near(first, 60) && near(second, 60) && near(rest, 180));
}
} // namespace

int main() {
    for (bool any : {false, true}) for (bool is_long : {false, true}) {
        Design d; d.any = any; d.is_long = is_long;
        check(d, 240000, 240000, 320000, "same-pass POOC percent closes");
        d.different_bars = true;
        check(d, 240000, 168000, 392000, "different-bar control");
        d.different_bars = false; d.immediately = true;
        check(d, 240000, 168000, 392000, "immediately=true re-bases after its fill");
        d.immediately = false; d.explicit_qty = true;
        check(d, 240000, 240000, 320000, "explicit quantity control");
        d.explicit_qty = false; d.percent = 80;
        check(d, 640000, 160000, 0, "over-request caps to the remaining position");
    }
    Design d; d.pooc = false;
    check(d, 240000, 240000, 320000, "FIFO next-open already freezes the call quantity");
    d.different_bars = true;
    check(d, 240000, 168000, 392000, "FIFO next-open different-bar control");
    d = Design{}; d.quantity = 896339.01;
    check(d, 268901.70, 268901.70, 358535.61, "p181342x fractional-lot quantity pin");
    d = Design{}; d.quantity = 3; d.step = 1; d.percent = 40;
    check(d, 1, 1, 1, "integer-lot floor and minimum remain in force");
    check_entry_id_basis(false); check_entry_id_basis(true);
    std::printf("R4-D native POOC immediate twin failures: %d\n", failures);
    return failures ? 1 : 0;
}
