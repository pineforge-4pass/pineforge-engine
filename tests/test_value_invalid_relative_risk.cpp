// EN-132 D6 rows 17/19 through the public native C ABI. Written before the
// producers. Main skips non-finite marked equity and derived risk limits;
// the three G witnesses below must therefore fail their exact code pins.
// Row 17 is already engine_invariant and is preserved by the brief's explicit
// "plain or unclassified" condition. Source row 15/16/18 witnesses live in
// test_value_invalid_relative_risk_source.cpp so this TU stays kernel-only.
#include <pineforge/native_c_api.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {
int checks = 0;
int failures = 0;
const char* row = "";

void check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s] line %d: %s\n", row, line, expression);
    }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

std::string text(const char* value) { return value ? value : "<null>"; }
bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

enum class Shape { Flat, RoundTrip, MarkOnly, Anchored };
enum class Risk { Absent, Drawdown, Intraday, Fills };

struct Script {
    pf_strategy_t host = nullptr;
    Shape shape = Shape::Flat;
    int calls = 0;
    int commands = 0;
    int applied = 0;
    double units = 4.0;
    double offset = 0.0;
    std::uint64_t parent = 0;
};

pf_native_request_v1 request() {
    pf_native_request_v1 out{};
    out.struct_size = sizeof(out);
    out.version = PF_NATIVE_API_VERSION;
    return out;
}

int on_bar(void* user, const pf_bar_t*, const pf_native_decision_v1*) {
    auto& script = *static_cast<Script*>(user);
    const int index = script.calls++;
    if (script.shape == Shape::Flat) return 0;
    if (index == 0) {
        auto entry = request();
        entry.intent = PF_NATIVE_INTENT_TRANSACT;
        entry.intent_value = script.units;
        entry.label = "E";
        CHECK(strategy_native_submit_v1(script.host, &entry, &script.parent, nullptr)
              == PF_NATIVE_OK);
        ++script.commands;
        CHECK(script.parent != 0);
        if (script.shape == Shape::Anchored) {
            auto child = request();
            child.intent = PF_NATIVE_INTENT_FLATTEN;
            child.label = "X";
            child.trigger = PF_NATIVE_TRIGGER_LIMIT;
            child.p1 = 0.0;
            child.anchor = PF_NATIVE_ANCHOR_FROM_OWNER_FILL;
            child.anchor_offset = script.offset;
            child.anchor_rounding = PF_NATIVE_ANCHOR_ROUNDING_RAW;
            child.owner = PF_NATIVE_OWNER_WAIT_FOR_APPLIED;
            child.owner_n = 1;
            child.owner_incarnations = &script.parent;
            child.visibility = PF_NATIVE_ARM_VISIBILITY_PENDING_UNTIL_ARMED;
            std::uint64_t handle = 0;
            CHECK(strategy_native_submit_v1(script.host, &child, &handle, nullptr)
                  == PF_NATIVE_OK);
            CHECK(handle != 0);
            ++script.commands;
        }
    } else if (index == 4 && script.shape == Shape::RoundTrip) {
        auto close = request();
        close.intent = PF_NATIVE_INTENT_FLATTEN;
        close.label = "C";
        CHECK(strategy_native_submit_v1(script.host, &close, nullptr, nullptr) == PF_NATIVE_OK);
        ++script.commands;
    }
    return 0;
}

int on_applied(void* user, const pf_native_applied_v1*, const pf_native_decision_v1*) {
    ++static_cast<Script*>(user)->applied;
    return 0;
}

int default_anchor(void*, const pf_native_anchored_level_view_v1*, double*) {
    return PF_NATIVE_ANSWER_DEFAULT;
}

struct Run {
    Script script;
    pf_report_t report{};
    int rc = PF_NATIVE_E_RUN_FAILED;
    std::string code;
    std::string args;
    Run(Shape shape, Risk risk, double value, bool percent, double capital,
        const std::vector<double>& prices, double units = 4.0,
        double offset = 0.0, bool restate = false) {
        script.shape = shape;
        script.units = units;
        script.offset = offset;
        pf_native_callbacks_v1 callbacks{};
        callbacks.struct_size = sizeof(callbacks);
        callbacks.version = PF_NATIVE_API_VERSION;
        callbacks.user = &script;
        callbacks.on_bar = on_bar;
        callbacks.on_applied = on_applied;
        if (restate) callbacks.on_anchored_level = default_anchor;
        script.host = strategy_native_host_create_v1(&callbacks);
        CHECK(script.host != nullptr);
        if (!script.host) return;
        pf_native_run_spec_v1 spec{};
        spec.struct_size = sizeof(spec);
        spec.session_key = "en132-g3-native";
        spec.run_number = 1;
        spec.input_tf = "1";
        spec.script_tf = "1";
        spec.ticker = "G3";
        spec.tickerid = "TEST:G3";
        spec.type = "crypto";
        spec.currency = "USD";
        spec.basecurrency = "G3";
        spec.description = "";
        spec.volumetype = "";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.chart_timezone = "";
        spec.initial_capital = capital;
        spec.point_value = 1.0;
        spec.account_fx = 1.0;
        spec.price_tick = 1.0;
        spec.fee_kind = PF_NATIVE_FEE_CASH_PER_EXECUTION;
        spec.fee_value = 0.0;
        spec.allowed_open_directions = 3;
        pf_native_run_spec_ext_v1 ext{};
        ext.struct_size = sizeof(ext);
        ext.version = PF_NATIVE_API_VERSION;
        if (risk != Risk::Absent) {
            ext.present_mask = PF_NATIVE_SPEC_EXT_RISK;
            ext.risk_has_max_drawdown = risk == Risk::Drawdown;
            ext.risk_max_drawdown = value;
            ext.risk_max_drawdown_percent = percent;
            ext.risk_has_max_intraday_loss = risk == Risk::Intraday;
            ext.risk_max_intraday_loss = value;
            ext.risk_max_intraday_loss_percent = percent;
            ext.risk_has_max_fills_per_day = risk == Risk::Fills;
            ext.risk_max_fills_per_day = 100;
        }
        CHECK(strategy_configure_native_ext_v1(script.host, &spec, &ext) == PF_NATIVE_OK);
        std::vector<pf_bar_t> bars;
        for (std::size_t i = 0; i < prices.size(); ++i) {
            const double p = prices[i];
            CHECK(std::isfinite(p) && p > 0.0);
            bars.push_back({p, p, p, p, 1.0, 1700000040000LL
                + static_cast<std::int64_t>(i) * 60000});
        }
        rc = strategy_native_run_v1(script.host, bars.data(), static_cast<int>(bars.size()),
                                     &report);
        code = text(strategy_get_last_error_code(script.host));
        args = text(strategy_get_last_error_args(script.host));
    }
    ~Run() {
        strategy_native_report_free_v1(&report);
        strategy_native_host_free(script.host);
    }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;
};

void expect_g(const Run& run, const char* field, const char* reason) {
    CHECK(run.rc == PF_NATIVE_E_RUN_FAILED);
    CHECK(run.code == "engine_command_value_invalid");
    const std::string expected = std::string("{\"field\":\"") + field
        + "\",\"reason\":\"" + reason + "\",\"stage\":\"risk\"}";
    if (run.code != "engine_command_value_invalid" || run.args != expected) {
        std::fprintf(stderr, "[%s] got code=%s args=%s; expected engine_command_value_invalid %s\n",
                     row, run.code.c_str(), run.args.c_str(), expected.c_str());
    }
    CHECK(run.args == expected);
    CHECK(run.report.total_trades == 0);
    CHECK(run.report.trades_len == 0);
}

void compare_money(const Run& a, const Run& b) {
    CHECK(a.rc == PF_NATIVE_OK && b.rc == PF_NATIVE_OK);
    CHECK(a.code.empty() && b.code.empty());
    CHECK(a.args.empty() && b.args.empty());
    CHECK(a.report.trades_len == b.report.trades_len);
    CHECK(same_bits(a.report.net_profit, b.report.net_profit));
    if (a.report.trades_len == b.report.trades_len) {
        for (int i = 0; i < a.report.trades_len; ++i) {
            const auto& x = a.report.trades[i];
            const auto& y = b.report.trades[i];
            CHECK(x.entry_time == y.entry_time && x.exit_time == y.exit_time);
            CHECK(same_bits(x.entry_price, y.entry_price));
            CHECK(same_bits(x.exit_price, y.exit_price));
            CHECK(same_bits(x.pnl, y.pnl));
            CHECK(same_bits(x.pnl_pct, y.pnl_pct));
            CHECK(same_bits(x.max_runup, y.max_runup));
            CHECK(same_bits(x.max_drawdown, y.max_drawdown));
            CHECK(same_bits(x.qty, y.qty));
            CHECK(same_bits(x.commission, y.commission));
            CHECK(x.is_long == y.is_long && x.open_at_end == y.open_at_end);
            CHECK(x.entry_bar_index == y.entry_bar_index && x.exit_bar_index == y.exit_bar_index);
        }
    }
    CHECK(a.report.equity_curve_len == b.report.equity_curve_len);
    if (a.report.equity_curve_len == b.report.equity_curve_len) {
        for (std::int64_t i = 0; i < a.report.equity_curve_len; ++i) {
            CHECK(a.report.equity_curve[i].time_ms == b.report.equity_curve[i].time_ms);
            CHECK(same_bits(a.report.equity_curve[i].equity, b.report.equity_curve[i].equity));
        }
    }
}

void row19_threshold_overflow() {
    for (const Risk risk : {Risk::Drawdown, Risk::Intraday}) {
        row = risk == Risk::Drawdown ? "row19_drawdown_threshold" : "row19_intraday_threshold";
        Run run(Shape::Flat, risk, 0x1p1023, true, 10000.0, std::vector<double>(40, 100.0));
        expect_g(run, "risk.threshold", "numeric_overflow");
        CHECK(run.script.commands == 0);
    }
}

void row19_nonfinite_equity_skip() {
    row = "row19_nonfinite_equity_skip";
    std::vector<double> prices(40, 0x1p1022);
    prices[0] = prices[1] = 100.0;
    Run run(Shape::MarkOnly, Risk::Fills, 0.0, false, 10000.0, prices);
    expect_g(run, "risk.equity", "nonfinite");
    CHECK(run.script.commands == 1);
    CHECK(run.script.applied == 1); // The finite opening settled before the bad mark.
}

void row17_already_typed_arming_overflow() {
    for (const bool restate : {false, true}) {
        row = restate ? "row17_prepared_arming_keep" : "row17_direct_arming_keep";
        Run run(Shape::Anchored, Risk::Absent, 0.0, false, 10000.0,
                std::vector<double>(40, 0x1p1023), 1.0, 0x1p1023, restate);
        CHECK(run.script.commands == 2);
        CHECK(run.rc == PF_NATIVE_E_RUN_FAILED);
        CHECK(run.code == "engine_invariant");
        CHECK(run.args == "{}");
        CHECK(run.report.trades_len == 0);
    }
}

void finite_controls() {
    std::vector<double> prices(40, 104.0);
    prices[0] = prices[1] = 100.0;
    for (const Risk risk : {Risk::Drawdown, Risk::Intraday}) {
        row = "row19_finite_threshold_bits";
        Run percent(Shape::RoundTrip, risk, 10.0, true, 10000.0, prices);
        Run absolute(Shape::RoundTrip, risk, 1000.0, false, 10000.0, prices);
        compare_money(percent, absolute);
        CHECK(percent.report.total_trades == 1);
        CHECK(same_bits(percent.report.net_profit, 16.0));
        row = "row19_finite_threshold_near_boundary_bits";
        Run near_percent(Shape::RoundTrip, risk, 1.0, true, 0x1p1023, prices);
        Run near_absolute(Shape::RoundTrip, risk, 0x1p1023 / 100.0, false, 0x1p1023, prices);
        compare_money(near_percent, near_absolute);
    }
    for (const double end_price : {75.0, 74.0}) {
        row = "row19_finite_zero_negative_equity_keep";
        std::vector<double> marks(40, end_price);
        marks[0] = marks[1] = 100.0;
        Run risk(Shape::MarkOnly, Risk::Fills, 0.0, false, 100.0, marks);
        Run absent(Shape::MarkOnly, Risk::Absent, 0.0, false, 100.0, marks);
        compare_money(risk, absent);
        CHECK(risk.script.applied == 1);
        CHECK(risk.report.equity_curve_len > 0);
        if (risk.report.equity_curve_len > 0) {
            const double expected = end_price == 75.0 ? 0.0 : -4.0;
            CHECK(same_bits(risk.report.equity_curve[risk.report.equity_curve_len - 1].equity,
                            expected));
        }
    }
}
} // namespace

int main() {
    row19_threshold_overflow();
    row19_nonfinite_equity_skip();
    row17_already_typed_arming_overflow();
    finite_controls();
    std::printf("value-invalid relative/risk native: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
