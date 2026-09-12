#include <pineforge/pineforge.h>

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int checks = 0;
int failures = 0;

#define CHECK(x)                                                             \
    do {                                                                     \
        ++checks;                                                            \
        if (!(x)) {                                                          \
            ++failures;                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);         \
        }                                                                    \
    } while (0)

void near(double a, double b) {
    const bool equal =
        std::abs(a - b) <= 1e-9 * std::max(1.0, std::max(std::abs(a), std::abs(b)));
    if (!equal) std::printf("actual=%.17g expected=%.17g\n", a, b);
    CHECK(equal);
}

using create_fn = pf_strategy_t (*)(const char*);
using free_fn = void (*)(pf_strategy_t);
using run_fn = void (*)(pf_strategy_t, pf_bar_t*, int, pf_report_t*);
using run_full_fn = void (*)(pf_strategy_t, pf_bar_t*, int, const char*, const char*,
                             int, int, pf_magnifier_distribution_t, pf_report_t*);
using report_free_fn = void (*)(pf_report_t*);
using set_kv_fn = void (*)(pf_strategy_t, const char*, const char*);
using set_mag_fn = void (*)(pf_strategy_t, int);
using set_i_fn = void (*)(pf_strategy_t, int);
using set_i64_fn = void (*)(pf_strategy_t, int64_t);
using set_ii_fn = void (*)(pf_strategy_t, int, int);
using set_str_fn = void (*)(pf_strategy_t, const char*);
using set_kv_int_fn = int (*)(pf_strategy_t, const char*, const char*);
using set_d_fn = void (*)(pf_strategy_t, double);
using set_str_d_fn = void (*)(pf_strategy_t, const char*, double);
using set_fx_fn = int (*)(pf_strategy_t, const int64_t*, const double*, int);
using set_aux_fn = int (*)(pf_strategy_t, const pf_bar_t*, int, const char*);
using set_nat_feed_fn = int (*)(pf_strategy_t, const char*, const pf_bar_t*, int);
using abi_fn = int (*)();
using version_fn = pf_version_t (*)();
using contract_fn = int (*)(pf_strategy_t);
using configure_fn = int (*)(pf_strategy_t, const pf_native_run_spec_v1*);
using last_error_fn = const char* (*)(pf_strategy_t);
using last_status_fn = int (*)(pf_strategy_t);
using position_fn = double (*)(pf_strategy_t);
using equity_fn = double (*)(pf_strategy_t);
using bars_fn = int64_t (*)(pf_strategy_t);
using stream_begin_fn = int (*)(pf_strategy_t, const pf_bar_t*, int, const char*, const char*);
using stream_push_fn = int (*)(pf_strategy_t, const pf_bar_t*);
using stream_end_fn = int (*)(pf_strategy_t, int);
using stream_fill_fn = int (*)(pf_strategy_t, pf_report_t*);

struct Abi {
    void* handle = nullptr;
    create_fn create = nullptr;
    free_fn free_strategy = nullptr;
    run_fn run = nullptr;
    run_full_fn run_full = nullptr;
    report_free_fn report_free = nullptr;
    set_kv_fn set_input = nullptr;
    set_kv_fn set_override = nullptr;
    set_mag_fn set_magnifier_vw = nullptr;
    set_i_fn set_trace = nullptr;
    set_i64_fn set_trade_start = nullptr;
    set_ii_fn set_realtime_tail = nullptr;
    set_i_fn set_probe_tail = nullptr;
    set_i_fn set_path_order = nullptr;
    set_i_fn set_broker_hash = nullptr;
    set_str_fn set_chart_tz = nullptr;
    set_str_fn set_sym_tz = nullptr;
    set_str_fn set_sym_session = nullptr;
    set_str_fn set_sym_type = nullptr;
    set_kv_int_fn set_sym_string = nullptr;
    set_d_fn set_sym_mintick = nullptr;
    set_d_fn set_sym_pointvalue = nullptr;
    set_str_d_fn set_sym_metadata = nullptr;
    set_fx_fn set_fx = nullptr;
    set_aux_fn set_aux = nullptr;
    set_nat_feed_fn set_native_feed = nullptr;
    abi_fn abi_version = nullptr;
    version_fn version_get = nullptr;
    contract_fn contract = nullptr;
    configure_fn configure = nullptr;
    last_error_fn last_error = nullptr;
    last_status_fn last_status = nullptr;
    position_fn position_size = nullptr;
    equity_fn current_equity = nullptr;
    bars_fn script_bars = nullptr;
    abi_fn stream_api_version = nullptr;
    stream_begin_fn stream_begin = nullptr;
    stream_push_fn stream_push_bar = nullptr;
    stream_end_fn stream_end = nullptr;
    stream_fill_fn stream_fill_report = nullptr;
    abi_fn example_abi = nullptr;
};

void* require_sym(void* handle, const char* name) {
    void* sym = dlsym(handle, name);
    if (!sym) {
        std::printf("FAIL missing symbol %s: %s\n", name, dlerror());
        ++failures;
    }
    ++checks;
    return sym;
}

template <typename T>
T load(void* handle, const char* name) {
    return reinterpret_cast<T>(require_sym(handle, name));
}

Abi load_library(const char* path) {
    Abi abi;
    abi.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    CHECK(abi.handle != nullptr);
    if (!abi.handle) {
        std::printf("dlopen %s: %s\n", path, dlerror());
        return abi;
    }
    std::printf("library=%s\n", path);
    abi.create = load<create_fn>(abi.handle, "strategy_create");
    abi.free_strategy = load<free_fn>(abi.handle, "strategy_free");
    abi.run = load<run_fn>(abi.handle, "run_backtest");
    abi.run_full = load<run_full_fn>(abi.handle, "run_backtest_full");
    abi.report_free = load<report_free_fn>(abi.handle, "report_free");
    abi.set_input = load<set_kv_fn>(abi.handle, "strategy_set_input");
    abi.set_override = load<set_kv_fn>(abi.handle, "strategy_set_override");
    abi.set_magnifier_vw = load<set_mag_fn>(abi.handle, "strategy_set_magnifier_volume_weighted");
    abi.set_trace = load<set_i_fn>(abi.handle, "strategy_set_trace_enabled");
    abi.set_trade_start = load<set_i64_fn>(abi.handle, "strategy_set_trade_start_time");
    abi.set_realtime_tail = load<set_ii_fn>(abi.handle, "strategy_set_realtime_tail");
    abi.set_probe_tail = load<set_i_fn>(abi.handle, "strategy_set_probe_suppress_tail_logic");
    abi.set_path_order = load<set_i_fn>(abi.handle, "strategy_set_path_order");
    abi.set_broker_hash = load<set_i_fn>(abi.handle, "strategy_set_broker_state_hash_recording");
    abi.set_chart_tz = load<set_str_fn>(abi.handle, "strategy_set_chart_timezone");
    abi.set_sym_tz = load<set_str_fn>(abi.handle, "strategy_set_syminfo_timezone");
    abi.set_sym_session = load<set_str_fn>(abi.handle, "strategy_set_syminfo_session");
    abi.set_sym_type = load<set_str_fn>(abi.handle, "strategy_set_syminfo_type");
    abi.set_sym_string = load<set_kv_int_fn>(abi.handle, "strategy_set_syminfo_string");
    abi.set_sym_mintick = load<set_d_fn>(abi.handle, "strategy_set_syminfo_mintick");
    abi.set_sym_pointvalue = load<set_d_fn>(abi.handle, "strategy_set_syminfo_pointvalue");
    abi.set_sym_metadata = load<set_str_d_fn>(abi.handle, "strategy_set_syminfo_metadata");
    abi.set_fx = load<set_fx_fn>(abi.handle, "strategy_set_account_currency_fx_series");
    abi.set_aux = load<set_aux_fn>(abi.handle, "strategy_set_aux_security_feed");
    abi.set_native_feed = load<set_nat_feed_fn>(abi.handle, "strategy_set_native_security_feed");
    abi.abi_version = load<abi_fn>(abi.handle, "pf_abi_version");
    abi.version_get = load<version_fn>(abi.handle, "pf_version_get");
    abi.contract = load<contract_fn>(abi.handle, "strategy_execution_contract");
    abi.configure = load<configure_fn>(abi.handle, "strategy_configure_native_v1");
    abi.last_error = load<last_error_fn>(abi.handle, "strategy_get_last_error");
    abi.last_status = load<last_status_fn>(abi.handle, "strategy_last_run_status");
    abi.position_size = load<position_fn>(abi.handle, "strategy_position_size");
    abi.current_equity = load<equity_fn>(abi.handle, "strategy_current_equity");
    abi.script_bars = load<bars_fn>(abi.handle, "strategy_script_bars_processed");
    abi.stream_api_version = load<abi_fn>(abi.handle, "strategy_stream_api_version");
    abi.stream_begin = load<stream_begin_fn>(abi.handle, "strategy_stream_begin");
    abi.stream_push_bar = load<stream_push_fn>(abi.handle, "strategy_stream_push_bar");
    abi.stream_end = load<stream_end_fn>(abi.handle, "strategy_stream_end");
    abi.stream_fill_report = load<stream_fill_fn>(abi.handle, "strategy_stream_fill_report");
    abi.example_abi = load<abi_fn>(abi.handle, "native_market_example_abi_version");
    if (abi.abi_version) {
        std::printf("pf_abi_version=%d PF_ABI_VERSION=%d example_abi=%d stream_api=%d\n",
                    abi.abi_version(), PF_ABI_VERSION,
                    abi.example_abi ? abi.example_abi() : -1,
                    abi.stream_api_version ? abi.stream_api_version() : -1);
    }
    if (abi.version_get) {
        const pf_version_t v = abi.version_get();
        std::printf("pf_version=%d.%d.%d sha=%s\n", v.major, v.minor, v.patch,
                    v.commit_sha ? v.commit_sha : "");
    }
    std::printf("sym strategy_create=%p run_backtest=%p run_backtest_full=%p report_free=%p\n",
                reinterpret_cast<void*>(abi.create), reinterpret_cast<void*>(abi.run),
                reinterpret_cast<void*>(abi.run_full), reinterpret_cast<void*>(abi.report_free));
    std::printf("sym strategy_configure_native_v1=%p strategy_execution_contract=%p\n",
                reinterpret_cast<void*>(abi.configure), reinterpret_cast<void*>(abi.contract));
    return abi;
}

pf_native_run_spec_v1 complete_spec(const char* session_key, uint64_t run_number) {
    pf_native_run_spec_v1 spec{};
    spec.struct_size = sizeof(spec);
    spec.session_key = session_key;
    spec.run_number = run_number;
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.ticker = "X";
    spec.tickerid = "EXCHANGE:X";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "native-market-example";
    spec.volumetype = "base";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "UTC";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.slippage_ticks = 0;
    spec.fee_kind = 2;  // NativeFeeKind::CashPerExecution
    spec.fee_value = 6.0;
    spec.optional_mask = 0;
    spec.close_execution = 0;         // NextEligiblePoint
    spec.allowed_open_directions = 3; // Both
    spec.quantity_grid = 0.0;
    spec.max_abs_units = 0.0;
    spec.initial_margin_fraction = 0.0;
    spec.max_open_lots = 0;
    return spec;
}

void five_minute_bars(pf_bar_t* bars) {
    const double ohlc[5][4] = {
        {100.0, 101.0, 99.0, 100.5},
        {102.0, 103.0, 101.0, 102.5},
        {103.0, 103.5, 102.5, 103.0},
        {103.5, 104.0, 103.0, 103.5},
        {104.0, 105.0, 103.5, 104.5},
    };
    for (int i = 0; i < 5; ++i) {
        bars[i].open = ohlc[i][0];
        bars[i].high = ohlc[i][1];
        bars[i].low = ohlc[i][2];
        bars[i].close = ohlc[i][3];
        bars[i].volume = 1.0;
        bars[i].timestamp = static_cast<int64_t>(i) * 300000;
    }
}

bool error_text(const Abi& abi, pf_strategy_t s, const char* needle) {
    if (!abi.last_error) return false;
    const char* text = abi.last_error(s);
    if (!text) return false;
    return std::strstr(text, needle) != nullptr;
}

void expect_closed_round_trip(const pf_report_t& report) {
    CHECK(report.trades_len == 1);
    CHECK(report.total_trades == 1);
    CHECK(report.trades != nullptr);
    CHECK(report.bar_magnifier_enabled == 0);
    std::printf("report diagnostics input_bars=%lld script_bars=%lld equity_len=%lld\n",
                static_cast<long long>(report.input_bars_processed),
                static_cast<long long>(report.script_bars_processed),
                static_cast<long long>(report.equity_curve_len));
    if (!report.trades || report.trades_len < 1) return;
    const pf_trade_t t = report.trades[0];
    CHECK(t.is_long == 1);
    CHECK(t.open_at_end == 0);
    near(t.qty, 1.0);
    near(t.entry_price, 102.0);
    near(t.exit_price, 104.0);
    near(t.commission, 12.0);
    near(t.pnl, -10.0);
    CHECK(t.entry_time == 300000);
    CHECK(t.exit_time == 1200000);
    CHECK(t.entry_bar_index == 1);
    CHECK(t.exit_bar_index == 4);
    near(report.net_profit, -10.0);
}

void expect_zeroed_ownership(const pf_report_t& report) {
    CHECK(report.trades == nullptr);
    CHECK(report.trades_len == 0);
    CHECK(report.security_diag == nullptr);
    CHECK(report.security_diag_len == 0);
    CHECK(report.trace == nullptr);
    CHECK(report.trace_len == 0);
    CHECK(report.trace_names == nullptr);
    CHECK(report.trace_names_len == 0);
    CHECK(report.equity_curve == nullptr);
    CHECK(report.equity_curve_len == 0);
    CHECK(report.broker_state_hash == nullptr);
    CHECK(report.broker_state_hash_len == 0);
}

bool nonempty_error(const Abi& abi, pf_strategy_t s) {
    const char* text = abi.last_error ? abi.last_error(s) : nullptr;
    return text != nullptr && text[0] != '\0';
}

bool successful_round_trip(const pf_report_t& report) {
    return report.trades_len == 1 && report.trades != nullptr
        && report.trades[0].open_at_end == 0
        && std::abs(report.trades[0].pnl + 10.0) <= 1e-9;
}

void expect_source_setter_failed(const Abi& abi, pf_bar_t* bars, const char* session,
                                 void (*apply)(const Abi&, pf_strategy_t),
                                 const char* needle, bool example_owned) {
    pf_strategy_t s = abi.create(nullptr);
    CHECK(s != nullptr);
    auto spec = complete_spec(session, 1);
    CHECK(abi.configure(s, &spec) == 0);
    const double pos = abi.position_size(s);
    const double eq = abi.current_equity(s);
    apply(abi, s);
    const char* err = abi.last_error(s);
    std::printf("setter %s last_error=%s\n", session, err ? err : "(null)");
    if (example_owned) {
        CHECK(err != nullptr && err[0] != '\0');
        if (needle) CHECK(std::strstr(err, needle) != nullptr);
    }
    near(abi.position_size(s), pos);
    near(abi.current_equity(s), eq);
    pf_report_t report{};
    abi.run_full(s, bars, 5, "5", "5", 0, 4, PF_MAGNIFIER_ENDPOINTS, &report);
    CHECK(nonempty_error(abi, s));
    CHECK(!successful_round_trip(report));
    near(abi.position_size(s), pos);
    near(abi.current_equity(s), eq);
    spec = complete_spec(session, 2);
    CHECK(abi.configure(s, &spec) == -1);
    CHECK(nonempty_error(abi, s));
    abi.report_free(&report);
    abi.free_strategy(s);
}

void apply_input(const Abi& abi, pf_strategy_t s) { abi.set_input(s, "Fast Length", "12"); }
void apply_override(const Abi& abi, pf_strategy_t s) {
    abi.set_override(s, "initial_capital", "1");
}
void apply_magnifier_vw(const Abi& abi, pf_strategy_t s) { abi.set_magnifier_vw(s, 1); }
void apply_trace(const Abi& abi, pf_strategy_t s) { abi.set_trace(s, 1); }
void apply_trade_start(const Abi& abi, pf_strategy_t s) { abi.set_trade_start(s, 1); }
void apply_realtime_tail(const Abi& abi, pf_strategy_t s) { abi.set_realtime_tail(s, 1, 1); }
void apply_probe_tail(const Abi& abi, pf_strategy_t s) { abi.set_probe_tail(s, 1); }
void apply_path_order(const Abi& abi, pf_strategy_t s) { abi.set_path_order(s, 1); }
void apply_broker_hash(const Abi& abi, pf_strategy_t s) { abi.set_broker_hash(s, 1); }
void apply_chart_tz(const Abi& abi, pf_strategy_t s) { abi.set_chart_tz(s, "UTC"); }
void apply_sym_tz(const Abi& abi, pf_strategy_t s) { abi.set_sym_tz(s, "UTC"); }
void apply_sym_session(const Abi& abi, pf_strategy_t s) { abi.set_sym_session(s, "24x7"); }
void apply_sym_type(const Abi& abi, pf_strategy_t s) { abi.set_sym_type(s, "crypto"); }
void apply_sym_string(const Abi& abi, pf_strategy_t s) {
    abi.set_sym_string(s, "ticker", "X");
}
void apply_sym_mintick(const Abi& abi, pf_strategy_t s) { abi.set_sym_mintick(s, 0.01); }
void apply_sym_pointvalue(const Abi& abi, pf_strategy_t s) { abi.set_sym_pointvalue(s, 1.0); }
void apply_sym_metadata(const Abi& abi, pf_strategy_t s) {
    abi.set_sym_metadata(s, "shares_outstanding_total", 1.0);
}
void apply_fx(const Abi& abi, pf_strategy_t s) {
    const int64_t ts = 0;
    const double rate = 1.0;
    abi.set_fx(s, &ts, &rate, 1);
}
void apply_aux(const Abi& abi, pf_strategy_t s) {
    pf_bar_t bar{};
    bar.open = bar.high = bar.close = 100.0;
    bar.low = 99.0;
    bar.volume = 1.0;
    abi.set_aux(s, &bar, 1, "1");
}
void apply_native_feed(const Abi& abi, pf_strategy_t s) {
    pf_bar_t bar{};
    bar.open = bar.high = bar.close = 100.0;
    bar.low = 99.0;
    bar.volume = 1.0;
    abi.set_native_feed(s, "D", &bar, 1);
}

bool ready(const Abi& abi) {
    return abi.handle && abi.create && abi.free_strategy && abi.run && abi.run_full
        && abi.report_free && abi.configure && abi.contract && abi.last_error
        && abi.position_size && abi.stream_begin && abi.stream_push_bar
        && abi.stream_end && abi.stream_fill_report && abi.set_input
        && abi.set_override && abi.set_magnifier_vw && abi.set_trace
        && abi.set_trade_start && abi.set_chart_tz && abi.set_fx && abi.set_aux
        && abi.set_native_feed && abi.abi_version;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s /path/to/native-market-example\n", argv[0]);
        return 2;
    }

    Abi abi = load_library(argv[1]);
    if (!ready(abi)) {
        std::printf("FAIL %d checks %d failures (library not usable)\n", checks, failures);
        if (abi.handle) dlclose(abi.handle);
        return 1;
    }
    CHECK(abi.abi_version() == PF_ABI_VERSION);
    CHECK(abi.stream_api_version() == 1);
    CHECK(abi.example_abi() == PF_ABI_VERSION);

    pf_bar_t bars[5];
    five_minute_bars(bars);

    {
        // Malformed transport arrays still reach the engine's documented
        // diagnostic path and do not consume a Ready native run.
        pf_strategy_t s = abi.create(nullptr);
        CHECK(s != nullptr);
        auto spec = complete_spec("native-example-warmup-shape", 1);
        CHECK(abi.configure(s, &spec) == 0);
        const double equity_before = abi.current_equity(s);
        CHECK(abi.stream_begin(s, nullptr, 1, "5", "5") == -1);
        CHECK(error_text(abi, s, "warmup"));
        CHECK(abi.stream_begin(s, bars, -1, "5", "5") == -1);
        CHECK(error_text(abi, s, "warmup"));
        near(abi.position_size(s), 0.0);
        near(abi.current_equity(s), equity_before);
        CHECK(abi.stream_begin(s, bars, 1, "5", "5") == 0);
        CHECK(abi.stream_end(s, 0) == 0);
        abi.free_strategy(s);
    }

    {
        pf_strategy_t s = abi.create(nullptr);
        CHECK(s != nullptr);
        CHECK(abi.contract(s) == 2);
        auto spec = complete_spec("native-example-batch", 1);
        CHECK(spec.struct_size == sizeof(pf_native_run_spec_v1));
        CHECK(abi.configure(s, &spec) == 0);
        CHECK(abi.last_error(s) != nullptr && abi.last_error(s)[0] == '\0');

        pf_report_t report{};
        abi.run_full(s, bars, 5, "5", "5", 0, 4, PF_MAGNIFIER_ENDPOINTS, &report);
        const char* err = abi.last_error(s);
        if (err && err[0] != '\0') std::printf("run_full last_error=%s\n", err);
        CHECK(err == nullptr || err[0] == '\0');
        CHECK(abi.last_status(s) == 0);
        near(abi.position_size(s), 0.0);
        expect_closed_round_trip(report);
        near(abi.current_equity(s), 9990.0);
        std::printf("strategy_script_bars_processed=%lld position=%g equity=%g\n",
                    static_cast<long long>(abi.script_bars(s)), abi.position_size(s),
                    abi.current_equity(s));

        abi.report_free(&report);
        expect_zeroed_ownership(report);
        abi.report_free(&report);
        expect_zeroed_ownership(report);
        abi.report_free(nullptr);

        spec = complete_spec("native-example-batch", 2);
        CHECK(abi.configure(s, &spec) == 0);
        pf_report_t second{};
        abi.run_full(s, bars, 5, "", "", 0, 4, PF_MAGNIFIER_ENDPOINTS, &second);
        err = abi.last_error(s);
        if (err && err[0] != '\0') std::printf("second run last_error=%s\n", err);
        CHECK(err == nullptr || err[0] == '\0');
        expect_closed_round_trip(second);
        abi.report_free(&second);
        abi.free_strategy(s);
    }

    {
        // Wrapper preflight preserves the output report on unsupported magnifier
        // arguments. Does not latch Failed; Ready remains usable.
        pf_strategy_t s = abi.create(nullptr);
        CHECK(s != nullptr);
        auto spec = complete_spec("native-example-magnifier-wrapper", 1);
        CHECK(abi.configure(s, &spec) == 0);
        const double equity_before = abi.current_equity(s);
        pf_report_t poisoned;
        std::memset(&poisoned, 0x5a, sizeof(poisoned));
        abi.run_full(s, bars, 5, "5", "5", 1, 4, PF_MAGNIFIER_ENDPOINTS, &poisoned);
        CHECK(error_text(abi, s, "magnifier"));
        unsigned char* bytes = reinterpret_cast<unsigned char*>(&poisoned);
        bool untouched = true;
        for (size_t i = 0; i < sizeof(poisoned); ++i) {
            if (bytes[i] != 0x5a) {
                untouched = false;
                break;
            }
        }
        CHECK(untouched);
        near(abi.position_size(s), 0.0);
        near(abi.current_equity(s), equity_before);

        std::memset(&poisoned, 0x5a, sizeof(poisoned));
        abi.run_full(s, bars, 5, "5", "5", 0, 8, PF_MAGNIFIER_UNIFORM, &poisoned);
        CHECK(error_text(abi, s, "magnifier"));
        bytes = reinterpret_cast<unsigned char*>(&poisoned);
        untouched = true;
        for (size_t i = 0; i < sizeof(poisoned); ++i) {
            if (bytes[i] != 0x5a) {
                untouched = false;
                break;
            }
        }
        CHECK(untouched);
        pf_report_t recovered{};
        abi.run_full(s, bars, 5, "5", "5", 0, 4, PF_MAGNIFIER_ENDPOINTS, &recovered);
        const char* recovered_err = abi.last_error(s);
        if (recovered_err && recovered_err[0] != '\0') {
            std::printf("magnifier-wrapper recovered last_error=%s\n", recovered_err);
        }
        CHECK(recovered_err == nullptr || recovered_err[0] == '\0');
        expect_closed_round_trip(recovered);
        abi.report_free(&recovered);
        abi.free_strategy(s);
    }

    {
        pf_strategy_t s = abi.create(nullptr);
        CHECK(s != nullptr);
        auto spec = complete_spec("native-example-bad-bars", 1);
        CHECK(abi.configure(s, &spec) == 0);
        pf_bar_t bad[5];
        five_minute_bars(bad);
        bad[2].timestamp = 600000 + 1;
        pf_report_t invalid{};
        abi.run_full(s, bad, 5, "5", "5", 0, 4, PF_MAGNIFIER_ENDPOINTS, &invalid);
        const char* invalid_err = abi.last_error(s);
        CHECK(invalid_err != nullptr && invalid_err[0] != '\0');
        CHECK(std::strstr(invalid_err, "canonical") != nullptr
              || std::strstr(invalid_err, "aligned") != nullptr
              || std::strstr(invalid_err, "increasing") != nullptr
              || std::strstr(invalid_err, "native bar") != nullptr);
        near(abi.position_size(s), 0.0);
        CHECK(!successful_round_trip(invalid));
        abi.report_free(&invalid);
        pf_report_t recovered{};
        abi.run_full(s, bars, 5, "5", "5", 0, 4, PF_MAGNIFIER_ENDPOINTS, &recovered);
        const char* recovered_err = abi.last_error(s);
        if (recovered_err && recovered_err[0] != '\0') {
            std::printf("bad-bars recovered last_error=%s\n", recovered_err);
        }
        CHECK(recovered_err == nullptr || recovered_err[0] == '\0');
        expect_closed_round_trip(recovered);
        abi.report_free(&recovered);
        abi.free_strategy(s);
    }

    {
        struct Case {
            const char* session;
            void (*apply)(const Abi&, pf_strategy_t);
            const char* needle;
            bool example_owned;
        };
        const Case cases[] = {
            {"set-input", apply_input, "set_input", true},
            {"set-override", apply_override, "source mutation", true},
            {"set-magnifier-vw", apply_magnifier_vw, "set_magnifier_volume_weighted", true},
            {"set-trace", apply_trace, nullptr, false},
            {"set-trade-start", apply_trade_start, nullptr, false},
            {"set-realtime-tail", apply_realtime_tail, nullptr, false},
            {"set-probe-tail", apply_probe_tail, nullptr, false},
            {"set-path-order", apply_path_order, nullptr, false},
            {"set-broker-hash", apply_broker_hash, nullptr, false},
            {"set-chart-tz", apply_chart_tz, nullptr, false},
            {"set-sym-tz", apply_sym_tz, nullptr, false},
            {"set-sym-session", apply_sym_session, nullptr, false},
            {"set-sym-type", apply_sym_type, nullptr, false},
            {"set-sym-string", apply_sym_string, nullptr, false},
            {"set-sym-mintick", apply_sym_mintick, nullptr, false},
            {"set-sym-pointvalue", apply_sym_pointvalue, nullptr, false},
            {"set-sym-metadata", apply_sym_metadata, nullptr, false},
            {"set-fx", apply_fx, nullptr, false},
            {"set-aux", apply_aux, nullptr, false},
            {"set-native-feed", apply_native_feed, nullptr, false},
        };
        for (const auto& c : cases) {
            expect_source_setter_failed(abi, bars, c.session, c.apply, c.needle,
                                        c.example_owned);
        }
    }

    {
        pf_strategy_t s = abi.create(nullptr);
        CHECK(s != nullptr);
        auto spec = complete_spec("native-example-simple", 1);
        CHECK(abi.configure(s, &spec) == 0);
        pf_report_t report{};
        abi.run(s, bars, 5, &report);
        const char* err = abi.last_error(s);
        if (err && err[0] != '\0') std::printf("run_backtest last_error=%s\n", err);
        CHECK(err == nullptr || err[0] == '\0');
        expect_closed_round_trip(report);
        abi.report_free(&report);
        abi.free_strategy(s);
    }

    {
        pf_strategy_t s = abi.create(nullptr);
        CHECK(s != nullptr);
        auto spec = complete_spec("native-example-stream", 1);
        CHECK(abi.configure(s, &spec) == 0);
        CHECK(abi.stream_begin(s, bars, 1, "5", "5") == 0);
        for (int i = 1; i < 5; ++i) CHECK(abi.stream_push_bar(s, &bars[i]) == 0);
        CHECK(abi.stream_end(s, 0) == 0);
        pf_report_t report{};
        CHECK(abi.stream_fill_report(s, &report) == 0);
        const char* err = abi.last_error(s);
        if (err && err[0] != '\0') std::printf("stream last_error=%s\n", err);
        CHECK(err == nullptr || err[0] == '\0');
        expect_closed_round_trip(report);
        near(abi.position_size(s), 0.0);
        abi.report_free(&report);
        abi.free_strategy(s);
    }

    abi.free_strategy(nullptr);
    dlclose(abi.handle);
    std::printf("%s %d checks %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
