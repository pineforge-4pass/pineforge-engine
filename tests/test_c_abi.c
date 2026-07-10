/*
 * test_c_abi.c — sanity check that <pineforge/pineforge.h> compiles
 * cleanly under a pure C compiler. If this stops building, some C++
 * syntax has leaked into the public header (templates, namespaces,
 * inline class methods, etc.).
 *
 * Beyond compilation, this also exercises:
 *   - POD struct field access through C
 *   - The pf_version_get runtime entrypoint (linked from libruntime)
 *   - Sizeof / layout sanity (not full layout parity — that lives in
 *     c_abi.cpp's static_asserts which run at C++ compile time)
 */

#include <pineforge/pineforge.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, (msg));    \
            ++fail;                                                            \
        }                                                                      \
    } while (0)

int main(void) {
    /* ── Version ─────────────────────────────────────────────────── */
    pf_version_t v = pf_version_get();
    CHECK(v.major == PINEFORGE_VERSION_MAJOR, "version major mismatch");
    CHECK(v.minor == PINEFORGE_VERSION_MINOR, "version minor mismatch");
    CHECK(v.patch == PINEFORGE_VERSION_PATCH, "version patch mismatch");
    CHECK(v.commit_sha != NULL,               "commit_sha is NULL");

    /* ── ABI version ────────────────────────────────────────────── */
    CHECK(pf_abi_version() == PF_ABI_VERSION, "pf_abi_version() != PF_ABI_VERSION");

    /* ── Metrics / equity-point field-access smoke ──────────────── */
    {
        pf_metrics_t m;
        pf_equity_point_t p;
        memset(&m, 0, sizeof(m));
        memset(&p, 0, sizeof(p));
        m.all.num_trades = 42;
        m.equity.sharpe_tv = 1.5;
        p.time_ms = 1700000000000LL;
        CHECK(m.all.num_trades == 42,             "m.all.num_trades roundtrip");
        CHECK(m.equity.sharpe_tv == 1.5,          "m.equity.sharpe_tv roundtrip");
        CHECK(p.time_ms == 1700000000000LL,       "p.time_ms roundtrip");
    }

    /* ── Bar field access ────────────────────────────────────────── */
    pf_bar_t bar;
    memset(&bar, 0, sizeof(bar));
    bar.open      = 100.0;
    bar.high      = 105.0;
    bar.low       =  99.0;
    bar.close     = 103.0;
    bar.volume    = 1234.5;
    bar.timestamp = 1700000000000LL;
    CHECK(bar.timestamp == 1700000000000LL, "bar.timestamp roundtrip");
    CHECK(bar.open      == 100.0,           "bar.open roundtrip");
    CHECK(bar.close     == 103.0,           "bar.close roundtrip");

    /* ── Realtime trade tick field access ───────────────────────── */
    pf_trade_tick_t tick;
    memset(&tick, 0, sizeof(tick));
    tick.timestamp = 1700000000123LL;
    tick.sequence = 987654321ULL;
    tick.price = 103.25;
    tick.quantity = 0.125;
    CHECK(tick.sequence == 987654321ULL, "tick.sequence roundtrip");
    CHECK(tick.quantity == 0.125,        "tick.quantity roundtrip");

    /* ── Trade ───────────────────────────────────────────────────── */
    pf_trade_t trade;
    memset(&trade, 0, sizeof(trade));
    trade.is_long      = 1;
    trade.qty          = 2.5;
    trade.entry_price  = 100.0;
    trade.exit_price   = 110.0;
    trade.pnl          = 25.0;
    CHECK(trade.is_long      == 1,    "trade.is_long");
    CHECK(trade.qty          == 2.5,  "trade.qty");
    CHECK(trade.exit_price   == 110.0,"trade.exit_price");

    /* ── Report ──────────────────────────────────────────────────── */
    pf_report_t report;
    memset(&report, 0, sizeof(report));
    CHECK(report.total_trades   == 0,    "report.total_trades default");
    CHECK(report.trades         == NULL, "report.trades default");
    CHECK(report.security_diag  == NULL, "report.security_diag default");
    CHECK(report.trace          == NULL, "report.trace default");

    /* ── Magnifier enum values ───────────────────────────────────── */
    CHECK(PF_MAGNIFIER_UNIFORM      == 0, "PF_MAGNIFIER_UNIFORM value");
    CHECK(PF_MAGNIFIER_ENDPOINTS    == 3, "PF_MAGNIFIER_ENDPOINTS value");
    CHECK(PF_MAGNIFIER_BACK_LOADED  == 5, "PF_MAGNIFIER_BACK_LOADED value");

    /* ── Sizeof sanity (catches accidental change of fundamental
     *     struct membership) ─────────────────────────────────────── */
    CHECK(sizeof(pf_bar_t)            >= 6 * sizeof(double),
          "pf_bar_t too small for OHLCV + timestamp");
    CHECK(sizeof(pf_trade_tick_t)     == 32,
          "pf_trade_tick_t must remain a four-field 32-byte POD");
    CHECK(sizeof(pf_trade_t)          >= 80,
          "pf_trade_t unexpectedly small");
    CHECK(sizeof(pf_report_t)         >= 80 + sizeof(pf_metrics_t)
                                              + sizeof(pf_equity_point_t*)
                                              + sizeof(int64_t),
          "pf_report_t unexpectedly small (must include metrics + equity_curve ptr + len)");
    CHECK(sizeof(pf_security_diag_t)  == 4 + 8 + 8 + 8 + /* possible padding */ 0
          || sizeof(pf_security_diag_t) == 32,
          "pf_security_diag_t unexpected size");

    if (fail == 0) {
        printf("test_c_abi: OK (pineforge v%d.%d.%d, %zu-byte report)\n",
               v.major, v.minor, v.patch, sizeof(pf_report_t));
        return 0;
    }
    fprintf(stderr, "test_c_abi: %d FAILURES\n", fail);
    return 1;
}
