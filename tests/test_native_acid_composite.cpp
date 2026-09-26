// test_native_acid_composite.cpp -- the G1 acid composite, C++ half (lane H-MEASURE,
// item A4-ACID-COVERAGE; extends the fourth audit's codex composite witness).
//
// ONE Pine-free strategy written against NativeStrategyHost that uses every
// feature the G1 acid test asks for (the fourth audit's G1 feature list):
//
//   F01 a Sized entry with a fee reserve
//   F02 a bracket with anchored legs on a price grid
//   F03 a re-issued exit with keep_binding
//   F04 a trail in ticks
//   F05 a maintenance-only margin model, kernel liquidation seen through open lots
//   F06 an FX curve with a step inside the run
//   F07 a risk limit (and its refusal: an opening refused with RiskLimit)
//   F08 calc-on-fills
//   F09 a 60-minute subscription over 15-minute bars + an auxiliary finer feed
//       with the interval query
//   F10 session-day first/last flags
//   F11 a partial close across lots and an exact-sum close
//   F12 a typed refusal path
//   F13 magnifier counters on the bare host
//   F14 the kernel-recorded report with per-bar broker-state hashes
//
// Source-free: it includes only <pineforge/...> kernel headers and links
// libpineforge_kernel.a. It runs the plain scenario (the C twin) in batch and
// stream, the keep_binding scenario (C++ only: C has no ReplaceOptions word) in
// batch and stream, a lower_tf-magnified batch, and a batch cut after bar 60
// (prefix closure). It prints
//   PASS|FAIL <feature>          one per requested feature
//   FIELD <run>.<key>=<value>    every shared ledger fact (the C half prints the
//                                identical set: test_native_acid_composite_c.c)
//   XFIELD <key>=<value> excl=<family>
//                                C++-only facts, each family named by the C half
//                                in an EXCLUDED line
//   DRIVING ...                  the batch/stream accounting, per documented rule
// and exits non-zero on any FAIL. Built with -DACID_WITH_C and linked with the
// C half, it also runs the C port in-process and compares the FIELD sets.
#include "native_acid_composite.h"

#include <pineforge/native_calendar.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/native_toolkit.hpp>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;
namespace tk = pineforge::native_toolkit;

static_assert(sizeof(Bar) == sizeof(pf_bar_t), "Bar is layout-compatible with pf_bar_t");

namespace {

// ============================================================ output
std::vector<std::string> g_fields;   // FIELD lines, for the in-process C comparison

std::string fmt(const char* f, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}
std::string D(double v) {
    char b[64];
    return acid_d(b, sizeof b, v);
}
using ull = unsigned long long;
using ll = long long;

void out(const std::string& s) { std::printf("%s\n", s.c_str()); }
void field(const std::string& key, const std::string& value) {
    const std::string l = "FIELD " + key + "=" + value;
    g_fields.push_back(l);
    out(l);
}
void xfield(const std::string& family, const std::string& key, const std::string& value) {
    out("XFIELD " + key + "=" + value + " excl=" + family);
}

// A feature is PASS when every one of its checks held.
struct Feature {
    std::string id, name;
    int total = 0, failed = 0;
    std::vector<std::string> failures;
};
std::vector<Feature> g_features;
Feature& feature(const std::string& id, const std::string& name) {
    for (auto& f : g_features)
        if (f.id == id) return f;
    g_features.push_back(Feature{id, name, 0, 0, {}});
    return g_features.back();
}
void fcheck(const std::string& id, const std::string& name, bool ok, const std::string& what) {
    Feature& f = feature(id, name);
    ++f.total;
    if (!ok) {
        ++f.failed;
        f.failures.push_back(what);
    }
}
bool same(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }
bool near_rel(double a, double b, double rel) {
    return std::fabs(a - b) <= rel * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

// ============================================================ tape
std::vector<Bar> g_b15, g_b5;
void build_tape() {
    pf_bar_t b15[ACID_BARS], b5[ACID_FEED];
    acid_build_tape(b15, b5);
    g_b15.resize(ACID_BARS);
    g_b5.resize(ACID_FEED);
    std::memcpy(g_b15.data(), b15, sizeof b15);
    std::memcpy(g_b5.data(), b5, sizeof b5);
}

// ============================================================ hand arithmetic
// The 0.25 ladder (native-engine.md:336-338): every tape price is a multiple of
// 0.05 and every level below is an exact binary64 multiple of 0.25.
double ceil_tick(double p) { return std::ceil(p / ACID_TICK) * ACID_TICK; }
double floor_tick(double p) { return std::floor(p / ACID_TICK) * ACID_TICK; }
double nearest_tick(double p) { return std::round(p / ACID_TICK) * ACID_TICK; }
// A Directional market fill: the adverse tick, then one tick of slippage
// (native-engine.md:183-186, :336-338).
double buy_fill(double open) { return ceil_tick(open) + ACID_TICK; }
double sell_fill(double open) { return floor_tick(open) - ACID_TICK; }
// The quantity-grid floor: the largest grid multiple at or below the quotient
// (native-engine.md:3119-3126), n * step.
double floor_grid(double q) {
    double n = std::floor(q / ACID_QTY_GRID);
    while ((n + 1.0) * ACID_QTY_GRID <= q) n += 1.0;
    while (n * ACID_QTY_GRID > q) n -= 1.0;
    return n * ACID_QTY_GRID;
}
// A percent fee: abs(units) x price x point_value x fx x fee / 100 (native-engine.md:188-189).
double fee(double u, double p, double fx) {
    return std::fabs(u) * p * ACID_POINT_VALUE * fx * ACID_FEE_PCT / 100.0;
}

// ============================================================ records
struct FillRec {
    int gi = 0;
    no::ExecutionAppliedEvent ev;
    std::string label;
    no::RequestOrigin origin = no::RequestOrigin::Host;
    uint64_t at_ord = 0;
    int at_ii = 0;
    int64_t at_eff = 0;
    double at_px = NAN;
    int at_qk = 0;
};
struct BarRec {
    NativeDecisionContext ctx;
    double px = NAN;
    int qk = 0;
    uint64_t quote_origin = 0;
    int64_t sd = -1, nsd = -1;
    uint64_t broker_half = 0;
    NativeRiskState risk;
};
struct TfRec {
    std::size_t sub = 0;
    Bar bar{};
    NativeCompletionKind completion{};
    int64_t delivered_at = 0;
    native_calendar::NativeInterval iv{};
    bool pulled = false;
    int gi = 0;
};
struct RecalcRec {
    uint64_t cause_ord = 0, cause_inc = 0;
    int gi = 0;
    std::optional<Bar> partial;
    double px = NAN;
    int qk = 0;
    uint64_t ord = 0;
    NativeRiskState risk;
};
struct CheckRec {
    NativeMarginCheckPoint p;
};
struct ReqRec {
    NativeMarginRequirementView v;
};
struct AnchorRec {
    NativeAnchoredLevelView v;
};
struct Refusal {
    std::string id, word;
    bool unchanged = false;
};
struct Snap {
    double units = 0, avg = 0, eq = 0;
    std::size_t lots = 0, working = 0;
    bool operator==(const Snap& o) const {
        return same(units, o.units) && same(avg, o.avg) && same(eq, o.eq) && lots == o.lots
               && working == o.working;
    }
};

const char* reject_word(no::RequestRejectReason r) {
    switch (r) {
    case no::RequestRejectReason::InvalidQuantity: return "InvalidQuantity";
    case no::RequestRejectReason::OffGrid: return "OffGrid";
    case no::RequestRejectReason::InvalidTrigger: return "InvalidTrigger";
    case no::RequestRejectReason::InvalidCapacity: return "InvalidCapacity";
    case no::RequestRejectReason::InvalidOwner: return "InvalidOwner";
    case no::RequestRejectReason::InvalidQuantityBasis: return "InvalidQuantityBasis";
    case no::RequestRejectReason::InvalidGroup: return "InvalidGroup";
    case no::RequestRejectReason::PlacementAdmission: return "PlacementAdmission";
    }
    return "?";
}
std::string submit_word(const no::SubmitResult& r) {
    if (r.status == no::SubmitStatus::Accepted) return "Accepted";
    return std::string("Rejected/") + (r.reason ? reject_word(*r.reason) : "none");
}
const char* replace_word(no::ReplaceStatus s) {
    switch (s) {
    case no::ReplaceStatus::Replaced: return "Replaced";
    case no::ReplaceStatus::ReplaceRejected: return "ReplaceRejected";
    case no::ReplaceStatus::NotWorking: return "NotWorking";
    case no::ReplaceStatus::InvalidHandle: return "InvalidHandle";
    }
    return "?";
}
const char* cancel_word(no::CancelStatus s) {
    switch (s) {
    case no::CancelStatus::Cancelled: return "Cancelled";
    case no::CancelStatus::NotWorking: return "NotWorking";
    case no::CancelStatus::InvalidHandle: return "InvalidHandle";
    }
    return "?";
}
const char* append_word(NativeAuxiliaryAppendError e) {
    switch (e) {
    case NativeAuxiliaryAppendError::None: return "None";
    case NativeAuxiliaryAppendError::HostFailed: return "HostFailed";
    case NativeAuxiliaryAppendError::Reentrant: return "Reentrant";
    case NativeAuxiliaryAppendError::NotRealtime: return "NotRealtime";
    case NativeAuxiliaryAppendError::NoAuxiliaryFeed: return "NoAuxiliaryFeed";
    case NativeAuxiliaryAppendError::InvalidBarArray: return "InvalidBarArray";
    case NativeAuxiliaryAppendError::InvalidBar: return "InvalidBar";
    case NativeAuxiliaryAppendError::UnorderedBars: return "UnorderedBars";
    case NativeAuxiliaryAppendError::InputPeriodAlreadyAccepted: return "InputPeriodAlreadyAccepted";
    case NativeAuxiliaryAppendError::AllocationFailure: return "AllocationFailure";
    }
    return "?";
}

// ============================================================ the host
struct Opts {
    std::string tag = "batch";
    bool stream = false;
    bool keep = false;       // re-issue X with ReplaceOptions{keep_binding}
    bool magnify = false;    // IntrabarPath::lower_tf over the 5-minute feed
    int bars = ACID_BARS;    // a batch cut short (prefix closure)
};

class AcidHost final : public NativeStrategyHost {
public:
    explicit AcidHost(Opts o) : opt_(std::move(o)) {}

    std::vector<FillRec> fills;
    std::vector<BarRec> bars;
    std::vector<TfRec> tfs;
    std::vector<RecalcRec> recalcs;
    mutable std::vector<CheckRec> checks;
    mutable std::vector<ReqRec> reqs;
    mutable std::vector<NativeMarginCallView> callviews;
    mutable std::vector<NativeOpenLot> lots_before_call;
    mutable bool lots_before_taken = false;
    mutable std::vector<AnchorRec> anchors;
    std::vector<no::MarginCallEvent> calls;
    std::vector<NativeOpenLot> lots_after_call;
    std::vector<std::pair<std::string, std::vector<NativeOpenLot>>> lots;
    std::vector<std::pair<std::string, std::vector<NativeWorkingRequest>>> working;
    std::vector<std::pair<std::string, std::optional<double>>> liq;
    std::vector<std::pair<int, std::optional<NativeTrailState>>> trails;
    std::vector<std::pair<std::string, NativeRiskState>> risks;
    std::vector<Refusal> refusals;
    std::vector<std::pair<std::string, double>> sized;
    std::vector<no::ReplaceResult> x_replaces;
    std::optional<NativeCurrentExecutionPreview> preview;
    tk::BracketReceipt bracket{};
    int inputs = 0, sub_bars = 0, begins = 0;
    std::optional<no::RequestHandle> h_e1, h_tp, h_sl, h_e2, h_x, h_e3, h_t3, h_e4a, h_e4b,
        h_e5, h_xf, h_p1, h_p2;
    std::optional<no::RequestHandle> h_d4[4];
    bool x_done = false;
    double p2_units = 0.0;
    std::vector<double> p2_book;

    // A test-only reach for the protected broker-state fold (engine.hpp:413):
    // the continuation factored out at a fixed execution hash.
    uint64_t broker_half() const { return broker_state_hash_from_execution_hash(0); }
    double net() const { return net_profit(); }

private:
    Opts opt_;
    std::optional<native_calendar::SessionCalendar> cal_;

    Snap snap(double mark) const {
        const auto p = physical_position();
        return Snap{p.signed_units, p.average_price, native_marked_equity(mark), p.lot_count,
                    native_working_requests().size()};
    }
    void refusal(const std::string& id, const std::string& word, const Snap& before, double mark) {
        refusals.push_back(Refusal{id, word, snap(mark) == before});
    }
    int64_t day_of(int64_t ms) const {
        if (!cal_) return -1;
        const auto d = native_calendar::session_day_ordinal(*cal_, ms);
        return d ? *d : -1;
    }

    // ------------------------------------------------------------ Monday 09:00
    void monday_open(const Bar& bar) {
        const double mark = bar.close;
        const Snap s0 = snap(mark);
        refusal("R01.transact_zero", submit_word(submit({no::Transact{0.0}, "R01", "zero"})), s0, mark);
        refusal("R02.off_grid", submit_word(submit({no::Transact{0.005}, "R02", "off grid"})), s0, mark);
        {
            no::Sized bad;
            bad.side = no::Side::Long;
            bad.basis = no::CashValue{-5.0};
            refusal("R03.bad_basis", submit_word(submit({bad, "R03", "negative cash"})), s0, mark);
        }
        {
            no::Sized big;
            big.side = no::Side::Long;
            big.basis = no::CashValue{5000000.0};
            big.time = no::SizeTime::AtAcceptance;
            refusal("R04.placement", submit_word(submit({big, "R04", "too big"})), s0, mark);
        }
        {
            const auto r = declare_timeframe_subscriptions_result({});
            refusal("R05.declare_outside_begin",
                    fmt("%s/%d/%d", r.status == NativeSetupStatus::Applied ? "Applied" : "Failed",
                        static_cast<int>(r.validation.error), static_cast<int>(r.validation.field)),
                    s0, mark);
        }
        {
            NativeFxCurve late{{bar.timestamp + 1}, {2.0}};
            const auto r = configure_native_fx_curve(late);
            refusal("R06.fx_curve_while_running",
                    fmt("%s/%d/%zu", r.status == NativeSetupStatus::Applied ? "Applied" : "Failed",
                        static_cast<int>(r.validation.error), r.validation.index),
                    s0, mark);
        }
        // F01: the kernel's own sizing as a pure query, at the price the
        // candidate will resolve (Directional + 1 tick: native-engine.md:3242-3252).
        no::Sized sz;
        sz.side = no::Side::Long;
        sz.basis = no::CashValue{ACID_E1_CASH};
        sz.time = no::SizeTime::AtMatch;
        sz.price = no::SizePrice::Resolved;
        sz.grid_policy = no::ExecutionGridPolicy::SnapToGrid;
        sz.reserve_percent_fee = true;
        const double fill_px = buy_fill(g_b15[G_E1FILL].open);
        sized.emplace_back("E1.reserve", native_sized_units(sz, fill_px, 0.0, ACID_ACCOUNT_FX).value_or(NAN));
        no::Sized plain = sz;
        plain.reserve_percent_fee = false;
        sized.emplace_back("E1.no_reserve",
                           native_sized_units(plain, fill_px, 0.0, ACID_ACCOUNT_FX).value_or(NAN));
        h_e1 = submit({sz, "E1", "sized entry"}).handle;
        if (!h_e1) return;
        no::Request take{no::Reduce{no::OwnerOpenedUnits{}}, "TP", "bracket"};
        take.trigger = no::Limit{0.0};
        take.anchor = no::FromOwnerFill{ACID_TP_OFFSET, false, no::NativeAnchorRounding::Raw};
        no::Request stop{no::Reduce{no::OwnerOpenedUnits{}}, "SL", "bracket"};
        stop.trigger = no::Stop{0.0};
        stop.anchor = no::FromOwnerFill{ACID_SL_OFFSET, false, no::NativeAnchorRounding::Raw};
        tk::BracketSpec spec;
        spec.parent = *h_e1;
        spec.take_profit = take;
        spec.stop_loss = stop;
        spec.anchor_rounding = no::NativeAnchorRounding::Directional;
        spec.visibility = no::NativeArmVisibility::PendingUntilArmed;
        bracket = tk::submit_bracket(*this, spec);
        h_tp = bracket.take_profit;
        h_sl = bracket.stop_loss;
        working.emplace_back("E1", native_working_requests());
        const Snap s1 = snap(mark);
        {
            no::Request bad = take;
            bad.label = "R07";
            bad.trigger = no::Limit{203.0};
            bad.owner = no::WaitForApplied{*h_e1};
            refusal("R07.written_anchor", submit_word(submit(bad)), s1, mark);
        }
        {
            no::Sized child;
            child.side = no::Side::Long;
            child.basis = no::CashValue{1000.0};
            no::Request bad{child, "R08", "sized child"};
            bad.owner = no::WaitForApplied{*h_e1};
            refusal("R08.sized_child", submit_word(submit(bad)), s1, mark);
        }
    }

    void reissue_x(int gi) {
        if (!h_x || x_done) return;
        const double level = ACID_X_LEVEL0 + ACID_TICK * static_cast<double>(gi - G_E2FILL);
        no::Request x{no::Flatten{}, "X", "protect"};
        x.trigger = no::Stop{level};
        no::ReplaceResult r;
        if (opt_.keep) {
            no::ReplaceOptions options;
            options.keep_binding = true;
            r = replace(*h_x, x, options);
        } else {
            r = replace(*h_x, x);
        }
        x_replaces.push_back(r);
        if (r.status == no::ReplaceStatus::Replaced && r.successor) h_x = r.successor;
    }

    // ------------------------------------------------------------ callbacks
    void on_native_run_begin() override {
        ++begins;
        if (const auto* s = native_state().spec) {
            cal_ = native_calendar::parse_session(s->session, s->timezone);
        }
    }
    void on_native_input(const Bar&, const NativeInputContext&) override { ++inputs; }
    void on_native_sub_bar(const Bar&, const NativeDecisionContext&) override { ++sub_bars; }

    void on_native_timeframe_bar(const Bar& b, const NativeTimeframeBarContext& c) override {
        TfRec t;
        t.sub = c.subscription;
        t.bar = b;
        t.completion = c.completion;
        t.delivered_at = c.delivered_at_ms;
        t.iv = c.interval;
        t.gi = static_cast<int>(bars.size());
        const auto pulled = native_series_bar(c.subscription);
        t.pulled = pulled && same(pulled->open, b.open) && same(pulled->high, b.high)
                   && same(pulled->low, b.low) && same(pulled->close, b.close)
                   && same(pulled->volume, b.volume) && pulled->timestamp == b.timestamp;
        tfs.push_back(t);
    }

    void on_native_recalculate(const Bar& bar, const NativeDecisionContext& ctx,
                               NativeCalculationReason reason,
                               const no::ExecutionAppliedEvent* cause) override {
        if (reason == NativeCalculationReason::BarClose) {
            on_native_bar(bar, ctx);
            return;
        }
        if (reason != NativeCalculationReason::OrderFill) return;
        RecalcRec r;
        r.gi = static_cast<int>(bars.size());
        if (cause) {
            r.cause_ord = cause->ordinal;
            r.cause_inc = cause->handle().incarnation;
        }
        r.partial = current_partial_bar();
        if (const auto pt = current_execution_point()) {
            r.px = pt->price;
            r.qk = static_cast<int>(pt->quote_kind);
        }
        r.ord = ctx.coordinate.ordinal;
        r.risk = native_risk_state();
        recalcs.push_back(r);
        // F08: a request born in the take-profit's fill recalculation.
        if (cause && h_tp && cause->handle() == *h_tp && !h_e2) {
            h_e2 = submit({no::Transact{ACID_E2_UNITS}, "E2", "re-entry on the fill"}).handle;
        }
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext& ctx) override {
        const int gi = static_cast<int>(bars.size());
        BarRec rec;
        rec.ctx = ctx;
        if (const auto pt = current_execution_point()) {
            rec.px = pt->price;
            rec.qk = static_cast<int>(pt->quote_kind);
            rec.quote_origin = pt->quote_origin_ordinal;
        }
        rec.sd = day_of(ctx.script_interval.open_ms);
        rec.nsd = day_of(ctx.script_interval.next_input_open_ms);
        rec.broker_half = broker_half();
        rec.risk = native_risk_state();
        bars.push_back(rec);
        const double mark = bar.close;

        if (gi == G_E1) monday_open(bar);
        if (gi == G_E1FILL) {
            lots.emplace_back("E1FILL", native_open_lots(mark));
            working.emplace_back("E1FILL", native_working_requests());
        }
        if (gi == G_E2FILL) {
            const Snap s0 = snap(mark);
            if (h_e1) {
                refusal("R09.replace_terminal",
                        replace_word(replace(*h_e1, no::Request{no::Transact{1.0}, "R09", ""}).status),
                        s0, mark);
            }
            if (h_sl) refusal("R10.cancel_cancelled_leg", cancel_word(cancel(*h_sl).status), s0, mark);
            lots.emplace_back("E2FILL", native_open_lots(mark));
            no::Request x{no::Flatten{}, "X", "protect"};
            x.trigger = no::Stop{ACID_X_LEVEL0};
            h_x = submit(x).handle;
        }
        if (gi >= G_X_FIRST && gi <= G_X_LAST) reissue_x(gi);
        if (gi == G_XFILL) lots.emplace_back("XFILL", native_open_lots(mark));
        if (gi == G_E3) {
            h_e3 = submit({no::Transact{ACID_E3_UNITS}, "E3", "trail entry"}).handle;
            if (h_e3) {
                no::Request t{no::Reduce{no::OwnerOpenedUnits{}}, "T3", "trail"};
                t.trigger = no::Trail{0.0, std::nullopt, no::TrailTicks{ACID_T3_TICKS}};
                t.owner = no::WaitForApplied{*h_e3};
                h_t3 = submit(t).handle;
            }
            working.emplace_back("E3", native_working_requests());
        }
        if (gi == G_E3FILL) lots.emplace_back("E3FILL", native_open_lots(mark));
        if (h_t3 && gi >= G_E3FILL && gi <= G_T3X) trails.emplace_back(gi, trail_state(*h_t3));
        if (gi == G_E4A) h_e4a = submit({no::Transact{ACID_E4A_UNITS}, "E4a", "short"}).handle;
        if (gi == G_E4B) h_e4b = submit({no::Transact{ACID_E4B_UNITS}, "E4b", "short"}).handle;
        if (gi == G_E4BFILL) {
            lots.emplace_back("E4BFILL", native_open_lots(mark));
            liq.emplace_back("E4BFILL", native_liquidation_price());
        }
        if (gi == G_PRESTEP) liq.emplace_back("PRESTEP", native_liquidation_price());
        if (gi == G_STEP) liq.emplace_back("STEP", native_liquidation_price());
        if (gi == G_PRESPIKE) liq.emplace_back("PRESPIKE", native_liquidation_price());
        if (gi == G_SPIKE) {
            liq.emplace_back("SPIKE", native_liquidation_price());
            lots.emplace_back("SPIKE", native_open_lots(mark));
            risks.emplace_back("SPIKE", native_risk_state());
            // F07: an opening while the intraday-loss block stands.
            h_e5 = submit({no::Transact{ACID_E5_UNITS}, "E5", "add while blocked"}).handle;
        }
        if (gi == G_E5REJ) {
            risks.emplace_back("E5REJ", native_risk_state());
            h_xf = submit({no::Flatten{}, "XF", "flatten while blocked"}).handle;
        }
        if (gi == G_XFFILL) {
            liq.emplace_back("XFFILL", native_liquidation_price());
            lots.emplace_back("XFFILL", native_open_lots(mark));
        }
        if (gi == G_D4) {
            risks.emplace_back("D4", native_risk_state());
            for (int k = 0; k < 4; ++k) {
                h_d4[k] = submit({no::Transact{acid_d4_units[k]}, fmt("E%d", 6 + k), "thursday"}).handle;
            }
            // C++ only (C-SURFACE-2): a current-execution preview, observed and
            // not executed. It must move no shared field.
            if (h_d4[0]) {
                preview = inspect_current_execution(
                    NativeCurrentExecution{*h_d4[0], NativeCurrentPriceRule::AsPresented});
            }
            working.emplace_back("D4", native_working_requests());
        }
        if (gi == G_D4FILL) lots.emplace_back("D4FILL", native_open_lots(mark));
        if (gi == G_P1) h_p1 = submit({no::Reduce{no::ExplicitUnits{ACID_P1_UNITS}}, "P1", "fifo partial"}).handle;
        if (gi == G_P1FILL) lots.emplace_back("P1FILL", native_open_lots(mark));
        if (gi == G_P2) {
            const auto book = native_open_lots(mark);
            p2_book.clear();
            for (const auto& lot : book) p2_book.push_back(lot.signed_units);
            if (book.size() >= 2) {
                p2_units = p2_book[0] + p2_book[1];   // the binary64 sum of the first two lots
                h_p2 = submit({no::Reduce{no::ExplicitUnits{p2_units}}, "P2", "exact sum"}).handle;
            }
        }
        if (gi == G_P2FILL) lots.emplace_back("P2FILL", native_open_lots(mark));
        if (gi == opt_.bars - 1) {
            lots.emplace_back("LAST", native_open_lots(mark));
            risks.emplace_back("LAST", native_risk_state());
        }
    }

    void on_native_applied(const no::ExecutionAppliedEvent& ev,
                           const NativeDecisionContext& ctx) override {
        FillRec f;
        f.gi = static_cast<int>(bars.size());
        f.ev = ev;
        f.label = ev.request().label;
        f.origin = ev.definition->origin;
        f.at_ord = ctx.coordinate.ordinal;
        f.at_ii = ctx.coordinate.interval_index;
        f.at_eff = ctx.coordinate.effective_time_ms;
        if (const auto pt = current_execution_point()) {
            f.at_px = pt->price;
            f.at_qk = static_cast<int>(pt->quote_kind);
        }
        fills.push_back(f);
        if (h_x && ev.handle() == *h_x) x_done = true;
    }

    bool margin_check_allowed(const NativeMarginCheckPoint& p) const override {
        checks.push_back(CheckRec{p});
        return true;
    }
    std::optional<NativeMarginDecision> resolve_margin_requirement(
            const NativeMarginRequirementView& v) const override {
        reqs.push_back(ReqRec{v});
        return std::nullopt;
    }
    std::optional<double> resolve_margin_call_units(const NativeMarginCallView& v) const override {
        callviews.push_back(v);
        if (!lots_before_taken) {
            lots_before_call = native_open_lots(v.mark);
            lots_before_taken = true;
        }
        return std::nullopt;
    }
    void on_native_margin_call(const no::MarginCallEvent& e) override {
        calls.push_back(e);
        lots_after_call = native_open_lots(e.mark);
    }
    std::optional<double> resolve_anchored_level(const NativeAnchoredLevelView& v) const override {
        anchors.push_back(AnchorRec{v});
        return std::nullopt;
    }
};

// ============================================================ spec
NativeRunSpec make_spec(const Opts& o) {
    NativeRunSpec s;
    s.identity = {"acid-composite", 1};
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
    s.price_tick = ACID_TICK;
    s.slippage_ticks = ACID_SLIPPAGE;
    s.price_grid = NativePriceGrid::QuantizeFillsAndTriggers;
    s.grid_rounding = NativeGridRounding::Directional;
    s.fee_kind = NativeFeeKind::Percent;
    s.fee_value = ACID_FEE_PCT;
    s.quantity_grid = ACID_QTY_GRID;
    NativeMarginModel m;
    m.initial_long = ACID_INITIAL_LONG;       // the long side: an opening gate, no liquidation
    m.initial_short = 0.0;                    // the short side: MAINTENANCE-ONLY
    m.maintenance_short = ACID_MAINT_SHORT;
    m.sizing = NativeLiquidationSizing::RestoreMinimum;
    m.shortfall_multiple = 1.0;
    m.check = NativeLiquidationCheck::PathAdverseExtreme;
    m.liquidation_label = ACID_LIQ_LABEL;
    m.liquidation_comment = ACID_LIQ_COMMENT;
    s.margin = m;
    NativeRiskLimits r;
    r.max_intraday_loss = NativeLossLimit{ACID_RISK_LOSS, false};
    r.day_basis = NativeRiskDay::SessionDay;
    r.action = NativeRiskAction::BlockOpenings;
    s.risk = r;
    s.report_policy = NativeReportPolicy::KernelRecorded;
    s.report_open_position_at_end = true;
    s.calculation = NativeCalculationTrigger::BarCloseAndFills;
    s.max_recalculations_per_point = ACID_MAX_RECALCS;
    NativeTimeframeSubscription hour;
    hour.tf = "60";
    NativeTimeframeSubscription half;
    half.tf = "30";
    half.source = NativeSeriesSource::AuxiliaryFeed;
    s.subscriptions = {hour, half};
    NativeAuxiliaryFeed feed;
    feed.tf = "5";
    // A stream declares what it knows at begin (the warmup's feed bars) and
    // appends the rest live, one input at a time (native-engine.md:2655-2668).
    // A batch cut short keeps the whole spec: prefix closure compares two runs
    // of the SAME spec (native-engine.md:2024-2027).
    const int feed_bars = o.stream ? 3 * ACID_WARMUP : ACID_FEED;
    feed.bars.assign(g_b5.begin(), g_b5.begin() + feed_bars);
    s.auxiliary_feed = feed;
    s.event_retention = NativeEventRetention::Full;
    if (o.magnify) {
        IntrabarPath::lower_tf lower;
        lower.bars = g_b5;
        lower.tf = "5";
        lower.samples = 4;
        s.intrabar.value = lower;
    }
    return s;
}

NativeFxCurve make_curve() {
    NativeFxCurve c;
    c.effective_from_ms = {ACID_FX_T0, ACID_FX_T1};
    c.account_per_quote = {ACID_FX_R0, ACID_FX_R1};
    return c;
}

// ============================================================ a run
struct Run {
    Opts opt;
    std::unique_ptr<AcidHost> host;
    ReportC report{};
    bool report_filled = false;
    NativeSetupResult setup{};
    NativeFxCurveSetupResult bad_curve{}, good_curve{};
    std::vector<std::pair<std::string, NativeAuxiliaryAppendResult>> appends;
    bool drive_ok = true;
    std::string last_error;
    uint64_t scalar = 0, continuation = 0, stream_hash = 0;
    NativeStateView state{};
    std::vector<NativeMarketEvent> events;
    ~Run() {
        if (report_filled) BacktestEngine::free_report(&report);
    }
};

std::unique_ptr<Run> execute(const Opts& o) {
    auto run = std::make_unique<Run>();
    run->opt = o;
    run->host = std::make_unique<AcidHost>(o);
    AcidHost& h = *run->host;
    run->setup = h.configure_native(make_spec(o));
    if (run->setup.status != NativeSetupStatus::Applied) {
        std::printf("configure refused: %d/%d %s\n", static_cast<int>(run->setup.validation.error),
                    static_cast<int>(run->setup.validation.field), h.last_error().c_str());
        run->drive_ok = false;
        return run;
    }
    // A curve refused while Ready changes nothing (F12); then the real curve.
    NativeFxCurve bad = make_curve();
    std::swap(bad.effective_from_ms[0], bad.effective_from_ms[1]);
    run->bad_curve = h.configure_native_fx_curve(bad);
    run->good_curve = h.configure_native_fx_curve(make_curve());
    h.set_broker_state_hash_recording(true);
    if (!o.stream) {
        h.run(g_b15.data(), o.bars);
        run->appends.emplace_back("after_batch", h.append_auxiliary_bars_result(&g_b5[0], 1));
    } else {
        run->drive_ok = h.stream_begin(g_b15.data(), ACID_WARMUP, "15", "15");
        for (int i = ACID_WARMUP; i < o.bars && run->drive_ok; ++i) {
            if (i == ACID_WARMUP + 2) {
                // Typed append refusals between inputs, changing nothing.
                Bar pair[2] = {g_b5[3 * i + 1], g_b5[3 * i]};
                run->appends.emplace_back("unordered_pair", h.append_auxiliary_bars_result(pair, 2));
                Bar stale = g_b5[3 * (i - 1)];
                run->appends.emplace_back("stale_bar", h.append_auxiliary_bars_result(&stale, 1));
            }
            const auto a = h.append_auxiliary_bars_result(&g_b5[3 * i], 3);
            if (a.status != NativeSetupStatus::Applied) {
                std::printf("append refused at %d: %s\n", i, append_word(a.error));
                run->drive_ok = false;
            }
            run->drive_ok = run->drive_ok && h.stream_push_bar(g_b15[i]);
        }
        run->drive_ok = run->drive_ok && h.stream_end(false);
        run->appends.emplace_back("after_end", h.append_auxiliary_bars_result(&g_b5[0], 1));
        run->stream_hash = h.stream_state_hash();
    }
    run->last_error = h.last_error();
    run->state = h.native_state();
    h.fill_report(&run->report);
    run->report_filled = true;
    run->scalar = h.broker_state_hash();
    run->continuation = h.native_continuation_hash();
    run->events = h.native_events(0);
    return run;
}

// ============================================================ event projection
// The flattening src/native_c_host.cpp:1918-2033 performs for
// strategy_native_events_v1, over the C++ rows (every C word is the C++
// enumerator's integer, pinned by static_asserts in native_c_host.cpp).
template <class E> uint32_t w(E e) { return static_cast<uint32_t>(e); }
void fill_cursor(pf_native_event_v1& o, const no::MatchCursor& c) {
    o.effective_time_ms = c.point.effective_time_ms;
    o.interval_index = c.point.interval_index;
    o.provenance = static_cast<uint8_t>(c.point.provenance);
    o.path_phase = static_cast<uint8_t>(c.point.path_phase);
}
pf_native_event_v1 blank(uint32_t kind, uint64_t ordinal) {
    pf_native_event_v1 o;
    std::memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.version = PF_NATIVE_API_VERSION;
    o.kind = kind;
    o.ordinal = ordinal;
    return o;
}
pf_native_event_v1 project(const NativeMarketEvent& e) {
    if (e.kind == NativeEventKind::Driver && e.driver) {
        auto o = blank(PF_NATIVE_EVENT_DRIVER_POINT, e.ordinal);
        o.raw_price = e.driver->raw_price;
        o.price = e.driver->raw_price;
        o.effective_time_ms = e.driver->coordinate.effective_time_ms;
        o.interval_index = e.driver->coordinate.interval_index;
        o.provenance = static_cast<uint8_t>(e.driver->coordinate.provenance);
        o.path_phase = static_cast<uint8_t>(e.driver->coordinate.path_phase);
        return o;
    }
    if (e.kind == NativeEventKind::Account && e.account) {
        auto o = blank(PF_NATIVE_EVENT_ACCOUNT, e.ordinal);
        o.price = e.account->marked_equity;
        o.raw_price = e.account->realized_balance;
        o.opened_units = e.account->signed_units;
        o.effective_time_ms = e.account->effective_time_ms;
        return o;
    }
    const uint64_t ord = e.ordinal;
    return std::visit([&](const auto& p) -> pf_native_event_v1 {
        using T = std::decay_t<decltype(p)>;
        pf_native_event_v1 o{};
        if constexpr (std::is_same_v<T, no::AcceptedEvent>) {
            o = blank(PF_NATIVE_EVENT_ACCEPTED, ord);
            o.incarnation = p.handle().incarnation;
        } else if constexpr (std::is_same_v<T, no::RejectedEvent>) {
            o = blank(PF_NATIVE_EVENT_REJECTED, ord);
            o.reason = w(p.reason);
        } else if constexpr (std::is_same_v<T, no::ReplacedEvent>) {
            o = blank(PF_NATIVE_EVENT_REPLACED, ord);
            o.incarnation = p.predecessor().incarnation;
            o.successor = p.successor().incarnation;
        } else if constexpr (std::is_same_v<T, no::ReplaceRejectedEvent>) {
            o = blank(PF_NATIVE_EVENT_REPLACE_REJECTED, ord);
            o.incarnation = p.target().incarnation;
            o.reason = w(p.reason);
        } else if constexpr (std::is_same_v<T, no::CancelledEvent>) {
            o = blank(PF_NATIVE_EVENT_CANCELLED, ord);
            o.incarnation = p.handle().incarnation;
            o.reason = w(p.reason);
        } else if constexpr (std::is_same_v<T, no::NotWorkingEvent>) {
            o = blank(PF_NATIVE_EVENT_NOT_WORKING, ord);
            o.incarnation = p.target.incarnation;
        } else if constexpr (std::is_same_v<T, no::InvalidHandleEvent>) {
            o = blank(PF_NATIVE_EVENT_INVALID_HANDLE, ord);
            o.incarnation = p.target.incarnation;
        } else if constexpr (std::is_same_v<T, no::NoEffectEvent>) {
            o = blank(PF_NATIVE_EVENT_NO_EFFECT, ord);
            o.incarnation = p.handle().incarnation;
            fill_cursor(o, p.cursor);
        } else if constexpr (std::is_same_v<T, no::MatchRejectedEvent>) {
            o = blank(PF_NATIVE_EVENT_MATCH_REJECTED, ord);
            o.incarnation = p.handle().incarnation;
            o.reason = w(p.reason);
            fill_cursor(o, p.cursor);
        } else if constexpr (std::is_same_v<T, no::ExecutionAppliedEvent>) {
            o = blank(PF_NATIVE_EVENT_APPLIED, ord);
            o.incarnation = p.handle().incarnation;
            o.raw_price = p.raw_price;
            o.resolved_price = p.resolved_price;
            o.price = p.resolved_price;
            o.closed_units = p.closed_units;
            o.opened_units = p.opened_units;
            o.cycle_before = p.cycle_before;
            o.cycle_after = p.cycle_after;
            o.terminal = p.terminal ? 1 : 0;
            if (p.terminal_reason) o.reason = w(*p.terminal_reason);
            fill_cursor(o, p.cursor);
        } else if constexpr (std::is_same_v<T, no::CloseBoundEvent>) {
            o = blank(PF_NATIVE_EVENT_CLOSE_BOUND, ord);
            o.incarnation = p.definition->handle.incarnation;
            o.reason = w(p.side);
            o.cycle_after = p.cycle;
            fill_cursor(o, p.cursor);
        } else if constexpr (std::is_same_v<T, no::ActivatedEvent>) {
            o = blank(PF_NATIVE_EVENT_ACTIVATED, ord);
            o.incarnation = p.definition->handle.incarnation;
            o.reason = w(p.kind);
            o.price = p.reached_price;
            o.raw_price = p.reached_price;
            fill_cursor(o, p.cursor);
        } else if constexpr (std::is_same_v<T, no::ReservationReducedEvent>) {
            o = blank(PF_NATIVE_EVENT_RESERVATION_REDUCED, ord);
            o.incarnation = p.recipient.incarnation;
            o.reason = w(p.effect);
            o.closed_units = p.actual_deduction;
        } else if constexpr (std::is_same_v<T, no::DeferredGroupAdjustmentEvent>) {
            o = blank(PF_NATIVE_EVENT_DEFERRED_GROUP, ord);
            o.incarnation = p.recipient.incarnation;
            o.reason = w(p.effect);
            o.closed_units = p.deferred_delta;
        } else if constexpr (std::is_same_v<T, no::QuantityBoundEvent>) {
            o = blank(PF_NATIVE_EVENT_QUANTITY_BOUND, ord);
            o.incarnation = p.definition->handle.incarnation;
            o.opened_units = p.source_units;
        } else if constexpr (std::is_same_v<T, no::ArmedEvent>) {
            o = blank(PF_NATIVE_EVENT_ARMED, ord);
            o.incarnation = p.definition->handle.incarnation;
        } else if constexpr (std::is_same_v<T, no::TermsResolvedEvent>) {
            o = blank(PF_NATIVE_EVENT_TERMS_RESOLVED, ord);
            o.incarnation = p.handle().incarnation;
            o.raw_price = p.input.raw_price;
            o.resolved_price = p.input.terms.resolved_price;
            o.price = p.input.terms.resolved_price;
            fill_cursor(o, p.cursor);
        } else if constexpr (std::is_same_v<T, no::MarginCallEvent>) {
            o = blank(PF_NATIVE_EVENT_MARGIN_CALL, ord);
            o.incarnation = p.handle().incarnation;
            o.reason = w(p.side);
            o.price = p.mark;
            o.resolved_price = p.mark;
            o.raw_price = p.liquidation_price;
            o.closed_units = p.units;
            o.opened_units = p.position_after;
            fill_cursor(o, p.cursor);
        } else if constexpr (std::is_same_v<T, no::NativeRiskEvent>) {
            o = blank(PF_NATIVE_EVENT_RISK, ord);
            o.reason = w(p.kind);
            o.price = p.observed;
            o.raw_price = p.limit;
            o.cycle_before = p.day_ordinal;
            o.successor = p.cursor.point.ordinal;
            fill_cursor(o, p.cursor);
        }
        return o;
    }, *e.command);
}

// ============================================================ C words of C++ values
uint32_t intent_word(const no::OrderIntent& i, double& value) {
    value = 0.0;
    return std::visit([&](const auto& a) -> uint32_t {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, no::Flatten>) {
            return PF_NATIVE_INTENT_FLATTEN;
        } else if constexpr (std::is_same_v<T, no::Reduce>) {
            if (const auto* u = std::get_if<no::ExplicitUnits>(&a.size)) value = u->units;
            else if (const auto* f = std::get_if<no::ScopeFraction>(&a.size)) value = f->fraction;
            return PF_NATIVE_INTENT_REDUCE;
        } else if constexpr (std::is_same_v<T, no::Transact>) {
            value = a.signed_units;
            return PF_NATIVE_INTENT_TRANSACT;
        } else if constexpr (std::is_same_v<T, no::ReverseTo>) {
            value = a.signed_units;
            return PF_NATIVE_INTENT_REVERSE_TO;
        } else if constexpr (std::is_same_v<T, no::HostSized>) {
            return PF_NATIVE_INTENT_HOST_SIZED;
        } else {
            if (const auto* c = std::get_if<no::CashValue>(&a.basis)) value = c->cash;
            else value = std::get<no::EquityFraction>(a.basis).fraction;
            return PF_NATIVE_INTENT_SIZED;
        }
    }, i);
}
uint32_t trigger_word(const no::Trigger& t, double& p1, double& p2) {
    p1 = p2 = 0.0;
    return std::visit([&](const auto& a) -> uint32_t {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, no::Market>) {
            return PF_NATIVE_TRIGGER_MARKET;
        } else if constexpr (std::is_same_v<T, no::Limit>) {
            p1 = a.price;
            return PF_NATIVE_TRIGGER_LIMIT;
        } else if constexpr (std::is_same_v<T, no::Stop>) {
            p1 = a.price;
            return PF_NATIVE_TRIGGER_STOP;
        } else if constexpr (std::is_same_v<T, no::StopLimit>) {
            p1 = a.stop;
            p2 = a.limit;
            return PF_NATIVE_TRIGGER_STOP_LIMIT;
        } else {
            p1 = a.offset;
            if (a.arm_price) p2 = *a.arm_price;
            return PF_NATIVE_TRIGGER_TRAIL;
        }
    }, t);
}

// ============================================================ FIELD emission
std::string lot_line(const NativeOpenLot& l) {
    return fmt("ord:%zu;inc:%llu;cyc:%lld;side:%d;eb:%d;et:%lld;ep:%s;u:%s;com:%s;mark:%s;upnl:%s;"
               "fav:%s;adv:%s;lab:%s;cmt:%s",
               l.ordinal, (ull)l.entry_incarnation, (ll)l.cycle, static_cast<int>(l.side),
               l.entry_bar_index, (ll)l.entry_time_ms, D(l.entry_price).c_str(),
               D(l.signed_units).c_str(), D(l.entry_commission).c_str(), D(l.mark).c_str(),
               D(l.unrealized_pnl).c_str(), D(l.favorable_excursion).c_str(),
               D(l.adverse_excursion).c_str(), l.entry_label.c_str(), l.entry_comment.c_str());
}
void emit_lots(const std::string& key, const std::vector<NativeOpenLot>& lots) {
    field(key, fmt("n:%zu", lots.size()));
    for (std::size_t i = 0; i < lots.size(); ++i) field(fmt("%s.%02zu", key.c_str(), i), lot_line(lots[i]));
}
std::string risk_str(const NativeRiskState& r) {
    return fmt("%d/%d/%d/%d/%lld/%llu/%u/%s/%s", r.blocked ? 1 : 0, r.reason ? 1 : 0,
               r.reason ? static_cast<int>(*r.reason) : 0, r.has_day ? 1 : 0, (ll)r.day_ordinal,
               (ull)r.fills_today, r.consecutive_loss_days, D(r.peak_equity).c_str(),
               D(r.day_open_equity).c_str());
}
std::string cursor_str(const no::MatchCursor& c) {
    return fmt("%llu/%lld/%s/%d/%d/%d", (ull)c.point.ordinal, (ll)c.point.effective_time_ms,
               D(c.t).c_str(), c.point.interval_index, static_cast<int>(c.point.provenance),
               static_cast<int>(c.point.path_phase));
}

void emit_run(const Run& run) {
    const AcidHost& h = *run.host;
    const std::string p = run.opt.tag;
    const ReportC& r = run.report;
    field(p + ".setup", fmt("status:%d;err:%d;field:%d", static_cast<int>(run.setup.status),
                             static_cast<int>(run.setup.validation.error),
                             static_cast<int>(run.setup.validation.field)));
    field(p + ".fx_curve", fmt("bad:%d/%d/%zu;good:%d/%d/%zu", static_cast<int>(run.bad_curve.status),
                                static_cast<int>(run.bad_curve.validation.error),
                                run.bad_curve.validation.index, static_cast<int>(run.good_curve.status),
                                static_cast<int>(run.good_curve.validation.error),
                                run.good_curve.validation.index));
    field(p + ".report",
          fmt("trades:%d;trades_len:%d;net:%s;inputs:%lld;scripts:%lld;mag_sub:%lld;mag_ticks:%lld;"
              "itf:%d;stf:%d;ratio:%d;agg:%d;mag:%d;eq_len:%lld;hash_len:%lld",
              r.total_trades, r.trades_len, D(r.net_profit).c_str(), (ll)r.input_bars_processed,
              (ll)r.script_bars_processed, (ll)r.magnifier_sub_bars_total,
              (ll)r.magnifier_sample_ticks_total, r.input_tf_seconds, r.script_tf_seconds,
              r.script_tf_ratio, r.needs_aggregation, r.bar_magnifier_enabled,
              (ll)r.equity_curve_len, (ll)r.broker_state_hash_len));
    for (int i = 0; i < r.trades_len; ++i) {
        const TradeC& t = r.trades[i];
        const Trade& row = h.get_report_trade(i);
        field(fmt("%s.trade.%02d", p.c_str(), i),
              fmt("et:%lld;xt:%lld;ep:%s;xp:%s;pnl:%s;pct:%s;long:%d;ru:%s;dd:%s;qty:%s;com:%s;"
                  "eb:%d;xb:%d;end:%d;eid:%s;xid:%s;xc:%s;cause:%d;inc:%llu",
                  (ll)t.entry_time, (ll)t.exit_time, D(t.entry_price).c_str(), D(t.exit_price).c_str(),
                  D(t.pnl).c_str(), D(t.pnl_pct).c_str(), t.is_long, D(t.max_runup).c_str(),
                  D(t.max_drawdown).c_str(), D(t.qty).c_str(), D(t.commission).c_str(),
                  t.entry_bar_index, t.exit_bar_index, t.open_at_end, row.entry_id.c_str(),
                  row.exit_id.c_str(), row.exit_comment.c_str(), h.closed_trade_close_cause(i),
                  (ull)row.entry_incarnation));
        xfield("C-SURFACE-2:closed-trade-entry-comment", fmt("%s.trade.%02d.entry_comment", p.c_str(), i),
               row.entry_comment);
    }
    // Per bar: the decision the calculation was handed, and the report row.
    std::string open_ended, drv;
    uint64_t half_digest = ACID_FNV_OFFSET, qo_digest = ACID_FNV_OFFSET;
    for (std::size_t i = 0; i < h.bars.size(); ++i) {
        const BarRec& b = h.bars[i];
        const auto& c = b.ctx;
        const auto& iv = c.script_interval;
        const uint64_t row = i < static_cast<std::size_t>(r.broker_state_hash_len) ? r.broker_state_hash[i] : 0;
        const double eq = i < static_cast<std::size_t>(r.equity_curve_len) ? r.equity_curve[i].equity : NAN;
        const double op = i < static_cast<std::size_t>(r.equity_curve_len) ? r.equity_curve[i].open_profit : NAN;
        field(fmt("%s.bar.%02zu", p.c_str(), i),
              fmt("ord:%llu;ii:%d;eff:%lld;sbo:%lld;floor:%lld;px:%s;qk:%d;prov:%d;ph:%d;comp:%d;"
                  "sub:%d/%d/%d;sess:%d%d%d;iv:%lld/%lld/%lld/%lld/%lld;sd:%lld/%lld;row:%llu;eq:%s;op:%s;"
                  "risk:%s",
                  (ull)c.coordinate.ordinal, c.coordinate.interval_index, (ll)c.coordinate.effective_time_ms,
                  (ll)c.script_bar_open_ms, (ll)c.decision_floor_ms, D(b.px).c_str(), b.qk,
                  static_cast<int>(c.coordinate.provenance), static_cast<int>(c.coordinate.path_phase),
                  static_cast<int>(c.coordinate.completion), c.sub_index, c.sub_count,
                  c.is_terminal_sub_bar ? 1 : 0, c.in_session ? 1 : 0, c.opens_session_day ? 1 : 0,
                  c.closes_session_day ? 1 : 0, (ll)iv.open_ms, (ll)iv.eligible_open_ms,
                  (ll)iv.last_traded_close_ms, (ll)iv.next_period_open_ms, (ll)iv.next_input_open_ms,
                  (ll)b.sd, (ll)b.nsd, (ull)row, D(eq).c_str(), D(op).c_str(), risk_str(b.risk).c_str()));
        open_ended += c.closes_session_day_open_ended ? '1' : '0';
        half_digest = acid_fnv_u64(half_digest, b.broker_half);
        qo_digest = acid_fnv_u64(qo_digest, b.quote_origin);
        if (i == 0 || i + 1 == h.bars.size()) {
            const auto& s = c.driver_statistics;
            drv += fmt("%s%zu:%d/%d/%d/%llu/%llu", drv.empty() ? "" : ",", i, s.intrabar_path_enabled ? 1 : 0,
                       s.sub_bars_per_script_bar, s.samples_per_sub_bar, (ull)s.sub_bars_processed,
                       (ull)s.sample_ticks_processed);
        }
    }
    xfield("COVERAGE:closes_session_day_open_ended", p + ".bars.open_ended", open_ended);
    xfield("PROTECTED:broker_state_hash_from_execution_hash", p + ".bars.broker_half_digest",
           fmt("%llu", (ull)half_digest));
    xfield("NOC:quote_origin_ordinal", p + ".bars.quote_origin_digest", fmt("%llu", (ull)qo_digest));
    xfield("NOC:driver_statistics", p + ".bars.driver_statistics", drv);
    for (std::size_t i = 0; i < h.fills.size(); ++i) {
        const FillRec& f = h.fills[i];
        const auto& e = f.ev;
        field(fmt("%s.fill.%02zu", p.c_str(), i),
              fmt("gi:%d;ord:%llu;inc:%llu;lot:%llu;raw:%s;res:%s;tkt:%s;cu:%s;ou:%s;fw:%s;cb:%lld;ca:%lld;"
                  "term:%d;tr:%d;at:%llu/%d/%lld/%s/%d",
                  f.gi, (ull)e.ordinal, (ull)e.handle().incarnation, (ull)e.opened_lot_incarnation,
                  D(e.raw_price).c_str(), D(e.resolved_price).c_str(), D(e.current_ticket).c_str(),
                  D(e.closed_units).c_str(), D(e.opened_units).c_str(), D(e.filled_working).c_str(),
                  (ll)e.cycle_before, (ll)e.cycle_after, e.terminal ? 1 : 0,
                  e.terminal_reason ? static_cast<int>(*e.terminal_reason) : -1, (ull)f.at_ord, f.at_ii,
                  (ll)f.at_eff, D(f.at_px).c_str(), f.at_qk));
        xfield("C-SURFACE-2:applied-origin-label", fmt("%s.fill.%02zu.who", p.c_str(), i),
               fmt("label:%s;origin:%d", f.label.c_str(), static_cast<int>(f.origin)));
    }
    for (std::size_t i = 0; i < h.tfs.size(); ++i) {
        const TfRec& t = h.tfs[i];
        field(fmt("%s.tf.%02zu", p.c_str(), i),
              fmt("sub:%zu;gi:%d;t:%lld;o:%s;h:%s;l:%s;c:%s;v:%s;comp:%d;at:%lld;iv:%lld/%lld/%lld/%lld/%lld;"
                  "pull:%d",
                  t.sub, t.gi, (ll)t.bar.timestamp, D(t.bar.open).c_str(), D(t.bar.high).c_str(),
                  D(t.bar.low).c_str(), D(t.bar.close).c_str(), D(t.bar.volume).c_str(),
                  static_cast<int>(t.completion), (ll)t.delivered_at, (ll)t.iv.open_ms,
                  (ll)t.iv.eligible_open_ms, (ll)t.iv.last_traded_close_ms, (ll)t.iv.next_period_open_ms,
                  (ll)t.iv.next_input_open_ms, t.pulled ? 1 : 0));
    }
    for (std::size_t i = 0; i < h.recalcs.size(); ++i) {
        const RecalcRec& c = h.recalcs[i];
        const Bar pb = c.partial.value_or(Bar{NAN, NAN, NAN, NAN, NAN, 0});
        field(fmt("%s.recalc.%02zu", p.c_str(), i),
              fmt("cause:%llu/%llu;gi:%d;part:%d/%s/%s/%s/%s/%s;px:%s;qk:%d;ord:%llu;risk:%s",
                  (ull)c.cause_ord, (ull)c.cause_inc, c.gi, c.partial ? 1 : 0, D(pb.open).c_str(),
                  D(pb.high).c_str(), D(pb.low).c_str(), D(pb.close).c_str(), D(pb.volume).c_str(),
                  D(c.px).c_str(), c.qk, (ull)c.ord, risk_str(c.risk).c_str()));
    }
    field(p + ".recalc.totals", fmt("driven:%llu;skipped:%llu", (ull)h.native_recalculation_count(),
                                    (ull)h.native_recalculations_skipped()));
    // Margin: every check point and requirement view, digested; the FX-roll
    // points and every breached view in full.
    int kinds[4] = {0, 0, 0, 0};
    uint64_t cd = ACID_FNV_OFFSET;
    int fx_n = 0;
    for (const auto& c : h.checks) {
        const auto& q = c.p;
        ++kinds[static_cast<int>(q.kind) & 3];
        cd = acid_fnv_u64(cd, w(q.kind));
        cd = acid_fnv_d(cd, q.position.signed_units);
        cd = acid_fnv_d(cd, q.position.average_price);
        cd = acid_fnv_u64(cd, q.position.lot_count);
        cd = acid_fnv_d(cd, q.mark);
        cd = acid_fnv_u64(cd, q.liquidation_resting ? 1 : 0);
        cd = acid_fnv_u64(cd, q.cursor.point.ordinal);
        cd = acid_fnv_u64(cd, (uint64_t)q.cursor.point.effective_time_ms);
        cd = acid_fnv_d(cd, q.cursor.t);
        if (q.kind == NativeMarginCheckKind::FxRoll) {
            field(fmt("%s.margin.fxroll.%02d", p.c_str(), fx_n++),
                  fmt("units:%s;avg:%s;lots:%zu;mark:%s;rest:%d;cur:%s", D(q.position.signed_units).c_str(),
                      D(q.position.average_price).c_str(), q.position.lot_count, D(q.mark).c_str(),
                      q.liquidation_resting ? 1 : 0, cursor_str(q.cursor).c_str()));
        }
    }
    field(p + ".margin.checks", fmt("n:%zu;bar_open:%d;after_applied:%d;calc:%d;fx_roll:%d;digest:%llu",
                                   h.checks.size(), kinds[0], kinds[1], kinds[2], kinds[3], (ull)cd));
    uint64_t rd = ACID_FNV_OFFSET;
    int rq_n = 0;
    for (const auto& rq : h.reqs) {
        const auto& v = rq.v;
        rd = acid_fnv_u64(rd, w(v.kind));
        rd = acid_fnv_d(rd, v.position.signed_units);
        rd = acid_fnv_d(rd, v.mark);
        rd = acid_fnv_d(rd, v.equity);
        rd = acid_fnv_d(rd, v.required);
        rd = acid_fnv_u64(rd, v.cursor.point.ordinal);
        if (v.kind == NativeMarginCheckKind::FxRoll || v.required > v.equity) {
            field(fmt("%s.margin.req.%02d", p.c_str(), rq_n++),
                  fmt("kind:%d;units:%s;mark:%s;eq:%s;req:%s;cur:%s", static_cast<int>(v.kind),
                      D(v.position.signed_units).c_str(), D(v.mark).c_str(), D(v.equity).c_str(),
                      D(v.required).c_str(), cursor_str(v.cursor).c_str()));
        }
    }
    field(p + ".margin.reqs", fmt("n:%zu;digest:%llu", h.reqs.size(), (ull)rd));
    for (std::size_t i = 0; i < h.callviews.size(); ++i) {
        const auto& v = h.callviews[i];
        field(fmt("%s.margin.callview.%02zu", p.c_str(), i),
              fmt("units:%s;avg:%s;lots:%zu;mark:%s;eq:%s;req:%s;cur:%s", D(v.position.signed_units).c_str(),
                  D(v.position.average_price).c_str(), v.position.lot_count, D(v.mark).c_str(),
                  D(v.equity).c_str(), D(v.required).c_str(), cursor_str(v.cursor).c_str()));
    }
    for (std::size_t i = 0; i < h.calls.size(); ++i) {
        const auto& c = h.calls[i];
        field(fmt("%s.margin.call.%02zu", p.c_str(), i),
              fmt("ord:%llu;inc:%llu;app:%llu;side:%d;mark:%s;eq:%s;req:%s;liq:%s;u:%s;pb:%s;pa:%s;cur:%s",
                  (ull)c.ordinal, (ull)c.handle().incarnation, (ull)c.applied.ordinal,
                  static_cast<int>(c.side), D(c.mark).c_str(), D(c.equity).c_str(), D(c.required).c_str(),
                  D(c.liquidation_price).c_str(), D(c.units).c_str(), D(c.position_before).c_str(),
                  D(c.position_after).c_str(), cursor_str(c.cursor).c_str()));
    }
    if (!h.calls.empty()) {
        emit_lots(p + ".lots.CALLBEFORE", h.lots_before_call);
        emit_lots(p + ".lots.CALLAFTER", h.lots_after_call);
    }
    for (const auto& [tag, lv] : h.liq) field(p + ".liq." + tag, D(lv.value_or(NAN)));
    for (const auto& [tag, lots] : h.lots) emit_lots(p + ".lots." + tag, lots);
    for (const auto& [tag, rows] : h.working) {
        field(p + ".working." + tag, fmt("n:%zu", rows.size()));
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& d = *rows[i].definition;
            double value = 0.0, p1 = 0.0, p2 = 0.0;
            const uint32_t iw = intent_word(d.request.intent, value);
            const uint32_t tw = trigger_word(d.request.trigger, p1, p2);
            double rem = 0.0;
            if (const auto* u = std::get_if<no::RemainingProjectionUnits>(&rows[i].remaining)) rem = u->q;
            const auto* anchor = std::get_if<no::FromOwnerFill>(&d.request.anchor);
            const auto* wait = std::get_if<no::WaitForApplied>(&d.request.owner);
            field(fmt("%s.working.%s.%02zu", p.c_str(), tag.c_str(), i),
                  fmt("inc:%llu;intent:%u/%s;trig:%u;p1:%s;p2:%s;rem:%zu/%s;ts:%zu;org:%d;owner:%zu;anc:%d/%s;"
                      "vis:%d;grp:%zu;lab:%s",
                      (ull)d.handle.incarnation, iw, D(value).c_str(), tw, D(p1).c_str(), D(p2).c_str(),
                      rows[i].remaining.index(), D(rem).c_str(), rows[i].trigger_state.index(),
                      static_cast<int>(d.origin), d.request.owner.index(), anchor ? 1 : 0,
                      D(anchor ? anchor->offset : 0.0).c_str(),
                      wait ? static_cast<int>(wait->visibility) : 0, d.request.group.index(),
                      d.request.label.c_str()));
        }
    }
    for (const auto& [gi, ts] : h.trails) {
        field(fmt("%s.trail.%02d", p.c_str(), gi),
              ts ? fmt("act:%d;best:%s;lvl:%s;aord:%llu", ts->activated ? 1 : 0, D(ts->best_price).c_str(),
                       D(ts->current_level).c_str(), (ull)ts->activation_ordinal)
                 : std::string("absent"));
    }
    for (std::size_t i = 0; i < h.anchors.size(); ++i) {
        const auto& v = h.anchors[i].v;
        field(fmt("%s.anchor.%02zu", p.c_str(), i),
              fmt("owner:%llu;app:%llu;lot:%llu;fill:%s;leg:%llu;side:%d;trig:%d;off:%s;tick:%s;lvl:%s;cur:%s",
                  (ull)v.owner.incarnation, (ull)v.owner_applied_ordinal, (ull)v.owner_lot_incarnation,
                  D(v.owner_fill_price).c_str(), (ull)v.leg.incarnation, static_cast<int>(v.leg_side),
                  static_cast<int>(v.trigger), D(v.offset).c_str(), D(v.price_tick).c_str(),
                  D(v.kernel_level).c_str(), cursor_str(v.owner_cursor).c_str()));
    }
    for (const auto& [tag, rs] : h.risks) field(p + ".risk." + tag, risk_str(rs));
    for (const auto& rf : h.refusals) field(p + ".refusal." + rf.id, fmt("%s;same:%d", rf.word.c_str(), rf.unchanged ? 1 : 0));
    for (const auto& [tag, u] : h.sized) field(p + ".sized." + tag, D(u));
    for (const auto& [id, a] : run.appends)
        field(p + ".append." + id, fmt("%s/%zu", append_word(a.error), a.index));
    field(p + ".bracket", fmt("tp:%d;sl:%d;trail:%d;all:%d", static_cast<int>(h.bracket.take_profit_outcome.state),
                              static_cast<int>(h.bracket.stop_loss_outcome.state),
                              static_cast<int>(h.bracket.trail_outcome.state),
                              h.bracket.every_requested_leg_accepted() ? 1 : 0));
    // X re-issues: the status of each replace (the keep variant is C++ only).
    std::string xs;
    for (const auto& x : h.x_replaces) xs += fmt("%s%s", xs.empty() ? "" : ",", replace_word(x.status));
    field(p + ".x_replaces", xs.empty() ? "none" : xs);
    // Events, flattened exactly as the C API flattens them.
    std::map<uint32_t, int> counts;
    uint64_t ed = ACID_FNV_OFFSET;
    for (const auto& e : run.events) {
        const pf_native_event_v1 pe = project(e);
        ++counts[pe.kind];
        ed = acid_event_fold(ed, &pe);
    }
    field(p + ".events.n", fmt("%zu", run.events.size()));
    for (const auto& [k, n] : counts) field(fmt("%s.events.kind.%02u", p.c_str(), k), fmt("%d", n));
    field(p + ".events.digest", fmt("%llu", (ull)ed));
    field(p + ".counts", fmt("inputs:%d;subbars:%d;begins:%d", h.inputs, h.sub_bars, h.begins));
    const auto& st = run.state;
    field(p + ".final",
          fmt("hash:%llu;cont:%llu;shash:%llu;life:%d;phase:%d;comp:%d;fail:%d/%d;floor:%lld;hw:%llu;drive:%d",
              (ull)run.scalar, (ull)run.continuation, (ull)run.stream_hash, static_cast<int>(st.kind),
              static_cast<int>(st.phase), static_cast<int>(st.completion), static_cast<int>(st.failure.code),
              static_cast<int>(st.failure.operation), (ll)st.decision_floor_ms, (ull)st.consumed_high_water,
              run.drive_ok ? 1 : 0));
    if (h.preview) {
        const auto& pv = *h.preview;
        xfield("C-SURFACE-2:inspect_current_execution", p + ".preview.E6",
               fmt("refusal:%d;ready:%d;ticket:%s;rows:%zu", pv.refusal ? static_cast<int>(*pv.refusal) : -1,
                   pv.settlement_readiness ? static_cast<int>(*pv.settlement_readiness) : -1,
                   D(pv.account.current_ticket).c_str(), pv.closed_row_pnl.size()));
    }
}

// ============================================================ helpers over a run
const FillRec* fill_of(const AcidHost& h, const std::optional<no::RequestHandle>& handle, int nth = 0) {
    if (!handle) return nullptr;
    for (const auto& f : h.fills)
        if (f.ev.handle() == *handle && nth-- == 0) return &f;
    return nullptr;
}
const std::vector<NativeOpenLot>* lots_at(const AcidHost& h, const std::string& tag) {
    for (const auto& [t, l] : h.lots)
        if (t == tag) return &l;
    return nullptr;
}
std::optional<double> liq_at(const AcidHost& h, const std::string& tag) {
    for (const auto& [t, l] : h.liq)
        if (t == tag) return l;
    return std::nullopt;
}
const NativeRiskState* risk_at(const AcidHost& h, const std::string& tag) {
    for (const auto& [t, r] : h.risks)
        if (t == tag) return &r;
    return nullptr;
}
std::vector<const no::CommandEvent*> commands(const Run& r) {
    std::vector<const no::CommandEvent*> out;
    for (const auto& e : r.events)
        if (e.kind == NativeEventKind::Command && e.command) out.push_back(&*e.command);
    return out;
}
template <class T> std::vector<const T*> of(const Run& r) {
    std::vector<const T*> out;
    for (const auto* c : commands(r))
        if (const auto* t = std::get_if<T>(c)) out.push_back(t);
    return out;
}
int trade_index_by_incarnation(const Run& r, uint64_t inc, int nth = 0) {
    for (int i = 0; i < r.report.trades_len; ++i)
        if (r.host->get_report_trade(i).entry_incarnation == inc && nth-- == 0) return i;
    return -1;
}

// ============================================================ the feature checks
void check_run_features(const Run& run) {
    const AcidHost& h = *run.host;
    const ReportC& rep = run.report;
    const std::string m = run.opt.tag;
    // The magnified run exists for F13 (and F14's report identities): a
    // lower_tf path moves fills onto sub-bar points, so the scenario's
    // hand-derived prices are the unmagnified run's and are not re-asserted.
    const bool scenario = !run.opt.magnify;
    auto C = [&](const char* id, const char* name, bool ok, const std::string& what) {
        if (!scenario && std::strcmp(id, "F13") != 0 && std::strcmp(id, "F14") != 0) return;
        fcheck(id, name, ok, m + ": " + what);
    };
    // ---------------- F01 Sized entry with a fee reserve
    {
        const char* id = "F01";
        const char* nm = "sized-entry-with-fee-reserve";
        const double px = buy_fill(g_b15[G_E1FILL].open);   // 200.10 -> 200.25 + 0.25
        const double units = floor_grid((ACID_E1_CASH / (1.0 + ACID_FEE_PCT / 100.0)) /
                                        (px * ACID_POINT_VALUE * ACID_ACCOUNT_FX));
        const double units_plain = floor_grid(ACID_E1_CASH / (px * ACID_POINT_VALUE * ACID_ACCOUNT_FX));
        const FillRec* f = fill_of(h, h.h_e1);
        C(id, nm, f != nullptr, "E1 filled");
        if (f) {
            C(id, nm, same(f->ev.resolved_price, px), fmt("E1 price %s == hand %s", D(f->ev.resolved_price).c_str(), D(px).c_str()));
            C(id, nm, f->gi == G_E1FILL, "E1 fills on bar 1 (AtMatch, next eligible point)");
            C(id, nm, same(f->ev.opened_units, units), fmt("E1 units %s == hand floor_0.01(24000/1.001/(price*5*1.2)) %s",
                                                           D(f->ev.opened_units).c_str(), D(units).c_str()));
        }
        C(id, nm, units < units_plain, fmt("the reserve costs units: %s < %s", D(units).c_str(), D(units_plain).c_str()));
        C(id, nm, !h.sized.empty() && same(h.sized[0].second, units), "native_sized_units(reserve) == the fill");
        C(id, nm, h.sized.size() > 1 && same(h.sized[1].second, units_plain), "native_sized_units(no reserve) == hand");
        const auto* lots = lots_at(h, "E1FILL");
        C(id, nm, lots && lots->size() == 1 && near_rel((*lots)[0].entry_commission, fee(units, px, ACID_ACCOUNT_FX), 1e-12),
          "E1's lot carries the percent fee of its fill");
    }
    // ---------------- F02 bracket with anchored legs on a price grid
    {
        const char* id = "F02";
        const char* nm = "bracket-anchored-legs-price-grid";
        C(id, nm, h.bracket.every_requested_leg_accepted() && h.h_tp && h.h_sl, "both legs accepted");
        const auto* w0 = [&]() -> const std::vector<NativeWorkingRequest>* {
            for (const auto& [t, v] : h.working) if (t == "E1") return &v;
            return nullptr; }();
        C(id, nm, w0 && w0->size() == 1, "PendingUntilArmed: before the fill only E1 is a working row");
        const double fill = buy_fill(g_b15[G_E1FILL].open);
        const double tp = ceil_tick(fill + ACID_TP_OFFSET);    // a sell limit rounds up
        const double sl = floor_tick(fill + ACID_SL_OFFSET);   // a sell stop rounds down
        C(id, nm, h.anchors.size() == 2, "two anchored materializations");
        if (h.anchors.size() == 2) {
            const auto& a = h.anchors[0].v;
            const auto& b = h.anchors[1].v;
            C(id, nm, same(a.kernel_level, tp) && a.trigger == NativeAnchoredTrigger::Limit
                          && a.leg_side == no::Side::Short && same(a.owner_fill_price, fill),
              fmt("TP level %s == ceil_tick(%s + 3.1) %s", D(a.kernel_level).c_str(), D(fill).c_str(), D(tp).c_str()));
            C(id, nm, same(b.kernel_level, sl) && b.trigger == NativeAnchoredTrigger::Stop,
              fmt("SL level %s == floor_tick(%s - 2.3) %s", D(b.kernel_level).c_str(), D(fill).c_str(), D(sl).c_str()));
        }
        const auto* w1 = [&]() -> const std::vector<NativeWorkingRequest>* {
            for (const auto& [t, v] : h.working) if (t == "E1FILL") return &v;
            return nullptr; }();
        bool tp_ok = false, sl_ok = false;
        if (w1) {
            for (const auto& row : *w1) {
                const auto& d = *row.definition;
                const bool abs = std::holds_alternative<no::Absolute>(d.request.anchor);
                if (h.h_tp && d.handle == *h.h_tp)
                    if (const auto* l = std::get_if<no::Limit>(&d.request.trigger)) tp_ok = abs && same(l->price, tp);
                if (h.h_sl && d.handle == *h.h_sl)
                    if (const auto* s = std::get_if<no::Stop>(&d.request.trigger)) sl_ok = abs && same(s->price, sl);
            }
        }
        C(id, nm, tp_ok && sl_ok, "after the arm the legs read Absolute at the installed levels");
        const FillRec* t = fill_of(h, h.h_tp);
        C(id, nm, t && same(t->ev.resolved_price, tp) && t->gi == G_TP, "TP fills AT its level (slippage capped by the limit) on bar 5");
        const int ti = h.h_e1 ? trade_index_by_incarnation(run, h.h_e1->incarnation) : -1;
        C(id, nm, ti >= 0 && h.closed_trade_close_cause(ti) == 2, "the TP row's close cause is BRACKET (2)");
        bool sl_group = false;
        for (const auto* c : of<no::CancelledEvent>(run))
            if (h.h_sl && c->handle() == *h.h_sl) sl_group = c->reason == no::CancelReason::Group;
        C(id, nm, sl_group, "the SL leaves the book as the TP's OCA sibling (CancelReason::Group)");
    }
    // ---------------- F04 trail in ticks
    {
        const char* id = "F04";
        const char* nm = "trail-in-ticks";
        const auto* w = [&]() -> const std::vector<NativeWorkingRequest>* {
            for (const auto& [t, v] : h.working) if (t == "E3") return &v;
            return nullptr; }();
        bool resolved = false;
        if (w)
            for (const auto& row : *w)
                if (h.h_t3 && row.definition->handle == *h.h_t3)
                    if (const auto* tr = std::get_if<no::Trail>(&row.definition->request.trigger))
                        resolved = same(tr->offset, ACID_T3_TICKS * ACID_TICK) && !tr->ticks;
        C(id, nm, resolved, "TrailTicks{6} resolved at acceptance to the price distance 1.5, spelling cleared");
        // The running best is the favourable enclosing tick of each print (a sell
        // trail: ceil), the level best - 6 ticks (native-engine.md:384-387, :900-915).
        const double best[3] = {ceil_tick(g_b15[G_E3FILL].high), ceil_tick(g_b15[G_E3FILL + 1].high),
                                ceil_tick(g_b15[G_E3FILL + 2].high)};
        for (int k = 0; k < 3; ++k) {
            const int gi = G_E3FILL + k;
            std::optional<NativeTrailState> ts;
            for (const auto& [g, s] : h.trails) if (g == gi) ts = s;
            C(id, nm, ts && ts->activated && same(ts->best_price, best[k]) && same(ts->current_level, best[k] - 1.5),
              fmt("bar %d trail best %s level %s", gi, ts ? D(ts->best_price).c_str() : "-",
                  ts ? D(ts->current_level).c_str() : "-"));
        }
        const FillRec* f = fill_of(h, h.h_t3);
        const double level = best[2] - 1.5;
        C(id, nm, f && f->gi == G_T3X && same(f->ev.resolved_price, floor_tick(level) - ACID_TICK),
          fmt("the trail exits on bar %d at level %s less one tick", G_T3X, D(level).c_str()));
        const int ti = h.h_e3 ? trade_index_by_incarnation(run, h.h_e3->incarnation) : -1;
        C(id, nm, ti >= 0 && h.closed_trade_close_cause(ti) == 2, "the trail row's close cause is BRACKET (2)");
    }
    // ---------------- F05 maintenance-only margin, kernel liquidation via open lots
    {
        const char* id = "F05";
        const char* nm = "maintenance-only-margin-kernel-liquidation-open-lots";
        const FillRec* a = fill_of(h, h.h_e4a);
        const FillRec* b = fill_of(h, h.h_e4b);
        C(id, nm, a && b, "the two short lots filled (no opening gate on the maintenance-only side)");
        if (a && b) {
            C(id, nm, same(a->ev.resolved_price, sell_fill(g_b15[G_E4B].open))
                          && same(b->ev.resolved_price, sell_fill(g_b15[G_E4BFILL].open)),
              "E4a/E4b fill at floor_tick(open) - 1 tick");
            // L = (K + S pv fx)/(Q pv fx (1 + m)), K = the marked-equity intercept
            // (native-engine.md:1201-1205); the lots' own booking gives K and S.
            const auto* lots = lots_at(h, "E4BFILL");
            if (lots && lots->size() == 2) {
                double realized_balance = ACID_CAPITAL;
                for (const auto& f : h.fills)
                    if (f.gi < G_E4A) (void)f;   // realized sums come from the closed rows below
                for (int i = 0; i < rep.trades_len; ++i)
                    if (rep.trades[i].exit_time < acid_day_ms(2)) realized_balance += rep.trades[i].pnl;
                double open_fees = 0, S = 0, Q = 0;
                for (const auto& l : *lots) {
                    open_fees += l.entry_commission;
                    S += -l.signed_units * l.entry_price;
                    Q += -l.signed_units;
                }
                const double K = realized_balance - open_fees;
                auto L = [&](double fx) {
                    return (K + S * ACID_POINT_VALUE * fx) / (Q * ACID_POINT_VALUE * fx * (1.0 + ACID_MAINT_SHORT));
                };
                const auto l12 = liq_at(h, "E4BFILL");
                const auto lpre = liq_at(h, "PRESTEP");
                const auto l125 = liq_at(h, "STEP");
                C(id, nm, lpre && near_rel(*lpre, L(ACID_FX_R0), 1e-12),
                  fmt("L at 10:45, before the 11:00 step, still at 1.20: %s", D(lpre.value_or(NAN)).c_str()));
                C(id, nm, l12 && near_rel(*l12, L(ACID_FX_R0), 1e-12),
                  fmt("L(1.20) %s == hand %s", D(l12.value_or(NAN)).c_str(), D(L(ACID_FX_R0)).c_str()));
                C(id, nm, l125 && near_rel(*l125, L(ACID_FX_R1), 1e-12),
                  fmt("L(1.25) %s == hand %s", D(l125.value_or(NAN)).c_str(), D(L(ACID_FX_R1)).c_str()));
                const double H = g_b15[G_SPIKE].high;
                C(id, nm, L(ACID_FX_R1) < H && H < L(ACID_FX_R0),
                  "the spike crosses L at the new rate only: the step is what liquidates");
                // RestoreMinimum at the adverse mark H (native-engine.md:1284-1294).
                const double fx = ACID_FX_R1;
                const double eqH = K + (S - Q * H) * ACID_POINT_VALUE * fx;
                const double reqH = Q * H * ACID_POINT_VALUE * fx * ACID_MAINT_SHORT;
                const double u = floor_grid((reqH - eqH) / (H * ACID_POINT_VALUE * fx * ACID_MAINT_SHORT));
                C(id, nm, h.calls.size() == 1, "exactly one kernel liquidation");
                C(id, nm, h.callviews.size() == 1 && same(h.callviews[0].mark, H),
                  "the call is sized at the path's adverse extreme (the spike high)");
                if (h.calls.size() == 1) {
                    const auto& c = h.calls[0];
                    C(id, nm, same(c.units, u), fmt("RestoreMinimum units %s == hand %s", D(c.units).c_str(), D(u).c_str()));
                    C(id, nm, same(c.mark, ceil_tick(L(fx)) + ACID_TICK),
                      fmt("fills at ceil_tick(L) + 1 tick: %s", D(c.mark).c_str()));
                    C(id, nm, c.handle().incarnation != 0 && c.definition->origin == no::RequestOrigin::KernelLiquidation,
                      "origin KernelLiquidation");
                    C(id, nm, same(c.position_before, -Q) && same(c.position_after, -(Q - u)), "book before/after");
                }
                // The open-lots reader: [E4a, E4b] before; E4b alone, shrunk, same incarnation, after.
                const auto& lb = h.lots_before_call;
                const auto& la = h.lots_after_call;
                C(id, nm, lb.size() == 2 && la.size() == 1 && h.h_e4b
                              && la[0].entry_incarnation == h.h_e4b->incarnation
                              && same(la[0].signed_units, -(Q - u)),
                  "open lots: FIFO across lots, the survivor keeps E4b's incarnation");
                int liq_rows = 0;
                for (int i = 0; i < rep.trades_len; ++i) {
                    const Trade& row = h.get_report_trade(i);
                    if (row.exit_id == ACID_LIQ_LABEL && row.exit_comment == ACID_LIQ_COMMENT
                        && h.closed_trade_close_cause(i) == 3)
                        ++liq_rows;
                }
                C(id, nm, liq_rows == 2, "two closed rows under the broker ticket, cause MARGIN_CALL (3)");
            }
        }
    }
    // ---------------- F06 FX curve with a step inside the run
    {
        const char* id = "F06";
        const char* nm = "fx-curve-step";
        int rolls = 0;
        const NativeMarginCheckPoint* roll = nullptr;
        for (const auto& c : h.checks)
            if (c.p.kind == NativeMarginCheckKind::FxRoll) {
                ++rolls;
                roll = &c.p;
            }
        C(id, nm, rolls == 1, fmt("one FxRoll point for one step (the restating 10:00 point is none): %d", rolls));
        C(id, nm, roll && roll->cursor.point.effective_time_ms == ACID_FX_T1, "the roll is taken at the step instant");
        const ReqRec* rv = nullptr;
        for (const auto& r : h.reqs)
            if (r.v.kind == NativeMarginCheckKind::FxRoll) rv = &r;
        C(id, nm, rv && near_rel(rv->v.required,
                                 -rv->v.position.signed_units * rv->v.mark * ACID_POINT_VALUE * ACID_FX_R1 * ACID_MAINT_SHORT, 1e-12),
          "the roll's requirement converts at the new rate");
        const FillRec* xf = fill_of(h, h.h_xf);
        const FillRec* e4b = fill_of(h, h.h_e4b);
        C(id, nm, xf && near_rel(xf->ev.current_ticket, fee(xf->ev.closed_units, xf->ev.resolved_price, ACID_FX_R1), 1e-12),
          "a fill after the step pays its fee at 1.25");
        C(id, nm, e4b && near_rel(e4b->ev.current_ticket, fee(e4b->ev.opened_units, e4b->ev.resolved_price, ACID_FX_R0), 1e-12),
          "a fill before the step pays its fee at 1.20");
    }
    // ---------------- F07 risk limit, and the risk refusal
    {
        const char* id = "F07";
        const char* nm = "risk-limit-and-refusal";
        const auto risk_events = of<no::NativeRiskEvent>(run);
        C(id, nm, risk_events.size() == 1, fmt("one breach recorded: %zu", risk_events.size()));
        if (risk_events.size() == 1 && h.calls.size() == 1) {
            const auto& e = *risk_events[0];
            const auto* at_spike = risk_at(h, "SPIKE");
            C(id, nm, e.kind == no::RiskLimitKind::MaxIntradayLoss && same(e.limit, ACID_RISK_LOSS)
                          && e.day_ordinal == ACID_DAY0_ORDINAL + 2,
              "MaxIntradayLoss, limit 2500, Wednesday's session day");
            C(id, nm, at_spike && near_rel(e.observed, at_spike->day_open_equity - h.calls[0].equity, 1e-12),
              fmt("observed %s == day-open equity - equity at the liquidation fill", D(e.observed).c_str()));
            C(id, nm, e.cursor.point.ordinal == h.calls[0].cursor.point.ordinal, "measured at the liquidation's drain");
        }
        const auto* sp = risk_at(h, "SPIKE");
        C(id, nm, sp && sp->blocked && sp->reason && *sp->reason == no::RiskLimitKind::MaxIntradayLoss,
          "blocked at the spike's close");
        bool refused = false;
        for (const auto* mr : of<no::MatchRejectedEvent>(run))
            if (h.h_e5 && mr->handle() == *h.h_e5)
                refused = mr->reason == no::MatchRejectReason::RiskLimit && mr->cursor.point.interval_index == G_E5REJ;
        C(id, nm, refused, "RISK REFUSAL: the opening E5 is MatchRejected(RiskLimit) at the next open");
        C(id, nm, fill_of(h, h.h_e5) == nullptr, "E5 never fills");
        const FillRec* xf = fill_of(h, h.h_xf);
        C(id, nm, xf && xf->gi == G_XFFILL, "a reduction (the flatten) still fills while blocked");
        const auto* d4 = risk_at(h, "D4");
        C(id, nm, d4 && !d4->blocked && d4->day_ordinal == ACID_DAY0_ORDINAL + 3, "Thursday opens unblocked");
        C(id, nm, fill_of(h, h.h_d4[0]) != nullptr, "and its opening fills");
    }
    // ---------------- F08 calc-on-fills
    {
        const char* id = "F08";
        const char* nm = "calc-on-fills";
        const std::size_t fills = h.fills.size();
        C(id, nm, h.native_recalculation_count() + h.native_recalculations_skipped() == fills,
          fmt("one OrderFill recalculation per applied execution: %llu + %llu == %zu",
              (ull)h.native_recalculation_count(), (ull)h.native_recalculations_skipped(), fills));
        C(id, nm, h.native_recalculations_skipped() == 2,
          "four fills at Thursday's open, a budget of 2: two dropped");
        C(id, nm, h.recalcs.size() == h.native_recalculation_count(), "every driven recalculation reached the host");
        bool causes_ok = true;
        std::set<uint64_t> fill_ords;
        for (const auto& f : h.fills) fill_ords.insert(f.ev.ordinal);
        for (const auto& r : h.recalcs) causes_ok = causes_ok && fill_ords.count(r.cause_ord) == 1;
        C(id, nm, causes_ok, "each recalculation's cause is an applied execution");
        const FillRec* tp = fill_of(h, h.h_tp);
        const FillRec* e2 = fill_of(h, h.h_e2);
        C(id, nm, tp && e2 && e2->gi == G_E2FILL, "E2, born in the take-profit's recalculation, fills at the next open");
    }
    // ---------------- F09 60-minute series + auxiliary feed + interval query
    {
        const char* id = "F09";
        const char* nm = "htf-subscription-aux-feed-interval";
        int n0 = 0, n1 = 0, bad = 0, pulls = 0;
        for (const auto& t : h.tfs) {
            const bool hour = t.sub == 0;
            (hour ? n0 : n1)++;
            pulls += t.pulled ? 1 : 0;
            const int64_t span = (hour ? 60 : 30) * ACID_MINUTE_MS;
            // Hand aggregation over [open, open + span) of the input (hourly) or feed (half-hour).
            const std::vector<Bar>& src = hour ? g_b15 : g_b5;
            double o = NAN, hi = -1e300, lo = 1e300, c = NAN, v = 0;
            int64_t last_ts = 0;
            for (const auto& b : src) {
                if (b.timestamp < t.bar.timestamp || b.timestamp >= t.bar.timestamp + span) continue;
                if (o != o) o = b.open;
                hi = std::max(hi, b.high);
                lo = std::min(lo, b.low);
                c = b.close;
                v += b.volume;
                last_ts = b.timestamp;
            }
            // The input bar the bucket rides on: the one whose period holds its last contributing bar.
            const int64_t carrier = last_ts - (last_ts - acid_day_ms(0)) % (15 * ACID_MINUTE_MS);
            const bool ok = same(t.bar.open, o) && same(t.bar.high, hi) && same(t.bar.low, lo)
                            && same(t.bar.close, c) && same(t.bar.volume, v)
                            && t.completion == NativeCompletionKind::Confirmed && t.delivered_at == carrier
                            && t.iv.open_ms == t.bar.timestamp && t.iv.next_period_open_ms == t.bar.timestamp + span;
            if (!ok) ++bad;
        }
        const int bars = run.opt.bars;
        const int hours = (bars / ACID_SLOTS) * 6 + (bars % ACID_SLOTS) / 4;
        const int halves = (bars / ACID_SLOTS) * 12 + (bars % ACID_SLOTS) / 2;
        C(id, nm, n0 == hours, fmt("\"60\" over the input: %d buckets (hand %d)", n0, hours));
        C(id, nm, n1 == halves, fmt("\"30\" from the \"5\" auxiliary feed: %d buckets (hand %d)", n1, halves));
        C(id, nm, bad == 0, fmt("every bucket is the hand aggregate, its interval and carrier the hand grid (%d bad)", bad));
        C(id, nm, pulls == static_cast<int>(h.tfs.size()), "native_series_bar answers the delivered bucket inside the callback");
    }
    // ---------------- F10 session-day first/last flags
    {
        const char* id = "F10";
        const char* nm = "session-day-flags";
        int bad = 0;
        for (std::size_t i = 0; i < h.bars.size(); ++i) {
            const auto& c = h.bars[i].ctx;
            const int gi = static_cast<int>(i);
            const bool last_of_run = gi == run.opt.bars - 1;
            const bool first_of_day = acid_slot_of(gi) == 0;
            const bool last_of_day = acid_slot_of(gi) == ACID_SLOTS - 1;
            // A batch's final bar closes its day (complete input); a stream reads
            // the calendar there (market_driver.hpp:143-147).
            const bool closes = last_of_day || (last_of_run && !run.opt.stream);
            const bool ok = c.in_session && c.opens_session_day == first_of_day && c.closes_session_day == closes
                            && c.closes_session_day_open_ended == last_of_day
                            && h.bars[i].sd == ACID_DAY0_ORDINAL + acid_day_of(gi);
            if (!ok) ++bad;
        }
        C(id, nm, bad == 0, fmt("opens on 09:00, closes on 14:45 (and a batch's final bar), day ordinals (%d bad)", bad));
    }
    // ---------------- F11 partial close across lots + exact-sum close
    {
        const char* id = "F11";
        const char* nm = "fifo-partial-across-lots-and-exact-sum";
        const auto* d4 = lots_at(h, "D4FILL");
        C(id, nm, d4 && d4->size() == 4, "four Thursday lots");
        const FillRec* p1 = fill_of(h, h.h_p1);
        C(id, nm, p1 && same(p1->ev.closed_units, 1.5) && p1->ev.closed_trade_count == 2, "P1 closes 1.5 in two rows");
        const double r1 = 1.5 - 1.1;                      // 0.3999999999999999
        const double keep7 = 0.7 - r1;                    // 0.30000000000000004
        if (p1 && p1->ev.closed_trade_count == 2) {
            const Trade& a = h.get_report_trade(static_cast<int>(p1->ev.first_trade_index));
            const Trade& b = h.get_report_trade(static_cast<int>(p1->ev.first_trade_index + 1));
            C(id, nm, same(a.qty, 1.1) && same(b.qty, r1), fmt("P1 rows %s + %s", D(a.qty).c_str(), D(b.qty).c_str()));
        }
        const auto* after1 = lots_at(h, "P1FILL");
        C(id, nm, after1 && after1->size() == 3 && same((*after1)[0].signed_units, keep7),
          fmt("E7 keeps %s", after1 && !after1->empty() ? D((*after1)[0].signed_units).c_str() : "-"));
        const double U = keep7 + 0.2;                     // fl(l0 + l1) == 0.5
        C(id, nm, same(h.p2_units, U) && same(U, 0.5), fmt("P2 = fl(l0 + l1) = %s", D(h.p2_units).c_str()));
        C(id, nm, (U - keep7) < 0.2, "non-vacuous: fl(U - l0) < l1, the case that used to leave a dust lot (K-ULP3)");
        const FillRec* p2 = fill_of(h, h.h_p2);
        C(id, nm, p2 && same(p2->ev.closed_units, U) && p2->ev.closed_trade_count == 2, "P2 closes U in two rows");
        if (p2 && p2->ev.closed_trade_count == 2) {
            const Trade& a = h.get_report_trade(static_cast<int>(p2->ev.first_trade_index));
            const Trade& b = h.get_report_trade(static_cast<int>(p2->ev.first_trade_index + 1));
            C(id, nm, same(a.qty, keep7) && same(b.qty, 0.2), "P2's rows are the two whole lots");
        }
        const auto* after2 = lots_at(h, "P2FILL");
        C(id, nm, after2 && after2->size() == 1 && h.h_d4[3] && (*after2)[0].entry_incarnation == h.h_d4[3]->incarnation
                      && same((*after2)[0].signed_units, 0.1),
          "EXACT SUM: only E9 (0.1) is left, no dust lot");
    }
    // ---------------- F12 typed refusal path
    {
        const char* id = "F12";
        const char* nm = "typed-refusal-path";
        const std::map<std::string, std::string> want = {
            {"R01.transact_zero", "Rejected/InvalidQuantity"},
            {"R02.off_grid", "Rejected/OffGrid"},
            {"R03.bad_basis", "Rejected/InvalidQuantityBasis"},
            {"R04.placement", "Rejected/PlacementAdmission"},
            {"R05.declare_outside_begin", fmt("Failed/%d/%d", static_cast<int>(NativeRunSpecError::WrongPhase),
                                              static_cast<int>(NativeRunSpecField::None))},
            {"R06.fx_curve_while_running", fmt("Failed/%d/0", static_cast<int>(NativeFxCurveError::WrongPhase))},
            {"R07.written_anchor", "Rejected/InvalidTrigger"},
            {"R08.sized_child", "Rejected/InvalidOwner"},
            {"R09.replace_terminal", "NotWorking"},
            {"R10.cancel_cancelled_leg", "NotWorking"},
        };
        int seen = 0;
        for (const auto& rf : h.refusals) {
            const auto it = want.find(rf.id);
            C(id, nm, it != want.end() && it->second == rf.word && rf.unchanged,
              fmt("%s: %s (want %s), unchanged %d", rf.id.c_str(), rf.word.c_str(),
                  it != want.end() ? it->second.c_str() : "?", rf.unchanged ? 1 : 0));
            ++seen;
        }
        C(id, nm, seen == static_cast<int>(want.size()), fmt("%d refusals issued", seen));
        C(id, nm, run.bad_curve.status == NativeSetupStatus::Failed
                      && run.bad_curve.validation.error == NativeFxCurveError::NotStrictlyIncreasing
                      && run.bad_curve.validation.index == 1 && run.good_curve.status == NativeSetupStatus::Applied,
          "a curve refused while Ready: NotStrictlyIncreasing at 1; the good curve then applies");
        for (const auto& [aid, a] : run.appends) {
            const bool ok = a.status == NativeSetupStatus::Failed
                            && ((aid == "after_batch" || aid == "after_end") ? a.error == NativeAuxiliaryAppendError::NotRealtime
                                                                             : a.error == NativeAuxiliaryAppendError::UnorderedBars);
            C(id, nm, ok, fmt("append %s: %s/%zu", aid.c_str(), append_word(a.error), a.index));
        }
    }
    // ---------------- F13 magnifier counters (the magnified run; zero elsewhere)
    {
        const char* id = "F13";
        const char* nm = "magnifier-counters-bare-host";
        if (run.opt.magnify) {
            const int bars = run.opt.bars;
            C(id, nm, rep.bar_magnifier_enabled == 1, "the report says magnified");
            C(id, nm, rep.magnifier_sub_bars_total == 3 * bars, fmt("sub-bars %lld == 3 x %d", (ll)rep.magnifier_sub_bars_total, bars));
            C(id, nm, rep.magnifier_sample_ticks_total == 4 * 3 * bars,
              fmt("sample ticks %lld == 4 x sub-bars", (ll)rep.magnifier_sample_ticks_total));
            C(id, nm, h.sub_bars == 3 * bars, fmt("on_native_sub_bar %d == sub-bars", h.sub_bars));
            const auto& s = h.bars.empty() ? NativeDriverStatistics{} : h.bars.back().ctx.driver_statistics;
            C(id, nm, s.intrabar_path_enabled && s.sub_bars_per_script_bar == 3 && s.samples_per_sub_bar == 4,
              "driver statistics: 3 sub-bars x 4 samples per script bar");
            C(id, nm, run.state.kind == NativeLifecycleKind::Completed, "the magnified run completes");
        } else {
            C(id, nm, rep.bar_magnifier_enabled == 0 && rep.magnifier_sub_bars_total == 0
                          && rep.magnifier_sample_ticks_total == 0 && h.sub_bars == 0,
              "an unmagnified run reports no magnifier");
        }
    }
    // ---------------- F14 KernelRecorded report + per-bar broker-state hashes
    {
        const char* id = "F14";
        const char* nm = "kernel-recorded-report-per-bar-hashes";
        const ll n = run.opt.bars;
        C(id, nm, rep.broker_state_hash_len == n && rep.equity_curve_len == n && rep.script_bars_processed == n,
          fmt("len identity %lld == %lld == %lld == %lld", (ll)rep.broker_state_hash_len, (ll)rep.equity_curve_len,
              (ll)rep.script_bars_processed, n));
        C(id, nm, rep.broker_state_hash_len > 0 && rep.broker_state_hash[rep.broker_state_hash_len - 1] == run.scalar,
          "the last row == broker_state_hash() (pineforge.h:447-452)");
        C(id, nm, run.state.kind == NativeLifecycleKind::Completed && run.drive_ok, "the run completes");
        if (rep.equity_curve_len == n) {
            const auto& last = rep.equity_curve[n - 1];
            C(id, nm, near_rel(last.equity, ACID_CAPITAL + h.net() + last.open_profit, 1e-12),
              "equity point = capital + net profit + open profit (pineforge.h:435-437)");
        }
        if (!run.opt.magnify && n == ACID_BARS) {
            const int last = rep.trades_len - 1;
            const bool ok = last >= 0 && rep.trades[last].open_at_end == 1 && h.closed_trade_close_cause(last) == 6
                            && same(rep.trades[last].exit_price, nearest_tick(g_b15[G_LAST].close))
                            && same(rep.trades[last].qty, 0.1);
            C(id, nm, ok, "the still-open E9 is the range-end row: the last close on the nearest tick, no slippage, cause 6");
        }
    }
}

// F03: keep_binding against the plain twin, within one driving mode.
void check_keep(const Run& keep, const Run& plain) {
    const char* id = "F03";
    const char* nm = "reissued-exit-keep-binding";
    const std::string m = keep.opt.stream ? "stream" : "batch";
    auto C = [&](bool ok, const std::string& what) { fcheck(id, nm, ok, m + ": " + what); };
    auto replaced = [](const Run& r) {
        int n = 0;
        for (const auto& x : r.host->x_replaces) n += x.status == no::ReplaceStatus::Replaced;
        return n;
    };
    C(replaced(keep) == 8 && replaced(plain) == 8, "eight re-issues, all Replaced, in both");
    // The X chain: every handle it ever had.
    auto chain = [](const Run& r) {
        std::set<uint64_t> s;
        for (const auto* a : of<no::AcceptedEvent>(r))
            if (a->request().label == "X") s.insert(a->handle().incarnation);
        for (const auto* rp : of<no::ReplacedEvent>(r))
            if (rp->successor_request().label == "X") s.insert(rp->successor().incarnation);
        return s;
    };
    auto bound = [&](const Run& r) {
        const auto c = chain(r);
        int n = 0;
        for (const auto* b : of<no::CloseBoundEvent>(r)) n += c.count(b->definition->handle.incarnation) ? 1 : 0;
        return n;
    };
    const int kb = bound(keep), pb = bound(plain);
    C(pb == 9 && kb == 1, fmt("CloseBoundEvents on the X chain: plain %d (one per successor), keep %d", pb, kb));
    int kept = 0;
    for (const auto* rp : of<no::ReplacedEvent>(keep)) kept += rp->successor_definition->kept_binding ? 1 : 0;
    int plain_kept = 0;
    for (const auto* rp : of<no::ReplacedEvent>(plain)) plain_kept += rp->successor_definition->kept_binding ? 1 : 0;
    C(kept == 8 && plain_kept == 0, fmt("successors carrying kept_binding: keep %d, plain %d", kept, plain_kept));
    // Every other event, ordinal for ordinal (native-engine.md:931-935: "taking
    // the timeline ordinal the event would have taken").
    auto others = [](const Run& r) {
        std::vector<std::pair<uint32_t, uint64_t>> v;
        for (const auto& e : r.events) {
            const auto pe = project(e);
            if (pe.kind == PF_NATIVE_EVENT_CLOSE_BOUND) continue;
            v.emplace_back(pe.kind, pe.ordinal);
        }
        return v;
    };
    const auto ko = others(keep), po = others(plain);
    std::size_t drop = 0;
    {   // plain's extra CloseBound events are the only difference
        std::size_t i = 0, j = 0;
        while (i < ko.size() && j < po.size()) {
            if (ko[i] == po[j]) { ++i; ++j; } else { ++drop; ++i; }
        }
        drop += (ko.size() - i) + (po.size() - j);
    }
    C(drop == 0 && ko.size() == po.size(), fmt("all %zu non-CloseBound events equal, kind and ordinal", ko.size()));
    bool fills_equal = keep.host->fills.size() == plain.host->fills.size();
    for (std::size_t i = 0; fills_equal && i < keep.host->fills.size(); ++i) {
        const auto& a = keep.host->fills[i].ev;
        const auto& b = plain.host->fills[i].ev;
        fills_equal = a.ordinal == b.ordinal && same(a.resolved_price, b.resolved_price)
                      && same(a.closed_units, b.closed_units) && same(a.opened_units, b.opened_units);
    }
    C(fills_equal, "the option changes the events, never what matches: every fill equal");
    const FillRec* xf = nullptr;
    for (const auto& f : keep.host->fills) if (f.label == "X") xf = &f;
    C(xf && xf->gi == G_XFILL && same(xf->ev.resolved_price, floor_tick(ACID_X_LEVEL0 + 8 * ACID_TICK) - ACID_TICK)
          && same(xf->ev.closed_units, ACID_E2_UNITS),
      "X (re-issued to 203.00) flattens E2 on bar 15 at 203.00 - 1 tick");
    int first_diff = -1, equal_rows = 0;
    const ll n = std::min(keep.report.broker_state_hash_len, plain.report.broker_state_hash_len);
    for (ll i = 0; i < n; ++i) {
        if (keep.report.broker_state_hash[i] == plain.report.broker_state_hash[i]) ++equal_rows;
        else if (first_diff < 0) first_diff = static_cast<int>(i);
    }
    xfield("C-SURFACE-2:replace-options", m + ".keep_binding",
           fmt("replaced:%d;closebound_plain:%d;closebound_keep:%d;kept:%d;events_equal:%zu;"
               "hash_rows_equal:%d/%lld;first_diff_row:%d;keep_final_hash:%llu",
               replaced(keep), pb, kb, kept, ko.size(), equal_rows, n, first_diff, (ull)keep.scalar));
}

// ============================================================ driving mode
void driving_section(const Run& b, const Run& s, const Run& cut) {
    auto D1 = [&](const char* cls, const char* what, bool ok, const std::string& detail, const char* cite) {
        out(fmt("DRIVING %s %s %s: %s [%s]", ok ? "PASS" : "FAIL", cls, what, detail.c_str(), cite));
        fcheck("DM", "driving-mode-accounting", ok, fmt("%s %s: %s", cls, what, detail.c_str()));
    };
    const AcidHost& hb = *b.host;
    const AcidHost& hs = *s.host;
    // MUST_EQUAL: the outcome is the batch<->stream oracle.
    auto same_trade = [](const TradeC& x, const TradeC& y) {
        return x.entry_time == y.entry_time && x.exit_time == y.exit_time && same(x.entry_price, y.entry_price)
               && same(x.exit_price, y.exit_price) && same(x.pnl, y.pnl) && same(x.pnl_pct, y.pnl_pct)
               && x.is_long == y.is_long && same(x.max_runup, y.max_runup) && same(x.max_drawdown, y.max_drawdown)
               && same(x.qty, y.qty) && same(x.commission, y.commission) && x.entry_bar_index == y.entry_bar_index
               && x.exit_bar_index == y.exit_bar_index && x.open_at_end == y.open_at_end;
    };
    bool trades = b.report.trades_len == s.report.trades_len;
    for (int i = 0; trades && i < b.report.trades_len; ++i) trades = same_trade(b.report.trades[i], s.report.trades[i]);
    D1("MUST_EQUAL", "closed+range-end rows", trades, fmt("%d rows, byte-identical", b.report.trades_len),
       "pineforge.h:1010-1011; native-engine.md:2039-2045");
    bool fills = hb.fills.size() == hs.fills.size();
    for (std::size_t i = 0; fills && i < hb.fills.size(); ++i) {
        const auto& x = hb.fills[i].ev;
        const auto& y = hs.fills[i].ev;
        fills = x.ordinal == y.ordinal && same(x.resolved_price, y.resolved_price) && same(x.closed_units, y.closed_units)
                && same(x.opened_units, y.opened_units) && same(x.current_ticket, y.current_ticket)
                && x.cursor.point.effective_time_ms == y.cursor.point.effective_time_ms;
    }
    D1("MUST_EQUAL", "fills", fills, fmt("%zu fills: ordinal, price, units, ticket, time", hb.fills.size()),
       "pineforge.h:1010-1011 (the trades, the position and the equity)");
    bool curve = b.report.equity_curve_len == s.report.equity_curve_len;
    for (ll i = 0; curve && i < b.report.equity_curve_len; ++i)
        curve = same(b.report.equity_curve[i].equity, s.report.equity_curve[i].equity)
                && b.report.equity_curve[i].time_ms == s.report.equity_curve[i].time_ms;
    D1("MUST_EQUAL", "equity curve", curve, fmt("%lld points", (ll)b.report.equity_curve_len), "pineforge.h:1010-1011");
    bool tfs = hb.tfs.size() == hs.tfs.size();
    for (std::size_t i = 0; tfs && i < hb.tfs.size(); ++i) {
        const auto& x = hb.tfs[i];
        const auto& y = hs.tfs[i];
        tfs = x.sub == y.sub && std::memcmp(&x.bar, &y.bar, sizeof(Bar)) == 0 && x.delivered_at == y.delivered_at
              && x.completion == y.completion && x.gi == y.gi && x.iv.open_ms == y.iv.open_ms
              && x.iv.next_period_open_ms == y.iv.next_period_open_ms;
    }
    D1("MUST_EQUAL", "series deliveries (the aux feed appended live)", tfs,
       fmt("%zu buckets, bucket for bucket and delivery point for delivery point", hb.tfs.size()),
       "native-engine.md:2665-2668");
    bool checks = hb.checks.size() == hs.checks.size();
    for (std::size_t i = 0; checks && i < hb.checks.size(); ++i) {
        const auto& x = hb.checks[i].p;
        const auto& y = hs.checks[i].p;
        checks = x.kind == y.kind && same(x.mark, y.mark) && x.cursor.point.ordinal == y.cursor.point.ordinal
                 && same(x.position.signed_units, y.position.signed_units);
    }
    D1("MUST_EQUAL", "margin check points incl. FxRoll", checks, fmt("%zu points", hb.checks.size()),
       "native-engine.md:3445-3453");
    int half_equal = 0;
    for (std::size_t i = 0; i < std::min(hb.bars.size(), hs.bars.size()); ++i)
        half_equal += hb.bars[i].broker_half == hs.bars[i].broker_half ? 1 : 0;
    D1("MUST_EQUAL", "broker half at every bar (C++ only: protected fold)", half_equal == static_cast<int>(hb.bars.size()),
       fmt("%d/%zu bars", half_equal, hb.bars.size()), "native-engine.md:2034-2037; engine.hpp:2117-2119");
    // LENGTH: the one identity of the array that holds across drivings.
    const bool len = b.report.broker_state_hash_len == b.report.script_bars_processed
                     && s.report.broker_state_hash_len == s.report.script_bars_processed
                     && b.report.equity_curve_len == b.report.broker_state_hash_len
                     && s.report.equity_curve_len == s.report.broker_state_hash_len;
    D1("LENGTH", "hash_len == curve_len == script_bars", len,
       fmt("batch %lld/%lld/%lld stream %lld/%lld/%lld", (ll)b.report.broker_state_hash_len, (ll)b.report.equity_curve_len,
           (ll)b.report.script_bars_processed, (ll)s.report.broker_state_hash_len, (ll)s.report.equity_curve_len,
           (ll)s.report.script_bars_processed),
       "native-engine.md:2006-2010; pineforge.h:447-448, :1009-1010");
    // KEYED: the rows fold the continuation, whose driving mode is part of it.
    int equal_rows = 0, first_equal = -1;
    const ll n = std::min(b.report.broker_state_hash_len, s.report.broker_state_hash_len);
    for (ll i = 0; i < n; ++i)
        if (b.report.broker_state_hash[i] == s.report.broker_state_hash[i]) {
            ++equal_rows;
            if (first_equal < 0) first_equal = static_cast<int>(i);
        }
    D1("KEYED", "per-bar broker_state_hash rows", equal_rows == 0,
       fmt("%d/%lld rows equal; the rows differ from index 0", equal_rows, n),
       "native-engine.md:2028-2032; pineforge.h:1003-1009; native_host.hpp:37-40");
    D1("KEYED", "final broker_state_hash / continuation hash", b.scalar != s.scalar && b.continuation != s.continuation,
       fmt("batch %llu/%llu stream %llu/%llu", (ull)b.scalar, (ull)b.continuation, (ull)s.scalar, (ull)s.continuation),
       "native-engine.md:2022-2032; native_c_api.h:3035-3041");
    // KEYED presentation: the final bar's session close.
    const auto& fb = hb.bars.back().ctx;
    const auto& fs = hs.bars.back().ctx;
    D1("KEYED", "closes_session_day on the final (mid-session) bar",
       fb.closes_session_day && !fs.closes_session_day && !fb.closes_session_day_open_ended
           && fs.closes_session_day == fb.closes_session_day_open_ended,
       fmt("batch %d (complete input) stream %d (reads on); batch open_ended %d == stream", fb.closes_session_day ? 1 : 0,
           fs.closes_session_day ? 1 : 0, fb.closes_session_day_open_ended ? 1 : 0),
       "market_driver.hpp:143-155; native_c_api.h:1428-1431");
    int sess_equal = 0;
    for (std::size_t i = 0; i + 1 < std::min(hb.bars.size(), hs.bars.size()); ++i) {
        const auto& x = hb.bars[i].ctx;
        const auto& y = hs.bars[i].ctx;
        sess_equal += (x.in_session == y.in_session && x.opens_session_day == y.opens_session_day
                       && x.closes_session_day == y.closes_session_day) ? 1 : 0;
    }
    D1("MUST_EQUAL", "session-day facts on every other bar", sess_equal == static_cast<int>(hb.bars.size()) - 1,
       fmt("%d/%zu", sess_equal, hb.bars.size() - 1), "market_driver.hpp:131-147");
    // WITHIN one driving: the last row is the scalar, and a shorter batch is a prefix.
    D1("WITHIN", "last row == broker_state_hash()",
       b.report.broker_state_hash[b.report.broker_state_hash_len - 1] == b.scalar
           && s.report.broker_state_hash[s.report.broker_state_hash_len - 1] == s.scalar,
       "batch and stream", "pineforge.h:447-452");
    bool prefix = cut.report.broker_state_hash_len == cut.opt.bars;
    for (ll i = 0; prefix && i < cut.report.broker_state_hash_len; ++i)
        prefix = cut.report.broker_state_hash[i] == b.report.broker_state_hash[i];
    D1("WITHIN", "prefix closure", prefix,
       fmt("a batch cut after bar %d records the full batch's first %d rows", cut.opt.bars - 1, cut.opt.bars),
       "native-engine.md:2024-2027; pineforge.h:1004-1008");
    // INFO: equal by measurement here, not promised by a doc line.
    uint64_t eb = ACID_FNV_OFFSET, es = ACID_FNV_OFFSET;
    for (const auto& e : b.events) { const auto pe = project(e); eb = acid_event_fold(eb, &pe); }
    for (const auto& e : s.events) { const auto pe = project(e); es = acid_event_fold(es, &pe); }
    out(fmt("DRIVING INFO events %s: batch %zu digest %llu, stream %zu digest %llu (not a documented invariant)",
            eb == es ? "equal" : "differ", b.events.size(), (ull)eb, s.events.size(), (ull)es));
    out(fmt("DRIVING INFO phase at the end: batch %d stream %d; completion batch %d stream %d",
            static_cast<int>(b.state.phase), static_cast<int>(s.state.phase), static_cast<int>(b.state.completion),
            static_cast<int>(s.state.completion)));
}

// ============================================================ findings
// Facts this witness measured that no requested feature asserts: printed, not
// checked, so the row stays a coverage witness and the finding goes to a lane.
void findings(const Run& plain, const Run& mag) {
    int bar_open = 0, after = 0, sample = 0, other = 0;
    for (const auto& c : mag.host->checks) {
        if (c.p.kind == NativeMarginCheckKind::BarOpen) ++bar_open;
        else if (c.p.kind == NativeMarginCheckKind::AfterApplied) ++after;
        else if (c.p.kind == NativeMarginCheckKind::IntrabarSample) ++sample;
        else ++other;
    }
    const auto* pc = plain.host->calls.empty() ? nullptr : &plain.host->calls.front();
    const auto* mc = mag.host->calls.empty() ? nullptr : &mag.host->calls.front();
    out(fmt("FINDING intrabar-margin: lower_tf run offers %d BarOpen + %d AfterApplied + %d IntrabarSample (+%d other) "
            "margin points for %d script bars / %lld samples; the spike that crosses L inside bar %d is liquidated "
            "at bar %d, price %s, in %zu slices (unmagnified: bar %d, price %s, %zu slice) "
            "[per-sample re-evaluation: NativeMarginCheckKind::IntrabarSample, native_host.hpp]",
            bar_open, after, sample, other, static_cast<int>(mag.host->bars.size()),
            (ll)mag.report.magnifier_sample_ticks_total, G_SPIKE,
            mc ? mc->cursor.point.interval_index : -1, mc ? D(mc->mark).c_str() : "-", mag.host->calls.size(),
            pc ? pc->cursor.point.interval_index : -1, pc ? D(pc->mark).c_str() : "-", plain.host->calls.size()));
    const auto& e2p = plain.host->fills.size() > 2 ? plain.host->fills[2] : FillRec{};
    const auto& e2m = mag.host->fills.size() > 2 ? mag.host->fills[2] : FillRec{};
    out(fmt("FINDING intrabar-birth: E2 (born in the take-profit's recalculation) fills on bar %d at %s unmagnified, "
            "on bar %d at %s under lower_tf (the next sub-bar open is the next discrete matching point: "
            "native-engine.md:1647-1649)",
            e2p.gi, D(e2p.ev.resolved_price).c_str(), e2m.gi, D(e2m.ev.resolved_price).c_str()));
}

// ============================================================ typed configuration
void typed_configuration() {
    // A quantizing grid with no ladder: GridRequiresPriceTick at PriceGrid
    // (native-engine.md:339-341). C++ fails the host (native_host.hpp:1156-1161).
    AcidHost bad(Opts{});
    NativeRunSpec spec = make_spec(Opts{});
    spec.price_tick = 0.0;
    const auto r = bad.configure_native(spec);
    field("cfg.refusal", fmt("status:%d;err:%d;field:%d", static_cast<int>(r.status),
                             static_cast<int>(r.validation.error), static_cast<int>(r.validation.field)));
    const auto st = bad.native_state();
    out(fmt("CPPONLY cfg.cpp_host_after_refusal=kind:%d;failure:%d (the C ext path keeps its handle Unconfigured)",
            static_cast<int>(st.kind), static_cast<int>(st.failure.code)));
    fcheck("F12", "typed-refusal-path",
           r.status == NativeSetupStatus::Failed && r.validation.error == NativeRunSpecError::GridRequiresPriceTick
               && r.validation.field == NativeRunSpecField::PriceGrid,
           "configure refusal is typed: GridRequiresPriceTick at PriceGrid");
}

#ifdef ACID_WITH_C
void sink(void* user, const char* l) {
    auto* v = static_cast<std::vector<std::string>*>(user);
    v->emplace_back(l);
}
#endif

}  // namespace

int main() {
    build_tape();
    out("# acid composite, C++ half: kernel-only host, source-free");
    typed_configuration();
    Opts ob; ob.tag = "batch";
    Opts os; os.tag = "stream"; os.stream = true;
    Opts okb; okb.tag = "keep.batch"; okb.keep = true;
    Opts oks; oks.tag = "keep.stream"; oks.keep = true; oks.stream = true;
    Opts om; om.tag = "mag"; om.magnify = true;
    Opts oc; oc.tag = "cut"; oc.bars = G_SPIKE + 1;
    const auto batch = execute(ob);
    const auto stream = execute(os);
    const auto keep_b = execute(okb);
    const auto keep_s = execute(oks);
    const auto mag = execute(om);
    const auto cut = execute(oc);
    emit_run(*batch);
    emit_run(*stream);
    emit_run(*mag);
    check_run_features(*batch);
    check_run_features(*stream);
    check_run_features(*mag);
    check_keep(*keep_b, *batch);
    check_keep(*keep_s, *stream);
    driving_section(*batch, *stream, *cut);
    findings(*batch, *mag);

#ifdef ACID_WITH_C
    {
        std::vector<std::string> c_lines;
        const int c_failures = acid_c_main(&sink, &c_lines);
        std::set<std::string> cpp(g_fields.begin(), g_fields.end()), c;
        std::set<std::string> excluded;
        for (const auto& l : c_lines) {
            if (l.rfind("FIELD ", 0) == 0) c.insert(l);
            if (l.rfind("EXCLUDED ", 0) == 0) excluded.insert(l.substr(9, l.find(' ', 9) - 9));
            if (l.rfind("PASS ", 0) == 0 || l.rfind("FAIL ", 0) == 0 || l.rfind("CONLY ", 0) == 0) out("C: " + l);
        }
        std::size_t only_cpp = 0, only_c = 0;
        for (const auto& l : cpp) if (!c.count(l)) { if (only_cpp++ < 20) out("CMP only-C++ " + l); }
        for (const auto& l : c) if (!cpp.count(l)) { if (only_c++ < 20) out("CMP only-C   " + l); }
        const bool equal = only_cpp == 0 && only_c == 0;
        out(fmt("CMP C vs C++ FIELD sets: C++ %zu, C %zu, only-C++ %zu, only-C %zu -> %s", cpp.size(), c.size(),
                only_cpp, only_c, equal ? "IDENTICAL" : "DIFFER"));
        fcheck("CX", "c-port-shared-fields-identical", equal, "C and C++ FIELD sets");
        const char* families[] = {"C-SURFACE-2:closed-trade-entry-comment", "C-SURFACE-2:applied-origin-label",
                                  "C-SURFACE-2:replace-options", "C-SURFACE-2:inspect_current_execution",
                                  "COVERAGE:closes_session_day_open_ended",
                                  "PROTECTED:broker_state_hash_from_execution_hash", "NOC:quote_origin_ordinal",
                                  "NOC:driver_statistics"};
        for (const char* f : families)
            fcheck("CX", "c-port-shared-fields-identical", excluded.count(f) == 1,
                   std::string("the C half names the excluded family ") + f);
        fcheck("CX", "c-port-shared-fields-identical", c_failures == 0, fmt("C-side checks: %d failures", c_failures));
    }
#endif
    // F01..F14 in the requested order, then the driving-mode accounting and the C comparison.
    std::stable_sort(g_features.begin(), g_features.end(), [](const Feature& a, const Feature& b) {
        const bool fa = a.id[0] == 'F', fb = b.id[0] == 'F';
        if (fa != fb) return fa;
        return fa ? a.id < b.id : a.id > b.id;
    });
    int failed_features = 0;
    for (const auto& f : g_features) {
        out(fmt("%s %s %s (%d checks)", f.failed ? "FAIL" : "PASS", f.id.c_str(), f.name.c_str(), f.total));
        for (const auto& w : f.failures) out("    - " + w);
        failed_features += f.failed ? 1 : 0;
    }
    int checks = 0, fails = 0;
    for (const auto& f : g_features) { checks += f.total; fails += f.failed; }
    out(fmt("acid composite: %d features, %d failed; %d checks, %d failures; %zu FIELD lines", static_cast<int>(g_features.size()),
            failed_features, checks, fails, g_fields.size()));
    return failed_features == 0 ? 0 : 1;
}
