// #858: a void-only source book needs no retained-sequence index work.
// This is a native adapter work test, not the public Binance reproduction.
// Observe existing counters through run()/strategy_exit(); do not replace
// commands with a model. Issued definitions and the later entry incarnation
// remain observable, so dropping the void calls is not an acceptable fix.
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
using Adapter = source::PineExecutionAdapter;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t start_ms = INT64_C(1736121600000);
constexpr int exit_bars = 12;
constexpr int total_bars = exit_bars + 5;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct ExitInput {
    int bar = 0;
    std::string id, parent, comment;
    double limit = 0.0, stop = 0.0;
    int issued_limits = 0, issued_stops = 0;
};

class AbsentExitHost final : public source::PineStrategyHost {
public:
    // Fixture-only access to the inherited, ledger-backed observations.
    using BacktestEngine::closed_trade_entry_comment;
    using BacktestEngine::closed_trade_exit_comment;
    using BacktestEngine::closed_trade_entry_id;
    using BacktestEngine::closed_trade_exit_id;
    using BacktestEngine::closed_trade_entry_incarnation;

    AbsentExitHost(int fanout, bool varying) : fanout_(fanout), varying_(varying) {
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(1.0);
        set_broker_state_hash_recording(false);
        fixture_retain_all_events();
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        ++callbacks;
        if (i < exit_bars) {
            require(live_position_size() == 0.0 && trade_count() == 0,
                    "absent-parent phase acquired a position or trade");
            for (int slot = 0; slot < fanout_; ++slot) {
                ExitInput input;
                input.bar = i;
                const auto suffix = std::to_string(slot) + "-"
                    + std::to_string(varying_ ? i % 3 : 0);
                input.id = "guard-" + suffix;
                input.parent = "parent-" + suffix;
                input.comment = varying_ ? "call-" + std::to_string(i) + "-" + suffix : "";
                input.limit = 150.0 + 8.0 * slot + (varying_ ? 3.0 * i : 0.0);
                input.stop = 50.0 + 4.0 * slot + (varying_ ? 2.0 * i : 0.0);
                inputs.push_back(input);
                strategy_exit(input.id, input.parent, input.limit, input.stop,
                              missing, missing, missing, 100.0, input.comment);
            }
        } else if (i == exit_bars) {
            // Include the next bar open's retirement, before any real entry.
            work = adapter_.exit_group_work();
            retirement = adapter_.retirement_work();
            measured = true;
            require(live_position_size() == 0.0 && trade_count() == 0,
                    "void calls acted before the later entry");
            strategy_entry("parent-0-0", true, missing, missing, 1.0, "tail-entry");
        } else if (i == exit_bars + 2) {
            // This bar reaches every earlier stop/limit. None may attach to
            // the later entry; the explicit close must be the closing command.
            require(live_position_size() == 1.0 && trade_count() == 0,
                    "a void exit activated against the later entry");
            strategy_close("parent-0-0", "tail-close");
        }
    }

    void observe_issued() {
        for (const auto& row : native_events(0)) {
            if (!row.command) continue;
            if (const auto* event = std::get_if<native_order::AcceptedEvent>(&*row.command))
                observe_definition(event->definition, event->ordinal, 0, "accepted");
            else if (const auto* event = std::get_if<native_order::ReplacedEvent>(&*row.command))
                observe_definition(event->successor_definition, event->ordinal,
                                   event->predecessor().incarnation, "replaced");
        }
        require(issued_exits == 2 * inputs.size(),
                "each supplied stop/limit exit must still issue both native definitions");
        for (const auto& input : inputs)
            require(input.issued_limits == 1 && input.issued_stops == 1,
                    "an exit input lost or duplicated a native leg");
        require(issued_entries == 1 && issued_closes == 1,
                "the later entry/close did not issue exactly once");
        require(entry_incarnation > last_exit_incarnation,
                "the later entry did not follow the void request incarnations");
    }

    Adapter::ExitGroupWork work{};
    Adapter::RetirementWork retirement{};
    std::vector<ExitInput> inputs;
    int callbacks = 0;
    bool measured = false;
    std::uint64_t entry_incarnation = 0, last_exit_incarnation = 0;

private:
    void observe_definition(const native_order::DefinitionRef& definition,
                            std::uint64_t ordinal, std::uint64_t predecessor,
                            const char* event) {
        require(static_cast<bool>(definition), "issued event has no native definition");
        const auto& request = definition->request;
        const auto incarnation = definition->handle.incarnation;
        require(incarnation != 0, "issued native definition has no incarnation");
        if (request.comment == "tail-entry" || request.comment == "tail-close") {
            if (request.comment == "tail-entry") {
                require(request.label == "parent-0-0", "later entry changed its id");
                ++issued_entries;
                entry_incarnation = incarnation;
            } else {
                require(request.label == "__close__parent-0-0", "later close changed its id");
                ++issued_closes;
            }
            std::printf("PF_ISSUE858_ISSUED fanout=%d varying=%d ordinal=%llu event=%s "
                        "kind=%s incarnation=%llu predecessor=%llu id=%s\n",
                        fanout_, varying_ ? 1 : 0, static_cast<unsigned long long>(ordinal),
                        event, request.comment.c_str(),
                        static_cast<unsigned long long>(incarnation),
                        static_cast<unsigned long long>(predecessor), request.label.c_str());
            return;
        }
        require(issued_exits < 2 * inputs.size(), "unexpected additional native definition");
        auto& input = inputs[issued_exits / 2];
        require(request.label == input.id && request.comment == input.comment,
                "issued definition did not carry its supplied exit id/comment");
        const char* kind = nullptr;
        double price = missing, supplied = missing;
        if (const auto* limit = std::get_if<native_order::Limit>(&request.trigger)) {
            kind = "limit";
            price = limit->price;
            supplied = input.limit;
            ++input.issued_limits;
        } else if (const auto* stop = std::get_if<native_order::Stop>(&request.trigger)) {
            kind = "stop";
            price = stop->price;
            supplied = input.stop;
            ++input.issued_stops;
        }
        // Adapter reach thresholds may differ from the supplied on-grid level
        // within its one-unit price tick. Moving inputs change by > one tick.
        require(kind && std::isfinite(price) && std::abs(price - supplied) <= 1.0,
                "native leg ignored its supplied level or changed trigger kind");
        ++issued_exits;
        last_exit_incarnation = std::max(last_exit_incarnation, incarnation);
        std::printf("PF_ISSUE858_ISSUED fanout=%d varying=%d ordinal=%llu event=%s "
                    "bar=%d id=%s parent=%s kind=%s supplied=%.17g price=%.17g "
                    "incarnation=%llu predecessor=%llu\n",
                    fanout_, varying_ ? 1 : 0, static_cast<unsigned long long>(ordinal),
                    event, input.bar, input.id.c_str(), input.parent.c_str(), kind,
                    supplied, price, static_cast<unsigned long long>(incarnation),
                    static_cast<unsigned long long>(predecessor));
    }

    int fanout_;
    bool varying_;
    std::size_t issued_exits = 0;
    int issued_entries = 0, issued_closes = 0;
};

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (int i = 0; i < total_bars; ++i) {
        const bool reach_old_exits = i == exit_bars + 2;
        bars.push_back({100.0, reach_old_exits ? 300.0 : 100.0,
                        reach_old_exits ? 1.0 : 100.0, 100.0, 1.0,
                        start_ms + static_cast<std::int64_t>(i) * 60000});
    }
    return bars;
}

struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
};

struct Outcome {
    TradeC trade{};
    std::uint64_t entry_incarnation = 0, sequence_operations = 0;
};

Outcome run_case(int fanout, bool varying) {
    AbsentExitHost host(fanout, varying);
    const auto bars = feed();
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(host.last_error().empty(), "native run failed: " + host.last_error());
    require(host.measured && host.callbacks == total_bars,
            "native run did not visit every input bar and the work boundary");
    require(host.inputs.size() == static_cast<std::size_t>(fanout * exit_bars),
            "native host did not execute the supplied number of exit calls");
    std::printf("PF_ISSUE858_WORK fanout=%d varying=%d bars=%d calls=%zu "
                "sequence_operations=%llu sequence_pages=%llu retained=%zu "
                "retired_rows=%llu retirement_visits=%llu\n",
                fanout, varying ? 1 : 0, exit_bars, host.inputs.size(),
                static_cast<unsigned long long>(host.work.sequence_index_operations),
                static_cast<unsigned long long>(host.work.sequence_index_pages),
                host.work.retained_placements,
                static_cast<unsigned long long>(host.retirement.rows_erased),
                static_cast<unsigned long long>(host.retirement.row_visits));
    host.observe_issued();
    Report report;
    host.fill_report(&report.value);
    require(host.trade_count() == 1 && report.value.trades_len == 1,
            "native witness did not produce exactly its explicit tail trade");
    const auto& trade = report.value.trades[0];
    require(!trade.open_at_end && trade.qty == 1.0 && trade.is_long
            && trade.entry_price == 100.0 && trade.exit_price == 100.0
            && trade.pnl == 0.0 && trade.commission == 0.0
            && trade.entry_bar_index == exit_bars + 1
            && trade.exit_bar_index == exit_bars + 3
            && host.closed_trade_entry_comment(0) == "tail-entry"
            && host.closed_trade_exit_comment(0) == "tail-close"
            && host.live_position_size() == 0.0,
            "native trade differs from the explicit one-unit 100-to-100 close");
    const auto incarnation = host.closed_trade_entry_incarnation(0);
    require(incarnation == host.entry_incarnation,
            "trade lost the actual native opening incarnation");
    std::printf("PF_ISSUE858_TRADE fanout=%d varying=%d entry_time=%lld exit_time=%lld "
                "entry_price=%.17g exit_price=%.17g qty=%.17g pnl=%.17g "
                "entry_incarnation=%llu entry_id=%s exit_id=%s\n",
                fanout, varying ? 1 : 0, static_cast<long long>(trade.entry_time),
                static_cast<long long>(trade.exit_time), trade.entry_price, trade.exit_price,
                trade.qty, trade.pnl, static_cast<unsigned long long>(incarnation),
                host.closed_trade_entry_id(0).c_str(), host.closed_trade_exit_id(0).c_str());
    return {trade, incarnation, host.work.sequence_index_operations};
}

bool same_trade(const TradeC& a, const TradeC& b) {
    return std::tie(a.entry_time, a.exit_time, a.entry_price, a.exit_price, a.pnl, a.pnl_pct,
                    a.is_long, a.max_runup, a.max_drawdown, a.qty, a.commission,
                    a.entry_bar_index, a.exit_bar_index, a.open_at_end)
        == std::tie(b.entry_time, b.exit_time, b.entry_price, b.exit_price, b.pnl, b.pnl_pct,
                    b.is_long, b.max_runup, b.max_drawdown, b.qty, b.commission,
                    b.entry_bar_index, b.exit_bar_index, b.open_at_end);
}
}  // namespace

int main() {
    try {
        const auto control = run_case(0, false);
        require(control.sequence_operations == 0,
                "no-exit control unexpectedly used the retained-sequence index");
        int red = 0;
        for (const auto& shape : {std::make_pair(1, false), std::make_pair(2, false),
                                  std::make_pair(2, true)}) {
            const auto observed = run_case(shape.first, shape.second);
            require(same_trade(observed.trade, control.trade),
                    "exit calls changed a reported tail trade field");
            require(observed.entry_incarnation > control.entry_incarnation,
                    "exit calls did not preserve their place in the native handle stream");
            if (observed.sequence_operations != control.sequence_operations) {
                ++red;
                std::printf("PF_ISSUE858_RED fanout=%d varying=%d "
                            "absent-parent exits performed retained-sequence index work\n",
                            shape.first, shape.second ? 1 : 0);
            }
        }
        std::printf("PF_ISSUE858_RESULT work_failures=%d\n", red);
        return red == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "PF_ISSUE858_FIXTURE_ERROR %s\n", error.what());
        return 2;
    }
}
