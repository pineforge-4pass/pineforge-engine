// A filled bracket spends the opening it was armed for. FIFO can close a
// different trade, leaving two physical lots for one occupied pyramiding slot.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge;

constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr double tick = 0.01;
constexpr double ticks = 50000.0;
constexpr double distance = tick * ticks;
constexpr std::int64_t start = 1704067200000;
constexpr std::int64_t hour = 3600000;

void require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

bool near(double got, double want) {
    return std::abs(got - want) < 1e-9;
}

class Host : public source::PineStrategyHost {
public:
    Host(int cap, bool any, double step, double capital) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.initial_capital = capital;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 10.0;
        config.pyramiding = cap;
        config.close_entries_rule_any = any;
        configure_pine_strategy(config);
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(tick);
        if (step > 0.0) set_syminfo_metadata("qty_step", step);
    }

    void replay(const std::vector<Bar>& bars) {
        run(bars.data(), static_cast<int>(bars.size()), "60", "60");
        require(last_error().empty(), last_error());
    }

protected:
    void tick_exit(const std::string& entry) {
        strategy_exit("Long exit", entry, na, na, na, na, na, 100.0, "", na, "", ticks, ticks);
    }
};

// The three prices are the opens at the first, second and replacement fills.
// The newer-first book is TradingView's Jan 18 sequence; the older-first book
// is Jan 24. The small tests use its printed sizes explicitly.
struct Book {
    double first;
    double second;
    double third;
    double first_qty;
    double second_qty;
    double third_qty;
};

constexpr Book older_first{40179.53, 39769.98, 39654.39, 0.24626, 0.24854, 0.24914};
constexpr Book newer_first{42807.04, 42915.32, 42738.70, 0.23252, 0.23199, 0.23258};
// Half the first entry is exactly one unit.
constexpr Book partial{100.0, 104.0, 103.0, 2.0, 1.0, 1.0};
constexpr double slice = 0.00000123456789;

enum class Exit { Ticks, Neither, NamedStop, PartialClose, Percent };

struct Lot {
    int entry_bar;
    double price;
    double units;
};

class SlotHost final : public Host {
public:
    SlotHost(Book book, Exit exit, bool is_long, bool any, double step, int slices)
        : Host(2, any, step, 100000.0), book_(book), exit_(exit), long_(is_long), slices_(slices) {}

    double price(double long_price) const {
        return long_ ? long_price : 2.0 * book_.first - long_price;
    }

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        const bool shared = exit_ == Exit::Ticks || exit_ == Exit::Neither;
        if (bar == 0) {
            strategy_entry(shared ? "Long" : "A", long_, na, na, book_.first_qty);
        }
        if (bar == 1) {
            strategy_entry(shared ? "Long" : "B", long_, na, na, book_.second_qty);
        }
        if (bar == 2) {
            if (exit_ == Exit::NamedStop) {
                strategy_exit("Stop", "B", na, price(book_.second - distance));
            } else if (exit_ == Exit::PartialClose) {
                strategy_close("A", "", 1.0);
            } else if (exit_ == Exit::Percent) {
                strategy_exit("Half", "A", na, price(book_.first - 1.0), na, na, na, 50.0);
            }
        }
        if (bar >= 3 && bar < 3 + slices_) strategy_close("Long", "", slice);
        if (bar == 3 + slices_) {
            before = lots();
            strategy_entry(shared ? "Long" : "C", long_, na, na, book_.third_qty);
        }
        if (shared) tick_exit("Long");
        if (bar == 4 + slices_) after = lots();
    }

    std::vector<Lot> before;
    std::vector<Lot> after;

private:
    std::vector<Lot> lots() const {
        std::vector<Lot> result;
        for (const auto& lot : pyramid_entries_) {
            result.push_back({lot.entry_bar_index, lot.price, lot.qty});
        }
        return result;
    }

    Book book_;
    Exit exit_;
    bool long_;
    int slices_;
};

void check_lots(const std::vector<Lot>& got, const std::vector<Lot>& want,
                const std::string& when) {
    require(got.size() == want.size(), when + ": expected " + std::to_string(want.size())
            + " lots, saw " + std::to_string(got.size()));
    for (std::size_t i = 0; i < got.size(); ++i) {
        require(got[i].entry_bar == want[i].entry_bar && near(got[i].price, want[i].price)
                && near(got[i].units, want[i].units), when + ": wrong lot " + std::to_string(i));
    }
}

void check_slot(const Book& book, Exit exit, bool is_long, bool any, double step, int slices = 0) {
    SlotHost host(book, exit, is_long, any, step, slices);
    // A lower low reaches exactly one stop. For shorts, reflect all four prices.
    const auto bar = [&](int i, double open, double low) {
        const double a = host.price(low);
        const double b = host.price(open + tick);
        return Bar{host.price(open), std::max(a, b), std::min(a, b), host.price(open),
                   1000.0, start + i * hour};
    };
    const bool half = exit == Exit::PartialClose || exit == Exit::Percent;
    const bool older = book.first > book.second;
    const double stop = half ? book.first - 1.0 : std::max(book.first, book.second) - distance;
    const double low = exit == Exit::Neither ? book.second - tick : stop - tick;
    std::vector<Bar> bars{bar(0, book.first, book.first - tick),
                          bar(1, book.first, book.first - tick),
                          bar(2, book.second, book.second - tick), bar(3, book.second, low)};
    for (int i = 4; i <= 4 + slices; ++i) {
        bars.push_back(bar(i, book.third, book.third - tick));
    }
    host.replay(bars);

    const double closed = exit == Exit::Neither ? 0.0
        : half ? 1.0 : older ? book.first_qty : book.second_qty;
    const double first_left = book.first_qty - (any ? 0.0 : closed) - slices * slice;
    const double second_left = book.second_qty - (any ? closed : 0.0);
    std::vector<Lot> want;
    if (first_left > 0.0) want.push_back({1, host.price(book.first), first_left});
    if (second_left > 0.0) want.push_back({2, host.price(book.second), second_left});
    check_lots(host.before, want, "before the third call");
    require(host.trade_count() == (closed > 0.0 ? 1 : 0) + slices, "wrong number of closing fills");
    if (closed > 0.0) {
        const auto trade = host.get_trade(0);
        const double fill = exit == Exit::PartialClose ? book.second : stop;
        const std::string exit_id = exit == Exit::PartialClose ? "__close__A"
            : exit == Exit::Percent ? "Half" : exit == Exit::NamedStop ? "Stop" : "Long exit";
        require(trade.is_long == is_long && trade.entry_bar_index == (any ? 2 : 1)
                && trade.exit_bar_index == 3 && trade.exit_id == exit_id
                && near(trade.exit_price, host.price(fill))
                && near(trade.qty, closed), "wrong closing fill before the third entry");
    }
    for (int i = 0; i < slices; ++i) {
        const auto trade = host.get_trade(i + 1);
        require(trade.entry_bar_index == 1 && trade.exit_bar_index == 4 + i
                && near(trade.exit_price, host.price(book.third)) && near(trade.qty, slice),
                "missing partial fill " + std::to_string(i));
    }
    if (!half && exit != Exit::Neither) {
        want.push_back({4 + slices, host.price(book.third), book.third_qty});
    }
    check_lots(host.after, want, "after the third call");
}

std::vector<Bar> btc_bars() {
    const auto path = std::filesystem::path(__FILE__).parent_path()
        / "fixtures/qty_step_mincontract/btc60/feed/bars.csv";
    std::ifstream input(path);
    std::string line;
    require(std::getline(input, line) && line == "timestamp,open,high,low,close,volume",
            "missing or invalid BTCUSDT hourly feed");
    std::vector<Bar> bars;
    while (std::getline(input, line)) {
        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream row(line);
        Bar bar{};
        require(static_cast<bool>(row >> bar.timestamp >> bar.open >> bar.high >> bar.low
                                      >> bar.close >> bar.volume), "invalid hourly bar");
        const auto expected_time = start + hour * static_cast<std::int64_t>(bars.size());
        require((row >> std::ws).eof() && bar.timestamp == expected_time
                && bar.low > 0.0 && bar.low <= std::min(bar.open, bar.close)
                && std::max(bar.open, bar.close) <= bar.high, "invalid hourly feed");
        bars.push_back(bar);
    }
    require(input.eof() && bars.size() == 1441, "expected the full Jan 1 to Mar 1 window");
    return bars;
}

class ReportHost final : public Host {
public:
    explicit ReportHost(int cap) : Host(cap, false, 0.0, 10000.0) {
        // TradingView starts these chart EMAs at the beginning of the supplied window.
        set_syminfo_metadata("chart_ema_na_warmup", 1.0);
    }

    void on_source_bar(const Bar& bar) override {
        const double fast = fast_.compute(bar.close);
        const double slow = slow_.compute(bar.close);
        if (trend_.compute(fast, slow)
            || (signed_position_size() > 0.0 && add_.compute(bar.close, fast))) {
            strategy_entry("Long", true);
        }
        tick_exit("Long");
    }

private:
    ta::EMA fast_{9};
    ta::EMA slow_{21};
    ta::Crossover trend_;
    ta::Crossover add_;
};

void check_report(int cap) {
    ReportHost host(cap);
    host.replay(btc_bars());
    const int count = host.trade_count();
    const int expected = cap == 1 ? 30 : 63;
    require(count == expected, "expected " + std::to_string(expected) + " closed rows, saw "
            + std::to_string(count));
    // The two stops on bar 126 and all closed rows in the three affected groups.
    struct Fill {
        int entry_bar;
        double entry;
        int exit_bar;
        double exit;
    };
    const std::vector<Fill> reported{
        {118, 43946.26, 126, 43446.26}, {124, 43931.10, 126, 43431.10},
        {415, 42807.04, 418, 42415.32}, {415, 42807.04, 424, 42307.04},
        {416, 42915.32, 424, 42307.04}, {423, 42738.70, 424, 42238.70},
        {553, 40179.53, 555, 39679.53}, {554, 39769.98, 559, 40154.39},
        {557, 39654.39, 559, 40154.39}, {557, 39654.39, 562, 40269.98},
        {808, 43119.06, 835, 42619.06}, {826, 42942.73, 838, 42442.73},
        {837, 42880.79, 838, 42380.79},
    };
    int other_level = 0;
    std::vector<Trade> trades;
    for (int i = 0; i < count; ++i) {
        const auto trade = host.get_trade(i);
        require(trade.is_long && trade.entry_id == "Long" && trade.exit_id == "Long exit"
                && trade.qty > 0.0 && !trade.open_at_end, "unexpected trade side, id or size");
        if (!near(std::abs(trade.exit_price - trade.entry_price), distance)) ++other_level;
        trades.push_back(trade);
    }
    // A bracket can outlive its physical lot. Its owner must at least have
    // entered by the exit bar and supplied that price, even for a FIFO split.
    for (const auto& trade : trades) {
        const auto owns_level = [&](const Trade& owner) {
            return owner.entry_bar_index <= trade.exit_bar_index
                && near(std::abs(trade.exit_price - owner.entry_price), distance);
        };
        require(std::any_of(trades.begin(), trades.end(), owns_level),
                "exit off every entry's bracket at bar " + std::to_string(trade.exit_bar_index));
    }
    require(other_level == (cap == 1 ? 0 : 20), "wrong number of FIFO rows at another level");
    if (cap == 1) return;
    for (const auto& fill : reported) {
        const auto matches = [&](const Trade& trade) {
            return trade.entry_bar_index == fill.entry_bar && near(trade.entry_price, fill.entry)
                && trade.exit_bar_index == fill.exit_bar && near(trade.exit_price, fill.exit);
        };
        require(std::count_if(trades.begin(), trades.end(), matches) == 1,
                "missing or duplicate reported fill from bar " + std::to_string(fill.entry_bar));
    }
}

int failures = 0;

template<class Check>
void row(const std::string& name, Check check) {
    try {
        check();
        std::cout << "PASS " << name << '\n';
    } catch (const std::exception& e) {
        ++failures;
        std::cerr << "FAIL " << name << ": " << e.what() << '\n';
    }
}
}

int main() {
    for (double step : {0.0, 0.00001}) {
        const std::string grid = step == 0.0 ? ", no quantity step" : ", declared quantity step";
        for (bool is_long : {true, false}) {
            const std::string side = is_long ? "long" : "short";
            row(side + ", older stop first" + grid, [&] {
                check_slot(older_first, Exit::Ticks, is_long, false, step);
            });
            row(side + ", newer stop first" + grid, [&] {
                check_slot(newer_first, Exit::Ticks, is_long, false, step);
            });
        }
        row("neither stop reached" + grid, [&] {
            check_slot(newer_first, Exit::Neither, true, false, step);
        });
        row("named stop with ANY" + grid, [&] {
            check_slot(newer_first, Exit::NamedStop, true, true, step);
        });
        row("partial strategy.close" + grid, [&] {
            check_slot(partial, Exit::PartialClose, true, false, step);
        });
        row("50% named exit" + grid, [&] {
            check_slot(partial, Exit::Percent, true, false, step);
        });
    }
    row("FIFO named stop with a declared step", [] {
        check_slot(newer_first, Exit::NamedStop, true, false, 0.00001);
    });
    // These closes all debit one record and cut the older of two surviving FIFO lots.
    row("newer stop then 256 unrounded partial closes", [] {
        check_slot(newer_first, Exit::Ticks, true, false, 0.0, 256);
    });
    row("BTCUSDT hourly, pyramiding 2", [] { check_report(2); });
    row("BTCUSDT hourly, pyramiding 1", [] { check_report(1); });
    return failures == 0 ? 0 : 1;
}
