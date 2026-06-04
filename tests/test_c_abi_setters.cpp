/*
 * test_c_abi_setters.cpp — exercises the runtime-library C-ABI setter
 * entry points in src/c_abi.cpp (lines ~119-205) that the existing
 * tests/test_c_abi.c does NOT touch:
 *
 *   strategy_set_trace_enabled, strategy_get_last_error,
 *   strategy_set_trade_start_time, strategy_set_chart_timezone,
 *   strategy_set_syminfo_timezone / _session / _mintick / _pointvalue /
 *   _metadata, and pf_version_string.
 *
 * Each entry point begins with a null-guard (`if (!s) return;` — or
 * `if (!s || !arg) return;`) and then forwards to a BacktestEngine
 * method. We hit BOTH branches of every one:
 *
 *   1. NULL handle  → the guard returns early, must not crash / mutate.
 *   2. Valid handle → the forwarding line runs against a real engine.
 *
 * WHY C++ (not the assigned .c): the only legitimate `pf_strategy_t`
 * value is a `pineforge::BacktestEngine*`. The public `strategy_create`
 * is emitted *per compiled strategy* by the codegen — it is NOT defined
 * in libpineforge — so a unit test cannot obtain a handle through the
 * public C ABI. `BacktestEngine::on_bar` is pure-virtual, so a valid
 * handle can only come from a concrete subclass, which requires C++. We
 * still drive every assertion through the `extern "C"` C-ABI symbols
 * (declared by the public C header), which is exactly the surface under
 * test; the concrete subclass merely lets us read engine state back
 * through its protected members to prove each forwarding line ran. This
 * mirrors the engine-subclass pattern used throughout the suite (e.g.
 * test_engine_trade_accessors.cpp).
 *
 * NDEBUG-proof: uses a self-rolled CHECK that increments a failure
 * counter and returns nonzero from main(), so it fails for real even in
 * the canonical Release (-DNDEBUG) build where bare assert() is a no-op.
 */

#include <pineforge/pineforge.h>   // the C ABI under test (extern "C")
#include <pineforge/engine.hpp>    // BacktestEngine (to mint a valid handle)
#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>        // is_na (absent-metadata sentinel)

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <limits>
#include <string>

static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__,       \
                         #cond);                                               \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

// Minimal concrete engine: a valid handle whose internal state the C-ABI
// setters mutate. The protected members are visible to this subclass, so
// we read them back to confirm each forwarding line actually ran.
namespace {
class ProbeEngine : public pineforge::BacktestEngine {
public:
    void on_bar(const pineforge::Bar&) override {}  // never invoked here

    // Thin protected-member accessors (this subclass owns the access).
    const std::string& tz_chart()    const { return chart_timezone_; }
    bool               trace_on()    const { return trace_enabled_; }
    int64_t            trade_start() const { return trade_start_time_; }
    const std::string& sym_tz()      const { return syminfo_.timezone; }
    const std::string& sym_session() const { return syminfo_.session; }
    double             sym_mintick() const { return syminfo_.mintick; }
    double             sym_pv()      const { return syminfo_.pointvalue; }
    // get_syminfo_metadata() is protected on BacktestEngine; expose it via
    // the subclass. It returns na<double>() (NaN) for keys never injected.
    double meta(const std::string& key) const { return get_syminfo_metadata(key); }
};
}

int main() {
    // ── pf_version_string: no handle involved ───────────────────────
    const char* vs = pf_version_string();
    CHECK(vs != nullptr);
    CHECK(vs[0] != '\0');                            // non-empty
    CHECK(std::strcmp(vs, PINEFORGE_VERSION_FULL) == 0);

    // ── NULL-handle guard branch for every setter ───────────────────
    // Each must take the `if (!s ...) return;` early-out and NOT crash.
    // strategy_get_last_error(NULL) returns NULL by contract.
    strategy_set_trace_enabled(nullptr, 1);
    strategy_set_trade_start_time(nullptr, 123456789LL);
    strategy_set_chart_timezone(nullptr, "Asia/Taipei");
    strategy_set_syminfo_timezone(nullptr, "America/New_York");
    strategy_set_syminfo_session(nullptr, "0930-1600:23456");
    strategy_set_syminfo_mintick(nullptr, 0.25);
    strategy_set_syminfo_pointvalue(nullptr, 50.0);
    strategy_set_syminfo_metadata(nullptr, "shares_outstanding_total", 1.0);
    CHECK(strategy_get_last_error(nullptr) == nullptr);

    // Secondary `|| !arg` guard arms (valid handle, NULL arg) — these
    // must early-out too, leaving engine state untouched.
    ProbeEngine guard_eng;
    pf_strategy_t gh = static_cast<pf_strategy_t>(&guard_eng);
    const std::string guard_tz_before      = guard_eng.sym_tz();       // "UTC"
    const std::string guard_session_before = guard_eng.sym_session();  // "24x7"
    strategy_set_syminfo_timezone(gh, nullptr);      // !tz      → return
    strategy_set_syminfo_session(gh, nullptr);       // !session → return
    strategy_set_syminfo_metadata(gh, nullptr, 9.0); // !key     → return
    CHECK(guard_eng.sym_tz() == guard_tz_before);
    CHECK(guard_eng.sym_session() == guard_session_before);

    // ── Valid-handle forwarding branch ──────────────────────────────
    ProbeEngine eng;
    pf_strategy_t h = static_cast<pf_strategy_t>(&eng);

    // last_error() defaults to empty string on a fresh engine; the C ABI
    // returns the c_str() of that → non-NULL and empty.
    const char* err = strategy_get_last_error(h);
    CHECK(err != nullptr);
    CHECK(err[0] == '\0');

    // trace_enabled: default false → 1/non-zero maps to true, 0 to false.
    CHECK(eng.trace_on() == false);
    strategy_set_trace_enabled(h, 1);
    CHECK(eng.trace_on() == true);
    strategy_set_trace_enabled(h, 0);
    CHECK(eng.trace_on() == false);
    strategy_set_trace_enabled(h, 7);                // any non-zero → true
    CHECK(eng.trace_on() == true);

    // trade_start_time: default is INT64_MIN; the setter forwards verbatim.
    CHECK(eng.trade_start() == std::numeric_limits<int64_t>::min());
    strategy_set_trade_start_time(h, 1700000000000LL);
    CHECK(eng.trade_start() == 1700000000000LL);

    // chart_timezone: default empty; a non-empty value is stored as-is,
    // and NULL normalises back to the empty-string (legacy UTC fast path).
    CHECK(eng.tz_chart().empty());
    strategy_set_chart_timezone(h, "Asia/Taipei");
    CHECK(eng.tz_chart() == std::string("Asia/Taipei"));
    strategy_set_chart_timezone(h, nullptr);
    CHECK(eng.tz_chart().empty());

    // syminfo timezone / session forward the C string into syminfo_.
    CHECK(eng.sym_tz() == std::string("UTC"));        // constructor default
    strategy_set_syminfo_timezone(h, "America/New_York");
    CHECK(eng.sym_tz() == std::string("America/New_York"));
    CHECK(eng.sym_session() == std::string("24x7"));  // constructor default
    strategy_set_syminfo_session(h, "0930-1600:23456");
    CHECK(eng.sym_session() == std::string("0930-1600:23456"));

    // mintick: positive accepted; non-positive ignored (engine-side guard).
    CHECK(eng.sym_mintick() == 0.01);                 // constructor default
    strategy_set_syminfo_mintick(h, 0.25);
    CHECK(eng.sym_mintick() == 0.25);
    strategy_set_syminfo_mintick(h, 0.0);             // non-positive → ignored
    CHECK(eng.sym_mintick() == 0.25);
    strategy_set_syminfo_mintick(h, -1.0);            // negative     → ignored
    CHECK(eng.sym_mintick() == 0.25);

    // pointvalue: positive accepted; non-positive ignored.
    CHECK(eng.sym_pv() == 1.0);                        // constructor default
    strategy_set_syminfo_pointvalue(h, 50.0);
    CHECK(eng.sym_pv() == 50.0);
    strategy_set_syminfo_pointvalue(h, 0.0);          // ignored
    CHECK(eng.sym_pv() == 50.0);

    // metadata: forwards (key, value) into the engine's metadata map,
    // surfaced via the public get_syminfo_metadata(). Absent → na (NaN).
    CHECK(pineforge::is_na(eng.meta("shares_outstanding_total")));
    strategy_set_syminfo_metadata(h, "shares_outstanding_total", 12345.0);
    CHECK(eng.meta("shares_outstanding_total") == 12345.0);
    // A key that was never injected still reports na.
    CHECK(pineforge::is_na(eng.meta("never_injected")));

    if (g_fail == 0) {
        std::printf("test_c_abi_setters: OK (pineforge %s)\n", vs);
        return 0;
    }
    std::fprintf(stderr, "test_c_abi_setters: %d FAILURES\n", g_fail);
    return 1;
}
