#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <iostream>
#include <limits>
#include <vector>

namespace {
using namespace pineforge;
constexpr double omitted = std::numeric_limits<double>::quiet_NaN();

struct Case {
    const char* name;
    int cap;
    bool buy_first;
    bool final_sell;
};

class Book final : public source::PineStrategyHost {
public:
    explicit Book(Case c) : c_(c) {
        source::PineStrategyConfig config;
        config.pyramiding = c.cap;
        config.default_qty_value = 2;
        config.margin_long = config.margin_short = 0;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
    }

    void on_source_bar(const Bar& bar) override {
        ++calls;
        switch (pine_bar_index()) {
        case 0: strategy_entry("a", true); break;
        case 1: strategy_entry("b", true, omitted, omitted, 3); break;
        case 2:
            before = native_open_lots(bar.close);
            if (c_.buy_first) strategy_entry("buy", true);
            strategy_entry("sell", false);
            if (!c_.buy_first) strategy_entry("buy", true);
            if (c_.final_sell) strategy_entry("last", false);
            break;
        case 3:
            after = physical_position().signed_units;
            strategy_close_all();
            break;
        }
    }

    int calls = 0;
    std::vector<NativeOpenLot> before;
    double after = omitted;

private:
    Case c_;
};

struct Slice {
    const char* entry;
    int entry_bar;
    const char* exit;
    int exit_bar;
    double units;
    bool is_long;
};

int run(Case c) {
    std::array<Bar, 6> bars{};
    for (int i = 0; i < static_cast<int>(bars.size()); ++i) {
        const double p = 64.0 + i;
        bars[i] = {p, p + 1, p - 1, p, 200, 86400000LL + i * 60000LL};
    }
    Book book(c);
    book.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false);
    if (!book.last_error().empty() || book.calls != static_cast<int>(bars.size()) ||
        book.before.size() != 2 || book.before[0].entry_label != "a" ||
        book.before[0].signed_units != 2 || book.before[1].entry_label != "b" ||
        book.before[1].signed_units != 3) {
        std::cerr << "NOT COMPARED " << c.name << ": expected two seed lots (+2, +3); "
                  << book.calls << " bar calls, " << book.before.size() << " lots, error '"
                  << book.last_error() << "'\n";
        return 2;
    }

    // In call order the sell transacts 5+2, the reversing buy 2+2,
    // and the optional final sell 2+2. At the open the buy fills first.
    // If the buy was called first instead, it was an ordinary add at the cap.
    const double expected = c.buy_first || c.final_sell ? -2.0 : 2.0;
    if (book.after != expected) {
        std::cerr << "FAIL " << c.name << ": position after opening fills expected " << expected
                  << ", got " << book.after << '\n';
        return 1;
    }

    std::vector<Slice> want{{"a", 1, "sell", 3, 2, true},
                            {"b", 2, "sell", 3, 3, true}};
    if (c.buy_first) {
        want.push_back({"sell", 3, "__close__", 4, 2, false});
    } else {
        want.push_back({"buy", 3, "sell", 3, 2, true});
        if (c.final_sell) {
            want.push_back({"buy", 3, "last", 3, 2, true});
            want.push_back({"last", 3, "__close__", 4, 2, false});
        } else {
            want.push_back({"buy", 3, "__close__", 4, 2, true});
        }
    }
    if (book.closed_trade_count() != want.size() || book.physical_position().signed_units != 0) {
        std::cerr << "FAIL " << c.name << ": expected " << want.size()
                  << " closed rows and a flat book\n";
        return 1;
    }
    for (std::size_t i = 0; i < want.size(); ++i) {
        const auto t = book.get_trade(static_cast<int>(i));
        const auto& w = want[i];
        if (t.entry_id != w.entry || t.exit_id != w.exit || t.qty != w.units ||
            t.is_long != w.is_long || t.entry_time != bars[w.entry_bar].timestamp ||
            t.exit_time != bars[w.exit_bar].timestamp) {
            std::cerr << "FAIL " << c.name << ": FIFO row " << i + 1
                      << ": expected " << w.entry << " -> "
                      << w.exit << " for " << w.units << ", got " << t.entry_id << " -> "
                      << t.exit_id << " for " << t.qty << '\n';
            return 1;
        }
    }
    std::cout << "PASS " << c.name << ": position " << expected << ", "
              << want.size() << " FIFO rows\n";
    return 0;
}
} // namespace

int main() {
    for (const Case c : {Case{"spare slot", 3, false, false},
                         Case{"add before sell", 2, true, false},
                         Case{"sell then buy at cap", 2, false, false},
                         Case{"sell, buy, sell at cap", 2, false, true}}) {
        if (const int result = run(c)) return result;
    }
    return 0;
}
