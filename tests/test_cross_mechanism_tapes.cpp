#include "order_print_tape_fixture.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <tuple>

namespace {

using pineforge::Bar;
using pineforge::CommissionType;
using pineforge::QtyType;
using pineforge::source::PineStrategyConfig;
using pineforge::source::PineStrategyHost;

constexpr double missing = order_print_tape::missing;
constexpr std::int64_t entry_time = 1743465600000LL;
constexpr std::int64_t cleanup_time = entry_time + 6 * 3600000LL;
constexpr std::int64_t trail_entry_time = 1772654400000LL;

struct TapeRow {
    std::int64_t entry = 0;
    std::int64_t exit = 0;
    double entry_price = missing;
    double exit_price = missing;
    double quantity = missing;
    bool is_long = false;
    std::string entry_id;
    std::string exit_id;
};

std::vector<TapeRow> read_tape(const std::string& name) {
    std::ifstream input(std::string(PINEFORGE_ORDER_PRINT_FIXTURE_ROOT)
        + "/cross_mechanism/" + name + "/tv_trades.csv");
    if (!input) throw std::runtime_error("missing TradingView tape: " + name);
    std::string line;
    std::getline(input, line);
    std::map<int, TapeRow> rows;
    std::map<int, int> parts;
    while (std::getline(input, line)) {
        const auto fields = order_print_tape::fields(line);
        if (fields.size() < 6) throw std::runtime_error("invalid TradingView row");
        const int number = std::stoi(fields[0]);
        auto& row = rows[number];
        if (fields[1] == "Entry long" || fields[1] == "Entry short") {
            row.entry = order_print_tape::timestamp(fields[2]);
            row.entry_price = std::stod(fields[4]);
            row.entry_id = fields[3];
            row.is_long = fields[1] == "Entry long";
            parts[number] |= 1;
        } else if (fields[1] == "Exit long" || fields[1] == "Exit short") {
            row.exit = order_print_tape::timestamp(fields[2]);
            row.exit_price = std::stod(fields[4]);
            row.exit_id = fields[3];
            parts[number] |= 2;
        } else {
            throw std::runtime_error("non-closed TradingView row");
        }
        row.quantity = std::stod(fields[5]);
    }
    std::vector<TapeRow> result;
    for (const auto& item : rows) {
        if (parts[item.first] != 3 || !std::isfinite(item.second.quantity)
            || item.second.quantity <= 0.0) {
            throw std::runtime_error("incomplete or empty TradingView trade");
        }
        result.push_back(item.second);
    }
    return result;
}

class CrossHost : public PineStrategyHost {
public:
    explicit CrossHost(double capital = 1000000.0, double margin = 0.0,
                       double fee = 0.0, bool percent = false, bool stock = false) {
        attach_pine_execution_adapter();
        PineStrategyConfig config;
        config.initial_capital = capital;
        config.pyramiding = 3;
        config.default_qty_type = static_cast<int>(percent
            ? QtyType::PERCENT_OF_EQUITY : QtyType::FIXED);
        config.default_qty_value = percent ? 100.0 : 1.0;
        config.margin_long = margin;
        config.margin_short = margin;
        if (fee > 0.0) {
            config.commission_type = static_cast<int>(CommissionType::CASH_PER_CONTRACT);
            config.commission_value = fee;
        }
        configure_pine_strategy(config);
        set_syminfo_timezone(stock ? "America/New_York" : "Etc/UTC");
        set_syminfo_session(stock ? "0930-1600" : "24x7");
        set_syminfo_mintick(0.01);
        set_syminfo_metadata("qty_step", stock ? 1.0 : 0.0001);
    }

    int pending_entries = 0;
    int frozen_entries = 0;
    int pending_after_cancel = -1;
    int partial_calls = 0;
    int percentage_calls = 0;
    double reversal_position = 0.0;
    double carried_best = missing;
    double reversal_transaction = missing;

protected:
    void entry(const std::string& id, bool is_long, double quantity) {
        strategy_entry(id, is_long, missing, missing, quantity);
    }

    void batch(bool is_long, double first, double second, double third) {
        entry("A", is_long, first);
        entry("B", is_long, second);
        entry("C", is_long, third);
        observe_entries();
    }

    void observe_entries() {
        // These read-only placement facts are finite only for the frozen market
        // batch route at pyramiding 3. Observe before a dependent exit/cancel;
        // margin and percent cases must leave them absent, not silently use F1.
        for (const auto& row : source_pending_view()) {
            if (row.type != FixtureIntentKind::MARKET
                && row.type != FixtureIntentKind::ENTRY) continue;
            ++pending_entries;
            if (std::isfinite(row.frozen_market_own_units)) ++frozen_entries;
            if (row.id == "A") reversal_transaction = row.frozen_market_transaction_units;
        }
    }
};

class NamedBracketHost final : public CrossHost {
public:
    void on_source_bar(const Bar& bar) override {
        if (bar.timestamp == entry_time) {
            batch(true, 1.0, 2.0, 3.0);
            strategy_exit("bracket", "B", 1827.0, 1815.0);
        }
        if (bar.timestamp == cleanup_time) strategy_close_all();
    }
};

class GlobalBracketHost final : public CrossHost {
public:
    void on_source_bar(const Bar& bar) override {
        if (bar.timestamp == entry_time) {
            batch(true, 1.0, 2.0, 3.0);
            strategy_exit("bracket", "", 1827.0, 1815.0);
        }
        if (bar.timestamp == cleanup_time) strategy_close_all();
    }
};

class CancelPendingHost final : public CrossHost {
public:
    void on_source_bar(const Bar& bar) override {
        if (bar.timestamp == entry_time) {
            batch(true, 1.0, 2.0, 3.0);
            strategy_cancel("B");
            pending_after_cancel = 0;
            for (const auto& row : source_pending_view()) {
                if (row.type == FixtureIntentKind::MARKET
                    || row.type == FixtureIntentKind::ENTRY) ++pending_after_cancel;
            }
        }
        if (bar.timestamp == cleanup_time) strategy_close_all();
    }
};

class OppositePartialHost final : public CrossHost {
public:
    void on_source_bar(const Bar& bar) override {
        if (bar.timestamp == entry_time) {
            entry("S", false, 0.0003);
            batch(true, 0.0005, 0.0006, 0.0022);
        }
        if (bar.timestamp == entry_time + 900000LL) {
            ++partial_calls;
            strategy_close("C", {}, missing, 50.0);
        }
        if (bar.timestamp == cleanup_time) strategy_close_all();
    }
};

class FullMarginBatchHost final : public CrossHost {
public:
    FullMarginBatchHost() : CrossHost(2733.0, 100.0, 1.0) {}
    void on_source_bar(const Bar& bar) override {
        if (bar.timestamp == entry_time) batch(false, 0.5, 0.5, 0.5);
        if (bar.timestamp == cleanup_time) strategy_close_all();
    }
};

class HalfMarginBatchHost final : public CrossHost {
public:
    HalfMarginBatchHost() : CrossHost(2735.0, 50.0, 1.0) {}
    void on_source_bar(const Bar& bar) override {
        if (bar.timestamp == entry_time) batch(true, 1.0, 1.0, 1.0);
        if (bar.timestamp == cleanup_time) strategy_close_all();
    }
};

class FractionalMarginHost final : public CrossHost {
public:
    FractionalMarginHost() : CrossHost(91.4, 100.0) {}
    void on_source_bar(const Bar& bar) override {
        if (bar.timestamp == entry_time) batch(false, 0.0003, 0.0005, 0.0492);
        if (bar.timestamp == entry_time + 1800000LL) strategy_close_all();
    }
};

class AllInPercentHost final : public CrossHost {
public:
    AllInPercentHost() : CrossHost(100000.0, 100.0, 0.0, true) {}
    void on_source_bar(const Bar& bar) override {
        if (bar.timestamp == entry_time + 900000LL) {
            strategy_entry("L", true);
            observe_entries();
            strategy_exit("take", "L", 1827.0, missing, missing, missing, missing, 50.0);
        }
        if (bar.timestamp == entry_time + 2700000LL) {
            strategy_exit("remaining", "L", 1900.0, missing,
                          missing, missing, missing, 50.0);
        }
        if (bar.timestamp == entry_time + 5400000LL) {
            reversal_position = signed_position_size();
            strategy_entry("S", false);
            observe_entries();
            strategy_exit("take", "S", 1820.0, missing, missing, missing, missing, 50.0);
        }
        if (signed_position_size() < 0.0 && trade_count() > 2 && !recreated_) {
            strategy_cancel("take");
            strategy_exit("remaining", "S", 1900.0, missing,
                          missing, missing, missing, 50.0);
            recreated_ = true;
            ++percentage_calls;
        }
        if (signed_position_size() < 0.0 && trade_count() > 3 && recreated_ && !finished_) {
            strategy_exit("last", "S", 1900.0, missing,
                          missing, missing, missing, 50.0);
            finished_ = true;
            ++percentage_calls;
        }
        if (bar.timestamp == entry_time + 86400000LL) strategy_close_all();
    }

private:
    bool recreated_ = false;
    bool finished_ = false;
};

class BatchCarriedTrailHost final : public CrossHost {
public:
    BatchCarriedTrailHost() : CrossHost(1000000.0, 0.0, 0.0, false, true) {}
    void on_source_bar(const Bar& bar) override {
        if (bar.timestamp == trail_entry_time) batch(false, 1.0, 1.0, 1.0);
        if (bar.timestamp == trail_entry_time + 900000LL) {
            strategy_exit("trail", "B", missing, missing, 23.0, 1.3134);
        }
        if (bar.timestamp == trail_entry_time + 1800000LL) {
            // Activation follows the high, survives the closing low, and is
            // carried into the next raw 262.345 open (source quote 262.35).
            for (const auto& request : native_working_requests()) {
                const auto state = trail_state(request.definition->handle);
                if (state && state->activated) carried_best = state->best_price;
            }
        }
        if (bar.timestamp == trail_entry_time + 2700000LL) {
            strategy_close("", {}, missing, missing, true);
        }
    }
};

enum class Shape { NamedBracket, GlobalBracket, CancelPending, OppositePartial,
                   FullMargin, HalfMargin, FractionalMargin, AllInPercent, CarriedTrail };

struct Case {
    const char* name;
    Shape shape;
    int trades;
    int placements;
    int frozen;
    bool asserted;
};

std::unique_ptr<CrossHost> make_host(Shape shape) {
    switch (shape) {
    case Shape::NamedBracket: return std::make_unique<NamedBracketHost>();
    case Shape::GlobalBracket: return std::make_unique<GlobalBracketHost>();
    case Shape::CancelPending: return std::make_unique<CancelPendingHost>();
    case Shape::OppositePartial: return std::make_unique<OppositePartialHost>();
    case Shape::FullMargin: return std::make_unique<FullMarginBatchHost>();
    case Shape::HalfMargin: return std::make_unique<HalfMarginBatchHost>();
    case Shape::FractionalMargin: return std::make_unique<FractionalMarginHost>();
    case Shape::AllInPercent: return std::make_unique<AllInPercentHost>();
    case Shape::CarriedTrail: return std::make_unique<BatchCarriedTrailHost>();
    }
    throw std::runtime_error("unknown cross-mechanism shape");
}

bool near(double actual, double expected) {
    return std::isfinite(actual) && std::abs(actual - expected) < 1e-8;
}

bool oracle_witness(const Case& tape, const std::vector<TapeRow>& rows) {
    if (static_cast<int>(rows.size()) != tape.trades) return false;
    std::set<std::string> entry_ids;
    int margin_rows = 0;
    double margin_quantity = 0.0;
    double bracket_quantity = 0.0;
    double short_quantity = 0.0;
    double recreated_quantity = missing;
    int fifo_rows = 0;
    double fifo_quantity = 0.0;
    bool opening_trail = false;
    for (const auto& row : rows) {
        entry_ids.insert(row.entry_id);
        if (row.exit_id == "Margin call") {
            ++margin_rows;
            margin_quantity += row.quantity;
        }
        if (row.exit_id == "bracket") bracket_quantity += row.quantity;
        if (!row.is_long) short_quantity += row.quantity;
        if (row.exit_id == "remaining" && row.entry_id == "S") recreated_quantity = row.quantity;
        if (row.exit_id == "Close entry(s) order C") {
            ++fifo_rows;
            fifo_quantity += row.quantity;
        }
        if (row.exit_id == "trail" && row.exit == trail_entry_time + 2700000LL
            && near(row.exit_price, 262.35)) opening_trail = true;
    }
    switch (tape.shape) {
    case Shape::NamedBracket: return entry_ids.size() == 3 && near(bracket_quantity, 2.0);
    case Shape::GlobalBracket: return entry_ids.size() == 3 && near(bracket_quantity, 6.0);
    case Shape::CancelPending: return entry_ids == std::set<std::string>{"A", "C"};
    case Shape::OppositePartial: return fifo_rows == 2 && near(fifo_quantity, 0.0011);
    case Shape::FullMargin:
    case Shape::HalfMargin: return entry_ids.size() == 3 && margin_rows > 0;
    case Shape::FractionalMargin:
        // One 0.0008 liquidation exactly drains A + B, but not C. Neither a
        // partial first lot nor a rounded-zero fragment can satisfy this tape.
        return margin_rows == 2 && near(margin_quantity, 0.0008)
            && rows[0].entry_id == "A" && rows[1].entry_id == "B";
    case Shape::AllInPercent:
        // The recreated 50% target uses the whole S incarnation, including
        // its completed margin slice, rather than half the reduced live book.
        return entry_ids == std::set<std::string>{"L", "S"} && margin_rows > 0
            && near(recreated_quantity, short_quantity * 0.5);
    case Shape::CarriedTrail: return entry_ids.size() == 3 && opening_trail;
    }
    return false;
}

auto row_key(const TapeRow& row) {
    return std::make_tuple(row.entry, row.exit, row.is_long, row.entry_id,
        std::llround(row.entry_price / 0.01), std::llround(row.exit_price / 0.01),
        std::llround(row.quantity / 1e-8));
}

void print_rows(const char* name, const char* origin, const std::vector<TapeRow>& rows) {
    for (const auto& row : rows) {
        std::printf("ROW,%s,%s,%lld,%lld,%d,%.17g,%.17g,%.17g,%s,%s\n",
            name, origin, static_cast<long long>(row.entry), static_cast<long long>(row.exit),
            row.is_long, row.entry_price, row.exit_price, row.quantity,
            row.entry_id.c_str(), row.exit_id.c_str());
    }
}

bool replay(const Case& tape, bool record) {
    auto expected = read_tape(tape.name);
    const bool witnessed = oracle_witness(tape, expected);
    const auto bars = order_print_tape::bars(std::string("cross_mechanism/") + tape.name);
    auto host = make_host(tape.shape);
    host->set_trade_start_time(bars.front().timestamp);
    host->run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    std::vector<TapeRow> actual;
    for (int index = 0; index < host->trade_count(); ++index) {
        const auto& trade = host->get_trade(index);
        actual.push_back({trade.entry_time, trade.exit_time, trade.entry_price, trade.exit_price,
                          trade.qty, trade.is_long, trade.entry_id, trade.exit_id});
    }
    const auto ordered = [](const TapeRow& first, const TapeRow& second) {
        return row_key(first) < row_key(second);
    };
    std::sort(actual.begin(), actual.end(), ordered);
    std::sort(expected.begin(), expected.end(), ordered);
    bool agrees = host->last_error().empty() && actual.size() == expected.size();
    for (std::size_t index = 0; index < std::min(actual.size(), expected.size()); ++index) {
        const auto& observed = actual[index];
        const auto& wanted = expected[index];
        // Same tolerances as order_print_tape_fixture.hpp: prices in symbol
        // ticks and quantity within 1e-8; timestamps and entry identity exact.
        agrees = agrees && observed.entry == wanted.entry && observed.exit == wanted.exit
            && observed.is_long == wanted.is_long && observed.entry_id == wanted.entry_id
            && std::llround(observed.entry_price / 0.01) == std::llround(wanted.entry_price / 0.01)
            && std::llround(observed.exit_price / 0.01) == std::llround(wanted.exit_price / 0.01)
            && near(observed.quantity, wanted.quantity);
    }
    bool mechanism = host->pending_entries == tape.placements && host->frozen_entries == tape.frozen;
    if (tape.shape == Shape::CancelPending) mechanism = mechanism && host->pending_after_cancel == 2;
    if (tape.shape == Shape::OppositePartial) {
        mechanism = mechanism && host->partial_calls == 1 && near(host->reversal_transaction, 0.0008);
    }
    if (tape.shape == Shape::AllInPercent) {
        mechanism = mechanism && host->reversal_position > 0.0 && host->percentage_calls == 2;
    }
    if (tape.shape == Shape::CarriedTrail) mechanism = mechanism && near(host->carried_best, 262.34);
    std::printf("PROOF,%s,pending=%d,frozen=%d,after_cancel=%d,partial=%d,percentage=%d,"
                "reversal=%.17g,transaction=%.17g,carried_best=%.17g,witness=%d,mechanism=%d\n",
        tape.name, host->pending_entries, host->frozen_entries, host->pending_after_cancel,
        host->partial_calls, host->percentage_calls, host->reversal_position,
        host->reversal_transaction, host->carried_best, witnessed, mechanism);
    std::printf("RESULT,%s,%s,engine=%zu,tv=%zu,%s,error=%s\n", tape.name,
        agrees ? "agree" : "disagree", actual.size(), expected.size(),
        tape.asserted ? "asserted" : "recorded-not-asserted", host->last_error().c_str());
    if (record || !agrees) {
        print_rows(tape.name, "engine", actual);
        print_rows(tape.name, "tv", expected);
    }
    return witnessed && (record || (mechanism && (!tape.asserted || agrees)));
}

}

int main(int argc, char** argv) {
    const bool record = argc == 2 && std::string(argv[1]) == "--record";
    if (argc > 1 && !record) return 2;
    const Case cases[] = {
        {"bracket_one", Shape::NamedBracket, 4, 3, 3, true},
        {"bracket_all", Shape::GlobalBracket, 3, 3, 3, true},
        {"cancel_pending", Shape::CancelPending, 2, 3, 3, true},
        // Recorded: both engine revisions emit an extra sub-step C fragment.
        {"opposite_partial_fifo", Shape::OppositePartial, 4, 4, 4, false},
        {"margin_batch_100", Shape::FullMargin, 5, 3, 0, true},
        // Recorded: neither revision reproduces all three TV margin checkpoints.
        {"margin_batch_50", Shape::HalfMargin, 6, 3, 0, false},
        // Recorded: both revisions liquidate C instead of only the A + B prefix.
        {"margin_fractional_boundary", Shape::FractionalMargin, 3, 3, 0, false},
        {"allin_percent_reversal", Shape::AllInPercent, 5, 2, 0, true},
        {"batch_carried_trail", Shape::CarriedTrail, 3, 3, 3, true},
    };
    int failures = 0;
    int asserted = 0;
    try {
        for (const auto& tape : cases) {
            if (tape.asserted) ++asserted;
            if (!replay(tape, record)) ++failures;
        }
    } catch (const std::exception& error) {
        std::printf("FAIL cross-mechanism fixture: %s\n", error.what());
        return 1;
    }
    std::printf("Cross-mechanism tapes: %zu cases, %d asserted, %zu recorded, %d failures%s\n",
        sizeof(cases) / sizeof(cases[0]), asserted,
        sizeof(cases) / sizeof(cases[0]) - asserted, failures,
        record ? " (observations only)" : "");
    return failures == 0 ? 0 : 1;
}
