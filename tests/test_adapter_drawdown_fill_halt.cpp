#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/execution.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kCapital = 10000.0;
constexpr double kPercent = 0.5;
constexpr std::int64_t kHour = 3600000;
constexpr char kDrawdownText[] = "Close Position (Max Drawdown)";
int failures = 0;

struct Tape {
    std::vector<Bar> bars;
    Tape(int count, double price) {
        for (int i = 0; i < count; ++i)
            bars.push_back({price, price, price, price, 1.0, i * kHour});
    }
    Tape& at(int index, double price) {
        bars[static_cast<std::size_t>(index)] =
            {price, price, price, price, 1.0, index * kHour};
        return *this;
    }
    Tape& from(int index, double price) {
        for (int i = index; i < static_cast<int>(bars.size()); ++i) at(i, price);
        return *this;
    }
    double fill_price(int order_bar) const {
        return bars[static_cast<std::size_t>(order_bar + 1)].open;
    }
};

std::string row_text(const Trade& row, int cause) {
    char buf[512];
    std::snprintf(buf, sizeof buf,
        "%s %.17g @%lld:%.17g->%lld:%.17g pnl=%.17g entry='%s' id='%s' c='%s' cause=%d",
        row.is_long ? "L" : "S", row.qty,
        static_cast<long long>(row.entry_time / kHour), row.entry_price,
        static_cast<long long>(row.exit_time / kHour), row.exit_price, row.pnl,
        row.entry_id.c_str(), row.exit_id.c_str(), row.exit_comment.c_str(), cause);
    return buf;
}

std::string outcome_text(const BacktestEngine& host, double position) {
    std::string out;
    for (int i = 0; i < host.report_trade_count(); ++i) {
        out += row_text(host.get_report_trade(i), host.closed_trade_close_cause(i));
        out += " | ";
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "pos=%.17g", position);
    return out + buf;
}

struct ExpectedRow {
    Trade trade;
    execution::CloseCause cause;
};

// Quantities and order bars come from the scenario. Market fills use the
// following open; the rule's close uses the triggering fill's bar and price.
ExpectedRow expected_row(const Tape& tape, const char* entry, bool is_long,
                         double qty, int entry_order, int exit_order,
                         const char* exit_id, const char* comment,
                         execution::CloseCause cause = execution::CloseCause::Script) {
    Trade row{};
    row.entry_id = entry;
    row.is_long = is_long;
    row.qty = qty;
    row.entry_time = (entry_order + 1) * kHour;
    row.exit_time = (exit_order + 1) * kHour;
    row.entry_price = tape.fill_price(entry_order);
    row.exit_price = tape.fill_price(exit_order);
    row.pnl = qty * (is_long ? row.exit_price - row.entry_price
                            : row.entry_price - row.exit_price);
    row.exit_id = exit_id;
    row.exit_comment = comment;
    return {std::move(row), cause};
}

class Probe : public source::PineStrategyHost {
public:
    Probe() {
        initial_capital_ = kCapital;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_ = 3;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        slippage_ = 0;
        syminfo_mintick_ = 1.0;
    }
    double position() const { return physical_position().signed_units; }
    std::string outcome() const { return outcome_text(*this, position()); }
    std::vector<double> positions;
protected:
    int step() {
        positions.push_back(position());
        set_pine_risk_max_drawdown(kPercent, true);
        return pine_bar_index();
    }
    void buy(const char* id, double qty) {
        strategy_entry(id, true, kNaN, kNaN, qty);
    }
};

struct PairInputs {
    double a, b, opening, closing;
    int entry_order = 0;
    int close_order = 2;
    int later_order = 5;
};

class CloseA final : public Probe {
public:
    explicit CloseA(PairInputs input) : input_(input) {
        process_orders_on_close_ = false;
    }
    void on_source_bar(const Bar&) override {
        const int i = step();
        if (i == input_.entry_order) {
            buy("A", input_.a);
            buy("B", input_.b);
        }
        if (i == input_.close_order) {
            strategy_close("A");
            strategy_entry("P", true, input_.opening - 20.0, kNaN, 1.0);
        }
        if (i == input_.later_order) buy("C", 1.0);
    }
private:
    PairInputs input_;
};

class CloseB final : public Probe {
public:
    explicit CloseB(PairInputs input) : input_(input) {
        process_orders_on_close_ = false;
    }
    void on_source_bar(const Bar&) override {
        const int i = step();
        if (i == input_.entry_order) {
            buy("A", input_.a);
            buy("B", input_.b);
        }
        if (i == input_.close_order) strategy_close("B");
        if (i == input_.later_order) buy("C", 1.0);
    }
private:
    PairInputs input_;
};

struct ReversalInputs {
    double qty = 30.0, opening = 100.0, reversing = 97.0;
    int entry_order = 0, reverse_order = 4, later_order = 7;
};

class Reverse final : public Probe {
public:
    explicit Reverse(ReversalInputs input) : input_(input) {
        process_orders_on_close_ = false;
    }
    void on_source_bar(const Bar&) override {
        const int i = step();
        if (i == input_.entry_order) buy("A", input_.qty);
        if (i == input_.reverse_order)
            strategy_entry("R", false, kNaN, kNaN, input_.qty);
        if (i == input_.later_order) buy("C", 1.0);
    }
private:
    ReversalInputs input_;
};

struct AddInputs {
    double a = 25.0, add = 5.0, opening = 100.0, adding = 97.0;
    double recovery = 100.0, later_qty = 1.0;
    int entry_order = 0, add_order = 3, close_order = 5;
    int later_order = 25, later_close_order = 27;
};

class Add final : public Probe {
public:
    explicit Add(AddInputs input) : input_(input) {
        process_orders_on_close_ = false;
    }
    void on_source_bar(const Bar&) override {
        const int i = step();
        if (i == input_.entry_order) buy("A", input_.a);
        if (i == input_.add_order) buy("A2", input_.add);
        if (i == input_.close_order) strategy_close_all();
        if (i == input_.later_order) buy("D", input_.later_qty);
        if (i == input_.later_close_order) strategy_close("D");
    }
private:
    AddInputs input_;
};

struct PeakInputs {
    double a = 30.0, b = 5.0, later_qty = 10.0;
    double opening = 100.0, high_close = 110.0, closing = 101.0;
    int entry_order = 0, close_order = 3, later_order = 6, all_close_order = 8;
};

class Peak final : public Probe {
public:
    explicit Peak(PeakInputs input) : input_(input) {
        process_orders_on_close_ = false;
    }
    void on_source_bar(const Bar&) override {
        const int i = step();
        if (i == input_.entry_order) {
            buy("A", input_.a);
            buy("B", input_.b);
        }
        if (i == input_.close_order) strategy_close("A");
        if (i == input_.later_order) buy("C", input_.later_qty);
        if (i == input_.all_close_order) strategy_close_all();
    }
private:
    PeakInputs input_;
};

struct HoldInputs {
    double qty = 30.0, later_qty = 1.0;
    double opening = 100.0, underwater = 97.0, closing = 99.0;
    int entry_order = 0, close_order = 5, later_order = 8, later_close_order = 10;
};

class Hold final : public Probe {
public:
    explicit Hold(HoldInputs input) : input_(input) {
        process_orders_on_close_ = false;
    }
    void on_source_bar(const Bar&) override {
        const int i = step();
        if (i == input_.entry_order) buy("A", input_.qty);
        if (i == input_.close_order) strategy_close("A");
        if (i == input_.later_order) buy("D", input_.later_qty);
        if (i == input_.later_close_order) strategy_close("D");
    }
private:
    HoldInputs input_;
};

struct TwoStepInputs {
    double a, b;
    // Prices stay on the probe's price grid (mintick 1).
    double base = 100.0, up = 105.0, down = 101.0;
    int a_order = 0, b_order = 2, close_order = 4, all_close_order = 8;
    int later_order = 25, later_close_order = 27;
    double later_qty = 1.0;
};

class TwoStep final : public Probe {
public:
    explicit TwoStep(TwoStepInputs input) : input_(input) {
        process_orders_on_close_ = false;
    }
    void on_source_bar(const Bar&) override {
        const int i = step();
        if (i == input_.a_order) buy("A", input_.a);
        if (i == input_.b_order) buy("B", input_.b);
        if (i == input_.close_order) strategy_close("A");
        if (i == input_.all_close_order) strategy_close_all();
        if (i == input_.later_order) buy("D", input_.later_qty);
        if (i == input_.later_close_order) strategy_close("D");
    }
private:
    TwoStepInputs input_;
};

struct RaisedPeakInputs {
    double a = 20.0, e = 12.0, b = 4.0;
    // Prices stay on the probe's price grid (mintick 1).
    double base = 100.0, up = 105.0, down = 101.0;
    int a_order = 0, close_order = 2, e_close_order = 4;
    int later_order = 25, later_close_order = 27;
    double later_qty = 1.0;
};

class RaisedPeak final : public Probe {
public:
    explicit RaisedPeak(RaisedPeakInputs input) : input_(input) {
        process_orders_on_close_ = false;
    }
    void on_source_bar(const Bar&) override {
        const int i = step();
        if (i == input_.a_order) buy("A", input_.a);
        if (i == input_.close_order) {
            strategy_close("A");
            buy("E", input_.e);
            buy("B", input_.b);
        }
        if (i == input_.e_close_order) strategy_close("E");
        if (i == input_.later_order) buy("D", input_.later_qty);
        if (i == input_.later_close_order) strategy_close("D");
    }
private:
    RaisedPeakInputs input_;
};

struct MarkedPeakInputs {
    double a = 20.0, b = 16.0, e = 2.0;
    // Prices stay on the probe's price grid (mintick 1).
    double base = 100.0, up = 105.0, down = 101.0;
    int a_order = 0, close_order = 2, b_close_order = 4;
    int later_order = 25, later_close_order = 27;
    double later_qty = 1.0;
};

class MarkedPeak final : public Probe {
public:
    explicit MarkedPeak(MarkedPeakInputs input) : input_(input) {
        process_orders_on_close_ = false;
    }
    void on_source_bar(const Bar&) override {
        const int i = step();
        if (i == input_.a_order) {
            buy("A", input_.a);
            buy("B", input_.b);
        }
        if (i == input_.close_order) {
            strategy_close("A");
            buy("E", input_.e);
        }
        if (i == input_.b_close_order) strategy_close("B");
        if (i == input_.later_order) buy("D", input_.later_qty);
        if (i == input_.later_close_order) strategy_close("D");
    }
private:
    MarkedPeakInputs input_;
};

struct SameSessionInputs {
    double a = 23.0, add = 5.0, c = 10.0;
    double base = 100.0, low = 97.0, recovery = 100.0;
    int a_order = 0, add_order = 2, c_order = 4, close_order = 6;
    int later_order = 25, later_close_order = 27;
    double later_qty = 1.0;
};

class SameSession final : public Probe {
public:
    explicit SameSession(SameSessionInputs input) : input_(input) {
        process_orders_on_close_ = false;
    }
    void on_source_bar(const Bar&) override {
        const int i = step();
        if (i == input_.a_order) buy("A", input_.a);
        if (i == input_.add_order) buy("A2", input_.add);
        if (i == input_.c_order) buy("C", input_.c);
        if (i == input_.close_order) strategy_close_all();
        if (i == input_.later_order) buy("D", input_.later_qty);
        if (i == input_.later_close_order) strategy_close("D");
    }
private:
    SameSessionInputs input_;
};

bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

template<class Host, class Input>
void run_adapter(const char* name, const Tape& tape, const Input& input,
                 const std::vector<ExpectedRow>& expected,
                 const std::vector<std::pair<int, double>>& positions = {}) {
    Host host(input);
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()), "60", "60", false);
    bool ok = host.last_error().empty() && near(host.position(), 0.0)
        && host.report_trade_count() == static_cast<int>(expected.size());
    std::string wanted;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& e = expected[i];
        wanted += row_text(e.trade, static_cast<int>(e.cause)) + " | ";
        if (i >= static_cast<std::size_t>(host.report_trade_count())) continue;
        const auto a = host.get_report_trade(static_cast<int>(i));
        ok = ok && a.entry_id == e.trade.entry_id && a.is_long == e.trade.is_long
            && near(a.qty, e.trade.qty) && a.entry_time == e.trade.entry_time
            && a.exit_time == e.trade.exit_time && near(a.entry_price, e.trade.entry_price)
            && near(a.exit_price, e.trade.exit_price) && near(a.pnl, e.trade.pnl)
            && a.exit_id == e.trade.exit_id && a.exit_comment == e.trade.exit_comment
            && !a.open_at_end && near(a.commission, 0.0)
            && host.closed_trade_close_cause(static_cast<int>(i)) == static_cast<int>(e.cause);
    }
    for (const auto& checkpoint : positions) {
        const auto i = static_cast<std::size_t>(checkpoint.first);
        ok = ok && i < host.positions.size() && near(host.positions[i], checkpoint.second);
    }
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n  observed %s\n  expected %spos=0\n  error %s\n",
                    name, host.outcome().c_str(), wanted.c_str(), host.last_error().c_str());
        for (const auto& checkpoint : positions) {
            const auto i = static_cast<std::size_t>(checkpoint.first);
            if (i < host.positions.size())
                std::printf("  bar %d position %.17g expected %.17g\n",
                            checkpoint.first, host.positions[i], checkpoint.second);
        }
    }
}

void closing_loss_halts() {
    // TradingView tape sha256:ee278ee1d5069aae226c93ae9287f1b1129a9e6ddb1d8e528f378e9b98260009
    const PairInputs s{30.0, 10.0, 100.0, 98.0};
    Tape tape(s.later_order + 4, s.opening);
    tape.from(s.close_order, s.closing).at(s.later_order + 2, s.opening - 20.0);
    const auto script = expected_row(tape, "A", true, s.a, s.entry_order,
                                    s.close_order, "__close__A", "");
    const auto rule = expected_row(tape, "B", true, s.b, s.entry_order,
                                  s.close_order, "", kDrawdownText,
                                  execution::CloseCause::RiskLimit);
    run_adapter<CloseA>("R1 closing loss halts", tape, s, {script, rule});
}

void marked_book_halts() {
    // TradingView tape sha256:5eb4683a5e0f5688d6a46520f577380b2c40c59d92b460617da6fe22f0154bb9
    const PairInputs s{25.0, 5.0, 100.0, 98.0};
    Tape tape(s.later_order + 3, s.opening);
    tape.from(s.close_order, s.closing);
    const double first_closed = std::min(s.a, s.b);
    run_adapter<CloseB>("R2 marked book halts", tape, s, {
        expected_row(tape, "A", true, first_closed, s.entry_order,
                     s.close_order, "__close__B", ""),
        expected_row(tape, "A", true, s.a - first_closed, s.entry_order,
                     s.close_order, "", kDrawdownText, execution::CloseCause::RiskLimit),
        expected_row(tape, "B", true, s.b, s.entry_order,
                     s.close_order, "", kDrawdownText, execution::CloseCause::RiskLimit)});
}

void reversal_halts() {
    // TradingView tape sha256:8a5bd165a195e74ecf72ce6336a369b7892828acef6ee5dccf662be38440dc4a
    const ReversalInputs s;
    Tape tape(s.later_order + 3, s.opening);
    tape.from(s.entry_order + 2, s.reversing);
    run_adapter<Reverse>("R3 reversal halts", tape, s, {
        expected_row(tape, "A", true, s.qty, s.entry_order, s.reverse_order, "R", ""),
        expected_row(tape, "R", false, s.qty, s.reverse_order, s.reverse_order,
                     "", kDrawdownText, execution::CloseCause::RiskLimit)});
}

void same_side_does_not_halt() {
    // TradingView tape sha256:0acb1158cea715b0e6cb5c6a838d997c6baf358c79df2433ad21a12fece399ac
    const AddInputs s;
    Tape tape(s.later_close_order + 3, s.opening);
    tape.from(s.entry_order + 2, s.adding).from(s.close_order + 1, s.recovery);
    run_adapter<Add>("R4 same side continues next day", tape, s, {
        expected_row(tape, "A", true, s.a, s.entry_order, s.close_order, "__close__", ""),
        expected_row(tape, "A2", true, s.add, s.add_order, s.close_order, "__close__", ""),
        expected_row(tape, "D", true, s.later_qty, s.later_order,
                     s.later_close_order, "__close__D", "")},
        {{s.add_order + 1, s.a + s.add}});
}

void bar_close_peak_does_not_halt() {
    // TradingView tape sha256:566c1d58849f9f1660274618e85c5d7f41dcee96e849c6ae411c0b27cca0ab09
    const PeakInputs s;
    Tape tape(s.all_close_order + 3, s.opening);
    tape.from(s.entry_order + 2, s.high_close).from(s.close_order + 1, s.closing);
    run_adapter<Peak>("R5 fill equity peak", tape, s, {
        expected_row(tape, "A", true, s.a, s.entry_order, s.close_order, "__close__A", ""),
        expected_row(tape, "B", true, s.b, s.entry_order, s.all_close_order, "__close__", ""),
        expected_row(tape, "C", true, s.later_qty, s.later_order,
                     s.all_close_order, "__close__", "")},
        {{s.close_order + 1, s.b}, {s.later_order + 1, s.b + s.later_qty}});
}

void underwater_without_fills_does_not_halt() {
    // TradingView tape sha256:5beeac719db4c7b6d84c31bf94f8bd9ac80cecaac8483e9900ec0b9532f36fd5
    const HoldInputs s;
    Tape tape(s.later_close_order + 3, s.opening);
    tape.from(s.entry_order + 2, s.underwater).from(s.close_order, s.closing);
    run_adapter<Hold>("R6 underwater hold recovers", tape, s, {
        expected_row(tape, "A", true, s.qty, s.entry_order, s.close_order, "__close__A", ""),
        expected_row(tape, "D", true, s.later_qty, s.later_order,
                     s.later_close_order, "__close__D", "")});
}

void profit_close_halts() {
    // TradingView tape sha256:a61368e0a2277f8939d4e338dca17545be75cbc6c86dac4b9c17bbcbf18a2f04
    const TwoStepInputs s{10.0, 20.0};
    Tape tape(s.later_close_order + 3, s.base);
    tape.from(s.b_order + 1, s.up).from(s.close_order + 1, s.down);
    // A realises s.a * (s.down - s.base) > 0; B is marked
    // s.b * (s.down - s.up). Equity is kCapital plus both amounts, so
    // kCapital - equity >= kPercent / 100 * kCapital at A's close fill.
    run_adapter<TwoStep>("R7 profit close halts", tape, s, {
        expected_row(tape, "A", true, s.a, s.a_order,
                     s.close_order, "__close__A", ""),
        expected_row(tape, "B", true, s.b, s.b_order,
                     s.close_order, "", kDrawdownText, execution::CloseCause::RiskLimit)});
}

void peak_ignores_opening_fill() {
    // TradingView tape sha256:47ed90bb5e9edb0199e36bd3ef64165c93769ba36904dc7834b026991c29c701
    const TwoStepInputs s{12.0, 4.0};
    Tape tape(s.later_close_order + 3, s.base);
    tape.from(s.b_order + 1, s.up).from(s.close_order + 1, s.down);
    // At A's close fill, equity = kCapital + s.a * (s.down - s.base)
    // + s.b * (s.down - s.up). The drawdown kCapital - equity is far below
    // kPercent / 100 * kCapital. B's opening-fill equity is
    // kCapital + s.a * (s.up - s.base); its drawdown to the close-fill
    // equity exceeds kPercent / 100 times that opening-fill equity.
    // Raising the peak at B's opening fill would therefore halt this row.
    run_adapter<TwoStep>("R8 peak ignores opening fill", tape, s, {
        expected_row(tape, "A", true, s.a, s.a_order,
                     s.close_order, "__close__A", ""),
        expected_row(tape, "B", true, s.b, s.b_order,
                     s.all_close_order, "__close__", ""),
        expected_row(tape, "D", true, s.later_qty, s.later_order,
                     s.later_close_order, "__close__D", "")});
}

void later_same_side_entry_fills() {
    // TradingView tape sha256:43dbd0bbe9a12b4c7252879895e295c6d20902b1f27129da13fbbac1c62180ae
    const SameSessionInputs s;
    Tape tape(s.later_close_order + 3, s.base);
    tape.from(s.add_order, s.low).from(s.close_order + 1, s.recovery);
    // At A2's fill A is marked s.a * (s.low - s.base), whose loss exceeds
    // kPercent / 100 * kCapital. No closing fill has tested that loss;
    // the same-side fill does not test it. A, A2 and C together cost
    // s.a * s.base + s.add * s.low + s.c * s.low, far below kCapital.
    run_adapter<SameSession>("R9 later same side entry fills", tape, s, {
        expected_row(tape, "A", true, s.a, s.a_order, s.close_order, "__close__", ""),
        expected_row(tape, "A2", true, s.add, s.add_order, s.close_order, "__close__", ""),
        expected_row(tape, "C", true, s.c, s.c_order, s.close_order, "__close__", ""),
        expected_row(tape, "D", true, s.later_qty, s.later_order,
                     s.later_close_order, "__close__D", "")},
        {{s.c_order + 1, s.a + s.add + s.c}});
}

void closing_fill_raises_peak() {
    // TradingView tape sha256:6c8004381906ce42c022be9b7214d6a583f0fd23e64bb73220be1888a5646ed9
    const RaisedPeakInputs s;
    Tape tape(s.later_close_order + 3, s.base);
    tape.from(s.close_order + 1, s.up).from(s.e_close_order + 1, s.down);
    // A realises s.a * (s.up - s.base) > 0, raising the peak to its
    // closing-fill equity, kCapital + s.a * (s.up - s.base).
    // E and B open at s.up and change nothing. At E's closing fill,
    // E realises s.e * (s.down - s.up) and B is marked s.b * (s.down - s.up).
    // The drawdown from the raised peak, (s.e + s.b) * (s.up - s.down),
    // is >= kPercent / 100 * (kCapital + s.a * (s.up - s.base)).
    // Equity there is kCapital + s.a * (s.up - s.base)
    // - (s.e + s.b) * (s.up - s.down) > kCapital, so a reference that
    // stays at kCapital would not halt. This row fails if a closing
    // fill does not raise the peak.
    run_adapter<RaisedPeak>("R10 closing fill raises peak", tape, s, {
        expected_row(tape, "A", true, s.a, s.a_order,
                     s.close_order, "__close__A", ""),
        expected_row(tape, "E", true, s.e, s.close_order,
                     s.e_close_order, "__close__E", ""),
        expected_row(tape, "B", true, s.b, s.close_order,
                     s.e_close_order, "", kDrawdownText, execution::CloseCause::RiskLimit)});
}

void closing_fill_peak_counts_open_profit() {
    // TradingView tape sha256:8996c131d435ddb85b3dce0931a4fc720f6d81e341fbf07c34acfde6505f511e
    const MarkedPeakInputs s;
    Tape tape(s.later_close_order + 3, s.base);
    tape.from(s.close_order + 1, s.up).from(s.b_close_order + 1, s.down);
    // At A's closing fill, marked equity is kCapital + s.a * (s.up - s.base)
    // + s.b * (s.up - s.base): A realised, B still open and marked at s.up.
    // That is the raised peak; E opens at s.up and changes nothing.
    // At B's closing fill, B realises s.b * (s.down - s.base) and E is marked
    // s.e * (s.down - s.up). Equity is kCapital + s.a * (s.up - s.base)
    // + s.b * (s.down - s.base) + s.e * (s.down - s.up).
    // Drawdown is (s.b + s.e) * (s.up - s.down),
    // >= kPercent / 100 * (kCapital + (s.a + s.b) * (s.up - s.base)).
    // A realised-only reference, kCapital + s.a * (s.up - s.base), is below
    // that equity, so it would not halt; neither would a reference at kCapital.
    // This row fails if the peak leaves out the open profit of units still held.
    run_adapter<MarkedPeak>("R11 closing fill peak counts open profit", tape, s, {
        expected_row(tape, "A", true, s.a, s.a_order,
                     s.close_order, "__close__A", ""),
        expected_row(tape, "B", true, s.b, s.a_order,
                     s.b_close_order, "__close__B", ""),
        expected_row(tape, "E", true, s.e, s.close_order,
                     s.b_close_order, "", kDrawdownText, execution::CloseCause::RiskLimit)});
}

} // namespace

int main() {
    closing_loss_halts();
    marked_book_halts();
    reversal_halts();
    same_side_does_not_halt();
    bar_close_peak_does_not_halt();
    underwater_without_fills_does_not_halt();
    profit_close_halts();
    peak_ignores_opening_fill();
    later_same_side_entry_fills();
    closing_fill_raises_peak();
    closing_fill_peak_counts_open_profit();
    std::printf("drawdown fill scenarios: failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
