// EN-132 S1a. Expectations are pinned from the lane brief, D1 and D2.
// This TU uses only the previous public header closure, so it is intended to
// compile on base; its actionable invalid rows then lack the required code.
// No toolchain, private fixture, market data or admission-helper call is used.
#include <pineforge/bar.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/selected_window.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr std::int64_t kT0 = 1704067200000LL;
constexpr std::int64_t kMinute = 60000LL;
int checks = 0;
int failures = 0;
int rows_run = 0;
std::string row_name;
std::string only_row;

#define CHECK(expr) do {                                                        \
    ++checks;                                                                  \
    if (!(expr)) {                                                             \
        ++failures;                                                            \
        std::printf("FAIL %s line %d: %s\n", row_name.c_str(), __LINE__, #expr); \
    }                                                                          \
} while (false)

enum class Door { Entry, Close, CloseToken, CloseAll, Exit, BracketCancel,
                  Cancel, CancelAll, Order };
enum class Arg { None, Id, Comment, OcaName, FromEntry, ExitId, Limit, Stop,
                 TrailPoints, TrailOffset, TrailPrice, Profit, Loss, Qty, Percent };
enum class Placement { Selected, Retained, Earlier, Ordinary, LegacyDropped };

struct Row {
    const char* label;
    Door door;
    Arg arg;
    const char* field;
    bool text;
};

// order has no comment operand. The two exit numeric exclusions have pinned
// infinity behavior in test_fills_edge{,_l4d}.cpp and are controls below.
const Row admitted[] = {
    {"entry.id", Door::Entry, Arg::Id, "entry.id", true},
    {"entry.comment", Door::Entry, Arg::Comment, "entry.comment", true},
    {"entry.oca_name", Door::Entry, Arg::OcaName, "entry.oca_name", true},
    {"entry.limit_price", Door::Entry, Arg::Limit, "entry.limit_price", false},
    {"entry.stop_price", Door::Entry, Arg::Stop, "entry.stop_price", false},
    {"order.id", Door::Order, Arg::Id, "order.id", true},
    {"order.oca_name", Door::Order, Arg::OcaName, "order.oca_name", true},
    {"order.limit_price", Door::Order, Arg::Limit, "order.limit_price", false},
    {"order.stop_price", Door::Order, Arg::Stop, "order.stop_price", false},
    {"exit.id", Door::Exit, Arg::Id, "exit.id", true},
    {"exit.from_entry", Door::Exit, Arg::FromEntry, "exit.from_entry", true},
    {"exit.comment", Door::Exit, Arg::Comment, "exit.comment", true},
    {"exit.oca_name", Door::Exit, Arg::OcaName, "exit.oca_name", true},
    {"exit.limit_price", Door::Exit, Arg::Limit, "exit.limit_price", false},
    {"exit.trail_offset", Door::Exit, Arg::TrailOffset, "exit.trail_offset", false},
    {"exit.trail_price", Door::Exit, Arg::TrailPrice, "exit.trail_price", false},
    {"exit.profit_ticks", Door::Exit, Arg::Profit, "exit.profit_ticks", false},
    {"exit.loss_ticks", Door::Exit, Arg::Loss, "exit.loss_ticks", false},
    {"close.id", Door::Close, Arg::Id, "close.id", true},
    {"close.comment", Door::Close, Arg::Comment, "close.comment", true},
    {"close_token.id", Door::CloseToken, Arg::Id, "close.id", true},
    {"close_token.comment", Door::CloseToken, Arg::Comment, "close.comment", true},
    {"exit_cancel_bracket.exit_id", Door::BracketCancel, Arg::ExitId,
        "exit_cancel_bracket.exit_id", true},
    {"exit_cancel_bracket.from_entry", Door::BracketCancel, Arg::FromEntry,
        "exit_cancel_bracket.from_entry", true},
    {"exit_cancel_bracket.comment", Door::BracketCancel, Arg::Comment,
        "exit_cancel_bracket.comment", true},
    {"cancel.id", Door::Cancel, Arg::Id, "cancel.id", true},
};

const Placement placements[] = {Placement::Selected, Placement::Retained,
    Placement::Earlier, Placement::Ordinary, Placement::LegacyDropped};

const char* placement_name(Placement placement) {
    switch (placement) {
        case Placement::Selected: return "selected";
        case Placement::Retained: return "retained";
        case Placement::Earlier: return "earlier";
        case Placement::Ordinary: return "ordinary";
        case Placement::LegacyDropped: return "legacy_dropped";
    }
    return "unknown";
}

bool is_selected(Placement placement) {
    return placement == Placement::Selected || placement == Placement::Retained
        || placement == Placement::Earlier;
}

bool has_legacy_gate(Door door) {
    return door != Door::Close && door != Door::Cancel && door != Door::CancelAll;
}

int emission_bar(Placement placement) {
    if (placement == Placement::Retained) return 1;
    if (placement == Placement::Earlier || placement == Placement::LegacyDropped) return 0;
    return 2;
}

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (int i = 0; i < 8; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1000.0, kT0 + i * kMinute});
    return bars;
}

struct Raw {
    std::string id = "L", comment = "ok", oca_name = "group", from_entry = "L";
    std::string exit_id = "X";
    double limit = kNaN, stop = kNaN, trail_points = kNaN, trail_offset = kNaN;
    double trail_price = kNaN, profit = kNaN, loss = kNaN, qty = kNaN, percent = kNaN;
};

void put_text(Raw& raw, Arg arg, const std::string& text) {
    switch (arg) {
        case Arg::Id: raw.id = text; return;
        case Arg::Comment: raw.comment = text; return;
        case Arg::OcaName: raw.oca_name = text; return;
        case Arg::FromEntry: raw.from_entry = text; return;
        case Arg::ExitId: raw.exit_id = text; return;
        default: CHECK(false); return;
    }
}

void put_number(Raw& raw, Arg arg, double number) {
    switch (arg) {
        case Arg::Limit: raw.limit = number; return;
        case Arg::Stop: raw.stop = number; return;
        case Arg::TrailPoints: raw.trail_points = number; return;
        case Arg::TrailOffset: raw.trail_offset = number; return;
        case Arg::TrailPrice: raw.trail_price = number; return;
        case Arg::Profit: raw.profit = number; return;
        case Arg::Loss: raw.loss = number; return;
        case Arg::Qty: raw.qty = number; return;
        case Arg::Percent: raw.percent = number; return;
        default: CHECK(false); return;
    }
}

class DoorHost final : public source::PineStrategyHost {
public:
    // The host exposes this accessor to derived classes only; the rows read it
    // from outside the class.
    using source::PineStrategyHost::signed_position_size;
    DoorHost(Door door, Placement placement, const Raw& raw, int oca_type = 0)
        : door_(door), placement_(placement), raw_(raw), oca_type_(oca_type) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 2.0;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
        set_syminfo_metadata("qty_step", 0.01);
        if (placement == Placement::LegacyDropped)
            set_trade_start_time(kT0 + 10 * kMinute);
    }

    int attempted = 0;
    int returned = 0;
    int last_source_index = -1;

    void on_source_bar(const Bar&) override {
        last_source_index = bar_index_;
        if (bar_index_ != emission_bar(placement_)) return;
        ++attempted;
        switch (door_) {
            case Door::Entry:
                strategy_entry(raw_.id, true, raw_.limit, raw_.stop, raw_.qty,
                               raw_.comment, raw_.oca_name, oca_type_, -1); break;
            case Door::Close:
                strategy_close(raw_.id, raw_.comment, raw_.qty, raw_.percent, false); break;
            case Door::CloseToken:
                strategy_close(raw_.id, raw_.comment, raw_.qty, raw_.percent, false, 17); break;
            case Door::CloseAll: strategy_close_all(); break;
            case Door::Exit:
                strategy_exit(raw_.id, raw_.from_entry, raw_.limit, raw_.stop,
                              raw_.trail_points, raw_.trail_offset, raw_.trail_price,
                              raw_.percent, raw_.comment, raw_.qty, raw_.oca_name,
                              raw_.profit, raw_.loss); break;
            case Door::BracketCancel:
                strategy_exit_cancel_bracket(raw_.exit_id, raw_.from_entry, raw_.comment); break;
            case Door::Cancel: strategy_cancel(raw_.id); break;
            case Door::CancelAll: strategy_cancel_all(); break;
            case Door::Order:
                strategy_order(raw_.id, true, raw_.qty, raw_.limit, raw_.stop,
                               raw_.oca_name, oca_type_); break;
        }
        ++returned;
    }

private:
    Door door_;
    Placement placement_;
    Raw raw_;
    int oca_type_;
};

bool begin_row(const Row& row, Placement placement, const char* variant) {
    row_name = std::string(row.label) + "/" + placement_name(placement) + "/" + variant;
    if (!only_row.empty() && only_row != row_name) return false;
    ++rows_run;
    std::printf("ROW %s\n", row_name.c_str());
    return true;
}

void run_and_check(DoorHost& host, Placement placement, const char* field,
                   const char* reason, bool invalid, bool default_fill = false,
                   bool reserved_id = false) {
    const auto bars = feed();
    if (is_selected(placement)) {
        pf_selected_window_config_v1 window{};
        window.struct_size = static_cast<std::uint32_t>(sizeof window);
        window.version = 1;
        window.start_ms = kT0 + 2 * kMinute;
        window.end_ms = kT0 + 8 * kMinute;
        CHECK(strategy_set_selected_window_v1(static_cast<BacktestEngine*>(&host), &window) == 0);
    }
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false);
    CHECK(host.attempted == 1);
    if (invalid) {
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(!host.last_error().empty());
        CHECK(std::string(run_failure_code_of(host))
              == (reserved_id ? "pine_invalid_argument" : "strategy_value_invalid"));
        const std::string name(field);
        const auto dot = name.find('.');
        const std::string expected = reserved_id
            ? std::string("{\"argument\":\"") + name.substr(dot + 1)
                + "\",\"function\":\"strategy." + name.substr(0, dot)
                + "\",\"rule\":\"unsupported_combination\"}"
            : std::string("{\"field\":\"") + field + "\",\"reason\":\"" + reason + "\"}";
        CHECK(std::string(run_failure_args_of(host)) == expected);
        CHECK(run_failure_code_class(run_failure_value_of(host).code) == RunFailureClass::strategy);
        CHECK(host.returned == (placement == Placement::Retained ? 1 : 0));
        CHECK(host.last_source_index == emission_bar(placement));
        CHECK(host.signed_position_size() == 0.0);
        CHECK(host.trade_count() == 0);
    } else {
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(host.last_error().empty());
        CHECK(run_failure_value_of(host).code == RunFailureCode::none);
        CHECK(host.returned == 1);
        CHECK(host.last_source_index == 7);
        if (is_selected(placement)) {
            pf_selected_window_counts_v1 counts{};
            counts.struct_size = static_cast<std::uint32_t>(sizeof counts);
            counts.version = 1;
            CHECK(strategy_selected_window_counts_v1(static_cast<BacktestEngine*>(&host), &counts) == 0);
            CHECK(counts.preroll_script_bars == 2);
            CHECK(counts.window_script_bars == 6);
        }
        if (default_fill) {
            const bool discarded = placement == Placement::Earlier
                || placement == Placement::LegacyDropped;
            CHECK(host.signed_position_size() == (discarded ? 0.0 : 2.0));
        }
    }
}

struct TextCase { const char* name; std::string bytes; };

void argument_rows() {
    const TextCase bad[] = {
        {"overlong2", "\xc0\xaf"}, {"overlong3", "\xe0\x80\xaf"},
        {"overlong4", "\xf0\x80\x80\xaf"}, {"surrogate", "\xed\xa0\x80"},
        {"truncated2", "\xc2"}, {"truncated3", "\xe2\x82"},
        {"truncated4", "\xf0\x90\x80"}, {"above_unicode_max", "\xf4\x90\x80\x80"},
        {"above_f4", "\xf5\x80\x80\x80"}, {"lone_continuation", "\x80"},
        {"bad_continuation", "\xc2" "A"}, {"ff_byte", "\xff"},
        {"invalid_after_nul", std::string("ok\0", 3) + "\x80"},
    };
    const TextCase good[] = {
        {"empty", ""}, {"ascii", "valid"}, {"nul", std::string("ok\0end", 6)},
        {"utf8_boundaries", "\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf"
                            "\xee\x80\x80\xef\xbf\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf"},
    };
    for (const Row& row : admitted) {
        for (Placement placement : placements) {
            const bool must_fail = placement != Placement::Earlier
                && (placement != Placement::LegacyDropped || !has_legacy_gate(row.door));
            if (row.text) {
                for (const auto& value : bad) {
                    if (!begin_row(row, placement, value.name)) continue;
                    Raw raw;
                    put_text(raw, row.arg, value.bytes);
                    DoorHost host(row.door, placement, raw);
                    run_and_check(host, placement, row.field, "invalid_unicode", must_fail);
                }
                for (const auto& value : good) {
                    if (!begin_row(row, placement, value.name)) continue;
                    Raw raw;
                    put_text(raw, row.arg, value.bytes);
                    DoorHost host(row.door, placement, raw);
                    // NUL remains text in a comment, but cannot be
                    // an ID/entry selector/OCA name used in composite keys.
                    const bool reserved = value.bytes.find('\0') != std::string::npos
                        && row.arg != Arg::Comment;
                    run_and_check(host, placement, row.field, "",
                                  reserved && must_fail, false, reserved);
                }
            } else {
                for (const bool negative : {false, true}) {
                    if (!begin_row(row, placement, negative ? "-inf" : "+inf")) continue;
                    Raw raw;
                    put_number(raw, row.arg, negative ? -kInf : kInf);
                    DoorHost host(row.door, placement, raw);
                    run_and_check(host, placement, row.field, "nonfinite", must_fail);
                }
                if (begin_row(row, placement, "na")) {
                    Raw raw;
                    put_number(raw, row.arg, kNaN);
                    DoorHost host(row.door, placement, raw);
                    run_and_check(host, placement, row.field, "", false);
                }
            }
        }
    }
}

void unchanged_rows() {
    const Row rows[] = {
        {"entry.qty_default", Door::Entry, Arg::Qty, "entry.qty", false},
        {"order.qty_cancel_default", Door::Order, Arg::Qty, "order.qty", false},
        {"exit.qty", Door::Exit, Arg::Qty, "exit.qty", false},
        {"exit.qty_percent", Door::Exit, Arg::Percent, "exit.qty_percent", false},
        {"close.qty", Door::Close, Arg::Qty, "close.qty", false},
        {"close.qty_percent", Door::Close, Arg::Percent, "close.qty_percent", false},
        {"close_token.qty", Door::CloseToken, Arg::Qty, "close.qty", false},
        {"close_token.qty_percent", Door::CloseToken, Arg::Percent, "close.qty_percent", false},
        {"exit.stop_price_pinned", Door::Exit, Arg::Stop, "exit.stop_price", false},
        {"exit.trail_points_pinned", Door::Exit, Arg::TrailPoints, "exit.trail_points", false},
        {"order.qty_ordinary", Door::Order, Arg::Qty, "order.qty", false},
    };
    for (const Row& row : rows) {
        for (Placement placement : placements) {
            for (int shape = 0; shape < 3; ++shape) {
                const char* variant = shape == 0 ? "na" : shape == 1 ? "+inf" : "-inf";
                if (!begin_row(row, placement, variant)) continue;
                Raw raw;
                put_number(raw, row.arg, shape == 0 ? kNaN : shape == 1 ? kInf : -kInf);
                const bool cancel_order = row.door == Door::Order
                    && std::string(row.label) == "order.qty_cancel_default";
                DoorHost host(row.door, placement, raw, cancel_order ? 1 : 0);
                const bool default_fill = row.door == Door::Entry || cancel_order
                    || (row.door == Door::Order && shape == 0);
                run_and_check(host, placement, row.field, "", false, default_fill);
            }
        }
    }
    for (Door door : {Door::CloseAll, Door::CancelAll}) {
        const Row row{door == Door::CloseAll ? "close_all" : "cancel_all",
                      door, Arg::None, "", false};
        for (Placement placement : placements) {
            if (!begin_row(row, placement, "no_values")) continue;
            DoorHost host(door, placement, Raw{});
            run_and_check(host, placement, "", "", false);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--only") only_row = argv[2];
    else if (argc != 1) {
        std::printf("usage: test_value_admission_doors [--only <exact row label>]\n");
        return 2;
    }
    argument_rows();
    unchanged_rows();
    CHECK(rows_run > 0);
    // Pin the full authored population as well as each individual row.
    if (only_row.empty()) CHECK(rows_run == 1755);
    else CHECK(rows_run == 1);
    std::printf("%d rows, %d checks, %d failures\n", rows_run, checks, failures);
    return failures == 0 ? 0 : 1;
}
