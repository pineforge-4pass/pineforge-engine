// The negative fast paths must not hide one qualifying request in a
// large resting limit book. All expected prices, sides and bars below come
// from the script inputs. No harvested transcript or new-summary access.
//
// Reuse the probed source library: a false quiet decision must fail even
// where another policy could also protect the eventual trade. These rows use
// only interfaces present before the maintained counts and can be overlaid on
// the base tree.
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/source/pine_quiet_bar.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace no = native_order;
namespace quiet = source::detail;
using Adapter = source::PineExecutionAdapter;

constexpr int kPopulation = 256;
constexpr double kPrice = 100.0;
constexpr double kDormantLimit = kPrice / 2.0;
constexpr double kMissing = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = INT64_C(1736121600000);
int checks = 0;
int failures = 0;

#define CHECK(condition) do {                                                 \
    ++checks;                                                                 \
    if (!(condition)) {                                                       \
        ++failures;                                                           \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
    }                                                                         \
} while (false)

// The adapter fixtures' existing private-member access convention. Read only:
// none of the tests writes a placement, a live handle or a maintained count.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Placements { friend auto access(Placements); };
struct OpenTrails { friend auto access(OpenTrails); };
template struct Access<Placements, &Adapter::placement_>;
template struct Access<OpenTrails, &Adapter::trail_state_at_open_>;

Bar bar(int i, double o = kPrice, double h = kPrice + 1.0,
        double l = kPrice - 1.0, double c = kPrice) {
    return {o, h, l, c, 1.0, kStart + static_cast<std::int64_t>(i) * 60000};
}

std::vector<Bar> tape(int count) {
    std::vector<Bar> result;
    for (int i = 0; i < count; ++i) result.push_back(bar(i));
    return result;
}

source::PineStrategyConfig configuration() {
    source::PineStrategyConfig config;
    config.initial_capital = 1000000.0;
    config.default_qty_type = static_cast<int>(QtyType::FIXED);
    config.default_qty_value = 1.0;
    config.pyramiding = kPopulation + 4;
    config.margin_long = config.margin_short = 0.0;
    config.commission_value = 0.0;
    config.slippage = 0;
    return config;
}

struct Fill {
    std::string id;
    double price;
    int bar;
    double opened;
    double closed;
    double position;
};

class Host final : public source::PineStrategyHost {
public:
    using Script = std::function<void(Host&, int)>;
    Host(source::PineStrategyConfig config, int untouched_through, Script script,
         double tick = 1.0)
        : script_(std::move(script)), untouched_through_(untouched_through) {
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(tick);
        set_margin_call_enabled(false);
        fixture_retain_all_events();
        market_admission_journal().retain_full_observation_history();
        quiet::quiet_bar_counts() = {};
    }

    std::vector<Fill> fills;
    int source_bars = 0;

    const auto& placements() const { return adapter_.*access(Placements{}); }
    const auto& open_trails() const { return adapter_.*access(OpenTrails{}); }

    void expect_hook_ran(quiet::QuietHook hook) const {
        const auto slot = static_cast<std::size_t>(hook);
        // These scripts have no fill recalculations: exactly one opening
        // follows each source callback, and this one must run the hook.
        CHECK(current_.asked[slot] - previous_.asked[slot] == 1);
        CHECK(current_.quiet[slot] - previous_.quiet[slot] == 0);
    }

    const Fill* fill(const std::string& id) const {
        const auto found = std::find_if(fills.begin(), fills.end(),
            [&](const Fill& row) { return row.id == id; });
        CHECK(found != fills.end());
        return found == fills.end() ? nullptr : &*found;
    }

    void on_source_bar(const Bar&) override {
        ++source_bars;
        current_ = quiet::quiet_bar_counts();
        const int i = pine_bar_index();
        if (i == 0) {
            for (int n = 0; n < kPopulation; ++n)
                strategy_entry("dormant-" + std::to_string(n), true,
                               kDormantLimit, kMissing, 1.0);
        } else if (i <= untouched_through_) {
            check_background(i);
        }
        script_(*this, i);
        previous_ = quiet::quiet_bar_counts();
    }

protected:
    void on_native_applied_event(const no::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        CHECK(event.request().label.compare(0, 8, "dormant-") != 0);
        fills.push_back({event.request().label, event.resolved_price,
                         event.interval_index(), event.opened_units, event.closed_units,
                         live_position_size()});
    }

private:
    void check_background(int i) {
        std::vector<no::DefinitionRef> definitions;
        for (const auto& working : native_working_requests()) {
            const auto& definition = working.definition;
            if (definition->request.label.compare(0, 8, "dormant-") != 0) continue;
            CHECK(std::holds_alternative<no::Limit>(definition->request.trigger));
            const auto found = placements().find(definition->handle.incarnation);
            CHECK(found != placements().end());
            if (found != placements().end()) {
                CHECK(found->second.family == source::PineOrderFamily::Entry);
                CHECK(found->second.exit_levels.limit == kDormantLimit);
            }
            definitions.push_back(definition);
        }
        CHECK(definitions.size() == static_cast<std::size_t>(kPopulation));
        if (i == 1) background_ = definitions;
        else CHECK(definitions == background_); // Same handles, order and definitions.
    }

    Script script_;
    int untouched_through_;
    std::vector<no::DefinitionRef> background_;
    quiet::QuietBarCounts previous_{}, current_{};
};

void run(Host& host, const std::vector<Bar>& bars) {
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.source_bars == static_cast<int>(bars.size()));
}

void expect_fill(const Host& host, const char* id, int at, double price,
                 double opened, double closed, double position) {
    const auto* fill = host.fill(id);
    if (!fill) {
        std::fprintf(stderr, "FAIL fill id=%s field=present actual=0 expected=1\n", id);
        return;
    }
    const auto expect_field = [id](const char* field, auto actual, auto expected) {
        if (actual != expected) {
            std::fprintf(stderr, "FAIL fill id=%s field=%s actual=%.17g expected=%.17g\n",
                         id, field, static_cast<double>(actual), static_cast<double>(expected));
        }
        CHECK(actual == expected);
    };
    expect_field("bar", fill->bar, at);
    expect_field("price", fill->price, price);
    expect_field("opened", fill->opened, opened);
    expect_field("closed", fill->closed, closed);
    expect_field("position", fill->position, position);
}

// (a) A single buy stop rests through two openings; only bar 3 reaches it.
// The extra marketable_stops row below is the actual OpenMarketableSells
// mutation witness: that hook requires both sides, so one buy alone cannot
// distinguish its false-negative mutation.
void buy_stop() {
    const double stop = kPrice + 10.0;
    auto bars = tape(6);
    bars[3] = bar(3, kPrice, stop + 2.0, kPrice - 1.0, kPrice);
    Host host(configuration(), 2, [=](Host& h, int i) {
        if (i == 1) h.strategy_entry("BUY", true, kMissing, stop, 1.0);
        if (i == 2) CHECK(h.live_position_size() == 0.0);
        if (i == 3) CHECK(h.live_position_size() == 1.0);
    });
    run(host, bars);
    expect_fill(host, "BUY", 3, stop, 1.0, 0.0, 1.0);
}

// The sell was queued first, but both stops are marketable at bar 2's open.
// The policy must defer that sell behind the buy. A false buy/sell count
// leaves the sell first and fails both this hook check and the fill ordering.
void marketable_stops() {
    const double distance = 2.0;
    Host host(configuration(), 1, [=](Host& h, int i) {
        if (i == 1) {
            h.strategy_entry("SELL", false, kMissing, kPrice + distance, 1.0);
            h.strategy_entry("BUY", true, kMissing, kPrice - distance, 1.0);
        }
        if (i == 2) h.expect_hook_ran(quiet::QuietHook::OpenMarketableSells);
    });
    run(host, tape(5));
    CHECK(host.fills.size() >= 2);
    if (host.fills.size() >= 2) {
        CHECK(host.fills[0].id == "BUY");
        CHECK(host.fills[1].id == "SELL");
    }
    expect_fill(host, "BUY", 2, kPrice, 1.0, 0.0, 1.0);
    expect_fill(host, "SELL", 2, kPrice, 0.0, 1.0, 0.0);
}

// (b) One buy limit, then a raw market add, beside the older limits. The raw
// order is deliberately not an entry(): close-time entry ranking cannot
// repair the queue first. Opening priority must put MARKET before LIMIT.
void marketable_limit() {
    const double limit = kPrice + 4.0;
    Host host(configuration(), 1, [=](Host& h, int i) {
        if (i == 1) {
            h.strategy_entry("LIMIT", true, limit, kMissing, 1.0);
            h.strategy_order("MARKET", true, 1.0);
        }
    });
    run(host, tape(5));
    CHECK(host.fills.size() >= 2);
    if (host.fills.size() >= 2) {
        CHECK(host.fills[0].id == "MARKET");
        CHECK(host.fills[1].id == "LIMIT");
    }
    expect_fill(host, "MARKET", 2, kPrice, 1.0, 0.0, 1.0);
    expect_fill(host, "LIMIT", 2, kPrice, 1.0, 0.0, 2.0);
}

// (c) Auto walks the low leg first on an equidistant bar. Both stops must be
// counted despite the N earlier limits; a quiet mutation leaves observation
// zero even if native matching still walks the same two fills.
void dual_stop() {
    const double distance = 5.0;
    auto bars = tape(5);
    bars[2] = bar(2, kPrice, kPrice + distance + 1.0,
                  kPrice - distance - 1.0, kPrice);
    Host host(configuration(), 1, [=](Host& h, int i) {
        if (i == 1) {
            h.strategy_entry("UP", true, kMissing, kPrice + distance, 1.0);
            h.strategy_entry("DOWN", false, kMissing, kPrice - distance, 1.0);
        }
        if (i == 2) {
            h.expect_hook_ran(quiet::QuietHook::DualEntryPath);
            CHECK(h.last_bar_dual_entry_path() == 2);
            CHECK(h.observe_last_bar_dual_entry_path_v1() == 2);
        }
    });
    run(host, bars);
    CHECK(host.fills.size() >= 2);
    if (host.fills.size() >= 2) {
        CHECK(host.fills[0].id == "DOWN");
        CHECK(host.fills[1].id == "UP");
    }
    // Both explicit stops are placed flat: src/source/pine_adapter.cpp:8196
    // and :8307 select Transact(+/-1). UP's unit closes DOWN, with no
    // opening remainder (src/engine_execution.cpp:937). Applied opened_units
    // is signed (src/engine_execution.cpp:769), so DOWN opens -1 and UP 0.
    expect_fill(host, "DOWN", 2, kPrice - distance, -1.0, 0.0, -1.0);
    expect_fill(host, "UP", 2, kPrice + distance, 0.0, 1.0, 0.0);
}

void expect_carried_trail(const Host& host, double best, double level) {
    const auto& captured = host.open_trails();
    CHECK(captured.size() == 1);
    if (captured.size() != 1) return;
    const auto& state = captured.begin()->second;
    CHECK(state.activated);
    CHECK(state.best_price == best);
    CHECK(state.current_level == level);
}

// (d) The trail arms on bar 2, survives its close, and carries its best through
// bar 3. A same-activation offset reissue changes the source operand while
// retaining the native ride. Bar 4 gaps below both stop levels. Skipping the
// capture fails the two independently calculated best/level checks.
void trail_capture() {
    const double offset = 4.0;
    const double activation = kPrice + 4.0;
    const double first_best = kPrice + 10.0;
    const double second_best = first_best + 1.0;
    auto bars = tape(6);
    bars[2] = bar(2, kPrice, first_best, kPrice - 1.0, first_best - 1.0);
    bars[3] = bar(3, first_best - 1.0, second_best, first_best - 2.0, first_best);
    bars[4] = bar(4, kPrice + 2.0, kPrice + 3.0, kPrice + 1.0, kPrice + 2.0);
    Host host(configuration(), 3, [=](Host& h, int i) {
        if (i == 0) h.strategy_entry("ENTRY", true, kMissing, kMissing, 1.0);
        if (i == 1)
            h.strategy_exit("TRAIL", "ENTRY", kMissing, kMissing,
                            kMissing, offset, activation);
        if (i == 3) {
            expect_carried_trail(h, first_best, first_best - offset);
            h.strategy_exit("TRAIL", "ENTRY", kMissing, kMissing,
                            kMissing, offset + 1.0, activation);
        }
        if (i == 4)
            expect_carried_trail(h, second_best, second_best - offset);
    });
    run(host, bars);
    expect_fill(host, "ENTRY", 1, kPrice, 1.0, 0.0, 1.0);
    expect_fill(host, "TRAIL", 4, bars[4].open, 0.0, 1.0, 0.0);
}

// (e) The existing DeclinedReversal tape with N priced orders beside it.
// Units = capital / entry, equity at the signal = units * signal close;
// the next opening's larger price makes the frozen reversal unaffordable.
// Its stop must stay suspended until the bar-5 reissue, then close at 95.
void declined_reversal() {
    auto config = configuration();
    config.initial_capital = 10000.0;
    config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    config.default_qty_value = 100.0;
    config.margin_long = config.margin_short = 100.0;
    config.pyramiding = 1;
    const double units = config.initial_capital / kPrice;
    const double signal = kPrice + 10.0;
    const double gap = signal + 1.0;
    const double stop = kPrice - 10.0;
    const double reissued_stop = kPrice - 5.0;
    const double equity = config.initial_capital + units * (signal - kPrice);
    CHECK((equity / signal) * gap > equity);
    const std::vector<Bar> bars{
        bar(0, kPrice, kPrice, kPrice, kPrice),
        bar(1, kPrice, signal + 2.0, kPrice - 1.0, signal),
        bar(2, gap, gap + 1.0, stop - 1.0, gap),
        bar(3, gap, gap + 1.0, stop - 2.0, gap),
        bar(4, signal, signal, signal, signal),
        bar(5, signal, signal, signal, signal),
        bar(6, reissued_stop + 1.0, reissued_stop + 2.0,
               stop - 1.0, reissued_stop),
        bar(7, reissued_stop, reissued_stop, reissued_stop, reissued_stop)};
    Host host(config, 1, [=](Host& h, int i) {
        if (i == 0) h.strategy_entry("LONG", true);
        if (i == 1) {
            h.strategy_exit("EXIT", "LONG", kMissing, stop);
            h.strategy_entry("REVERSAL", false);
        }
        if (i == 2) {
            h.expect_hook_ran(quiet::QuietHook::CoofDeclinedReversal);
            CHECK(h.live_position_size() == units);
            CHECK(h.trade_count() == 0);
        }
        if (i == 5) h.strategy_exit("EXIT", "LONG", kMissing, reissued_stop);
    }, 0.01);
    run(host, bars);
    expect_fill(host, "LONG", 1, kPrice, units, 0.0, units);
    CHECK(std::none_of(host.fills.begin(), host.fills.end(),
        [](const Fill& fill) { return fill.id == "REVERSAL"; }));
    expect_fill(host, "EXIT", 6, reissued_stop, 0.0, units, 0.0);
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const auto& trade = host.get_trade(0);
        CHECK(trade.is_long);
        CHECK(trade.entry_bar_index == 1);
        CHECK(trade.exit_bar_index == 6);
        CHECK(trade.entry_price == kPrice);
        CHECK(trade.exit_price == reissued_stop);
        CHECK(trade.qty == units);
    }
}

// (f) The default-sized entry was created on bar 2; all N limits date to 0.
// The single candidate must receive its DefaultGross receipt at opening 3,
// as well as its ordinary next-open fill. A false quiet admission gate fails
// the probe and the receipt check, even if the native market fill survives.
void previous_bar_admission() {
    auto config = configuration();
    config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    config.default_qty_value = 100.0;
    config.margin_long = config.margin_short = 100.0;
    config.pyramiding = 1;
    const double units = config.initial_capital * config.default_qty_value / 100.0 / kPrice;
    Host host(config, 2, [](Host& h, int i) {
        if (i == 2) h.strategy_entry("NEW", true);
        if (i == 3) h.expect_hook_ran(quiet::QuietHook::OpenMarketAdmission);
    });
    run(host, tape(6));
    expect_fill(host, "NEW", 3, kPrice, units, 0.0, units);
    int reviews = 0;
    for (const auto& event : host.market_admission_journal().events()) {
        const auto* review = std::get_if<admission::ReviewEvent>(&event);
        if (!review || review->receipt.checkpoint != admission::Checkpoint::DefaultGross)
            continue;
        for (const auto& row : review->reviewed) {
            if (row.id != "NEW") continue;
            ++reviews;
            CHECK(review->receipt.bar == 3);
            CHECK(row.bar == 2);
            CHECK(row.buy);
        }
    }
    CHECK(reviews == 1);
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        if (std::strcmp(argv[1], "buy_stop") == 0) buy_stop();
        else if (std::strcmp(argv[1], "marketable_stops") == 0) marketable_stops();
        else if (std::strcmp(argv[1], "marketable_limit") == 0) marketable_limit();
        else if (std::strcmp(argv[1], "dual_stop") == 0) dual_stop();
        else if (std::strcmp(argv[1], "trail_capture") == 0) trail_capture();
        else if (std::strcmp(argv[1], "declined_reversal") == 0) declined_reversal();
        else if (std::strcmp(argv[1], "previous_bar_admission") == 0) previous_bar_admission();
        else return 2;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: %s\n", argv[1], error.what());
        return 1;
    }
    std::printf("test_adapter_bar_open_candidates %s: %d checks, %d failures\n",
                argv[1], checks, failures);
    return failures == 0 ? 0 : 1;
}
