/*
 * native_acid_composite.h -- what the two halves of the acid composite share.
 *
 * Lane H-MEASURE, item A4-ACID-COVERAGE. tests/test_native_acid_composite.cpp drives the
 * scenario through NativeStrategyHost, tests/test_native_acid_composite_c.c drives the very same
 * commands through <pineforge/native_c_api.h>; both print one `FIELD key=value`
 * line per shared ledger fact and the two sets must be identical, run by run.
 * This header is plain C (C99/C11 and C++17 alike) and includes only the two
 * public C headers, so the C half really is compiled by a C compiler.
 *
 * What lives here, so neither half can drift from the other:
 *   - the tape: 88 fifteen-minute input bars (Mon 5 .. Thu 8 Oct 2026, UTC,
 *     session 0900-1500 Mon-Fri; Thursday stops at 12:45) and the 264
 *     five-minute auxiliary bars that aggregate exactly to them;
 *   - the run specification's numbers and the FX curve;
 *   - the scenario's bar indices (every command is issued at one of them);
 *   - the event projection: pf_native_event_v1 is what C reads back, and the
 *     C++ half flattens native_events() into the same POD (the translation
 *     src/native_c_host.cpp:1918-2033 performs) before the same digest.
 */
#ifndef PINEFORGE_TEST_NATIVE_ACID_COMPOSITE_H
#define PINEFORGE_TEST_NATIVE_ACID_COMPOSITE_H

#include <pineforge/pineforge.h>
#include <pineforge/native_c_api.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Calendar ──────────────────────────────────────────────────────── */
#define ACID_DAYS            4
#define ACID_SLOTS           24   /* 09:00 .. 14:45 */
#define ACID_LAST_DAY_SLOTS  16   /* Thursday stops at 12:45: mid-session */
#define ACID_BARS            (3 * ACID_SLOTS + ACID_LAST_DAY_SLOTS)   /* 88 */
#define ACID_FEED            (3 * ACID_BARS)                          /* 264 */
#define ACID_WARMUP          ACID_SLOTS   /* a stream warms up on Monday */
#define ACID_DAY0_ORDINAL    20731        /* days_from_civil(2026, 10, 5), a Monday */
#define ACID_MINUTE_MS       60000LL

/* Monday 5 Oct 2026 00:00 UTC. */
#define ACID_DAY0_MS ((int64_t)ACID_DAY0_ORDINAL * 1440LL * ACID_MINUTE_MS)

static inline int64_t acid_day_ms(int day) { return ACID_DAY0_MS + (int64_t)day * 1440LL * ACID_MINUTE_MS; }
static inline int acid_day_of(int gi) { return gi / ACID_SLOTS; }
static inline int acid_slot_of(int gi) { return gi % ACID_SLOTS; }
static inline int64_t acid_bar_ms(int gi) {
    return acid_day_ms(acid_day_of(gi)) + (540LL + 15LL * acid_slot_of(gi)) * ACID_MINUTE_MS;
}

/* ── The scenario: every command is issued at one of these bars ───────── */
enum {
    /* Monday: Sized entry + anchored bracket, take-profit, re-entry, re-issued exit. */
    G_E1 = 0, G_E1FILL = 1, G_TP = 5, G_E2FILL = 6, G_X_FIRST = 7, G_X_LAST = 14, G_XFILL = 15,
    /* Tuesday: entry + trail in ticks. */
    G_E3 = 24, G_E3FILL = 25, G_T3X = 28,
    /* Wednesday: two short lots, the FX step, the kernel liquidation, the risk block. */
    G_E4A = 48, G_E4B = 49, G_E4BFILL = 50, G_PRESTEP = 54, G_STEP = 56, G_PRESPIKE = 59,
    G_SPIKE = 60, G_E5REJ = 61, G_XFFILL = 62,
    /* Thursday: four lots, a FIFO partial close across lots, an exact-sum close. */
    G_D4 = 72, G_D4FILL = 73, G_P1 = 74, G_P1FILL = 75, G_P2 = 76, G_P2FILL = 77,
    G_LAST = ACID_BARS - 1
};

/* ── The run specification's numbers (both halves spell the same spec) ── */
#define ACID_CAPITAL        100000.0
#define ACID_POINT_VALUE    5.0
#define ACID_ACCOUNT_FX     1.20
#define ACID_TICK           0.25
#define ACID_SLIPPAGE       1u
#define ACID_FEE_PCT        0.1
#define ACID_QTY_GRID       0.01
#define ACID_INITIAL_LONG   0.20
#define ACID_MAINT_SHORT    0.25
#define ACID_RISK_LOSS      2500.0
#define ACID_MAX_RECALCS    2u
#define ACID_LIQ_LABEL      "ACID-desk"
#define ACID_LIQ_COMMENT    "maintenance 25%"
/* The FX curve (EUR -> USD): a point that RESTATES the scalar at Wed 10:00,
 * which is no step, and one real step at Wed 11:00. */
#define ACID_FX_T0          (acid_day_ms(2) + 600LL * ACID_MINUTE_MS)
#define ACID_FX_T1          (acid_day_ms(2) + 660LL * ACID_MINUTE_MS)
#define ACID_FX_R0          1.20
#define ACID_FX_R1          1.25

/* Scenario quantities. */
#define ACID_E1_CASH        24000.0
#define ACID_TP_OFFSET      3.1     /* price units, off the 0.25 ladder on purpose */
#define ACID_SL_OFFSET      (-2.3)
#define ACID_E2_UNITS       10.0
#define ACID_X_LEVEL0       201.00
#define ACID_E3_UNITS       8.0
#define ACID_T3_TICKS       6.0
#define ACID_E4A_UNITS      (-5.0)
#define ACID_E4B_UNITS      (-285.0)
#define ACID_E5_UNITS       (-10.0)
#define ACID_P1_UNITS       1.5

static const double acid_d4_units[4] = {1.1, 0.7, 0.2, 0.1};

/* ── The tape (prices in hundredths, so every aggregate is exact) ─────── */
typedef struct acid_t4 { int o, h, l, c; } acid_t4;

static inline acid_t4 acid_mk(int o, int h, int l, int c) {
    acid_t4 t;
    t.o = o; t.h = h; t.l = l; t.c = c;
    return t;
}
static inline acid_t4 acid_quiet(int o, int c) {
    return acid_mk(o, (o > c ? o : c) + 10, (o < c ? o : c) - 10, c);
}
static inline double acid_px(int hundredths) { return (double)hundredths / 100.0; }

/* The 88 input bars as hundredths. Every scripted bar is written out; the
 * quiet bars between them open at the previous close. */
static inline void acid_tape_t4(acid_t4* t) {
    static const acid_t4 mon[7] = {
        {19990, 20015, 19980, 20002},  /* 09:00 E1 + bracket submitted */
        {20010, 20040, 20000, 20030},  /* 09:15 E1 fills at the open 200.10 */
        {20030, 20090, 20020, 20080},
        {20080, 20190, 20070, 20170},
        {20170, 20290, 20160, 20280},
        {20290, 20410, 20270, 20360},  /* 10:15 the take-profit at 203.75 */
        {20355, 20420, 20330, 20350}   /* 10:30 E2 fills; X submitted */
    };
    static const int mon_x[8] = {20360, 20375, 20365, 20380, 20370, 20385, 20375, 20390};
    static const int mon_tail[8] = {20280, 20265, 20275, 20260, 20270, 20285, 20275, 20280};
    static const acid_t4 tue[5] = {
        {20480, 20500, 20470, 20495},  /* 09:00 E3 + trail submitted */
        {20505, 20630, 20500, 20610},  /* 09:15 E3 fills at the open 205.05 */
        {20610, 20720, 20590, 20700},
        {20700, 20760, 20660, 20740},
        {20735, 20745, 20580, 20600}   /* 10:00 the trail exits */
    };
    static const int tue_tail[8] = {20610, 20595, 20605, 20590, 20600, 20615, 20605, 20600};
    static const acid_t4 wed[3] = {
        {20980, 21000, 20960, 20990},  /* 09:00 E4a submitted */
        {21010, 21030, 20990, 21000},  /* 09:15 E4a fills; E4b submitted */
        {20995, 21010, 20970, 20980}   /* 09:30 E4b fills */
    };
    static const int wed_quiet[9] = {20990, 20975, 20985, 20970, 20980, 20995, 20985, 20990, 20980};
    static const acid_t4 wed_spike[3] = {
        {21010, 21300, 20990, 21240},  /* 12:00 the spike: the kernel liquidation */
        {21230, 21250, 21200, 21220},  /* 12:15 E5 refused (RiskLimit); flatten submitted */
        {21220, 21240, 21190, 21210}   /* 12:30 the flatten fills */
    };
    static const int wed_tail[9] = {21215, 21200, 21210, 21195, 21205, 21220, 21210, 21200, 21205};
    static const acid_t4 thu[6] = {
        {21100, 21120, 21080, 21110},  /* 09:00 E6..E9 submitted */
        {21115, 21140, 21100, 21130},  /* 09:15 E6..E9 fill at the open */
        {21130, 21150, 21110, 21140},  /* 09:30 P1 submitted */
        {21145, 21160, 21120, 21150},  /* 09:45 P1 fills */
        {21150, 21170, 21140, 21160},  /* 10:00 P2 (the exact sum) submitted */
        {21165, 21180, 21150, 21170}   /* 10:15 P2 fills */
    };
    static const int thu_tail[10] = {21200, 21230, 21260, 21290, 21320, 21350, 21380, 21410,
                                     21440, 21460};
    int gi = 0, i, prev;
    for (i = 0; i < 7; ++i) t[gi++] = mon[i];
    prev = mon[6].c;
    for (i = 0; i < 8; ++i) { t[gi] = acid_quiet(prev, mon_x[i]); prev = t[gi++].c; }
    t[gi] = acid_mk(prev, prev + 10, 20250, 20270); prev = t[gi++].c;   /* 12:45 X fills */
    for (i = 0; i < 8; ++i) { t[gi] = acid_quiet(prev, mon_tail[i]); prev = t[gi++].c; }
    for (i = 0; i < 5; ++i) t[gi++] = tue[i];
    prev = tue[4].c;
    for (i = 0; i < 19; ++i) { t[gi] = acid_quiet(prev, tue_tail[i % 8]); prev = t[gi++].c; }
    for (i = 0; i < 3; ++i) t[gi++] = wed[i];
    prev = wed[2].c;
    for (i = 0; i < 9; ++i) { t[gi] = acid_quiet(prev, wed_quiet[i]); prev = t[gi++].c; }
    for (i = 0; i < 3; ++i) t[gi++] = wed_spike[i];
    prev = wed_spike[2].c;
    for (i = 0; i < 9; ++i) { t[gi] = acid_quiet(prev, wed_tail[i]); prev = t[gi++].c; }
    for (i = 0; i < 6; ++i) t[gi++] = thu[i];
    prev = thu[5].c;
    for (i = 0; i < 10; ++i) { t[gi] = acid_quiet(prev, thu_tail[i]); prev = t[gi++].c; }
}

/* Three five-minute bars whose aggregate is exactly the input bar, walking its
 * extremes in the order the kernel's AUTO path walks them (the high first when
 * |H - O| < |O - L|). */
static inline void acid_split(acid_t4 b, acid_t4 f[3]) {
    const int high_first = (b.h - b.o < 0 ? b.o - b.h : b.h - b.o) <
                           (b.o - b.l < 0 ? b.l - b.o : b.o - b.l);
    int x1, x2;
    if (high_first) {
        x1 = (b.h + b.l) / 2; x2 = (b.l + b.c) / 2;
        f[0] = acid_mk(b.o, b.h, b.o < x1 ? b.o : x1, x1);
        f[1] = acid_mk(x1, x1 > x2 ? x1 : x2, b.l, x2);
    } else {
        x1 = (b.h + b.l) / 2; x2 = (b.h + b.c) / 2;
        f[0] = acid_mk(b.o, b.o > x1 ? b.o : x1, b.l, x1);
        f[1] = acid_mk(x1, b.h, x1 < x2 ? x1 : x2, x2);
    }
    f[2] = acid_mk(x2, x2 > b.c ? x2 : b.c, x2 < b.c ? x2 : b.c, b.c);
}

static inline void acid_build_tape(pf_bar_t* b15, pf_bar_t* b5) {
    acid_t4 t[ACID_BARS];
    int gi, k;
    acid_tape_t4(t);
    for (gi = 0; gi < ACID_BARS; ++gi) {
        acid_t4 f[3];
        double vol = 0.0;
        const int64_t t0 = acid_bar_ms(gi);
        acid_split(t[gi], f);
        for (k = 0; k < 3; ++k) {
            pf_bar_t* s = &b5[3 * gi + k];
            const double v5 = (double)(100 + (gi * 7 + k * 13) % 50);
            vol += v5;
            s->open = acid_px(f[k].o); s->high = acid_px(f[k].h);
            s->low = acid_px(f[k].l);  s->close = acid_px(f[k].c);
            s->volume = v5;
            s->timestamp = t0 + (int64_t)k * 5LL * ACID_MINUTE_MS;
        }
        b15[gi].open = acid_px(t[gi].o); b15[gi].high = acid_px(t[gi].h);
        b15[gi].low = acid_px(t[gi].l);  b15[gi].close = acid_px(t[gi].c);
        b15[gi].volume = vol;
        b15[gi].timestamp = t0;
    }
}

/* ── Digest (FNV-1a, 64-bit) ────────────────────────────────────────── */
#define ACID_FNV_OFFSET 14695981039346656037ull
static inline uint64_t acid_fnv_u64(uint64_t h, uint64_t v) {
    int i;
    for (i = 0; i < 8; ++i) {
        h ^= (v >> (8 * i)) & 0xffu;
        h *= 1099511628211ull;
    }
    return h;
}
static inline uint64_t acid_fnv_d(uint64_t h, double d) {
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    return acid_fnv_u64(h, u);
}
/* Every field of the flattened event, in declaration order. */
static inline uint64_t acid_event_fold(uint64_t h, const pf_native_event_v1* e) {
    h = acid_fnv_u64(h, e->kind);
    h = acid_fnv_u64(h, e->reason);
    h = acid_fnv_u64(h, e->ordinal);
    h = acid_fnv_u64(h, e->incarnation);
    h = acid_fnv_u64(h, e->successor);
    h = acid_fnv_d(h, e->raw_price);
    h = acid_fnv_d(h, e->resolved_price);
    h = acid_fnv_d(h, e->price);
    h = acid_fnv_d(h, e->closed_units);
    h = acid_fnv_d(h, e->opened_units);
    h = acid_fnv_u64(h, (uint64_t)e->cycle_before);
    h = acid_fnv_u64(h, (uint64_t)e->cycle_after);
    h = acid_fnv_u64(h, (uint64_t)e->effective_time_ms);
    h = acid_fnv_u64(h, (uint64_t)(int64_t)e->interval_index);
    h = acid_fnv_u64(h, e->provenance);
    h = acid_fnv_u64(h, e->path_phase);
    h = acid_fnv_u64(h, e->terminal);
    return h;
}

/* ── Formatting: %.17g round-trips every binary64 ─────────────────────── */
static inline const char* acid_d(char* buf, size_t n, double v) {
    if (v != v) snprintf(buf, n, "nan");
    else snprintf(buf, n, "%.17g", v);
    return buf;
}

/* ── The line sink the C half writes through ────────────────────────── */
typedef void (*acid_sink_fn)(void* user, const char* line);

/* The C half: runs the C port and hands every output line to `sink`.
 * Answers the number of C-side check failures. */
int acid_c_main(acid_sink_fn sink, void* user);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PINEFORGE_TEST_NATIVE_ACID_COMPOSITE_H */
