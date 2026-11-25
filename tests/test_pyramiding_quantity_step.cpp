// Spending the newer entry's bracket frees a slot even when the FIFO fill
// leaves both physical lots open. The size of a slot is one entry, not units.
#include <filesystem>
#define PINEFORGE_ORDER_PRINT_FIXTURE_ROOT (std::filesystem::path(__FILE__).parent_path() / "fixtures").string()
#include "order_print_tape_fixture.hpp"
#undef PINEFORGE_ORDER_PRINT_FIXTURE_ROOT

#include <array>
#include <utility>

namespace {
using namespace pineforge;
constexpr double na = order_print_tape::missing;
constexpr int start = 414, finish = 424;

class Host final : public source::PineStrategyHost {
public:
    Host(bool buy, double step, bool unmargined) : buy_(buy) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.pyramiding = 2;
        if (unmargined) config.margin_long = config.margin_short = 0;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        if (step > 0) set_syminfo_metadata("qty_step", step);
    }
    void on_source_bar(const Bar&) override {
        const int i = start + pine_bar_index();
        if (i == 414) strategy_entry("E", buy_, na, na, 0.75);
        if (i == 415) strategy_entry("E", buy_, na, na, 0.25);
        // Refused while both records are live; accepted after the higher
        // entry's bracket fills. A partial FIFO lot is still in the book.
        if (i == 417 || i == 422) strategy_entry("E", buy_, na, na, 0.50);
        strategy_exit("X", "E", na, na, na, na, na, 100, "", na, "", 50000, 50000);
        if (i == 422 || i == 423) {
            auto& lots = i == 422 ? before : after;
            for (const auto& lot : pyramid_entries_)
                lots.emplace_back(start + lot.entry_bar_index, lot.qty);
        }
    }
    using Lots = std::vector<std::pair<int, double>>;
    Lots before, after;
private:
    bool buy_;
};

bool check(const std::vector<Bar>& bars, bool buy, double step, bool unmargined) {
    Host host(buy, step, unmargined);
    host.run(bars.data() + start, finish - start + 1, "60", "60");
    const Host::Lots before{{415, 0.50}, {416, 0.25}};
    const Host::Lots after{{415, 0.50}, {416, 0.25}, {423, 0.50}};
    bool ok = host.last_error().empty() && host.before == before && host.after == after
        && host.trade_count() == 4 && host.report_trade_count() == 4;
    struct Slice { int fifo, bracket; double qty; };
    const std::array<Slice, 4> want{{{415, 416, 0.25}, {415, 415, 0.50},
                                      {416, 415, 0.25}, {423, 423, 0.50}}};
    for (int i = 0; i < 4; ++i) {
        const auto [entry, origin, qty] = want[i];
        const double level = bars.at(origin).open - 50000 * 0.01;
        int hit = origin;
        while (hit <= finish && bars.at(hit).low > level) ++hit;
        if (hit > finish || bars.at(hit).open < level)
            throw std::runtime_error("expected a lower-level touch without a gap");
        for (int b = origin; b <= hit; ++b)
            if (bars[b].high >= bars[origin].open + 50000 * 0.01)
                throw std::runtime_error("unexpected upper-level touch");
        if (i >= host.trade_count()) continue;
        const auto t = host.get_trade(i);
        ok &= t.entry_id == "E" && t.exit_id == "X" && t.is_long == buy && t.qty == qty
            && t.entry_bar_index == entry - start && t.exit_bar_index == hit - start
            && t.entry_time == bars[entry].timestamp && t.exit_time == bars[hit].timestamp
            && std::abs(t.entry_price - bars[entry].open) < 1e-7
            && std::abs(t.exit_price - level) < 1e-7;
    }
    std::printf("%s %s step=%g %s lots=%zu->%zu closed=%d/4 %s\n", ok ? "PASS" : "FAIL",
                buy ? "long" : "short", step, unmargined ? "zero-margin" : "ordinary",
                host.before.size(), host.after.size(), host.trade_count(), host.last_error().c_str());
    return ok;
}
} // namespace

int main() {
    const auto bars = order_print_tape::bars("qty_step_mincontract/btc60/feed");
    if (bars.size() != 1441) throw std::runtime_error("incomplete hourly feed");
    bool ok = true;
    for (bool buy : {true, false}) {
        ok &= check(bars, buy, 0.25, false);
        ok &= check(bars, buy, 0.25, true);
    }
    ok &= check(bars, true, 0.01, true);
    ok &= check(bars, true, 0.125, false);
    ok &= check(bars, true, 0.01, false);
    ok &= check(bars, true, 0, false);
    return ok ? 0 : 1;
}
