// A hand-written strategy library that records outputs, for
// scripts/test_run_json_outputs.py: the entry points docker/run_json.py binds
// (strategy_create, run_backtest_full, ...) over a bare NativeStrategyHost,
// and the three per-library exports of group pf_outputs that a generated
// module carries (strategy_outputs_api_version, strategy_outputs_manifest,
// strategy_signal_safety_receipt). It stands in for a generated module: the
// manifest is a fixed document in the manifest's canonical bytes (sorted
// keys, no spaces), and the host records:
//
//   slot 0  output o0's value: the close
//   slot 1  output o0's colour, rgba-u32: up bars green, others red
//   slot 2  output o1's mark, bool-as-1-0: close > open; an event when 1
//   slot 3  output o3's value: the change of the close, na on bars 0 and 1
//   output o2: an event with the message "close=<close>" on bars 2, 5, 8, ...
//   constant 0: output h0's price, 50; constant 1: its colour, rgba-u32
//
// Input "publish" = "false" publishes no bar (a run with no rows). With the
// environment variable PF_OUTPUTS_FIXTURE_UNDECLARED set at creation the
// module declares nothing, so switching recording on is refused.
#include <pineforge/pineforge.h>
#include <pineforge/checked_settings.hpp>
#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace pineforge;

constexpr const char* kManifest =
    R"({"changes_trading":[],"constants":[{"index":0,"output":"h0","param":"price"},)"
    R"({"encoding":"rgba-u32","index":1,"output":"h0","param":"color"}],)"
    R"("declaration":{"kind":"indicator","overlay":true,"title":"Outputs fixture"},)"
    R"("message_format":"pineforge/v1","not_outputs":[],)"
    R"("outputs":[{"colors":{"color":{"slot":1}},"id":"o0","index":0,"kind":"plot","line":3,"series":[0],"title":"Close"},)"
    R"({"events":"mark","id":"o1","index":1,"kind":"plotshape","line":4,"mark_rule":"bool-true","series":[2],"title":"Up"},)"
    R"({"events":"per-call","freq":"once_per_bar_close","id":"o2","index":2,"kind":"alert","line":5,"series":[]},)"
    R"({"id":"o3","index":3,"kind":"plot","line":6,"series":[3],"title":"Change"},)"
    R"({"colors":{"color":{"constant":1}},"id":"h0","index":4,"kind":"hline","line":7,"price":{"constant":0,"default":50},"title":"Mid"}],)"
    R"("schema_version":"pineforge-outputs-manifest/v1",)"
    R"("series":[{"output":"o0","role":"value","slot":0},{"encoding":"rgba-u32","output":"o0","role":"color","slot":1},)"
    R"({"encoding":"bool-as-1-0","output":"o1","role":"value","slot":2},{"output":"o3","role":"value","slot":3}],)"
    R"("source_sha256":"0000000000000000000000000000000000000000000000000000000000000000"})";

double rgba(std::uint32_t argb) {
    return static_cast<double>((argb << 8) | (argb >> 24));
}

class FixtureHost final : public NativeStrategyHost {
public:
    FixtureHost() {
        if (!std::getenv("PF_OUTPUTS_FIXTURE_UNDECLARED")) declare_outputs(4, 5, 2);
    }

    bool publish = true;
    std::uint64_t runs = 0;

    void on_native_run_begin() override {
        output_run_begin();
        bars_ = 0;
        previous_close_ = std::numeric_limits<double>::quiet_NaN();
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        if (!publish) return;
        const int k = bars_++;
        const bool up = bar.close > bar.open;
        if (outputs_enabled_) {
            output_bar(context.script_bar_open_ms, context.script_interval.next_period_open_ms);
            output_value(0, bar.close);
            output_value(1, rgba(up ? 0xFF4CAF50u : 0xFFF23645u));
            output_value(2, up ? 1.0 : 0.0);
            if (up) output_event(1, 1.0);
            output_value(3, k >= 2 ? bar.close - previous_close_
                                   : std::numeric_limits<double>::quiet_NaN());
            if (k % 3 == 2) {
                char text[64];
                std::snprintf(text, sizeof(text), "close=%.2f", bar.close);
                output_event(2, std::numeric_limits<double>::quiet_NaN(), text);
            }
            output_constant(0, 50.0);
            output_constant(1, rgba(0xFF787B86u));
        }
        previous_close_ = bar.close;
    }

private:
    int bars_ = 0;
    double previous_close_ = 0.0;
};

FixtureHost* host_of(pf_strategy_t s) {
    return static_cast<FixtureHost*>(static_cast<BacktestEngine*>(s));
}

NativeRunSpec spec_for(std::uint64_t run, const char* input_tf, const char* script_tf) {
    NativeRunSpec spec;
    spec.identity.session_key = "outputs-fixture";
    spec.identity.run_number = run;
    spec.input_tf = input_tf && *input_tf ? input_tf : "5";
    spec.script_tf = script_tf && *script_tf ? script_tf : spec.input_tf;
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::Percent;
    spec.fee_value = 0.0;
    return spec;
}

}  // namespace

extern "C" {

pf_strategy_t strategy_create(const char*) {
    try {
        return static_cast<BacktestEngine*>(new FixtureHost());
    } catch (...) {
        return nullptr;
    }
}

void strategy_free(pf_strategy_t s) {
    delete host_of(s);
}

void strategy_set_input(pf_strategy_t s, const char* key, const char* value) {
    if (s && key && value && std::strcmp(key, "publish") == 0)
        host_of(s)->publish = std::strcmp(value, "false") != 0;
}

void strategy_set_override(pf_strategy_t, const char*, const char*) {}

void run_backtest_full(pf_strategy_t s, pf_bar_t* bars, int n, const char* input_tf,
                       const char* script_tf, int, int, pf_magnifier_distribution_t,
                       pf_report_t* out) {
    if (out) std::memset(out, 0, sizeof(*out));
    if (!s || n < 0 || (n > 0 && !bars)) return;
    try {
        FixtureHost* host = host_of(s);
        if (host->configure_native(spec_for(++host->runs, input_tf, script_tf)).status
            != NativeSetupStatus::Applied) {
            return;
        }
        std::vector<Bar> input;
        for (int i = 0; i < n; ++i)
            input.push_back({bars[i].open, bars[i].high, bars[i].low, bars[i].close,
                             bars[i].volume, bars[i].timestamp});
        host->run(input.data(), n);
        if (out) host->fill_report(reinterpret_cast<ReportC*>(out));
    } catch (...) {
    }
}

void run_backtest(pf_strategy_t s, pf_bar_t* bars, int n, pf_report_t* out) {
    run_backtest_full(s, bars, n, "", "", 0, 4, PF_MAGNIFIER_ENDPOINTS, out);
}

void report_free(pf_report_t* report) {
    BacktestEngine::free_report(reinterpret_cast<ReportC*>(report));
}

uint32_t strategy_outputs_api_version(void) {
    return PF_OUTPUTS_API_VERSION;
}

int strategy_outputs_manifest(pf_strategy_t s, char* json, size_t capacity, size_t* required,
                              char* error, size_t error_capacity) {
    if (required) *required = 0;
    return checked_settings::boundary(error, error_capacity, [&] {
        checked_settings::require(s != nullptr, "null strategy");
        checked_settings::receipt(kManifest, json, capacity, required);
    });
}

int strategy_signal_safety_receipt(pf_strategy_t s, char*, size_t, size_t* required,
                                   char* error, size_t error_capacity) {
    if (required) *required = 0;
    return checked_settings::boundary(error, error_capacity, [&] {
        checked_settings::require(s != nullptr, "null strategy");
        checked_settings::require(false, "the fixture module carries no safety receipt",
                                  PF_SETTINGS_UNSUPPORTED);
    });
}

}  // extern "C"
