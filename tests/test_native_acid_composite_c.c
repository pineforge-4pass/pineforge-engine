/*
 * test_native_acid_composite_c.c -- the G1 acid composite, C half (lane H-MEASURE, item
 * A4-ACID-COVERAGE; extends the fourth audit's codex C port).
 *
 * The same strategy as test_native_acid_composite.cpp, written against
 * <pineforge/native_c_api.h> only: the same run specification (typed configure,
 * strategy_configure_native_ext_result_v1), the same FX curve (typed,
 * strategy_configure_native_fx_curve_ext_v1), the same commands at the same
 * bars, the bracket legs spelled by hand (the toolkit is C++-only,
 * native-engine.md:1052-1060), and the re-issued exit X spelled as a plain
 * replace (C has no ReplaceOptions word: C-SURFACE-2, ruled to 1.1.0). It
 * prints one `FIELD key=value` line per shared ledger fact -- the set the C++
 * half prints for the same runs -- plus one `EXCLUDED <family> <reason>` line
 * per C++-only family, and `PASS|FAIL C<n> ...` for the checks only C can make
 * (the typed configuration path on one handle, the interval query's phase).
 *
 * C11, no C++ construct: it includes only pineforge.h and native_c_api.h
 * (through native_acid_composite.h). Standalone it prints to stdout; compiled with
 * -DACID_C_NO_MAIN it is the C translation unit of the linked twin, whose C++
 * half compares the two FIELD sets in process.
 *
 *   (a CTest row: built with test_native_acid_composite.cpp, see tests/CMakeLists.txt)
 */
#include "native_acid_composite.h"

#include <stdarg.h>
#include <stdlib.h>

/* ── Output ─────────────────────────────────────────────────────────── */
static acid_sink_fn g_sink;
static void* g_user;
static int g_fail;

static void line(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_sink(g_user, buf);
}
static void cfield(const char* key, const char* fmt, ...) {
    char value[3584];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(value, sizeof value, fmt, ap);
    va_end(ap);
    line("FIELD %s=%s", key, value);
}
static void ccheck(const char* id, const char* name, int ok, const char* what) {
    line("%s %s %s: %s", ok ? "PASS" : "FAIL", id, name, what);
    if (!ok) ++g_fail;
}
#define D(buf, v) acid_d(buf, sizeof buf, (v))

/* ── Tape ───────────────────────────────────────────────────────────── */
static pf_bar_t g_b15[ACID_BARS];
static pf_bar_t g_b5[ACID_FEED];

static double ceil_tick(double p) { return ceil(p / ACID_TICK) * ACID_TICK; }
static double buy_fill(double open) { return ceil_tick(open) + ACID_TICK; }

/* ── Records ────────────────────────────────────────────────────────── */
enum { MAX_FILLS = 32, MAX_TFS = 128, MAX_RECALC = 32, MAX_CHECKS = 4096, MAX_REQS = 1024,
       MAX_CALLS = 8, MAX_LOT_SNAPS = 16, MAX_LOTS = 8, MAX_WSNAPS = 8, MAX_WROWS = 8,
       MAX_TAGGED = 16, MAX_REFUSALS = 16, MAX_ANCHORS = 8, MAX_TRAILS = 8, MAX_APPENDS = 4,
       MAX_XREP = 16 };

typedef struct { pf_native_open_lot_v1 lot; char label[32]; char comment[48]; } lot_rec;
typedef struct { char tag[16]; int n; lot_rec lots[MAX_LOTS]; } lots_snap;
typedef struct { pf_native_working_v1 w; char label[32]; } working_row;
typedef struct { char tag[16]; int n; working_row rows[MAX_WROWS]; } working_snap;
typedef struct { int gi; pf_native_applied_v1 a; pf_native_decision_v1 at; } fill_rec;
typedef struct { uint32_t sub; pf_bar_t bar; uint32_t comp; int64_t delivered_at;
                 pf_native_timeframe_interval_v1 iv; int iv_rc; int pulled; int gi; } tf_rec;
typedef struct { uint64_t cord, cinc; int gi; int has_partial; pf_bar_t partial;
                 pf_native_decision_v1 at; pf_native_risk_state_v1 risk; } recalc_rec;
typedef struct { char tag[16]; double v; } tagged_double;
typedef struct { char tag[16]; pf_native_risk_state_v1 r; } tagged_risk;
typedef struct { char id[40]; char word[48]; int same; } refusal_rec;
typedef struct { int gi; int rc; pf_native_trail_state_v1 s; } trail_rec;
typedef struct { char id[24]; uint32_t error; int32_t index; } append_rec;
typedef struct { double units, avg, eq; uint64_t lots; int working; } snap_t;

typedef struct host_state {
    /* options */
    const char* tag;
    int stream, magnify, bars;
    /* the handle */
    pf_strategy_t h;
    /* scenario */
    int nbars;
    uint64_t e1, tp, sl, e2, x, e3, t3, e4a, e4b, e5, xf, p1, p2, d4[4];
    int tp_accepted, sl_accepted, x_done;
    int inputs, sub_bars, begins;
    int interval_outside_rc;      /* the interval query outside on_timeframe_bar */
    int interval_inside_bad;      /* inside it: how many answered anything but OK */
    /* records */
    pf_native_decision_v1 bar_at[ACID_BARS];
    pf_native_risk_state_v1 bar_risk[ACID_BARS];
    fill_rec fills[MAX_FILLS]; int nfills;
    tf_rec tfs[MAX_TFS]; int ntfs;
    recalc_rec recalcs[MAX_RECALC]; int nrecalcs;
    pf_native_margin_view_v1 checks[MAX_CHECKS]; int nchecks;
    pf_native_margin_view_v1 reqs[MAX_REQS]; int nreqs;
    pf_native_margin_view_v1 callviews[MAX_CALLS]; int ncallviews;
    pf_native_margin_call_v1 calls[MAX_CALLS]; int ncalls;
    lots_snap lots_before, lots_after; int lots_before_taken;
    lots_snap lot_snaps[MAX_LOT_SNAPS]; int nlot_snaps;
    working_snap wsnaps[MAX_WSNAPS]; int nwsnaps;
    tagged_double liq[MAX_TAGGED]; int nliq;
    tagged_risk risks[MAX_TAGGED]; int nrisks;
    tagged_double sized[4]; int nsized;
    refusal_rec refusals[MAX_REFUSALS]; int nrefusals;
    pf_native_anchored_level_view_v1 anchors[MAX_ANCHORS]; int nanchors;
    trail_rec trails[MAX_TRAILS]; int ntrails;
    append_rec appends[MAX_APPENDS]; int nappends;
    char x_replaces[MAX_XREP][24]; int nxrep;
} host_state;

/* ── Small helpers ──────────────────────────────────────────────────── */
static pf_native_request_v1 req0(uint32_t intent, double value, const char* label, const char* comment) {
    pf_native_request_v1 r;
    memset(&r, 0, sizeof r);
    r.struct_size = (uint32_t)sizeof r;
    r.version = PF_NATIVE_API_VERSION;
    r.intent = intent;
    r.intent_value = value;
    r.label = label;
    r.comment = comment;
    return r;
}
static const char* reject_word(uint32_t r) {
    switch (r) {
    case PF_NATIVE_REJECT_INVALID_QUANTITY: return "InvalidQuantity";
    case PF_NATIVE_REJECT_OFF_GRID: return "OffGrid";
    case PF_NATIVE_REJECT_INVALID_TRIGGER: return "InvalidTrigger";
    case PF_NATIVE_REJECT_INVALID_CAPACITY: return "InvalidCapacity";
    case PF_NATIVE_REJECT_INVALID_OWNER: return "InvalidOwner";
    case PF_NATIVE_REJECT_INVALID_QUANTITY_BASIS: return "InvalidQuantityBasis";
    case PF_NATIVE_REJECT_INVALID_GROUP: return "InvalidGroup";
    case PF_NATIVE_REJECT_PLACEMENT_ADMISSION: return "PlacementAdmission";
    default: return "?";
    }
}
static const char* status_word(int rc) {
    switch (rc) {
    case PF_NATIVE_OK: return "Replaced";
    case PF_NATIVE_E_REJECTED: return "ReplaceRejected";
    case PF_NATIVE_E_NOT_WORKING: return "NotWorking";
    case PF_NATIVE_E_INVALID_TARGET: return "InvalidHandle";
    default: return "?";
    }
}
static const char* append_word(uint32_t e) {
    switch (e) {
    case PF_NATIVE_APPEND_ERROR_NONE: return "None";
    case PF_NATIVE_APPEND_ERROR_HOST_FAILED: return "HostFailed";
    case PF_NATIVE_APPEND_ERROR_REENTRANT: return "Reentrant";
    case PF_NATIVE_APPEND_ERROR_NOT_REALTIME: return "NotRealtime";
    case PF_NATIVE_APPEND_ERROR_NO_AUXILIARY_FEED: return "NoAuxiliaryFeed";
    case PF_NATIVE_APPEND_ERROR_INVALID_BAR_ARRAY: return "InvalidBarArray";
    case PF_NATIVE_APPEND_ERROR_INVALID_BAR: return "InvalidBar";
    case PF_NATIVE_APPEND_ERROR_UNORDERED_BARS: return "UnorderedBars";
    case PF_NATIVE_APPEND_ERROR_INPUT_PERIOD_ALREADY_ACCEPTED: return "InputPeriodAlreadyAccepted";
    case PF_NATIVE_APPEND_ERROR_ALLOCATION_FAILURE: return "AllocationFailure";
    default: return "?";
    }
}
/* Submit and answer the word the C++ half prints for the same SubmitResult. */
static uint64_t submit_w(host_state* st, const pf_native_request_v1* r, char* word, size_t n) {
    uint64_t inc = 0;
    uint32_t reject = 0;
    const int rc = strategy_native_submit_v1(st->h, r, &inc, &reject);
    if (word) {
        if (rc == PF_NATIVE_OK) snprintf(word, n, "Accepted");
        else if (rc == PF_NATIVE_E_REJECTED) snprintf(word, n, "Rejected/%s", reject_word(reject));
        else snprintf(word, n, "C-error/%d", rc);
    }
    return rc == PF_NATIVE_OK ? inc : 0;
}
static uint64_t submit(host_state* st, const pf_native_request_v1* r) { return submit_w(st, r, NULL, 0); }

static snap_t snap(host_state* st, double mark) {
    snap_t s;
    memset(&s, 0, sizeof s);
    strategy_native_position_v1(st->h, &s.units, &s.avg, &s.lots);
    strategy_native_marked_equity_v1(st->h, mark, &s.eq);
    s.working = strategy_native_working_len_v1(st->h);
    return s;
}
static int same_snap(snap_t a, snap_t b) {
    return memcmp(&a.units, &b.units, sizeof(double)) == 0 && memcmp(&a.avg, &b.avg, sizeof(double)) == 0
           && memcmp(&a.eq, &b.eq, sizeof(double)) == 0 && a.lots == b.lots && a.working == b.working;
}
static void refusal(host_state* st, const char* id, const char* word, snap_t before, double mark) {
    refusal_rec* r;
    if (st->nrefusals >= MAX_REFUSALS) return;
    r = &st->refusals[st->nrefusals++];
    snprintf(r->id, sizeof r->id, "%s", id);
    snprintf(r->word, sizeof r->word, "%s", word);
    r->same = same_snap(snap(st, mark), before);
}
static void take_lots(host_state* st, lots_snap* out, const char* tag, double mark) {
    int i, n = strategy_native_open_lot_count_v1(st->h, mark);
    snprintf(out->tag, sizeof out->tag, "%s", tag);
    out->n = 0;
    for (i = 0; i < n && i < MAX_LOTS; ++i) {
        lot_rec* l = &out->lots[out->n];
        memset(&l->lot, 0, sizeof l->lot);
        l->lot.struct_size = (uint32_t)sizeof l->lot;
        l->lot.version = PF_NATIVE_API_VERSION;
        if (strategy_native_open_lot_get_v1(st->h, i, &l->lot) != PF_NATIVE_OK) continue;
        snprintf(l->label, sizeof l->label, "%s", l->lot.entry_label ? l->lot.entry_label : "");
        snprintf(l->comment, sizeof l->comment, "%s", l->lot.entry_comment ? l->lot.entry_comment : "");
        ++out->n;
    }
}
static void snap_lots(host_state* st, const char* tag, double mark) {
    if (st->nlot_snaps < MAX_LOT_SNAPS) take_lots(st, &st->lot_snaps[st->nlot_snaps++], tag, mark);
}
static void snap_working(host_state* st, const char* tag) {
    working_snap* w;
    int i, n;
    if (st->nwsnaps >= MAX_WSNAPS) return;
    w = &st->wsnaps[st->nwsnaps++];
    snprintf(w->tag, sizeof w->tag, "%s", tag);
    w->n = 0;
    n = strategy_native_working_len_v1(st->h);
    for (i = 0; i < n && i < MAX_WROWS; ++i) {
        working_row* row = &w->rows[w->n];
        memset(&row->w, 0, sizeof row->w);
        row->w.struct_size = (uint32_t)sizeof row->w;
        row->w.version = PF_NATIVE_API_VERSION;
        if (strategy_native_working_get_v1(st->h, i, &row->w) != PF_NATIVE_OK) continue;
        snprintf(row->label, sizeof row->label, "%s", row->w.label ? row->w.label : "");
        ++w->n;
    }
}
static void snap_liq(host_state* st, const char* tag) {
    tagged_double* t;
    if (st->nliq >= MAX_TAGGED) return;
    t = &st->liq[st->nliq++];
    snprintf(t->tag, sizeof t->tag, "%s", tag);
    t->v = NAN;
    strategy_native_liquidation_price_v1(st->h, &t->v);   /* NaN when absent */
}
static pf_native_risk_state_v1 risk_now(host_state* st) {
    pf_native_risk_state_v1 r;
    memset(&r, 0, sizeof r);
    r.struct_size = (uint32_t)sizeof r;
    r.version = PF_NATIVE_API_VERSION;
    strategy_native_risk_state_v1(st->h, &r);
    return r;
}
static void snap_risk(host_state* st, const char* tag) {
    tagged_risk* t;
    if (st->nrisks >= MAX_TAGGED) return;
    t = &st->risks[st->nrisks++];
    snprintf(t->tag, sizeof t->tag, "%s", tag);
    t->r = risk_now(st);
}

/* ── The scenario: Monday 09:00 ─────────────────────────────────────── */
static pf_native_request_v1 sized_req(double cash, uint32_t time, int reserve, const char* label,
                                      const char* comment) {
    pf_native_request_v1 r = req0(PF_NATIVE_INTENT_SIZED, cash, label, comment);
    r.side = PF_NATIVE_SIDE_LONG;
    r.size_basis = PF_NATIVE_SIZE_BASIS_CASH;
    r.size_time = time;
    r.grid_policy = PF_NATIVE_GRID_SNAP;
    r.size_price = PF_NATIVE_SIZE_PRICE_RESOLVED;
    r.reserve_percent_fee = (uint32_t)reserve;
    return r;
}
static pf_native_request_v1 leg_req(uint64_t* parent, uint32_t trigger, double offset, uint32_t rounding,
                                    const char* label) {
    pf_native_request_v1 r = req0(PF_NATIVE_INTENT_REDUCE, 0.0, label, "bracket");
    r.reduce_size = PF_NATIVE_REDUCE_OWNER_OPENED;
    r.trigger = trigger;
    r.p1 = 0.0;
    r.anchor = PF_NATIVE_ANCHOR_FROM_OWNER_FILL;
    r.anchor_offset = offset;
    r.anchor_rounding = rounding;
    r.owner = PF_NATIVE_OWNER_WAIT_FOR_APPLIED;
    r.owner_n = 1;
    r.owner_incarnations = parent;
    return r;
}

static void monday_open(host_state* st, const pf_bar_t* bar) {
    const double mark = bar->close;
    snap_t s0 = snap(st, mark), s1;
    char word[64];
    pf_native_request_v1 r;
    {
        r = req0(PF_NATIVE_INTENT_TRANSACT, 0.0, "R01", "zero");
        submit_w(st, &r, word, sizeof word);
        refusal(st, "R01.transact_zero", word, s0, mark);
        r = req0(PF_NATIVE_INTENT_TRANSACT, 0.005, "R02", "off grid");
        submit_w(st, &r, word, sizeof word);
        refusal(st, "R02.off_grid", word, s0, mark);
        r = sized_req(-5.0, PF_NATIVE_SIZE_AT_MATCH, 0, "R03", "negative cash");
        submit_w(st, &r, word, sizeof word);
        refusal(st, "R03.bad_basis", word, s0, mark);
        r = sized_req(5000000.0, PF_NATIVE_SIZE_AT_ACCEPTANCE, 0, "R04", "too big");
        submit_w(st, &r, word, sizeof word);
        refusal(st, "R04.placement", word, s0, mark);
    }
    {
        uint32_t err = 999, field = 999;
        const int rc = strategy_native_declare_subscriptions_ext_v1(st->h, NULL, 0, NULL, &err, &field);
        snprintf(word, sizeof word, "%s/%u/%u", rc == PF_NATIVE_OK ? "Applied" : "Failed", err, field);
        refusal(st, "R05.declare_outside_begin", word, s0, mark);
    }
    {
        int64_t t = bar->timestamp + 1;
        double rate = 2.0;
        pf_native_fx_curve_v1 c;
        pf_native_fx_curve_error_t err = PF_NATIVE_FX_CURVE_ERROR_NONE;
        uint64_t idx = 0;
        int rc;
        memset(&c, 0, sizeof c);
        c.struct_size = (uint32_t)sizeof c;
        c.n = 1;
        c.effective_from_ms = &t;
        c.account_per_quote = &rate;
        rc = strategy_configure_native_fx_curve_ext_v1(st->h, &c, &err, &idx);
        snprintf(word, sizeof word, "%s/%d/%llu", rc == 0 ? "Applied" : "Failed", (int)err,
                 (unsigned long long)idx);
        refusal(st, "R06.fx_curve_while_running", word, s0, mark);
    }
    {
        /* F01: the kernel's sizing as a pure query, at the candidate's price. */
        const double px = buy_fill(g_b15[G_E1FILL].open);
        double u = NAN;
        r = sized_req(ACID_E1_CASH, PF_NATIVE_SIZE_AT_MATCH, 1, "E1", "sized entry");
        strategy_native_sized_units_v1(st->h, &r, px, 0.0, ACID_ACCOUNT_FX, &u);
        snprintf(st->sized[0].tag, sizeof st->sized[0].tag, "E1.reserve");
        st->sized[0].v = u;
        r.reserve_percent_fee = 0;
        u = NAN;
        strategy_native_sized_units_v1(st->h, &r, px, 0.0, ACID_ACCOUNT_FX, &u);
        snprintf(st->sized[1].tag, sizeof st->sized[1].tag, "E1.no_reserve");
        st->sized[1].v = u;
        st->nsized = 2;
    }
    r = sized_req(ACID_E1_CASH, PF_NATIVE_SIZE_AT_MATCH, 1, "E1", "sized entry");
    st->e1 = submit(st, &r);
    if (!st->e1) return;
    /* The bracket the toolkit builds (native_toolkit.hpp:138-183), by hand:
     * WAIT_FOR_APPLIED children of E1 in one OCA group (the parent's
     * incarnation), one cohort per leg, Directional rounding, pending. */
    r = leg_req(&st->e1, PF_NATIVE_TRIGGER_LIMIT, ACID_TP_OFFSET, PF_NATIVE_ANCHOR_ROUNDING_DIRECTIONAL, "TP");
    r.visibility = PF_NATIVE_ARM_VISIBILITY_PENDING_UNTIL_ARMED;
    r.group_kind = PF_NATIVE_GROUP_MEMBER;
    r.group_id = st->e1;
    r.group_cohort = 1;
    r.group_effect = PF_NATIVE_GROUP_CANCEL;
    st->tp = submit(st, &r);
    st->tp_accepted = st->tp != 0;
    r = leg_req(&st->e1, PF_NATIVE_TRIGGER_STOP, ACID_SL_OFFSET, PF_NATIVE_ANCHOR_ROUNDING_DIRECTIONAL, "SL");
    r.visibility = PF_NATIVE_ARM_VISIBILITY_PENDING_UNTIL_ARMED;
    r.group_kind = PF_NATIVE_GROUP_MEMBER;
    r.group_id = st->e1;
    r.group_cohort = 2;
    r.group_effect = PF_NATIVE_GROUP_CANCEL;
    st->sl = submit(st, &r);
    st->sl_accepted = st->sl != 0;
    snap_working(st, "E1");
    s1 = snap(st, mark);
    r = leg_req(&st->e1, PF_NATIVE_TRIGGER_LIMIT, ACID_TP_OFFSET, PF_NATIVE_ANCHOR_ROUNDING_RAW, "R07");
    r.p1 = 203.0;   /* a written anchored level */
    submit_w(st, &r, word, sizeof word);
    refusal(st, "R07.written_anchor", word, s1, mark);
    r = sized_req(1000.0, PF_NATIVE_SIZE_AT_MATCH, 0, "R08", "sized child");
    r.owner = PF_NATIVE_OWNER_WAIT_FOR_APPLIED;
    r.owner_n = 1;
    r.owner_incarnations = &st->e1;
    submit_w(st, &r, word, sizeof word);
    refusal(st, "R08.sized_child", word, s1, mark);
}

static void reissue_x(host_state* st, int gi) {
    const double level = ACID_X_LEVEL0 + ACID_TICK * (double)(gi - G_E2FILL);
    pf_native_request_v1 r;
    uint64_t succ = 0;
    uint32_t reject = 0;
    int rc;
    if (!st->x || st->x_done) return;
    r = req0(PF_NATIVE_INTENT_FLATTEN, 0.0, "X", "protect");
    r.trigger = PF_NATIVE_TRIGGER_STOP;
    r.p1 = level;
    /* C has no ReplaceOptions (native_host.hpp:1206): a plain replace. */
    rc = strategy_native_replace_ext_v1(st->h, st->x, &r, &succ, &reject);
    if (st->nxrep < MAX_XREP) snprintf(st->x_replaces[st->nxrep++], 24, "%s", status_word(rc));
    if (rc == PF_NATIVE_OK) st->x = succ;
}

static void on_bar_close(host_state* st, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    const int gi = st->nbars++;
    const double mark = bar->close;
    pf_native_request_v1 r;
    int k;
    if (gi < ACID_BARS) {
        st->bar_at[gi] = *at;
        st->bar_risk[gi] = risk_now(st);
    }
    {   /* The interval query answers only inside on_timeframe_bar. */
        pf_native_timeframe_interval_v1 iv;
        memset(&iv, 0, sizeof iv);
        iv.struct_size = (uint32_t)sizeof iv;
        iv.version = PF_NATIVE_API_VERSION;
        if (gi == G_E1) st->interval_outside_rc = strategy_native_timeframe_bar_interval_v1(st->h, &iv);
    }
    if (gi == G_E1) monday_open(st, bar);
    if (gi == G_E1FILL) {
        snap_lots(st, "E1FILL", mark);
        snap_working(st, "E1FILL");
    }
    if (gi == G_E2FILL) {
        snap_t s0 = snap(st, mark);
        if (st->e1) {
            uint64_t succ = 0;
            uint32_t reject = 0;
            r = req0(PF_NATIVE_INTENT_TRANSACT, 1.0, "R09", "");
            refusal(st, "R09.replace_terminal",
                    status_word(strategy_native_replace_ext_v1(st->h, st->e1, &r, &succ, &reject)), s0, mark);
        }
        if (st->sl) refusal(st, "R10.cancel_cancelled_leg", status_word(strategy_native_cancel_v1(st->h, st->sl)), s0, mark);
        snap_lots(st, "E2FILL", mark);
        r = req0(PF_NATIVE_INTENT_FLATTEN, 0.0, "X", "protect");
        r.trigger = PF_NATIVE_TRIGGER_STOP;
        r.p1 = ACID_X_LEVEL0;
        st->x = submit(st, &r);
    }
    if (gi >= G_X_FIRST && gi <= G_X_LAST) reissue_x(st, gi);
    if (gi == G_XFILL) snap_lots(st, "XFILL", mark);
    if (gi == G_E3) {
        r = req0(PF_NATIVE_INTENT_TRANSACT, ACID_E3_UNITS, "E3", "trail entry");
        st->e3 = submit(st, &r);
        if (st->e3) {
            r = req0(PF_NATIVE_INTENT_REDUCE, 0.0, "T3", "trail");
            r.reduce_size = PF_NATIVE_REDUCE_OWNER_OPENED;
            r.trigger = PF_NATIVE_TRIGGER_TRAIL;
            r.p1 = ACID_T3_TICKS;
            r.trail_offset_in_ticks = 1;
            r.owner = PF_NATIVE_OWNER_WAIT_FOR_APPLIED;
            r.owner_n = 1;
            r.owner_incarnations = &st->e3;
            st->t3 = submit(st, &r);
        }
        snap_working(st, "E3");
    }
    if (gi == G_E3FILL) snap_lots(st, "E3FILL", mark);
    if (st->t3 && gi >= G_E3FILL && gi <= G_T3X && st->ntrails < MAX_TRAILS) {
        trail_rec* t = &st->trails[st->ntrails++];
        memset(&t->s, 0, sizeof t->s);
        t->s.struct_size = (uint32_t)sizeof t->s;
        t->s.version = PF_NATIVE_API_VERSION;
        t->gi = gi;
        t->rc = strategy_native_trail_state_v1(st->h, st->t3, &t->s);
    }
    if (gi == G_E4A) {
        r = req0(PF_NATIVE_INTENT_TRANSACT, ACID_E4A_UNITS, "E4a", "short");
        st->e4a = submit(st, &r);
    }
    if (gi == G_E4B) {
        r = req0(PF_NATIVE_INTENT_TRANSACT, ACID_E4B_UNITS, "E4b", "short");
        st->e4b = submit(st, &r);
    }
    if (gi == G_E4BFILL) {
        snap_lots(st, "E4BFILL", mark);
        snap_liq(st, "E4BFILL");
    }
    if (gi == G_PRESTEP) snap_liq(st, "PRESTEP");
    if (gi == G_STEP) snap_liq(st, "STEP");
    if (gi == G_PRESPIKE) snap_liq(st, "PRESPIKE");
    if (gi == G_SPIKE) {
        snap_liq(st, "SPIKE");
        snap_lots(st, "SPIKE", mark);
        snap_risk(st, "SPIKE");
        r = req0(PF_NATIVE_INTENT_TRANSACT, ACID_E5_UNITS, "E5", "add while blocked");
        st->e5 = submit(st, &r);
    }
    if (gi == G_E5REJ) {
        snap_risk(st, "E5REJ");
        r = req0(PF_NATIVE_INTENT_FLATTEN, 0.0, "XF", "flatten while blocked");
        st->xf = submit(st, &r);
    }
    if (gi == G_XFFILL) {
        snap_liq(st, "XFFILL");
        snap_lots(st, "XFFILL", mark);
    }
    if (gi == G_D4) {
        static const char* labels[4] = {"E6", "E7", "E8", "E9"};
        snap_risk(st, "D4");
        for (k = 0; k < 4; ++k) {
            r = req0(PF_NATIVE_INTENT_TRANSACT, acid_d4_units[k], labels[k], "thursday");
            st->d4[k] = submit(st, &r);
        }
        snap_working(st, "D4");
    }
    if (gi == G_D4FILL) snap_lots(st, "D4FILL", mark);
    if (gi == G_P1) {
        r = req0(PF_NATIVE_INTENT_REDUCE, ACID_P1_UNITS, "P1", "fifo partial");
        r.reduce_size = PF_NATIVE_REDUCE_EXPLICIT_UNITS;
        st->p1 = submit(st, &r);
    }
    if (gi == G_P1FILL) snap_lots(st, "P1FILL", mark);
    if (gi == G_P2) {
        lots_snap book;
        take_lots(st, &book, "book", mark);
        if (book.n >= 2) {
            /* the binary64 sum of the first two lots */
            const double units = book.lots[0].lot.signed_units + book.lots[1].lot.signed_units;
            r = req0(PF_NATIVE_INTENT_REDUCE, units, "P2", "exact sum");
            r.reduce_size = PF_NATIVE_REDUCE_EXPLICIT_UNITS;
            st->p2 = submit(st, &r);
        }
    }
    if (gi == G_P2FILL) snap_lots(st, "P2FILL", mark);
    if (gi == st->bars - 1) {
        snap_lots(st, "LAST", mark);
        snap_risk(st, "LAST");
    }
}

/* ── Callbacks ──────────────────────────────────────────────────────── */
static int cb_run_begin(void* user) { ++((host_state*)user)->begins; return 0; }
static int cb_input(void* user, const pf_bar_t* bar, int32_t i, int32_t c) {
    (void)bar; (void)i; (void)c;
    ++((host_state*)user)->inputs;
    return 0;
}
static int cb_sub_bar(void* user, const pf_bar_t* sub, const pf_native_decision_v1* at) {
    (void)sub; (void)at;
    ++((host_state*)user)->sub_bars;
    return 0;
}
static int cb_bar(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at) {
    on_bar_close((host_state*)user, bar, at);
    return 0;
}
static int cb_recalculate(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at, uint32_t reason,
                          const pf_native_applied_v1* cause) {
    host_state* st = (host_state*)user;
    recalc_rec* rc;
    if (reason == PF_NATIVE_CALC_BAR_CLOSE) {
        on_bar_close(st, bar, at);
        return 0;
    }
    if (reason != PF_NATIVE_CALC_ORDER_FILL || st->nrecalcs >= MAX_RECALC) return 0;
    rc = &st->recalcs[st->nrecalcs++];
    memset(rc, 0, sizeof *rc);
    rc->gi = st->nbars;
    if (cause) {
        rc->cord = cause->ordinal;
        rc->cinc = cause->incarnation;
    }
    rc->has_partial = strategy_native_partial_bar_v1(st->h, &rc->partial) == PF_NATIVE_OK;
    rc->at = *at;
    rc->risk = risk_now(st);
    /* F08: a request born in the take-profit's fill recalculation. */
    if (cause && st->tp && cause->incarnation == st->tp && !st->e2) {
        pf_native_request_v1 r = req0(PF_NATIVE_INTENT_TRANSACT, ACID_E2_UNITS, "E2", "re-entry on the fill");
        st->e2 = submit(st, &r);
    }
    return 0;
}
static int cb_applied(void* user, const pf_native_applied_v1* a, const pf_native_decision_v1* at) {
    host_state* st = (host_state*)user;
    if (st->nfills < MAX_FILLS) {
        fill_rec* f = &st->fills[st->nfills++];
        f->gi = st->nbars;
        f->a = *a;
        f->at = *at;
    }
    if (st->x && a->incarnation == st->x) st->x_done = 1;
    return 0;
}
static int cb_timeframe_bar(void* user, const pf_bar_t* bar, uint32_t sub, uint32_t completion,
                            int64_t delivered_at) {
    host_state* st = (host_state*)user;
    tf_rec* t;
    pf_bar_t pulled;
    if (st->ntfs >= MAX_TFS) return 0;
    t = &st->tfs[st->ntfs++];
    memset(t, 0, sizeof *t);
    t->sub = sub;
    t->bar = *bar;
    t->comp = completion;
    t->delivered_at = delivered_at;
    t->gi = st->nbars;
    t->iv.struct_size = (uint32_t)sizeof t->iv;
    t->iv.version = PF_NATIVE_API_VERSION;
    t->iv_rc = strategy_native_timeframe_bar_interval_v1(st->h, &t->iv);
    if (t->iv_rc != PF_NATIVE_OK) ++st->interval_inside_bad;
    memset(&pulled, 0, sizeof pulled);
    t->pulled = strategy_native_series_bar_v1(st->h, sub, &pulled) == PF_NATIVE_OK
                && memcmp(&pulled, bar, sizeof pulled) == 0;
    return 0;
}
static int cb_margin_call(void* user, const pf_native_event_v1* e) {
    host_state* st = (host_state*)user;
    if (st->ncalls < MAX_CALLS) {
        pf_native_margin_call_v1* c = &st->calls[st->ncalls];
        memset(c, 0, sizeof *c);
        c->struct_size = (uint32_t)sizeof *c;
        c->version = PF_NATIVE_API_VERSION;
        if (strategy_native_margin_call_v1(st->h, e->ordinal, c) == PF_NATIVE_OK) {
            ++st->ncalls;
            take_lots(st, &st->lots_after, "CALLAFTER", c->mark);
        }
    }
    return 0;
}
static int cb_margin_check(void* user, const pf_native_margin_view_v1* v, int32_t* allowed) {
    host_state* st = (host_state*)user;
    (void)allowed;
    if (st->nchecks < MAX_CHECKS) st->checks[st->nchecks++] = *v;
    return PF_NATIVE_ANSWER_DEFAULT;
}
static int cb_margin_requirement(void* user, const pf_native_margin_view_v1* v, pf_native_margin_decision_v1* out) {
    host_state* st = (host_state*)user;
    (void)out;
    if (st->nreqs < MAX_REQS) st->reqs[st->nreqs++] = *v;
    return PF_NATIVE_ANSWER_DEFAULT;
}
static int cb_margin_call_units(void* user, const pf_native_margin_view_v1* v, double* units) {
    host_state* st = (host_state*)user;
    (void)units;
    if (st->ncallviews < MAX_CALLS) st->callviews[st->ncallviews++] = *v;
    if (!st->lots_before_taken) {
        take_lots(st, &st->lots_before, "CALLBEFORE", v->mark);
        st->lots_before_taken = 1;
    }
    return PF_NATIVE_ANSWER_DEFAULT;
}
static int cb_anchored_level(void* user, const pf_native_anchored_level_view_v1* v, double* level) {
    host_state* st = (host_state*)user;
    (void)level;
    if (st->nanchors < MAX_ANCHORS) st->anchors[st->nanchors++] = *v;
    return PF_NATIVE_ANSWER_DEFAULT;
}

/* ── The run specification, spelled in C ────────────────────────────── */
static pf_native_run_spec_v1 base_spec(double tick) {
    pf_native_run_spec_v1 s;
    memset(&s, 0, sizeof s);
    s.struct_size = (uint32_t)sizeof s;
    s.session_key = "acid-composite";
    s.run_number = 1;
    s.input_tf = "15";
    s.script_tf = "15";
    s.ticker = "ACIDX";
    s.tickerid = "ACID:ACIDX";
    s.type = "cfd";
    s.currency = "EUR";
    s.basecurrency = "";
    s.description = "acid composite";
    s.volumetype = "";
    s.timezone = "UTC";
    s.session = "0900-1500:23456";
    s.chart_timezone = "";
    s.initial_capital = ACID_CAPITAL;
    s.point_value = ACID_POINT_VALUE;
    s.account_fx = ACID_ACCOUNT_FX;
    s.price_tick = tick;
    s.slippage_ticks = ACID_SLIPPAGE;
    s.fee_kind = PF_NATIVE_FEE_PERCENT;
    s.fee_value = ACID_FEE_PCT;
    s.optional_mask = PF_NATIVE_SPEC_OPTIONAL_QUANTITY_GRID;
    s.quantity_grid = ACID_QTY_GRID;
    s.close_execution = PF_NATIVE_CLOSE_EXECUTION_NEXT_ELIGIBLE_POINT;
    s.allowed_open_directions = PF_NATIVE_OPEN_DIRECTIONS_BOTH;
    return s;
}
static pf_native_subscription_v1 g_subs[2];
static const uint32_t g_sources[2] = {PF_NATIVE_SERIES_SOURCE_INPUT, PF_NATIVE_SERIES_SOURCE_AUXILIARY_FEED};
static pf_native_run_spec_ext_v1 ext_spec(const host_state* st) {
    pf_native_run_spec_ext_v1 e;
    memset(&e, 0, sizeof e);
    memset(g_subs, 0, sizeof g_subs);
    g_subs[0].struct_size = (uint32_t)sizeof g_subs[0];
    g_subs[0].tf = "60";
    g_subs[1].struct_size = (uint32_t)sizeof g_subs[1];
    g_subs[1].tf = "30";
    e.struct_size = (uint32_t)sizeof e;
    e.version = PF_NATIVE_API_VERSION;
    e.present_mask = PF_NATIVE_SPEC_EXT_REPORT | PF_NATIVE_SPEC_EXT_PRICE_GRID | PF_NATIVE_SPEC_EXT_CALCULATION
                     | PF_NATIVE_SPEC_EXT_MARGIN | PF_NATIVE_SPEC_EXT_SUBSCRIPTIONS | PF_NATIVE_SPEC_EXT_RISK
                     | PF_NATIVE_SPEC_EXT_AUXILIARY_FEED | PF_NATIVE_SPEC_EXT_EVENT_RETENTION;
    e.report_policy = PF_NATIVE_REPORT_KERNEL_RECORDED;
    e.report_open_position_at_end = 1;
    e.price_grid = PF_NATIVE_PRICE_GRID_QUANTIZE_FILLS_AND_TRIGGERS;
    e.grid_rounding = PF_NATIVE_GRID_ROUNDING_DIRECTIONAL;
    e.calculation = PF_NATIVE_CALC_TRIGGER_BAR_CLOSE_AND_FILLS;
    e.max_recalculations_per_point = ACID_MAX_RECALCS;
    e.open_bar_view = PF_NATIVE_OPEN_BAR_VIEW_COMPLETE;
    e.margin_sizing = PF_NATIVE_LIQUIDATION_SIZING_RESTORE_MINIMUM;
    e.margin_check = PF_NATIVE_LIQUIDATION_PATH_ADVERSE_EXTREME;
    e.margin_initial_long = ACID_INITIAL_LONG;
    e.margin_initial_short = 0.0;                  /* the maintenance-only side */
    e.margin_has_maintenance_short = 1;
    e.margin_maintenance_short = ACID_MAINT_SHORT;
    e.margin_shortfall_multiple = 1.0;
    e.margin_equity_basis = PF_NATIVE_MARGIN_EQUITY_MARKED;
    e.margin_level_base = PF_NATIVE_MARGIN_LEVEL_MARKED_EQUITY;
    e.margin_liquidation_label = ACID_LIQ_LABEL;
    e.margin_liquidation_comment = ACID_LIQ_COMMENT;
    e.subscriptions = g_subs;
    e.subscriptions_n = 2;
    e.subscription_sources = g_sources;
    e.risk_has_max_intraday_loss = 1;
    e.risk_max_intraday_loss = ACID_RISK_LOSS;
    e.risk_day_basis = PF_NATIVE_RISK_DAY_SESSION;
    e.risk_action = PF_NATIVE_RISK_BLOCK_OPENINGS;
    e.auxiliary_tf = "5";
    e.auxiliary_bars = g_b5;
    e.auxiliary_n = st->stream ? 3 * ACID_WARMUP : ACID_FEED;
    e.event_retention = PF_NATIVE_EVENT_RETENTION_FULL;
    if (st->magnify) {
        e.present_mask |= PF_NATIVE_SPEC_EXT_INTRABAR;
        e.intrabar_kind = PF_NATIVE_INTRABAR_LOWER_TF;
        e.intrabar_samples = 4;
        e.intrabar_distribution = PF_MAGNIFIER_ENDPOINTS;
        e.intrabar_volume_weighted = 0;
        e.intrabar_volume_weighted_min_samples = 2;
        e.intrabar_volume_weighted_max_samples = 64;
        e.intrabar_sample_eligibility = PF_NATIVE_SAMPLE_CONTINUOUS_SEGMENTS;
        e.intrabar_n = ACID_FEED;
        e.intrabar_tf = "5";
        e.intrabar_bars = g_b5;
    }
    return e;
}

/* ── Emission (the C++ half's formats, character for character) ─────── */
static void risk_str(char* out, size_t n, const pf_native_risk_state_v1* r) {
    char a[64], b[64];
    snprintf(out, n, "%u/%u/%u/%u/%lld/%llu/%u/%s/%s", r->blocked, r->has_reason, r->has_reason ? r->reason : 0u,
             r->has_day, (long long)r->day_ordinal, (unsigned long long)r->fills_today, r->consecutive_loss_days,
             D(a, r->peak_equity), D(b, r->day_open_equity));
}
static void cursor_str(char* out, size_t n, uint64_t ord, int64_t eff, double t, int32_t ii, uint8_t prov,
                       uint8_t ph) {
    char a[64];
    snprintf(out, n, "%llu/%lld/%s/%d/%d/%d", (unsigned long long)ord, (long long)eff, D(a, t), ii, prov, ph);
}
static void emit_lots(const char* p, const char* tag, const lots_snap* s) {
    char key[96];
    int i;
    snprintf(key, sizeof key, "%s.lots.%s", p, tag);
    cfield(key, "n:%d", s->n);
    for (i = 0; i < s->n; ++i) {
        const pf_native_open_lot_v1* l = &s->lots[i].lot;
        char a[64], b[64], c[64], d[64], e[64], f[64], g[64];
        snprintf(key, sizeof key, "%s.lots.%s.%02d", p, tag, i);
        cfield(key, "ord:%llu;inc:%llu;cyc:%lld;side:%u;eb:%d;et:%lld;ep:%s;u:%s;com:%s;mark:%s;upnl:%s;fav:%s;adv:%s;"
                    "lab:%s;cmt:%s",
               (unsigned long long)l->ordinal, (unsigned long long)l->entry_incarnation, (long long)l->cycle, l->side,
               l->entry_bar_index, (long long)l->entry_time_ms, D(a, l->entry_price), D(b, l->signed_units),
               D(c, l->entry_commission), D(d, l->mark), D(e, l->unrealized_pnl), D(f, l->favorable_excursion),
               D(g, l->adverse_excursion), s->lots[i].label, s->lots[i].comment);
    }
}

typedef struct run_out {
    pf_report_t rep;
    int rep_ok;
    int drive_ok;
    int setup_rc;
    uint32_t setup_err, setup_field;
    int bad_rc, good_rc;
    pf_native_fx_curve_error_t bad_err, good_err;
    uint64_t bad_idx, good_idx;
    uint64_t hash, cont, shash;
    pf_native_state_v1 state;
    uint64_t recalc_driven, recalc_skipped;
} run_out;

static void emit_run(host_state* st, run_out* ro) {
    const char* p = st->tag;
    const pf_report_t* r = &ro->rep;
    char key[128], a[64], b[64], c[64], d[64], e[64], f[64], g[64], h[64], i2[64], j[64], rs[512], cs[256];
    int i;
    snprintf(key, sizeof key, "%s.setup", p);
    cfield(key, "status:%d;err:%u;field:%u", ro->setup_rc == PF_NATIVE_OK ? 0 : 1, ro->setup_err, ro->setup_field);
    snprintf(key, sizeof key, "%s.fx_curve", p);
    cfield(key, "bad:%d/%d/%llu;good:%d/%d/%llu", ro->bad_rc == 0 ? 0 : 1, (int)ro->bad_err,
           (unsigned long long)ro->bad_idx, ro->good_rc == 0 ? 0 : 1, (int)ro->good_err,
           (unsigned long long)ro->good_idx);
    snprintf(key, sizeof key, "%s.report", p);
    cfield(key, "trades:%d;trades_len:%d;net:%s;inputs:%lld;scripts:%lld;mag_sub:%lld;mag_ticks:%lld;itf:%d;stf:%d;"
                "ratio:%d;agg:%d;mag:%d;eq_len:%lld;hash_len:%lld",
           r->total_trades, r->trades_len, D(a, r->net_profit), (long long)r->input_bars_processed,
           (long long)r->script_bars_processed, (long long)r->magnifier_sub_bars_total,
           (long long)r->magnifier_sample_ticks_total, r->input_tf_seconds, r->script_tf_seconds, r->script_tf_ratio,
           r->needs_aggregation, r->bar_magnifier_enabled, (long long)r->equity_curve_len,
           (long long)r->broker_state_hash_len);
    for (i = 0; i < r->trades_len; ++i) {
        const pf_trade_t* t = &r->trades[i];
        const char* eid = strategy_closed_trade_entry_id(st->h, i);
        const char* xid = strategy_closed_trade_exit_id(st->h, i);
        const char* xc = strategy_closed_trade_exit_comment(st->h, i);
        snprintf(key, sizeof key, "%s.trade.%02d", p, i);
        cfield(key, "et:%lld;xt:%lld;ep:%s;xp:%s;pnl:%s;pct:%s;long:%d;ru:%s;dd:%s;qty:%s;com:%s;eb:%d;xb:%d;end:%d;"
                    "eid:%s;xid:%s;xc:%s;cause:%d;inc:%llu",
               (long long)t->entry_time, (long long)t->exit_time, D(a, t->entry_price), D(b, t->exit_price),
               D(c, t->pnl), D(d, t->pnl_pct), t->is_long, D(e, t->max_runup), D(f, t->max_drawdown), D(g, t->qty),
               D(h, t->commission), t->entry_bar_index, t->exit_bar_index, t->open_at_end, eid ? eid : "",
               xid ? xid : "", xc ? xc : "", strategy_closed_trade_close_cause(st->h, i),
               (unsigned long long)strategy_closed_trade_entry_incarnation(st->h, i));
    }
    for (i = 0; i < st->nbars && i < ACID_BARS; ++i) {
        const pf_native_decision_v1* at = &st->bar_at[i];
        const uint64_t row = i < r->broker_state_hash_len ? r->broker_state_hash[i] : 0;
        const double eq = i < r->equity_curve_len ? r->equity_curve[i].equity : NAN;
        const double op = i < r->equity_curve_len ? r->equity_curve[i].open_profit : NAN;
        risk_str(rs, sizeof rs, &st->bar_risk[i]);
        snprintf(key, sizeof key, "%s.bar.%02d", p, i);
        cfield(key, "ord:%llu;ii:%d;eff:%lld;sbo:%lld;floor:%lld;px:%s;qk:%d;prov:%d;ph:%d;comp:%d;sub:%d/%d/%d;"
                    "sess:%d%d%d;iv:%lld/%lld/%lld/%lld/%lld;sd:%lld/%lld;row:%llu;eq:%s;op:%s;risk:%s",
               (unsigned long long)at->ordinal, at->interval_index, (long long)at->effective_time_ms,
               (long long)at->script_bar_open_ms, (long long)at->decision_floor_ms, D(a, at->price), at->quote_kind,
               at->provenance, at->path_phase, at->completion, at->sub_index, at->sub_count, at->is_terminal_sub_bar,
               at->in_session, at->opens_session_day, at->closes_session_day,
               (long long)at->script_interval_open_ms, (long long)at->script_interval_eligible_open_ms,
               (long long)at->script_interval_last_traded_close_ms, (long long)at->script_interval_next_period_open_ms,
               (long long)at->script_interval_next_input_open_ms,
               at->has_session_day ? (long long)at->session_day_ordinal : -1LL,
               at->has_next_input_session_day ? (long long)at->next_input_session_day_ordinal : -1LL,
               (unsigned long long)row, D(b, eq), D(c, op), rs);
    }
    for (i = 0; i < st->nfills; ++i) {
        const pf_native_applied_v1* x = &st->fills[i].a;
        const pf_native_decision_v1* at = &st->fills[i].at;
        snprintf(key, sizeof key, "%s.fill.%02d", p, i);
        cfield(key, "gi:%d;ord:%llu;inc:%llu;lot:%llu;raw:%s;res:%s;tkt:%s;cu:%s;ou:%s;fw:%s;cb:%lld;ca:%lld;term:%d;"
                    "tr:%d;at:%llu/%d/%lld/%s/%d",
               st->fills[i].gi, (unsigned long long)x->ordinal, (unsigned long long)x->incarnation,
               (unsigned long long)x->opened_lot_incarnation, D(a, x->raw_price), D(b, x->resolved_price),
               D(c, x->ticket), D(d, x->closed_units), D(e, x->opened_units), D(f, x->filled_working),
               (long long)x->cycle_before, (long long)x->cycle_after, x->terminal,
               x->has_terminal_reason ? (int)x->terminal_reason : -1, (unsigned long long)at->ordinal,
               at->interval_index, (long long)at->effective_time_ms, D(g, at->price), at->quote_kind);
    }
    for (i = 0; i < st->ntfs; ++i) {
        const tf_rec* t = &st->tfs[i];
        snprintf(key, sizeof key, "%s.tf.%02d", p, i);
        cfield(key, "sub:%u;gi:%d;t:%lld;o:%s;h:%s;l:%s;c:%s;v:%s;comp:%u;at:%lld;iv:%lld/%lld/%lld/%lld/%lld;pull:%d",
               t->sub, t->gi, (long long)t->bar.timestamp, D(a, t->bar.open), D(b, t->bar.high), D(c, t->bar.low),
               D(d, t->bar.close), D(e, t->bar.volume), t->comp, (long long)t->delivered_at,
               (long long)t->iv.open_ms, (long long)t->iv.eligible_open_ms, (long long)t->iv.last_traded_close_ms,
               (long long)t->iv.next_period_open_ms, (long long)t->iv.next_input_open_ms, t->pulled);
    }
    for (i = 0; i < st->nrecalcs; ++i) {
        const recalc_rec* x = &st->recalcs[i];
        const double nan = NAN;
        risk_str(rs, sizeof rs, &x->risk);
        snprintf(key, sizeof key, "%s.recalc.%02d", p, i);
        cfield(key, "cause:%llu/%llu;gi:%d;part:%d/%s/%s/%s/%s/%s;px:%s;qk:%d;ord:%llu;risk:%s",
               (unsigned long long)x->cord, (unsigned long long)x->cinc, x->gi, x->has_partial,
               D(a, x->has_partial ? x->partial.open : nan), D(b, x->has_partial ? x->partial.high : nan),
               D(c, x->has_partial ? x->partial.low : nan), D(d, x->has_partial ? x->partial.close : nan),
               D(e, x->has_partial ? x->partial.volume : nan), D(f, x->at.price), x->at.quote_kind,
               (unsigned long long)x->at.ordinal, rs);
    }
    snprintf(key, sizeof key, "%s.recalc.totals", p);
    cfield(key, "driven:%llu;skipped:%llu", (unsigned long long)ro->recalc_driven,
           (unsigned long long)ro->recalc_skipped);
    {
        int kinds[4] = {0, 0, 0, 0}, fx_n = 0, rq_n = 0;
        uint64_t cd = ACID_FNV_OFFSET, rd = ACID_FNV_OFFSET;
        for (i = 0; i < st->nchecks; ++i) {
            const pf_native_margin_view_v1* q = &st->checks[i];
            ++kinds[q->kind & 3u];
            cd = acid_fnv_u64(cd, q->kind);
            cd = acid_fnv_d(cd, q->signed_units);
            cd = acid_fnv_d(cd, q->average_price);
            cd = acid_fnv_u64(cd, q->lot_count);
            cd = acid_fnv_d(cd, q->mark);
            cd = acid_fnv_u64(cd, q->liquidation_resting ? 1u : 0u);
            cd = acid_fnv_u64(cd, q->cursor_ordinal);
            cd = acid_fnv_u64(cd, (uint64_t)q->cursor_effective_time_ms);
            cd = acid_fnv_d(cd, q->cursor_t);
            if (q->kind == PF_NATIVE_MARGIN_CHECK_FX_ROLL) {
                cursor_str(cs, sizeof cs, q->cursor_ordinal, q->cursor_effective_time_ms, q->cursor_t,
                           q->cursor_interval_index, q->cursor_provenance, q->cursor_path_phase);
                snprintf(key, sizeof key, "%s.margin.fxroll.%02d", p, fx_n++);
                cfield(key, "units:%s;avg:%s;lots:%llu;mark:%s;rest:%u;cur:%s", D(a, q->signed_units),
                       D(b, q->average_price), (unsigned long long)q->lot_count, D(c, q->mark),
                       q->liquidation_resting, cs);
            }
        }
        snprintf(key, sizeof key, "%s.margin.checks", p);
        cfield(key, "n:%d;bar_open:%d;after_applied:%d;calc:%d;fx_roll:%d;digest:%llu", st->nchecks, kinds[0],
               kinds[1], kinds[2], kinds[3], (unsigned long long)cd);
        for (i = 0; i < st->nreqs; ++i) {
            const pf_native_margin_view_v1* v = &st->reqs[i];
            rd = acid_fnv_u64(rd, v->kind);
            rd = acid_fnv_d(rd, v->signed_units);
            rd = acid_fnv_d(rd, v->mark);
            rd = acid_fnv_d(rd, v->equity);
            rd = acid_fnv_d(rd, v->required);
            rd = acid_fnv_u64(rd, v->cursor_ordinal);
            if (v->kind == PF_NATIVE_MARGIN_CHECK_FX_ROLL || v->required > v->equity) {
                cursor_str(cs, sizeof cs, v->cursor_ordinal, v->cursor_effective_time_ms, v->cursor_t,
                           v->cursor_interval_index, v->cursor_provenance, v->cursor_path_phase);
                snprintf(key, sizeof key, "%s.margin.req.%02d", p, rq_n++);
                cfield(key, "kind:%u;units:%s;mark:%s;eq:%s;req:%s;cur:%s", v->kind, D(a, v->signed_units),
                       D(b, v->mark), D(c, v->equity), D(d, v->required), cs);
            }
        }
        snprintf(key, sizeof key, "%s.margin.reqs", p);
        cfield(key, "n:%d;digest:%llu", st->nreqs, (unsigned long long)rd);
    }
    for (i = 0; i < st->ncallviews; ++i) {
        const pf_native_margin_view_v1* v = &st->callviews[i];
        cursor_str(cs, sizeof cs, v->cursor_ordinal, v->cursor_effective_time_ms, v->cursor_t,
                   v->cursor_interval_index, v->cursor_provenance, v->cursor_path_phase);
        snprintf(key, sizeof key, "%s.margin.callview.%02d", p, i);
        cfield(key, "units:%s;avg:%s;lots:%llu;mark:%s;eq:%s;req:%s;cur:%s", D(a, v->signed_units),
               D(b, v->average_price), (unsigned long long)v->lot_count, D(c, v->mark), D(d, v->equity),
               D(e, v->required), cs);
    }
    for (i = 0; i < st->ncalls; ++i) {
        const pf_native_margin_call_v1* x = &st->calls[i];
        cursor_str(cs, sizeof cs, x->cursor_ordinal, x->cursor_effective_time_ms, x->cursor_t,
                   x->cursor_interval_index, x->cursor_provenance, x->cursor_path_phase);
        snprintf(key, sizeof key, "%s.margin.call.%02d", p, i);
        cfield(key, "ord:%llu;inc:%llu;app:%llu;side:%u;mark:%s;eq:%s;req:%s;liq:%s;u:%s;pb:%s;pa:%s;cur:%s",
               (unsigned long long)x->ordinal, (unsigned long long)x->incarnation,
               (unsigned long long)x->applied_ordinal, x->side, D(a, x->mark), D(b, x->equity), D(c, x->required),
               D(d, x->liquidation_price), D(e, x->units), D(f, x->position_before), D(g, x->position_after), cs);
    }
    if (st->ncalls > 0) {
        emit_lots(p, "CALLBEFORE", &st->lots_before);
        emit_lots(p, "CALLAFTER", &st->lots_after);
    }
    for (i = 0; i < st->nliq; ++i) {
        snprintf(key, sizeof key, "%s.liq.%s", p, st->liq[i].tag);
        cfield(key, "%s", D(a, st->liq[i].v));
    }
    for (i = 0; i < st->nlot_snaps; ++i) emit_lots(p, st->lot_snaps[i].tag, &st->lot_snaps[i]);
    for (i = 0; i < st->nwsnaps; ++i) {
        const working_snap* w = &st->wsnaps[i];
        int k;
        snprintf(key, sizeof key, "%s.working.%s", p, w->tag);
        cfield(key, "n:%d", w->n);
        for (k = 0; k < w->n; ++k) {
            const pf_native_working_v1* x = &w->rows[k].w;
            const int from_owner = x->anchor == PF_NATIVE_ANCHOR_FROM_OWNER_FILL;
            snprintf(key, sizeof key, "%s.working.%s.%02d", p, w->tag, k);
            cfield(key, "inc:%llu;intent:%u/%s;trig:%u;p1:%s;p2:%s;rem:%u/%s;ts:%u;org:%u;owner:%u;anc:%d/%s;vis:%u;"
                        "grp:%u;lab:%s",
                   (unsigned long long)x->incarnation, x->intent, D(a, x->intent_value), x->trigger, D(b, x->p1),
                   D(c, x->p2), x->remaining_kind,
                   D(d, x->remaining_kind == PF_NATIVE_REMAINING_UNITS ? x->remaining_units : 0.0), x->trigger_state,
                   x->origin, x->owner, from_owner, D(e, from_owner ? x->anchor_offset : 0.0),
                   x->owner == PF_NATIVE_OWNER_WAIT_FOR_APPLIED ? x->visibility : 0u, x->group_kind,
                   w->rows[k].label);
        }
    }
    for (i = 0; i < st->ntrails; ++i) {
        const trail_rec* t = &st->trails[i];
        snprintf(key, sizeof key, "%s.trail.%02d", p, t->gi);
        if (t->rc == PF_NATIVE_OK) {
            cfield(key, "act:%u;best:%s;lvl:%s;aord:%llu", t->s.activated, D(a, t->s.best_price),
                   D(b, t->s.current_level), (unsigned long long)t->s.activation_ordinal);
        } else {
            cfield(key, "absent");
        }
    }
    for (i = 0; i < st->nanchors; ++i) {
        const pf_native_anchored_level_view_v1* v = &st->anchors[i];
        cursor_str(cs, sizeof cs, v->owner_cursor_ordinal, v->owner_cursor_effective_time_ms, v->owner_cursor_t,
                   v->owner_cursor_interval_index, v->owner_cursor_provenance, v->owner_cursor_path_phase);
        snprintf(key, sizeof key, "%s.anchor.%02d", p, i);
        cfield(key, "owner:%llu;app:%llu;lot:%llu;fill:%s;leg:%llu;side:%u;trig:%u;off:%s;tick:%s;lvl:%s;cur:%s",
               (unsigned long long)v->owner, (unsigned long long)v->owner_applied_ordinal,
               (unsigned long long)v->owner_lot_incarnation, D(a, v->owner_fill_price), (unsigned long long)v->leg,
               v->leg_side, v->trigger, D(b, v->offset), D(c, v->price_tick), D(d, v->kernel_level), cs);
    }
    for (i = 0; i < st->nrisks; ++i) {
        risk_str(rs, sizeof rs, &st->risks[i].r);
        snprintf(key, sizeof key, "%s.risk.%s", p, st->risks[i].tag);
        cfield(key, "%s", rs);
    }
    for (i = 0; i < st->nrefusals; ++i) {
        snprintf(key, sizeof key, "%s.refusal.%s", p, st->refusals[i].id);
        cfield(key, "%s;same:%d", st->refusals[i].word, st->refusals[i].same);
    }
    for (i = 0; i < st->nsized; ++i) {
        snprintf(key, sizeof key, "%s.sized.%s", p, st->sized[i].tag);
        cfield(key, "%s", D(a, st->sized[i].v));
    }
    for (i = 0; i < st->nappends; ++i) {
        snprintf(key, sizeof key, "%s.append.%s", p, st->appends[i].id);
        cfield(key, "%s/%d", append_word(st->appends[i].error), st->appends[i].index);
    }
    snprintf(key, sizeof key, "%s.bracket", p);
    cfield(key, "tp:%d;sl:%d;trail:0;all:%d", st->tp_accepted ? 2 : 3, st->sl_accepted ? 2 : 3,
           st->tp_accepted && st->sl_accepted);
    {
        char xs[512] = "";
        size_t used = 0;
        for (i = 0; i < st->nxrep; ++i)
            used += (size_t)snprintf(xs + used, sizeof xs - used, "%s%s", i ? "," : "", st->x_replaces[i]);
        snprintf(key, sizeof key, "%s.x_replaces", p);
        cfield(key, "%s", st->nxrep ? xs : "none");
    }
    {   /* Every event, read back page by page (native_c_api.h:2796-2810). */
        static pf_native_event_v1 page[256];
        int counts[32];
        uint64_t after = 0, digest = ACID_FNV_OFFSET;
        long total = 0;
        int n, k;
        memset(counts, 0, sizeof counts);
        for (;;) {
            for (k = 0; k < 256; ++k) {
                memset(&page[k], 0, sizeof page[k]);
                page[k].struct_size = (uint32_t)sizeof page[k];
                page[k].version = PF_NATIVE_API_VERSION;
            }
            n = strategy_native_events_v1(st->h, after, page, 256);
            if (n <= 0) break;
            for (k = 0; k < n; ++k) {
                if (page[k].kind < 32) ++counts[page[k].kind];
                digest = acid_event_fold(digest, &page[k]);
            }
            total += n;
            after = page[n - 1].ordinal;
        }
        snprintf(key, sizeof key, "%s.events.n", p);
        cfield(key, "%ld", total);
        for (k = 0; k < 32; ++k) {
            if (!counts[k]) continue;
            snprintf(key, sizeof key, "%s.events.kind.%02d", p, k);
            cfield(key, "%d", counts[k]);
        }
        snprintf(key, sizeof key, "%s.events.digest", p);
        cfield(key, "%llu", (unsigned long long)digest);
    }
    snprintf(key, sizeof key, "%s.counts", p);
    cfield(key, "inputs:%d;subbars:%d;begins:%d", st->inputs, st->sub_bars, st->begins);
    snprintf(key, sizeof key, "%s.final", p);
    cfield(key, "hash:%llu;cont:%llu;shash:%llu;life:%u;phase:%u;comp:%u;fail:%u/%u;floor:%lld;hw:%llu;drive:%d",
           (unsigned long long)ro->hash, (unsigned long long)ro->cont, (unsigned long long)ro->shash,
           ro->state.lifecycle, ro->state.phase, ro->state.completion, ro->state.failure_code,
           ro->state.failure_operation, (long long)ro->state.decision_floor_ms,
           (unsigned long long)ro->state.consumed_high_water, ro->drive_ok);
    (void)i2; (void)j;
}

/* ── One run through the C API ──────────────────────────────────────── */
static host_state* run_c(const char* tag, int stream, int magnify, int typed_config_checks) {
    host_state* st = (host_state*)calloc(1, sizeof(host_state));
    run_out ro;
    pf_native_callbacks_v1 cb;
    pf_native_run_spec_v1 base = base_spec(ACID_TICK);
    pf_native_run_spec_ext_v1 ext;
    int i;
    if (!st) return NULL;
    memset(&ro, 0, sizeof ro);
    st->tag = tag;
    st->stream = stream;
    st->magnify = magnify;
    st->bars = ACID_BARS;
    memset(&cb, 0, sizeof cb);
    cb.struct_size = (uint32_t)sizeof cb;
    cb.version = PF_NATIVE_API_VERSION;
    cb.user = st;
    cb.on_run_begin = cb_run_begin;
    cb.on_input = cb_input;
    cb.on_bar = cb_bar;
    cb.on_applied = cb_applied;
    cb.on_timeframe_bar = cb_timeframe_bar;
    cb.on_margin_call = cb_margin_call;
    cb.on_recalculate = cb_recalculate;
    cb.on_sub_bar = cb_sub_bar;
    cb.on_margin_requirement = cb_margin_requirement;
    cb.on_margin_check = cb_margin_check;
    cb.on_margin_call_units = cb_margin_call_units;
    cb.on_anchored_level = cb_anchored_level;
    st->h = strategy_native_host_create_v1(&cb);
    if (!st->h) {
        line("FAIL C0 c-host: strategy_native_host_create_v1 refused the table");
        ++g_fail;
        return st;
    }
    ext = ext_spec(st);
    if (typed_config_checks) {
        /* C1: the typed configuration path on the handle the run then uses. A
         * grid with no ladder is refused by name and the handle stays
         * Unconfigured and usable (native_c_api.h:3062-3066, :3088-3099). */
        pf_native_run_spec_v1 bad = base_spec(0.0);
        uint32_t err = 999, field = 999;
        pf_native_state_v1 s;
        const int rc = strategy_configure_native_ext_result_v1(st->h, &bad, &ext, &err, &field);
        memset(&s, 0, sizeof s);
        s.struct_size = (uint32_t)sizeof s;
        s.version = PF_NATIVE_API_VERSION;
        strategy_native_state_v1(st->h, &s);
        cfield("cfg.refusal", "status:%d;err:%u;field:%u", rc == PF_NATIVE_OK ? 0 : 1, err, field);
        ccheck("C1", "c-typed-configuration", rc == PF_NATIVE_E_ARGUMENT && err == PF_NATIVE_SPEC_ERROR_GRID_REQUIRES_PRICE_TICK
                   && field == PF_NATIVE_SPEC_FIELD_PRICE_GRID && s.lifecycle == PF_NATIVE_LIFECYCLE_UNCONFIGURED,
               "a refused spec answers E_ARGUMENT, GRID_REQUIRES_PRICE_TICK at PRICE_GRID, handle still UNCONFIGURED");
    }
    ro.setup_err = 999;
    ro.setup_field = 999;
    ro.setup_rc = strategy_configure_native_ext_result_v1(st->h, &base, &ext, &ro.setup_err, &ro.setup_field);
    if (typed_config_checks) {
        uint32_t err = 999, field = 999;
        pf_native_state_v1 s;
        const int again = strategy_configure_native_ext_result_v1(st->h, &base, &ext, &err, &field);
        memset(&s, 0, sizeof s);
        s.struct_size = (uint32_t)sizeof s;
        s.version = PF_NATIVE_API_VERSION;
        strategy_native_state_v1(st->h, &s);
        ccheck("C1", "c-typed-configuration", ro.setup_rc == PF_NATIVE_OK && ro.setup_err == 0 && ro.setup_field == 0,
               "the same handle then configures: OK with NONE/NONE written");
        ccheck("C1", "c-typed-configuration", again == PF_NATIVE_E_STATE && err == PF_NATIVE_SPEC_ERROR_WRONG_PHASE
                   && field == PF_NATIVE_SPEC_FIELD_NONE && s.lifecycle == PF_NATIVE_LIFECYCLE_READY,
               "configuring a Ready handle answers E_STATE, WRONG_PHASE/NONE, and leaves it Ready");
    }
    if (ro.setup_rc != PF_NATIVE_OK) {
        line("FAIL C0 c-host: configure refused err %u field %u: %s", ro.setup_err, ro.setup_field,
             strategy_get_last_error(st->h));
        ++g_fail;
        return st;
    }
    {   /* The typed FX curve: refused while Ready (unordered), then applied. */
        int64_t t_bad[2] = {ACID_FX_T1, ACID_FX_T0}, t_good[2] = {ACID_FX_T0, ACID_FX_T1};
        double rates_bad[2] = {ACID_FX_R1, ACID_FX_R0}, rates[2] = {ACID_FX_R0, ACID_FX_R1};
        pf_native_fx_curve_v1 c;
        memset(&c, 0, sizeof c);
        c.struct_size = (uint32_t)sizeof c;
        c.n = 2;
        c.effective_from_ms = t_bad;
        c.account_per_quote = rates_bad;
        ro.bad_rc = strategy_configure_native_fx_curve_ext_v1(st->h, &c, &ro.bad_err, &ro.bad_idx);
        c.effective_from_ms = t_good;
        c.account_per_quote = rates;
        ro.good_rc = strategy_configure_native_fx_curve_ext_v1(st->h, &c, &ro.good_err, &ro.good_idx);
        if (typed_config_checks) {
            ccheck("C1", "c-typed-configuration", ro.bad_rc != 0 && ro.bad_err == PF_NATIVE_FX_CURVE_ERROR_NOT_STRICTLY_INCREASING
                       && ro.bad_idx == 1 && ro.good_rc == 0,
                   "the typed FX curve: NOT_STRICTLY_INCREASING at 1, then the good curve applies");
        }
    }
    strategy_set_broker_state_hash_recording(st->h, 1);
    memset(&ro.rep, 0, sizeof ro.rep);
    if (!stream) {
        uint32_t aerr = 999;
        int32_t aidx = -1;
        ro.drive_ok = strategy_native_run_v1(st->h, g_b15, ACID_BARS, &ro.rep) == PF_NATIVE_OK;
        ro.rep_ok = 1;
        strategy_native_append_auxiliary_bars_ext_v1(st->h, &g_b5[0], 1, &aerr, &aidx);
        snprintf(st->appends[st->nappends].id, 24, "after_batch");
        st->appends[st->nappends].error = aerr;
        st->appends[st->nappends++].index = aidx;
    } else {
        uint32_t aerr;
        int32_t aidx;
        ro.drive_ok = strategy_stream_begin(st->h, g_b15, ACID_WARMUP, "15", "15") == 0;
        for (i = ACID_WARMUP; i < ACID_BARS && ro.drive_ok; ++i) {
            if (i == ACID_WARMUP + 2) {
                pf_bar_t pair[2];
                pf_bar_t stale = g_b5[3 * (i - 1)];
                pair[0] = g_b5[3 * i + 1];
                pair[1] = g_b5[3 * i];
                aerr = 999; aidx = -1;
                strategy_native_append_auxiliary_bars_ext_v1(st->h, pair, 2, &aerr, &aidx);
                snprintf(st->appends[st->nappends].id, 24, "unordered_pair");
                st->appends[st->nappends].error = aerr;
                st->appends[st->nappends++].index = aidx;
                aerr = 999; aidx = -1;
                strategy_native_append_auxiliary_bars_ext_v1(st->h, &stale, 1, &aerr, &aidx);
                snprintf(st->appends[st->nappends].id, 24, "stale_bar");
                st->appends[st->nappends].error = aerr;
                st->appends[st->nappends++].index = aidx;
            }
            aerr = 999; aidx = -1;
            if (strategy_native_append_auxiliary_bars_ext_v1(st->h, &g_b5[3 * i], 3, &aerr, &aidx) != PF_NATIVE_OK) {
                line("C append refused at %d: %s", i, append_word(aerr));
                ro.drive_ok = 0;
            }
            ro.drive_ok = ro.drive_ok && strategy_stream_push_bar(st->h, &g_b15[i]) == 0;
        }
        ro.drive_ok = ro.drive_ok && strategy_stream_end(st->h, 0) == 0;
        aerr = 999; aidx = -1;
        strategy_native_append_auxiliary_bars_ext_v1(st->h, &g_b5[0], 1, &aerr, &aidx);
        snprintf(st->appends[st->nappends].id, 24, "after_end");
        st->appends[st->nappends].error = aerr;
        st->appends[st->nappends++].index = aidx;
        ro.rep_ok = strategy_stream_fill_report(st->h, &ro.rep) == 0;
        ro.shash = strategy_stream_state_hash(st->h);
    }
    ro.hash = strategy_broker_state_hash(st->h);
    strategy_native_continuation_hash_v1(st->h, &ro.cont);
    memset(&ro.state, 0, sizeof ro.state);
    ro.state.struct_size = (uint32_t)sizeof ro.state;
    ro.state.version = PF_NATIVE_API_VERSION;
    strategy_native_state_v1(st->h, &ro.state);
    strategy_native_recalculations_v1(st->h, &ro.recalc_driven, &ro.recalc_skipped);
    emit_run(st, &ro);
    if (typed_config_checks) {
        ccheck("C1", "c-typed-configuration", ro.drive_ok && ro.state.lifecycle == PF_NATIVE_LIFECYCLE_COMPLETED,
               "the run on the handle that was first refused completes");
    }
    ccheck("C2", "c-interval-query-phase",
           st->interval_outside_rc == PF_NATIVE_E_STATE && st->interval_inside_bad == 0 && st->ntfs > 0,
           "strategy_native_timeframe_bar_interval_v1: OK inside every on_timeframe_bar, E_STATE outside");
    if (ro.rep_ok) strategy_native_report_free_v1(&ro.rep);
    return st;
}

int acid_c_main(acid_sink_fn sink, void* user) {
    host_state* runs[3];
    int i;
    g_sink = sink;
    g_user = user;
    g_fail = 0;
    acid_build_tape(g_b15, g_b5);
    line("# acid composite, C half: native_c_api.h only");
    runs[0] = run_c("batch", 0, 0, 1);
    runs[1] = run_c("stream", 1, 0, 0);
    runs[2] = run_c("mag", 0, 1, 0);
    /* What the C++ half prints and C cannot: named, with the doc line (read on
     * tree 6df7850a). The first four are C-SURFACE-2, ruled to 1.1.0; two of
     * them have no exclusion sentence on the tree yet. */
    line("EXCLUDED C-SURFACE-2:closed-trade-entry-comment the report-row accessors are entry_id / exit_id / "
         "exit_comment / close_cause (pineforge.h:1171, :1184, :1186, :1253) and pf_trade_t has no comment "
         "field (pineforge.h:174-211); no sentence on the tree names the entry comment's absence; ruled to 1.1.0");
    line("EXCLUDED C-SURFACE-2:applied-origin-label pf_native_applied_v1 carries neither the request's origin "
         "nor its label (native_c_api.h:1512-1531); no sentence on the tree names the absence; ruled to 1.1.0");
    line("EXCLUDED C-SURFACE-2:replace-options keep_handle / keep_binding: \"neither has a C spelling\" "
         "(native-engine.md:936; native_host.hpp:1206 \"No C spelling: the C replace takes no options\"); the C "
         "twin re-issues X with a plain replace; ruled to 1.1.0");
    line("EXCLUDED C-SURFACE-2:inspect_current_execution [--] in the COVERAGE block (native_c_api.h:101-104; "
         "native-engine.md:3929-3932; native_host.hpp:1070-1071); ruled to 1.1.0");
    line("EXCLUDED COVERAGE:closes_session_day_open_ended no byte in pf_native_decision_v1, the third of the "
         "COVERAGE block's recorded asymmetries (native_c_api.h:195-203, :1428-1431)");
    line("EXCLUDED PROTECTED:broker_state_hash_from_execution_hash a protected BacktestEngine member "
         "(engine.hpp:413), not a @host-seam: no C spelling by construction");
    line("EXCLUDED NOC:quote_origin_ordinal NativeCurrentPointView::quote_origin_ordinal (native_host.hpp:650) "
         "has no field in pf_native_decision_v1 (native_c_api.h:1440-1486) and is not among the recorded "
         "asymmetries (native_c_api.h:169-203)");
    line("EXCLUDED NOC:driver_statistics NativeDecisionContext::driver_statistics (market_driver.hpp:166) has "
         "no field in pf_native_decision_v1 and is not among the recorded asymmetries; the report's magnifier "
         "counters (pf_report_t) are the shared totals");
    for (i = 0; i < 3; ++i) {
        if (runs[i]) {
            if (runs[i]->h) strategy_native_host_free(runs[i]->h);
            free(runs[i]);
        }
    }
    line("acid composite (C): %d C-side failures", g_fail);
    return g_fail;
}

#ifndef ACID_C_NO_MAIN
static void print_line(void* user, const char* l) {
    (void)user;
    printf("%s\n", l);
}
int main(void) { return acid_c_main(print_line, NULL) == 0 ? 0 : 1; }
#endif
