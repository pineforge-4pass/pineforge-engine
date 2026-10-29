// Sealed report rows keep their global indices but have no C view.
// Include the C declarations first: engine.hpp suppresses per-strategy decls.
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/run_failure.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <string>

namespace {

using pineforge::BacktestEngine;
using pineforge::Trade;
using pineforge::execution::CloseCause;

int checks = 0;
int failures = 0;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
    } \
} while (0)

template<class F>
void no_throw(const char* accessor, int index, F action) {
    try {
        action();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL %s(%d) threw: %s\n", accessor, index, error.what());
    } catch (...) {
        ++failures;
        std::fprintf(stderr, "FAIL %s(%d) threw a non-standard exception\n", accessor, index);
    }
}

class Probe final : public BacktestEngine {
public:
    Probe() : BacktestEngine(NativeConsumerBindTag{}) {}
    void on_bar(const pineforge::Bar&) override {}
    void append(const Trade& trade) { trades_.push_back(trade); }
    void append_range_end(const Trade& trade) { range_end_trades_.push_back(trade); }
    void seal(std::size_t end) { trades_.seal(end); }
    std::size_t base() const { return trades_.base(); }
    pf_strategy_t handle() { return static_cast<BacktestEngine*>(this); }
};

Trade row(int n) {
    Trade trade{};
    trade.entry_incarnation = 100 + n;
    trade.entry_id = "entry-" + std::to_string(n) + std::string(40, 'e');
    trade.exit_id = "exit-" + std::to_string(n) + std::string(40, 'x');
    trade.exit_comment = "comment-" + std::to_string(n) + std::string(40, 'c');
    return trade;
}

void absent(pf_strategy_t handle, int index) {
    // Catch each call separately: the red test names every escaping accessor.
    no_throw("strategy_closed_trade_entry_incarnation", index, [&] {
        CHECK(strategy_closed_trade_entry_incarnation(handle, index) == 0);
    });
    no_throw("strategy_closed_trade_entry_id", index, [&] {
        CHECK(strategy_closed_trade_entry_id(handle, index) == nullptr);
    });
    no_throw("strategy_closed_trade_exit_id", index, [&] {
        CHECK(strategy_closed_trade_exit_id(handle, index) == nullptr);
    });
    no_throw("strategy_closed_trade_exit_comment", index, [&] {
        CHECK(strategy_closed_trade_exit_comment(handle, index) == nullptr);
    });
    no_throw("strategy_closed_trade_close_cause", index, [&] {
        CHECK(strategy_closed_trade_close_cause(handle, index) == -1);
    });
    if (handle) {
        no_throw("BacktestEngine::closed_trade_close_cause", index, [&] {
            CHECK(static_cast<BacktestEngine*>(handle)->closed_trade_close_cause(index) == -1);
        });
    }
}

void text(const char* actual, const std::string& expected) {
    CHECK(actual != nullptr);
    if (actual) CHECK(std::strcmp(actual, expected.c_str()) == 0);
}

void present(Probe& probe, int index, const Trade& expected, int cause) {
    const auto handle = probe.handle();
    CHECK(strategy_closed_trade_entry_incarnation(handle, index) == expected.entry_incarnation);
    const char* entry = strategy_closed_trade_entry_id(handle, index);
    const char* exit = strategy_closed_trade_exit_id(handle, index);
    const char* comment = strategy_closed_trade_exit_comment(handle, index);
    text(entry, expected.entry_id);
    text(exit, expected.exit_id);
    text(comment, expected.exit_comment);
    const Trade& resident = probe.materialize_report_trade(index);
    CHECK(entry == resident.entry_id.c_str());
    CHECK(exit == resident.exit_id.c_str());
    CHECK(comment == resident.exit_comment.c_str());
    CHECK(strategy_closed_trade_close_cause(handle, index) == cause);
    CHECK(probe.closed_trade_close_cause(index) == cause);
}

void internal_read_matches(const Probe& probe, int index, const Trade& expected) {
    const auto actual = probe.get_report_trade(index);
    CHECK(actual.entry_incarnation == expected.entry_incarnation);
    CHECK(actual.entry_id == expected.entry_id);
    CHECK(actual.exit_id == expected.exit_id);
    CHECK(actual.exit_comment == expected.exit_comment);
    CHECK(actual.open_at_end == expected.open_at_end);
    CHECK(actual.exit_from_bracket == expected.exit_from_bracket);
    CHECK(actual.close_cause == expected.close_cause);
}

void resident_window() {
    Probe probe;
    std::array<Trade, 4> closed{{row(0), row(1), row(2), row(3)}};
    // A valid legacy row can have incarnation 0; it must still expose its ids.
    closed[0].entry_incarnation = 0;
    closed[1].exit_from_bracket = true;
    closed[2].close_cause = CloseCause::Liquidation;
    closed[2].exit_from_bracket = true;
    closed[3].close_cause = CloseCause::RiskLimit;
    const std::array<int, 4> causes{{1, 2, 3, 4}};
    for (const Trade& trade : closed) probe.append(trade);

    std::array<Trade, 2> range_end{{row(10), row(11)}};
    range_end[0].open_at_end = range_end[1].open_at_end = true;
    range_end[0].close_cause = CloseCause::Liquidation;
    range_end[0].exit_from_bracket = true;
    range_end[1].entry_id.clear();
    range_end[1].exit_id.clear();
    range_end[1].exit_comment.clear();
    for (const Trade& trade : range_end) probe.append_range_end(trade);

    CHECK(probe.base() == 0);
    CHECK(probe.trade_count() == 4);
    CHECK(probe.report_trade_count() == 6);
    for (int i = 0; i < 4; ++i) present(probe, i, closed[i], causes[i]);
    present(probe, 4, range_end[0], 6);
    present(probe, 5, range_end[1], 6);
    absent(probe.handle(), -1);
    absent(probe.handle(), std::numeric_limits<int>::min());
    absent(probe.handle(), 6);
    absent(probe.handle(), std::numeric_limits<int>::max());

    // L8c owns publication; its storage effect is the real ledger seal here.
    probe.seal(2);
    CHECK(probe.base() == 2);
    CHECK(probe.trade_count() == 4);
    CHECK(probe.report_trade_count() == 6);
    present(probe, 0, closed[0], causes[0]);
    present(probe, 1, closed[1], causes[1]); // Sealing preserves retained bytes.
    internal_read_matches(probe, 1, closed[1]);
    present(probe, 2, closed[2], causes[2]); // base is a global index.
    present(probe, 3, closed[3], causes[3]);
    present(probe, 4, range_end[0], 6);
    present(probe, 5, range_end[1], 6);
    absent(probe.handle(), 6);

    probe.seal(4);
    CHECK(probe.base() == 4);
    CHECK(probe.trade_count() == 4);
    CHECK(probe.report_trade_count() == 6);
    for (int i = 0; i < 4; ++i) present(probe, i, closed[i], causes[i]);
    internal_read_matches(probe, 3, closed[3]);
    present(probe, 4, range_end[0], 6);
    present(probe, 5, range_end[1], 6);

    // A new resident row keeps its global index; range-end rows follow it.
    Trade next = row(20);
    next.close_cause = CloseCause::FillCap;
    probe.append(next);
    CHECK(probe.base() == 4);
    CHECK(probe.trade_count() == 5);
    CHECK(probe.report_trade_count() == 7);
    present(probe, 3, closed[3], causes[3]);
    present(probe, 4, next, 5);
    present(probe, 5, range_end[0], 6);
    present(probe, 6, range_end[1], 6);
    absent(probe.handle(), 7);
}

void empty_windows() {
    absent(nullptr, 0);
    Probe probe;
    absent(probe.handle(), 0);
    probe.append(row(0));
    probe.seal(1);
    CHECK(probe.base() == 1);
    CHECK(probe.report_trade_count() == 1);
    present(probe, 0, row(0), 1); // Fully sealed, with no range-end suffix.
    absent(probe.handle(), 1);

    Probe range_only;
    Trade end = row(1);
    end.open_at_end = true;
    range_only.append_range_end(end);
    CHECK(range_only.trade_count() == 0);
    present(range_only, 0, end, 6);
    absent(range_only.handle(), 1);
}

} // namespace

int main() {
    no_throw("resident_window", 0, resident_window);
    no_throw("empty_windows", 0, empty_windows);
    std::printf("test_trade_accessors_l8d: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
