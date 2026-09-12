#include <pineforge/pineforge.h>
#include <pineforge/native_host.hpp>

#include <cstddef>
#include <cstring>
#include <exception>
#include <vector>

namespace {

static_assert(sizeof(pf_bar_t) == sizeof(pineforge::Bar),
              "pf_bar_t / pineforge::Bar size mismatch");
static_assert(offsetof(pf_bar_t, open) == offsetof(pineforge::Bar, open),
              "pf_bar_t::open offset mismatch");
static_assert(offsetof(pf_bar_t, high) == offsetof(pineforge::Bar, high),
              "pf_bar_t::high offset mismatch");
static_assert(offsetof(pf_bar_t, low) == offsetof(pineforge::Bar, low),
              "pf_bar_t::low offset mismatch");
static_assert(offsetof(pf_bar_t, close) == offsetof(pineforge::Bar, close),
              "pf_bar_t::close offset mismatch");
static_assert(offsetof(pf_bar_t, volume) == offsetof(pineforge::Bar, volume),
              "pf_bar_t::volume offset mismatch");
static_assert(offsetof(pf_bar_t, timestamp) == offsetof(pineforge::Bar, timestamp),
              "pf_bar_t::timestamp offset mismatch");

static_assert(sizeof(pf_report_t) == sizeof(pineforge::ReportC),
              "pf_report_t / pineforge::ReportC size mismatch");
static_assert(offsetof(pf_report_t, total_trades) == offsetof(pineforge::ReportC, total_trades),
              "pf_report_t::total_trades offset mismatch");
static_assert(offsetof(pf_report_t, trades) == offsetof(pineforge::ReportC, trades),
              "pf_report_t::trades offset mismatch");
static_assert(offsetof(pf_report_t, net_profit) == offsetof(pineforge::ReportC, net_profit),
              "pf_report_t::net_profit offset mismatch");
static_assert(offsetof(pf_report_t, security_diag) == offsetof(pineforge::ReportC, security_diag),
              "pf_report_t::security_diag offset mismatch");
static_assert(offsetof(pf_report_t, metrics) == offsetof(pineforge::ReportC, metrics),
              "pf_report_t::metrics offset mismatch");
static_assert(offsetof(pf_report_t, equity_curve) == offsetof(pineforge::ReportC, equity_curve),
              "pf_report_t::equity_curve offset mismatch");
static_assert(offsetof(pf_report_t, equity_curve_len)
                  == offsetof(pineforge::ReportC, equity_curve_len),
              "pf_report_t::equity_curve_len offset mismatch");
static_assert(offsetof(pf_report_t, broker_state_hash)
                  == offsetof(pineforge::ReportC, broker_state_hash),
              "pf_report_t::broker_state_hash offset mismatch");

class NativeMarketExample final : public pineforge::NativeStrategyHost {
    int bars_ = 0;

    void on_native_run_begin() override { bars_ = 0; }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ == 1 && physical_position().signed_units == 0.0) {
            submit_market({pineforge::order_action::Transact{1.0}, "native-open", ""});
        } else if (bars_ == 4 && physical_position().signed_units != 0.0) {
            submit_market({pineforge::execution::Flatten{}, "native-flat", ""});
        }
    }

public:
    void note_c_error(const char* text) noexcept {
        try {
            last_error_ = text ? text : "";
        } catch (...) {
        }
    }
};

pineforge::BacktestEngine* as_engine(pf_strategy_t s) {
    return static_cast<pineforge::BacktestEngine*>(s);
}

NativeMarketExample* as_example(pf_strategy_t s) {
    return s ? dynamic_cast<NativeMarketExample*>(as_engine(s)) : nullptr;
}

void note_error(pf_strategy_t s, const char* text) {
    if (auto* example = as_example(s)) example->note_c_error(text);
}

template <typename Fn>
void catch_source(pf_strategy_t s, Fn&& fn) {
    if (!s) return;
    try {
        fn();
    } catch (const std::exception& e) {
        note_error(s, e.what());
    } catch (...) {
        note_error(s, "native host refuses source mutation");
    }
}

bool magnifier_unsupported(int bar_magnifier, int magnifier_samples,
                           pf_magnifier_distribution_t dist) {
    return bar_magnifier != 0 || magnifier_samples != 4
        || dist != PF_MAGNIFIER_ENDPOINTS;
}

pineforge::Bar copy_c_bar(const pf_bar_t& in) {
    pineforge::Bar out;
    out.open = in.open;
    out.high = in.high;
    out.low = in.low;
    out.close = in.close;
    out.volume = in.volume;
    out.timestamp = in.timestamp;
    return out;
}

void publish_report(pineforge::BacktestEngine* engine, pf_report_t* out) {
    if (!engine || !out) return;
    pineforge::ReportC local{};
    try {
        engine->fill_report(&local);
        std::memcpy(out, &local, sizeof(local));
    } catch (...) {
        pineforge::BacktestEngine::free_report(&local);
        throw;
    }
}

void run_c_batch(pf_strategy_t s, pf_bar_t* bars, int n,
                 const char* input_tf, const char* script_tf, bool with_tf,
                 pf_report_t* out) {
    if (!s) return;
    auto* engine = as_engine(s);
    try {
        std::vector<pineforge::Bar> copied;
        const pineforge::Bar* ptr = nullptr;
        if (n > 0 && bars != nullptr) {
            copied.resize(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) copied[static_cast<size_t>(i)] = copy_c_bar(bars[i]);
            ptr = copied.data();
        }
        if (with_tf) {
            engine->run(ptr, n, input_tf ? input_tf : "", script_tf ? script_tf : "");
        } else {
            engine->run(ptr, n);
        }
        publish_report(engine, out);
    } catch (const std::exception& e) {
        note_error(s, e.what());
    } catch (...) {
        note_error(s, "native example C batch failed");
    }
}

}  // namespace

extern "C" {

PF_API pf_strategy_t strategy_create(const char*) {
    try {
        pineforge::BacktestEngine* engine = new NativeMarketExample();
        return static_cast<pf_strategy_t>(engine);
    } catch (...) {
        return nullptr;
    }
}

PF_API void strategy_free(pf_strategy_t s) {
    delete as_engine(s);
}

PF_API void strategy_set_input(pf_strategy_t s, const char* key, const char* value) {
    catch_source(s, [&] {
        as_engine(s)->set_input(key ? key : "", value ? value : "");
    });
}

PF_API void strategy_set_override(pf_strategy_t s, const char* key, const char* value) {
    catch_source(s, [&] {
        // No BacktestEngine override mutator exists. Forward to the public
        // guarded source-configuration entry; refuse_source_mutation latches
        // Failed before inputs_ is written.
        as_engine(s)->set_input(key ? key : "strategy_set_override",
                                value ? value : "");
    });
}

PF_API void strategy_set_magnifier_volume_weighted(pf_strategy_t s, int on) {
    catch_source(s, [&] {
        as_engine(s)->set_magnifier_volume_weighted(on != 0);
    });
}

PF_API void run_backtest(pf_strategy_t s, pf_bar_t* bars, int n, pf_report_t* out) {
    run_c_batch(s, bars, n, nullptr, nullptr, false, out);
}

PF_API void run_backtest_full(pf_strategy_t s, pf_bar_t* bars, int n,
                              const char* input_tf, const char* script_tf,
                              int bar_magnifier, int magnifier_samples,
                              pf_magnifier_distribution_t magnifier_dist,
                              pf_report_t* out) {
    if (!s) return;
    if (magnifier_unsupported(bar_magnifier, magnifier_samples, magnifier_dist)) {
        // Wrapper preflight only (pending root run_tf magnifier refusal).
        // Does not latch Failed; Ready is preserved until the core consumer
        // rejects bool/samples/dist itself.
        note_error(s, "native example refuses unsupported magnifier arguments");
        return;
    }
    run_c_batch(s, bars, n, input_tf, script_tf, true, out);
}

PF_API void report_free(pf_report_t* report) {
    if (!report) return;
    pineforge::ReportC local{};
    std::memcpy(&local, report, sizeof(local));
    pineforge::BacktestEngine::free_report(&local);
    std::memcpy(report, &local, sizeof(local));
}

// Pull PF_API runtime/stream/native-config symbols from the static engine.
PF_API int native_market_example_abi_version() { return pf_abi_version(); }

}  // extern "C"
