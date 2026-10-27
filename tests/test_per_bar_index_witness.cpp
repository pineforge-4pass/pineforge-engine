#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdio>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using pineforge::Bar;
using pineforge::QtyType;
using pineforge::source::PineStrategyConfig;
using pineforge::source::PineStrategyHost;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

enum class Witness {
    FarStop,
    ExplicitFromEntry,
    SameIdReplacement,
    FillAndCancel,
    Reversal,
    OneBracket,
    Pooc,
    SamePoint,
};

std::vector<Bar> bars(Witness witness) {
    std::vector<Bar> result(6);
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i].timestamp = 1736121600000LL + static_cast<std::int64_t>(i) * 60000;
        result[i].open = 100.0;
        result[i].high = 101.0;
        result[i].low = 99.0;
        result[i].close = 100.0;
        result[i].volume = 1000.0;
    }
    switch (witness) {
    case Witness::FarStop:
        result[1].high = 111.0;
        result[1].low = 99.0;
        break;
    case Witness::ExplicitFromEntry:
        result[2].low = 97.0;
        break;
    case Witness::SameIdReplacement:
        result[2].open = 98.0;
        result[2].high = 100.0;
        result[2].low = 97.0;
        break;
    case Witness::OneBracket:
        result[2].high = 106.0;
        break;
    case Witness::FillAndCancel:
    case Witness::Reversal:
    case Witness::Pooc:
    case Witness::SamePoint:
        break;
    }
    return result;
}

class WitnessHost final : public PineStrategyHost {
public:
    explicit WitnessHost(Witness witness) : witness_(witness) {
        PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 4;
        config.process_orders_on_close = witness == Witness::Pooc;
        configure_pine_strategy(config);
        set_syminfo_mintick(1.0);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
    }

    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        switch (witness_) {
        case Witness::FarStop:
            // Hand oracle: bar 1 reaches only HIT's 110 stop; FAR-LONG at
            // 120 and FAR-SHORT at 80 remain standing. close_all on bar 2
            // closes the one HIT lot, so exactly one trade is observable.
            if (index == 0) {
                strategy_entry("FAR-LONG", true, kNaN, 120.0);
                strategy_entry("FAR-SHORT", false, kNaN, 80.0);
                strategy_entry("HIT", true, kNaN, 110.0);
            } else if (index == 2) {
                strategy_close_all();
            }
            break;
        case Witness::ExplicitFromEntry:
            // Hand oracle: L1 and L2 each fill one lot at the bar 1 open.
            // The same visible exit id X arms separate stops, 98 for L1 and
            // 99 for L2; bar 2's low 97 closes both, producing two X rows.
            if (index == 0) {
                strategy_entry("L1", true, kNaN, kNaN, 1.0);
                strategy_entry("L2", true, kNaN, kNaN, 1.0);
            } else if (index == 1) {
                strategy_exit("X", "L1", kNaN, 98.0);
                strategy_exit("X", "L2", kNaN, 99.0);
            }
            break;
        case Witness::SameIdReplacement:
            // Hand oracle: R's first limit at 90 is replaced by 99 while it
            // is standing; bar 2 opens at 98 and fills the replacement. The
            // unrelated limits never reach, and close_all leaves one R row.
            if (index == 0) {
                strategy_entry("R", true, 90.0, kNaN);
                strategy_entry("FAR-A", true, 1.0, kNaN);
                strategy_entry("FAR-B", false, 200.0, kNaN);
            } else if (index == 1) {
                strategy_entry("R", true, 99.0, kNaN);
            } else if (index == 3) {
                strategy_close_all();
            }
            break;
        case Witness::FillAndCancel:
            // Hand oracle: FILL is the only market order and creates one
            // lot; CANCEL is withdrawn on bar 1 before its far limit can
            // reach, so the final book has no CANCEL working label and one
            // close_all trade.
            if (index == 0) {
                strategy_entry("FILL", true);
                strategy_entry("CANCEL", true, 1.0, kNaN);
                strategy_entry("FAR", false, 200.0, kNaN);
            } else if (index == 1) {
                strategy_cancel("CANCEL");
            } else if (index == 2) {
                strategy_close_all();
            }
            break;
        case Witness::Reversal:
            // Hand oracle: L fills at bar 1's open, S is placed with many
            // unrelated standing rows, and bar 2 reverses L. One closed
            // long row therefore has entry L and exit S.
            if (index == 0) {
                strategy_entry("L", true);
                strategy_entry("FAR-L", true, 1.0, kNaN);
                strategy_entry("FAR-S", false, 200.0, kNaN);
            } else if (index == 1) {
                strategy_entry("S", false);
            }
            break;
        case Witness::OneBracket:
            // Hand oracle: L1 and L2 fill at bar 1; only L1 owns X, so the
            // bar 2 high 106 closes L1 at its 105 limit and leaves L2 open.
            if (index == 0) {
                strategy_entry("L1", true, kNaN, kNaN, 1.0);
                strategy_entry("L2", true, kNaN, kNaN, 1.0);
            } else if (index == 1) {
                strategy_exit("X", "L1", 105.0, 95.0);
            }
            break;
        case Witness::Pooc:
            // Hand oracle: with process_orders_on_close, POOC fills POOC at
            // bar 0's close (100), close_all at bar 1's close (100), and
            // records one zero-P&L trade.
            if (index == 0) {
                strategy_entry("POOC", true);
            } else if (index == 1) {
                strategy_close_all();
            }
            break;
        case Witness::SamePoint:
            // Hand oracle: A and B are the only reached same-point
            // opposite entries; the distant FAR rows are not candidates.
            // The first A lot is closed by B, yielding one closed A/B row.
            if (index == 0) {
                strategy_entry("FAR-L", true, 1.0, kNaN);
                strategy_entry("FAR-S", false, 200.0, kNaN);
                strategy_entry("A", true, kNaN, kNaN, 1.0);
                strategy_entry("B", false, kNaN, kNaN, 1.0);
            }
            break;
        }
    }

    bool has_working_label(const std::string& label) const {
        for (const auto& row : native_working_requests()) {
            if (row.definition && row.definition->request.label == label) return true;
        }
        return false;
    }

private:
    Witness witness_;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void run_witness(Witness witness) {
    WitnessHost host(witness);
    auto input = bars(witness);
    host.run(input.data(), static_cast<int>(input.size()), "1", "1");
    require(host.last_error().empty(), host.last_error().c_str());
    switch (witness) {
    case Witness::FarStop:
        require(host.trade_count() == 1, "far-stop witness must close exactly HIT");
        require(host.get_trade(0).entry_id == "HIT", "far-stop witness chose a distant stop");
        require(host.get_trade(0).entry_price == 110.0, "far-stop witness used the wrong stop price");
        break;
    case Witness::ExplicitFromEntry:
        require(host.trade_count() == 2, "explicit from_entry witness must close both lots");
        require(host.get_trade(0).entry_id == "L1" && host.get_trade(1).entry_id == "L2",
                "same exit id lost explicit from_entry ownership");
        require(host.get_trade(0).exit_id == "X" && host.get_trade(1).exit_id == "X",
                "explicit from_entry exits used the wrong id");
        break;
    case Witness::SameIdReplacement:
        require(host.trade_count() == 1, "same-id replacement witness must close one lot");
        require(host.get_trade(0).entry_id == "R" && host.get_trade(0).entry_price == 98.0,
                "same-id replacement did not use the replacement row");
        break;
    case Witness::FillAndCancel:
        require(host.trade_count() == 1, "fill/cancel witness must close the market lot");
        require(host.get_trade(0).entry_id == "FILL", "fill/cancel witness chose another entry");
        require(!host.has_working_label("CANCEL"), "cancelled standing order is still working");
        break;
    case Witness::Reversal:
        require(host.trade_count() == 1, "reversal witness must close the first lot");
        require(host.get_trade(0).entry_id == "L" && host.get_trade(0).exit_id == "S",
                "reversal witness lost the opposite entry");
        break;
    case Witness::OneBracket:
        require(host.trade_count() == 1, "bracket witness must close only L1");
        require(host.get_trade(0).entry_id == "L1" && host.get_trade(0).exit_id == "X",
                "bracket witness acted on the wrong entry");
        break;
    case Witness::Pooc:
        require(host.trade_count() == 1, "POOC witness must close one trade");
        require(host.get_trade(0).entry_id == "POOC"
                    && host.get_trade(0).entry_price == 100.0
                    && host.get_trade(0).exit_price == 100.0,
                "POOC witness used the wrong close prices");
        break;
    case Witness::SamePoint:
        require(host.trade_count() == 1, "same-point witness must close A once");
        require(host.get_trade(0).entry_id == "A" && host.get_trade(0).exit_id == "B",
                "same-point witness chose a distant or foreign entry");
        break;
    }
}

} // namespace

int main() {
    try {
        run_witness(Witness::FarStop);
        run_witness(Witness::ExplicitFromEntry);
        run_witness(Witness::SameIdReplacement);
        run_witness(Witness::FillAndCancel);
        run_witness(Witness::Reversal);
        run_witness(Witness::OneBracket);
        run_witness(Witness::Pooc);
        run_witness(Witness::SamePoint);
        std::puts("test_per_bar_index_witness: passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_per_bar_index_witness: %s\n", error.what());
        return 1;
    }
}
