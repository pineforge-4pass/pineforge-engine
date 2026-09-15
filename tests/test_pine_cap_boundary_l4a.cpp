#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

#define broker_fill_event_seq_ fixture_applied_receipt_count()

// Literal native characterization of the temporary Pine source boundary.
// These fixtures pin dispatch, ownership, and metadata contracts; they are
// neither a Pine execution nor evidence of TradingView parity.
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using namespace pineforge;
namespace {
int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL %d %s\n", __LINE__, #x); } } while (0)
using compat::pine::CapAttachment;
constexpr int64_t day = 1743379200000LL;
constexpr int64_t step = 900000;
const char* keys[] = {"intraday_cap_skip_noop_market_fills",
                      "intraday_cap_defer_pooc_close",
                      "intraday_cap_count_pooc_full_close_fills"};
const char* cap_comment = "Close Position (Max number of filled orders in one day)";

class Probe : public pineforge::source::PineStrategyHost {
public:
    Probe() { configure_fixture(); } // Exercise the real native default.
    explicit Probe(CapAttachment attachment) : pineforge::source::PineStrategyHost(attachment) {
        configure_fixture();
    }
    void configure_fixture() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1;
        process_orders_on_close_ = true;
        pyramiding_ = 0;
        commission_value_ = 0;
        slippage_ = 0;
    }
    void on_source_bar(const Bar&) override {}
    void limit(int value) { adapter_.cap = value; }
    int limit() const { return adapter_.cap.configuration().limit; }
    bool flag(int index) const {
        const auto& c = adapter_.cap.configuration();
        return index == 0 ? c.skip_noop_market : index == 1 ? c.defer_pooc_close
                                                         : c.count_pooc_full_close;
    }
    CapAttachment attachment() const { return adapter_.cap.attachment(); }
    int slots() const { return adapter_.cap.budget().charged_slots(); }
    bool latched() const { return adapter_.cap.budget().latched(); }
    bool due() const { return position_close_obligation_.pending(); }
    bool cause() const { return adapter_.cap.due_cause().has_value(); }
    uint64_t action() const { return adapter_.cap.next_action(); }
    uint64_t fills() const { return broker_fill_event_seq_; }
    double position() const { return signed_position_size(); }
    double metadata(const char* key) const { return get_syminfo_metadata(key); }
    void reset() { run(nullptr, 0); }
    void clone_cap_policy_from(const Probe& other) { adapter_.cap = other.adapter_.cap; }
};

class GeneratedShapeMetadataOracle final : public pineforge::source::PineStrategyHost {
public:
    explicit GeneratedShapeMetadataOracle(double margin_long = 100.0,
                                          double margin_short = 100.0) {
        source::PineStrategyConfig config;
        config.margin_long = margin_long;
        config.margin_short = margin_short;
        configure_pine_strategy(config);
        // This is the generated-constructor ordering: attach both policy
        // adapters before C metadata is transported through BacktestEngine*.
        attach_pine_execution_adapter();
    }

    void on_source_bar(const Bar&) override {}
    CapAttachment cap_attachment() const { return adapter_.cap.attachment(); }
    bool priority_attached() const { return adapter_.priority.attached(); }
    bool retained_parent_first() const { return adapter_.priority.retained_parent_first(); }
    double margin_long() const { return margin_long_; }
    double margin_short() const { return margin_short_; }
};

// This is the actual runtime export, whose handle dispatch is BacktestEngine*.
// Calling a subclass's convenience method would miss a forwarding regression.
void metadata(Probe& engine, const char* key, double value) {
    strategy_set_syminfo_metadata(static_cast<BacktestEngine*>(&engine), key, value);
}
void configure(Probe& engine, int mask) {
    for (int index = 0; index < 3; ++index)
        metadata(engine, keys[index], (mask & (1 << index)) ? 1.0 : 0.0);
}

void test_generated_shape_metadata_oracle() {
    GeneratedShapeMetadataOracle defaults;
    CHECK(defaults.cap_attachment() == CapAttachment::LegacySource);
    CHECK(defaults.priority_attached());
    CHECK(defaults.retained_parent_first());

    // The real C export receives a BacktestEngine* and must dispatch to the
    // source override, first carrying priority/cap metadata and then applying
    // the default-100 margin fallback.
    strategy_set_syminfo_metadata(static_cast<BacktestEngine*>(&defaults),
                                  "flat_retained_child_fresh_parent_order", 0.0);
    CHECK(!defaults.retained_parent_first());
    strategy_set_syminfo_metadata(static_cast<BacktestEngine*>(&defaults),
                                  "intraday_cap_skip_noop_market_fills", 1.0);
    CHECK(defaults.cap_attachment() == CapAttachment::LegacySource);
    strategy_set_syminfo_metadata(static_cast<BacktestEngine*>(&defaults),
                                  "margin_long", 25.0);
    strategy_set_syminfo_metadata(static_cast<BacktestEngine*>(&defaults),
                                  "margin_short", 50.0);
    CHECK(defaults.margin_long() == 25.0);
    CHECK(defaults.margin_short() == 50.0);

    GeneratedShapeMetadataOracle explicit_margins(75.0, 80.0);
    strategy_set_syminfo_metadata(static_cast<BacktestEngine*>(&explicit_margins),
                                  "margin_long", 25.0);
    strategy_set_syminfo_metadata(static_cast<BacktestEngine*>(&explicit_margins),
                                  "margin_short", 50.0);
    CHECK(explicit_margins.margin_long() == 75.0);
    CHECK(explicit_margins.margin_short() == 80.0);
}

void test_real_c_abi_metadata_and_native_attachment() {
    const double values[] = {0.0, -0.0, -1.0,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::denorm_min(), 0.25, 1.0,
        std::numeric_limits<double>::max()};
    for (CapAttachment attachment : {CapAttachment::LegacySource, CapAttachment::None}) {
        for (int mask = 0; mask < 8; ++mask) {
            for (int selected = 0; selected < 3; ++selected) {
                for (double value : values) {
                    Probe engine(attachment);
                    configure(engine, mask);
                    metadata(engine, keys[selected], value);
                    for (int index = 0; index < 3; ++index) {
                        const bool configured = index == selected
                            ? std::isfinite(value) && value > 0
                            : (mask & (1 << index)) != 0;
                        CHECK(engine.flag(index) == configured);
                    }
                    CHECK(engine.attachment() == attachment);
                    CHECK(engine.limit() == 0);
                    CHECK(engine.slots() == 0);
                    CHECK(!engine.due());
                    CHECK(std::isnan(value) ? std::isnan(engine.metadata(keys[selected]))
                                           : engine.metadata(keys[selected]) == value);
                }
            }
        }
    }
    Probe opted_out(CapAttachment::None);
    configure(opted_out, 7);
    opted_out.limit(2); // Existing generated protected assignment must install Pine.
    CHECK(opted_out.attachment() == CapAttachment::LegacySource);
    CHECK(opted_out.limit() == 2);
    // One retained configuration owner preserves pre-statement metadata.
    for (int index = 0; index < 3; ++index) CHECK(opted_out.flag(index));
}

enum class Commands { Noop, FirstFill, CloseThenLaterEntry, CloseWithReverse };
class Script : public Probe {
public:
    Script(Commands commands, bool direction)
        : commands(commands), direction(direction) {}
    Script(Commands commands, bool direction, CapAttachment attachment)
        : Probe(attachment), commands(commands), direction(direction) {}
    Commands commands;
    bool direction;
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("FIRST", direction);
        if (bar_index_ == 1) {
            if (commands == Commands::Noop) strategy_entry("NOOP", direction);
            if (commands == Commands::CloseThenLaterEntry) strategy_close("FIRST");
            if (commands == Commands::CloseWithReverse) {
                strategy_entry("REVERSE", !direction);
                strategy_close("FIRST");
            }
        }
        if (bar_index_ == 2 && commands == Commands::CloseThenLaterEntry)
            strategy_entry("LATER", direction);
    }
};

void test_native_default_and_constructor_frontend_activation() {
    const Bar bars[] = {{100,120,80,110,50,day},
                        {110,125,85,112,50,day+step},
                        {112,130,90,115,50,day+2*step}};
    const double values[] = {0.0, -0.0, -1.0,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::denorm_min(), 0.25, 1.0,
        std::numeric_limits<double>::max()};
    for (bool direction : {false, true}) {
        Script absent(Commands::Noop, direction);
        absent.run(bars, 3);
        for (double value : values) {
            Script native(Commands::Noop, direction);
            Script detached(Commands::Noop, direction, CapAttachment::None);
            for (const char* key : keys) {
                metadata(native, key, value);
                metadata(detached, key, value);
            }
            CHECK(native.attachment() == CapAttachment::None);
            CHECK(native.limit() == 0);
            native.run(bars, 3);
            detached.run(bars, 3);
            CHECK(native.attachment() == CapAttachment::None);
            CHECK(native.position() == absent.position());
            CHECK(native.position() == detached.position());
            CHECK(native.fills() == 1);
            CHECK(native.trade_count() == 0);
            CHECK(native.slots() == 0);
            CHECK(!native.latched() && !native.due() && !native.cause());
        }
    }

    // Mirrors generated-constructor timing using literal native C++ only:
    // explicit attach -> real C metadata setter -> first risk statement.
    // The legacy source control has no attach call; its protected assignment
    // must honor the same metadata rather than silently dropping it.
    class StatementScript : public Script {
    public:
        StatementScript(bool frontend, bool direction)
            : Script(Commands::Noop, direction) {
            if (frontend) enable_pine_intraday_cap();
        }
        void on_source_bar(const Bar& bar) override {
            if (bar_index_ == 0) limit(2);
            Script::on_source_bar(bar);
        }
    };
    for (bool direction : {false, true}) {
        for (int mask = 0; mask < 8; ++mask) {
            StatementScript frontend(true, direction);
            StatementScript legacy(false, direction);
            CHECK(frontend.attachment() == CapAttachment::LegacySource);
            CHECK(legacy.attachment() == CapAttachment::None);
            configure(frontend, mask);
            configure(legacy, mask);
            CHECK(legacy.attachment() == CapAttachment::None);
            CHECK(frontend.limit() == 0 && legacy.limit() == 0);
            frontend.run(bars, 3);
            legacy.run(bars, 3);
            const bool skip_noop = mask & 1;
            CHECK(frontend.fills() == (skip_noop ? 1u : 2u));
            CHECK(frontend.slots() == (skip_noop ? 1 : 2));
            CHECK(frontend.trade_count() == (skip_noop ? 0 : 1));
            CHECK(frontend.position() == (skip_noop ? (direction ? 1 : -1) : 0));
            CHECK(legacy.attachment() == CapAttachment::LegacySource);
            CHECK(legacy.fills() == frontend.fills());
            CHECK(legacy.slots() == frontend.slots());
            CHECK(legacy.position() == frontend.position());
            CHECK(legacy.trade_count() == frontend.trade_count());
            if (!skip_noop) {
                const Trade& a = frontend.get_trade(0);
                const Trade& b = legacy.get_trade(0);
                CHECK(a.entry_price == b.entry_price && a.exit_price == b.exit_price);
                CHECK(a.entry_time == b.entry_time && a.exit_time == b.exit_time);
                CHECK(a.qty == b.qty && a.pnl == b.pnl);
                CHECK(a.entry_id == b.entry_id && a.exit_id == b.exit_id);
                CHECK(a.exit_comment == b.exit_comment);
            }
            // Repeated explicit selection is idempotent and never renews quota.
            const int spent = frontend.slots();
            frontend.enable_pine_intraday_cap();
            CHECK(frontend.slots() == spent);
        }
    }
}

void check_risk_row(const Trade& row, const char* id, double entry, double exit,
                    int64_t entry_time, int64_t exit_time) {
    CHECK(row.entry_id == id);
    CHECK(row.entry_price == entry);
    CHECK(row.exit_price == exit);
    CHECK(row.entry_time == entry_time);
    CHECK(row.exit_time == exit_time);
    CHECK(row.exit_id.empty());
    CHECK(row.exit_comment == cap_comment);
}

// Every mask traverses the real placement, fill, direct-close and next-open
// paths. Expected prices are literals selected before execution. A controls
// the no-op fixture, B the next-open boundary, C the later-entry quota; their
// sibling settings must not silently turn on a bundle.
void test_all_eight_policy_combinations_on_engine_paths() {
    for (int mask = 0; mask < 8; ++mask) {
        for (bool is_long : {false, true}) {
            const bool a = mask & 1, b = mask & 2, c = mask & 4;
            Bar bars[] = {
                {100, 120, 80, is_long ? 110.0 : 90.0, 50, day},
                {100, 120, 80, is_long ? 112.0 : 88.0, 50, day+step},
                {is_long ? 113.0 : 87.0, 125, 75,
                 is_long ? 114.0 : 86.0, 50, day+2*step},
                {is_long ? 115.0 : 85.0, 130, 70, 100, 50, day+3*step},
            };
            Script noop(Commands::Noop, is_long);
            noop.limit(2);
            configure(noop, mask);
            noop.run(bars, 4);
            CHECK(noop.slots() == (a ? 1 : 2));
            CHECK(noop.fills() == (a ? 1 : 2));
            CHECK(noop.trade_count() == (a ? 0 : 1));
            CHECK(noop.latched() == !a);
            CHECK(noop.position() == (a ? (is_long ? 1.0 : -1.0) : 0.0));
            if (!a && noop.trade_count() == 1)
                check_risk_row(noop.get_trade(0), "FIRST", is_long ? 110 : 90,
                    b ? (is_long ? 113 : 87) : (is_long ? 120 : 80),
                    day, b ? day+2*step : day+step);

            // Explicit B-off 120/80 controls preserve the characterized
            // favorable-extreme immediate close, not the signal close price.
            Script first(Commands::FirstFill, is_long);
            first.limit(1);
            configure(first, mask);
            first.run(bars, 4);
            CHECK(first.slots() == 1);
            CHECK(first.fills() == 2);
            CHECK(first.trade_count() == 1);
            CHECK(first.latched());
            CHECK(first.position() == 0);
            CHECK(!first.due());
            CHECK(!first.cause());
            if (first.trade_count() == 1)
                check_risk_row(first.get_trade(0), "FIRST", is_long ? 110 : 90,
                    b ? 100 : (is_long ? 120 : 80), day, b ? day+step : day);

            Script close(Commands::CloseThenLaterEntry, is_long);
            close.limit(2);
            configure(close, mask);
            close.run(bars, 4);
            CHECK(close.slots() == 2);
            CHECK(close.fills() == (c ? 2 : 4));
            CHECK(close.trade_count() == (c ? 1 : 2));
            CHECK(close.position() == 0);
            CHECK(close.latched());
            if (close.trade_count() >= 1) {
                const auto& row = close.get_trade(0);
                CHECK(row.entry_id == "FIRST");
                CHECK(row.exit_id == "__close__FIRST");
                CHECK(row.exit_price == (is_long ? 112 : 88));
                CHECK(row.exit_time == day+step);
            }
            if (!c && close.trade_count() == 2)
                check_risk_row(close.get_trade(1), "LATER", is_long ? 114 : 86,
                    b ? (is_long ? 115 : 85) : (is_long ? 125 : 75),
                    day+2*step, b ? day+3*step : day+2*step);

            Script reverse(Commands::CloseWithReverse, is_long);
            reverse.limit(2);
            configure(reverse, mask);
            reverse.run(bars, 4);
            CHECK(reverse.slots() == 2);
            CHECK(reverse.fills() == 4); // Entry, explicit close, entry, risk close.
            CHECK(reverse.trade_count() == 2);
            CHECK(reverse.position() == 0);
            CHECK(reverse.latched());
            if (reverse.trade_count() == 2) {
                CHECK(reverse.get_trade(0).exit_id == "__close__FIRST");
                CHECK(reverse.get_trade(0).exit_price == (is_long ? 112 : 88));
                check_risk_row(reverse.get_trade(1), "REVERSE", is_long ? 112 : 88,
                    b ? (is_long ? 113 : 87) : (is_long ? 112 : 88),
                    day+step, b ? day+2*step : day+step);
            }
        }
    }
}

void test_native_none_and_assignment_opt_in_use_actual_fill_paths() {
    Bar bars[] = {{100,120,80,110,50,day}, {100,120,80,112,50,day+step},
                  {113,125,75,114,50,day+2*step}};
    Script native(Commands::Noop, true, CapAttachment::None);
    configure(native, 7);
    native.run(bars, 3);
    CHECK(native.attachment() == CapAttachment::None);
    CHECK(native.trade_count() == 0);
    CHECK(native.position() == 1);
    CHECK(native.fills() == 1);
    CHECK(native.slots() == 0);
    CHECK(!native.latched());
    CHECK(!native.due());
    CHECK(!native.cause());
    CHECK(native.action() == 1);

    for (CapAttachment source : {CapAttachment::None, CapAttachment::LegacySource}) {
        Script installed(Commands::Noop, true, source);
        installed.limit(2);
        installed.run(bars, 3);
        CHECK(installed.attachment() == CapAttachment::LegacySource);
        CHECK(installed.trade_count() == 1);
        CHECK(installed.position() == 0);
        CHECK(installed.slots() == 2);
        CHECK(installed.latched());
        if (installed.trade_count() == 1)
            check_risk_row(installed.get_trade(0), "FIRST", 110, 120, day, day+step);
    }
}

// Ordinary dispatch must consume the due risk close before a resting limit
// gets the opening gap. The unlimited control proves that exact resting
// order would otherwise fill; the distinct exit cause identifies who won.
void test_due_next_open_precedes_resting_price_exit() {
    class RestingExit : public Probe {
    public:
        explicit RestingExit(bool due_close) {
            limit(due_close ? 1 : 0);
            set_syminfo_metadata("intraday_cap_defer_pooc_close", 1.0);
        }
        void on_source_bar(const Bar&) override {
            if (bar_index_ != 0) return;
            strategy_entry("FIRST", true);
            strategy_exit("RESTING", "FIRST", 140.0,
                std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN(), 100.0, "resting limit");
        }
    };
    const Bar bars[] = {{100,120,80,110,50,day},
                        {150,160,145,155,50,day+step},
                        {156,165,150,160,50,day+2*step}};
    for (bool due_close : {false, true}) {
        RestingExit engine(due_close);
        engine.run(bars, 3);
        CHECK(engine.trade_count() == 1);
        CHECK(engine.fills() == 2);
        CHECK(engine.position() == 0);
        CHECK(!engine.due());
        CHECK(!engine.cause());
        if (engine.trade_count() == 1) {
            const auto& row = engine.get_trade(0);
            CHECK(row.entry_id == "FIRST");
            CHECK(row.entry_time == day);
            CHECK(row.entry_price == 110);
            CHECK(row.exit_time == day+step);
            CHECK(row.exit_price == 150);
            CHECK(row.exit_id == (due_close ? "" : "RESTING"));
            CHECK(row.exit_comment == (due_close ? cap_comment : "resting limit"));
        }
    }
}

void test_statement_time_limit_changes_preserve_spent_day() {
    class Changing : public Probe {
    public:
        Changing() { pyramiding_ = 10; }
        int limits[7] = {};
        int slots_before[7] = {};
        void on_source_bar(const Bar&) override {
            if (bar_index_ == 0) limit(3);
            const bool execute_conditional_rule = bar_index_ == 2;
            if (execute_conditional_rule) { limit(4); limit(3); }
            if (bar_index_ == 3) limit(0);
            if (bar_index_ == 4) limit(-2);
            if (bar_index_ == 5) limit(5);
            limits[bar_index_] = limit();
            slots_before[bar_index_] = slots();
            strategy_entry("E"+std::to_string(bar_index_), true);
        }
    } engine;
    Bar bars[] = {{100,100,100,100,50,day}, {100,100,100,100,50,day+step},
                  {100,100,100,100,50,day+2*step}, {100,100,100,100,50,day+3*step},
                  {100,100,100,100,50,day+4*step}, {100,100,100,100,50,day+5*step},
                  {100,100,100,100,50,day+86400000}};
    engine.run(bars, 7);
    const int limits[] = {3,3,3,0,-2,5,5};
    const int slots[] = {0,1,2,3,3,3,3}; // Renewal occurs when today's order is placed.
    for (int i = 0; i < 7; ++i) {
        CHECK(engine.limits[i] == limits[i]);
        CHECK(engine.slots_before[i] == slots[i]);
    }
    CHECK(engine.trade_count() == 3);
    CHECK(engine.fills() == 7);
    CHECK(engine.position() == 3);
    CHECK(engine.slots() == 1);
    CHECK(!engine.latched());
    for (int i = 0; i < engine.trade_count(); ++i) {
        CHECK(engine.get_trade(i).entry_id == "E"+std::to_string(i));
        CHECK(engine.get_trade(i).exit_time == day+2*step);
        CHECK(engine.get_trade(i).exit_price == 100);
    }
}

void test_copy_and_engine_reset_preserve_configuration_not_ownership() {
    Script source(Commands::FirstFill, true);
    source.limit(1);
    configure(source, 7);
    const Bar bar{100,120,80,110,50,day};
    source.run(&bar, 1);
    CHECK(source.due());
    CHECK(source.cause());
    CHECK(source.slots() == 1);
    CHECK(source.action() == 2);
    Script copied(Commands::FirstFill, true);
    copied.clone_cap_policy_from(source);
    CHECK(copied.due());
    CHECK(copied.cause());
    CHECK(copied.slots() == 1);
    CHECK(copied.action() == 2);
    copied.reset();
    CHECK(!copied.due());
    CHECK(!copied.cause());
    CHECK(copied.slots() == 0);
    CHECK(copied.action() == 1);
    CHECK(!copied.latched());
    CHECK(copied.limit() == 1);
    CHECK(copied.attachment() == CapAttachment::LegacySource);
    for (int index = 0; index < 3; ++index) CHECK(copied.flag(index));
    CHECK(source.due()); // Resetting a value copy cannot consume the source owner.
    CHECK(source.cause());
    CHECK(source.slots() == 1);
    metadata(copied, keys[1], 0.0);
    copied.limit(4);
    CHECK(source.flag(1));
    CHECK(source.limit() == 1);
    CHECK(!copied.flag(1));
    CHECK(copied.limit() == 4);
    Probe bare(CapAttachment::None);
    bare.reset();
    CHECK(bare.attachment() == CapAttachment::None);
    CHECK(bare.limit() == 0);
}
} // namespace

int main() {
    test_generated_shape_metadata_oracle();
    test_real_c_abi_metadata_and_native_attachment();
    test_native_default_and_constructor_frontend_activation();
    test_all_eight_policy_combinations_on_engine_paths();
    test_native_none_and_assignment_opt_in_use_actual_fill_paths();
    test_due_next_open_precedes_resting_price_exit();
    test_statement_time_limit_changes_preserve_spent_day();
    test_copy_and_engine_reset_preserve_configuration_not_ownership();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
