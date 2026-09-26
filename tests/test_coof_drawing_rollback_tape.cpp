// K-DRAWSNAP: TradingView's calc_on_order_fills drawing-rollback tape.
//
// TradingView rolls a fill recalculation's drawing edits back with the script's
// `var` variables: in the tape `coof-drawing-rollback`
// (tests/fixtures/coof_drawing_rollback/, one `lab tv` export) a `var line`
// whose x2 is bumped on every execution reads x2 == n, the rolled-back `var`
// counter, in every order comment, while the `varip` counter m (never rolled
// back) runs ahead of n: recalculations ran and their drawing edits did not
// persist. The engine's generated-state checkpoint must therefore hold the
// drawing arenas, and K-DRAWSNAP made that checkpoint cheap
// (include/pineforge/drawing.hpp); this row replays the tape's window through
// the Pine host to show it stayed exact.
//
// The host is the probe lowered by hand the way the code generator (codegen
// cg/popfix 7a39cb3) lowers it: the same members, the same by-value
// `_PFScriptState` checkpoint of every script variable and all four drawing
// arenas, and the same body. `array.size(label.all)` is lowered to the label
// arena's live count; that generator spells `label.all` as `0`.
#include <pineforge/bar.hpp>
#include <pineforge/drawing.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifndef PINEFORGE_K_DRAWSNAP_FIXTURE_DIR
#error "PINEFORGE_K_DRAWSNAP_FIXTURE_DIR must name tests/fixtures/coof_drawing_rollback"
#endif

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/coof_drawing_rollback/bars.inc"

constexpr std::int64_t kMinute = 60'000;

// strategy("PF coof drawing rollback", overlay = true, calc_on_order_fills = true)
class RollbackTapeHost final : public source::PineStrategyHost {
public:
    Line l;
    int n = 0;
    int m = 0;
    Label lab;
    std::string tag;
    DrawingArena<LineRec> _pf_lines_{50};
    DrawingArena<BoxRec> _pf_boxes_{50};
    DrawingArena<LabelRec> _pf_labels_{50};
    DrawingArena<LinefillRec> _pf_linefills_{50};
    bool _var_initialized = false;
    bool _pf_var_init_l = false;

    struct State {
        Line l;
        int n;
        Label lab;
        std::string tag;
        DrawingArena<LineRec> lines;
        DrawingArena<BoxRec> boxes;
        DrawingArena<LabelRec> labels;
        DrawingArena<LinefillRec> linefills;
        bool var_initialized;
        bool var_init_l;
    };
    std::optional<State> checkpoint_;
    int recalculated_executions = 0;

    RollbackTapeHost() {
        // TradingView's v6 strategy() defaults, which the tape ran with.
        source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = true;
        cfg.initial_capital = 100000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        cfg.default_qty_value = 100.0;
        configure_pine_strategy(cfg);
    }

    void snapshot_script_state() override {
        checkpoint_.emplace(State{l, n, lab, tag, _pf_lines_, _pf_boxes_, _pf_labels_,
                                  _pf_linefills_, _var_initialized, _pf_var_init_l});
    }

    void restore_script_state() override {
        if (!checkpoint_) return;
        l = checkpoint_->l;
        n = checkpoint_->n;
        lab = checkpoint_->lab;
        tag = checkpoint_->tag;
        _pf_lines_ = checkpoint_->lines;
        _pf_boxes_ = checkpoint_->boxes;
        _pf_labels_ = checkpoint_->labels;
        _pf_linefills_ = checkpoint_->linefills;
        _var_initialized = checkpoint_->var_initialized;
        _pf_var_init_l = checkpoint_->var_init_l;
    }

    void commit_script_state() override { snapshot_script_state(); }

    void on_source_bar(const Bar&) override {
        if (!_var_initialized) _var_initialized = true;
        if (!_pf_var_init_l) {
            l = pf_line_new(_pf_lines_, 0, 0.0, 0, 0.0, XLoc::bar_index, false, false);
            _pf_var_init_l = true;
        }
        pf_line_set_x2(_pf_lines_, l, pf_line_get_x2(_pf_lines_, l) + 1);
        n += 1;
        m += 1;
        if (m > n) ++recalculated_executions;
        pf_label_delete(_pf_labels_, lab);
        lab = pf_label_new(_pf_labels_, pine_bar_index(), current_bar_.high, std::string("k"),
                           XLoc::bar_index, YLoc::price);
        tag = "x2=" + std::to_string(pf_line_get_x2(_pf_lines_, l)) + " n=" + std::to_string(n)
            + " m=" + std::to_string(m)
            + " labels=" + std::to_string(_pf_labels_.order().size());
        const int minute = pine_minute(current_bar_.timestamp, "UTC");
        if (minute == 0 && signed_position_size() == 0.0)
            strategy_entry("L", true, na<double>(), na<double>(), na<double>(), tag);
        if (minute == 30)
            strategy_close("L", tag, na<double>(), na<double>(), false, 85899345939ULL);
    }
};

// One order comment of the tape, "x2=5 n=5 m=5 labels=1".
struct Comment {
    long long x2 = -1, n = -1, m = -1, labels = -1;
};

bool parse_comment(const std::string& text, Comment& out) {
    return std::sscanf(text.c_str(), "x2=%lld n=%lld m=%lld labels=%lld",
                       &out.x2, &out.n, &out.m, &out.labels) == 4;
}

// One trade of either side: entry and exit times (UTC ms), prices, comments.
struct TapeTrade {
    std::int64_t entry_ms = 0, exit_ms = 0;
    double entry_price = 0.0, exit_price = 0.0, qty = 0.0;
    std::string entry_comment, exit_comment;
};

std::int64_t days_from_civil(int y, unsigned mo, unsigned d) {
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// The tape dates each fill at its bar's open, in UTC+8.
std::int64_t tape_ms(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

std::vector<TapeTrade> read_tape() {
    std::ifstream in(std::string(PINEFORGE_K_DRAWSNAP_FIXTURE_DIR)
                     + "/coof-drawing-rollback/tv_trades.csv");
    std::vector<TapeTrade> trades;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 6) continue;
        const std::size_t number = static_cast<std::size_t>(std::stoul(cell[0]));
        if (trades.size() < number) trades.resize(number);
        TapeTrade& t = trades[number - 1];
        if (cell[1].rfind("Entry", 0) == 0) {
            t.entry_ms = tape_ms(cell[2]);
            t.entry_comment = cell[3];
            t.entry_price = std::stod(cell[4]);
            t.qty = std::stod(cell[5]);
        } else {
            t.exit_ms = tape_ms(cell[2]);
            t.exit_comment = cell[3];
            t.exit_price = std::stod(cell[4]);
        }
    }
    return trades;
}

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (const FeedBar& row : kEth15m) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    return bars;
}

void test_tape_comments_roll_drawings_back() {
    std::printf("test_tape_comments_roll_drawings_back\n");
    const std::vector<TapeTrade> tape = read_tape();
    CHECK(tape.size() == 41u);
    int tape_recalculated = 0;
    for (const TapeTrade& t : tape) {
        for (const std::string* text : {&t.entry_comment, &t.exit_comment}) {
            Comment c;
            CHECK(parse_comment(*text, c));
            CHECK(c.x2 == c.n);        // TradingView rolled the line back with n
            CHECK(c.labels == 1);      // one live label: delete + new per execution
            if (c.m > c.n) ++tape_recalculated;
        }
    }
    CHECK(tape_recalculated == 81);    // recalculations ran before 81 of 82 comments
}

void test_engine_replays_tape() {
    std::printf("test_engine_replays_tape\n");
    const std::vector<Bar> bars = feed();
    RollbackTapeHost host;
    host.set_syminfo_mintick(0.01);
    host.set_syminfo_metadata("qty_step", 0.0001);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("    last_error: %s\n", host.last_error().c_str());

    std::vector<TapeTrade> engine;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        engine.push_back({t.entry_time, t.exit_time, t.entry_price, t.exit_price, t.qty,
                          t.entry_comment, t.exit_comment});
    }
    const std::vector<TapeTrade> tape = read_tape();
    std::printf("    engine %zu trades, tape %zu; %d executions ran after a recalculation\n",
                engine.size(), tape.size(), host.recalculated_executions);
    CHECK(engine.size() == tape.size());
    int same = 0;
    for (std::size_t i = 0; i < engine.size() && i < tape.size(); ++i) {
        const TapeTrade& e = engine[i];
        const TapeTrade& t = tape[i];
        // The tape prints prices to the tick and quantities to four decimals.
        const bool match = e.entry_ms == t.entry_ms && e.exit_ms == t.exit_ms
            && std::fabs(e.entry_price - t.entry_price) < 0.005
            && std::fabs(e.exit_price - t.exit_price) < 0.005
            && std::fabs(e.qty - t.qty) < 0.00005
            && e.entry_comment == t.entry_comment && e.exit_comment == t.exit_comment;
        if (match) {
            ++same;
            continue;
        }
        std::printf("    trade %zu: engine %lld %.2f x %.4f [%s] -> %lld %.2f [%s]\n"
                    "              tape   %lld %.2f x %.4f [%s] -> %lld %.2f [%s]\n",
                    i + 1, (long long)e.entry_ms, e.entry_price, e.qty, e.entry_comment.c_str(),
                    (long long)e.exit_ms, e.exit_price, e.exit_comment.c_str(),
                    (long long)t.entry_ms, t.entry_price, t.qty, t.entry_comment.c_str(),
                    (long long)t.exit_ms, t.exit_price, t.exit_comment.c_str());
    }
    std::printf("    %d of %zu trades identical to the tape: times, prices, quantities and "
                "both comments\n", same, tape.size());
    CHECK(same == 41);
    // The replay is not vacuous: recalculations ran and were rolled back.
    CHECK(host.recalculated_executions > 0);
    CHECK(host.m > host.n);
}

}  // namespace

int main() {
    test_tape_comments_roll_drawings_back();
    test_engine_replays_tape();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
