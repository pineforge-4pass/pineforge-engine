// R5 lane PERF-L1: the lean per-bar path -- exact guards and frames built in
// place -- against the computation it replaced.
//
// The lane puts a guard in front of five per-point bodies: the applied
// notification drain (after every callback and every matched winner), the
// every-modeled-point recalculation (at every driver point), and the margin
// model's three check points (bar open, calculation, FX roll). Each guard
// tests the precondition its body returns on first -- nothing queued, another
// cadence, no margin model, a model that checks elsewhere, no staged curve --
// so a point with nothing to do costs a branch instead of a call with a
// stack-protected frame. It also builds each callback's current execution
// frame where the frame lives, one copy of the decision context instead of
// three, and keeps the magnifier's sub-bar and sample lists in consumer-owned
// buffers.
//
// Two witnesses:
//   1. The guards against their bodies, bit for bit. Every scenario below and
//      a randomized sweep run twice, with the guards on and with
//      set_point_guards(false), under which every guard calls its body
//      unconditionally: the pre-lane computation. The host folds, inside
//      every NativeStrategyHost hook and hash_host_extension, the
//      continuation hash, the whole current execution frame, the lifecycle
//      view and the book, and counts every hook it answers; the two runs
//      must agree on every fold, every count, the trades, the events and
//      the final continuation and broker-state hashes.
//   2. The values, pinned. The fixed scenarios' folds and final hashes were
//      harvested at the lane's base (f71cd820, built with -DPF_L1_HARVEST and
//      run with PF_LEAN_PATH_DUMP=1) and hold unchanged: the frames built in
//      place and the scratch buffers present and hash exactly what the
//      aggregates and locals they replace did, in every callback kind.
//
// Fail-before: at the lane's base the consumer has no set_point_guards(), so
// this TU does not compile there (the lane report records the first
// diagnostic); the harvest build compiles witness 1 out.
//
// Source-free: this TU runs in the kernel-only profile.
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int passed = 0;
int failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++passed;                                                            \
        } else {                                                                 \
            ++failed;                                                            \
            std::fprintf(stderr, "CHECK FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

constexpr std::int64_t kT0 = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000;

struct Fnv {
    std::uint64_t h = 1469598103934665603ULL;
    void bytes(const void* p, std::size_t n) {
        const auto* c = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= c[i];
            h *= 1099511628211ULL;
        }
    }
    template <class T> void v(const T& x) { bytes(&x, sizeof x); }
};

void fold_coordinate(Fnv& f, const NativeCoordinate& c) {
    f.v(c.ordinal); f.v(c.interval_index); f.v(c.open_ms); f.v(c.eligible_open_ms);
    f.v(c.last_traded_close_ms); f.v(c.next_period_open_ms); f.v(c.next_input_open_ms);
    f.v(c.effective_time_ms); f.v(c.source_price_time_ms);
    f.v(static_cast<int>(c.provenance)); f.v(static_cast<int>(c.path_phase));
    f.v(static_cast<int>(c.completion));
}

void fold_interval(Fnv& f, const native_calendar::NativeInterval& i) {
    f.v(i.open_ms); f.v(i.eligible_open_ms); f.v(i.last_traded_close_ms);
    f.v(i.next_period_open_ms); f.v(i.next_input_open_ms);
}

void fold_decision(Fnv& f, const NativeDecisionContext& d) {
    fold_coordinate(f, d.coordinate);
    f.v(d.decision_floor_ms);
    fold_interval(f, d.input_interval);
    fold_interval(f, d.script_interval);
    f.v(d.sub_index); f.v(d.sub_count); f.v(d.is_terminal_sub_bar);
    f.v(d.in_session); f.v(d.opens_session_day); f.v(d.closes_session_day);
    f.v(d.closes_session_day_open_ended);
    f.v(d.sub_bar_open_ms); f.v(d.script_bar_open_ms);
    f.v(d.driver_statistics.intrabar_path_enabled);
    f.v(d.driver_statistics.sub_bars_per_script_bar);
    f.v(d.driver_statistics.samples_per_sub_bar);
    f.v(d.driver_statistics.sub_bars_processed);
    f.v(d.driver_statistics.sample_ticks_processed);
}

enum Hook : int {
    PrepareBegin, RunBegin, Input, Tick, TimeframeBar, BarOpen, Bar_, Recalculate, SubBar,
    Applied, Terms, Precommit, MarginRequirement, MarginAllowed, MarginUnits, MarginCall,
    AnchoredLevel, OwnsExcursions, ClosedExcursion, HashExtension, HookCount
};
constexpr const char* kHookNames[] = {
    "prepare_native_begin", "on_native_run_begin", "on_native_input", "on_native_tick",
    "on_native_timeframe_bar", "on_native_bar_open", "on_native_bar", "on_native_recalculate",
    "on_native_sub_bar", "on_native_applied", "resolve_execution_terms",
    "validate_execution_precommit", "resolve_margin_requirement", "margin_check_allowed",
    "resolve_margin_call_units", "on_native_margin_call", "resolve_anchored_level",
    "owns_lot_excursions", "closed_lot_excursion", "hash_host_extension"};
static_assert(sizeof(kHookNames) / sizeof(kHookNames[0]) == HookCount, "one name per hook");

// What one run showed its host, and what it left behind.
struct Outcome {
    std::uint64_t fold = 0;
    std::uint64_t calls[HookCount] = {};
    std::uint64_t continuation = 0;
    std::uint64_t broker = 0;
    std::uint64_t trades = 0;
    std::uint64_t trade_digest = 0;
    std::uint64_t events = 0;
    int kind = 0;
    int failure = 0;
};

// The run shapes. The fixed scenarios take them in order; the randomized
// sweep draws them.
enum class Shape : int {
    Batch, Margin, MarginCalc, MarginFx, EveryPoint, Synthesized, SynthesizedEveryPoint,
    VolumeWeighted, Lower, Aggregated, Subscribed, Anchored, Risk, Ticks, StreamBars, Count
};
constexpr const char* kShapeNames[] = {
    "batch", "margin", "margin_calculation", "margin_fx_roll", "every_point", "synthesized",
    "synthesized_every_point", "volume_weighted", "lower", "aggregated", "subscribed",
    "anchored", "risk", "ticks", "stream_bars"};
constexpr int kShapes = static_cast<int>(Shape::Count);
static_assert(sizeof(kShapeNames) / sizeof(kShapeNames[0]) == kShapes, "one name per shape");

struct LeanHost final : NativeStrategyHost {
    Shape shape = Shape::Batch;
    std::uint64_t seed = 1;
    Fnv fold;
    std::uint64_t calls[HookCount] = {};
    int closes = 0;
    NativeCalculationReason reason = NativeCalculationReason::BarClose;
    bool reading = false;

    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }

    std::uint64_t draw() {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    }

    // Everything a host can read about the point it is called at.
    void observe(Hook hook) const {
        auto& self = const_cast<LeanHost&>(*this);
        ++self.calls[hook];
        if (reading) return;  // hash_host_extension inside a read below
        self.reading = true;
        Fnv& f = self.fold;
        f.v(static_cast<int>(hook));
        f.v(native_continuation_hash());
        const auto point = current_execution_point();
        f.v(point.has_value());
        if (point) {
            fold_decision(f, point->decision);
            f.v(point->price);
            f.v(static_cast<int>(point->quote_kind));
            f.v(point->quote_origin_ordinal);
        }
        const auto state = native_state();
        f.v(static_cast<int>(state.kind));
        f.v(static_cast<int>(state.phase));
        f.v(state.decision_floor_ms);
        const auto book = physical_position();
        f.v(book.signed_units);
        f.v(book.lot_count);
        f.v(native_recalculation_count());
        self.reading = false;
    }
    void observe_context(Hook hook, const NativeDecisionContext& context) const {
        observe(hook);
        fold_decision(const_cast<LeanHost&>(*this).fold, context);
    }

    void trade() {
        const std::uint64_t r = draw() % 10;
        const double q = 1.0 + static_cast<double>(draw() % 3);
        const auto& bar = current_bar_;
        switch (r) {
        case 0: case 1: (void)submit({no::Transact{q}, "l", ""}); break;
        case 2: (void)submit({no::Transact{-q}, "s", ""}); break;
        case 3: (void)submit({no::Flatten{}, "x", ""}); break;
        case 4: {
            no::Request limit{no::Transact{q}, "lim", ""};
            limit.trigger = no::Limit{std::round((bar.close - 1.0) * 100.0) / 100.0};
            (void)submit(limit);
            break;
        }
        case 5: {
            no::Request stop{no::Transact{-q}, "stp", ""};
            stop.trigger = no::Stop{std::round((bar.close - 1.5) * 100.0) / 100.0};
            (void)submit(stop);
            break;
        }
        case 6: (void)cancel_all(); break;
        default: break;
        }
    }

    void prepare_native_begin(const NativeBeginArgs&) override { observe(PrepareBegin); }
    void on_native_run_begin() override {
        observe(RunBegin);
        if (shape != Shape::Anchored) return;
        const auto parent = submit({no::Transact{1.0}, "entry", ""});
        if (!parent.handle) return;
        no::Request leg{no::Reduce{no::OwnerOpenedUnits{}}, "leg", "bracket"};
        leg.trigger = no::Stop{0.0};
        leg.owner = no::WaitForApplied{*parent.handle};
        leg.anchor = no::FromOwnerFill{-2.0, true};
        (void)submit(leg);
    }
    void on_native_input(const Bar&, const NativeInputContext& context) override {
        observe(Input);
        fold_interval(fold, context.input_interval);
        fold_interval(fold, context.script_interval);
    }
    void on_native_tick(const Bar&, const NativeTickContext& context) override {
        observe_context(Tick, context.decision);
        if (draw() % 3 == 0) trade();
    }
    void on_native_timeframe_bar(const Bar& bar, const NativeTimeframeBarContext&) override {
        observe(TimeframeBar);
        fold.v(bar.close);
    }
    void on_native_bar_open(const Bar& bar, const NativeDecisionContext& context) override {
        observe_context(BarOpen, context);
        fold.v(bar.high);
        if (draw() % 4 == 0) trade();
    }
    void on_native_recalculate(const Bar& bar, const NativeDecisionContext& ctx,
                               NativeCalculationReason why,
                               const no::ExecutionAppliedEvent* cause) override {
        observe_context(Recalculate, ctx);
        fold.v(static_cast<int>(why));
        reason = why;
        NativeStrategyHost::on_native_recalculate(bar, ctx, why, cause);
    }
    void on_native_bar(const Bar&, const NativeDecisionContext& context) override {
        observe_context(Bar_, context);
        if (reason != NativeCalculationReason::BarClose) return;
        const int index = closes++;
        // A broker-state read from the callback: hash_host_extension runs
        // inside it.
        if (index % 5 == 2) fold.v(broker_state_hash());
        if (shape == Shape::Margin || shape == Shape::MarginCalc || shape == Shape::MarginFx) {
            if (index == 0) (void)submit({no::Transact{20.0}, "entry", ""});
            if (index == 5) (void)submit({no::Transact{-15.0}, "short", ""});
            return;
        }
        trade();
    }
    void on_native_sub_bar(const Bar& sub, const NativeDecisionContext& context) override {
        observe_context(SubBar, context);
        fold.v(sub.close);
    }
    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        observe_context(Applied, context);
        fold.v(event.ordinal);
    }
    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        observe(Terms);
        return NativeStrategyHost::resolve_execution_terms(facts);
    }
    NativePrecommitVerdict validate_execution_precommit(
            const NativePrecommitView& view) const override {
        observe(Precommit);
        return NativeStrategyHost::validate_execution_precommit(view);
    }
    std::optional<NativeMarginDecision> resolve_margin_requirement(
            const NativeMarginRequirementView& view) const override {
        observe(MarginRequirement);
        return NativeStrategyHost::resolve_margin_requirement(view);
    }
    bool margin_check_allowed(const NativeMarginCheckPoint& point) const override {
        observe(MarginAllowed);
        const_cast<LeanHost&>(*this).fold.v(static_cast<int>(point.kind));
        return NativeStrategyHost::margin_check_allowed(point);
    }
    std::optional<double> resolve_margin_call_units(
            const NativeMarginCallView& view) const override {
        observe(MarginUnits);
        return NativeStrategyHost::resolve_margin_call_units(view);
    }
    void on_native_margin_call(const no::MarginCallEvent& event) override {
        observe(MarginCall);
        fold.v(event.units);
    }
    std::optional<double> resolve_anchored_level(
            const NativeAnchoredLevelView& view) const override {
        observe(AnchoredLevel);
        return NativeStrategyHost::resolve_anchored_level(view);
    }
    bool owns_lot_excursions() const noexcept override {
        observe(OwnsExcursions);
        return shape == Shape::Batch;
    }
    ClosedLotExcursion closed_lot_excursion(const ClosedLotExcursionFacts& facts) const override {
        observe(ClosedExcursion);
        return NativeStrategyHost::closed_lot_excursion(facts);
    }
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        observe(HashExtension);
        NativeStrategyHost::hash_host_extension(sink);
    }
};

NativeRunSpec base_spec() {
    NativeRunSpec s;
    s.identity = {"perf-l1-lean-path", 1};
    s.input_tf = "1";
    s.script_tf = "1";
    s.ticker = "MOCK";
    s.tickerid = "TEST:MOCK";
    s.type = "crypto";
    s.currency = "USD";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 100000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.5;
    return s;
}

std::vector<Bar> walk_bars(int n, std::int64_t step, std::uint64_t seed) {
    std::vector<Bar> bars;
    double p = 100.0;
    for (int i = 0; i < n; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        const double o = p;
        const double c = std::round((o + static_cast<double>(static_cast<int>(seed % 9) - 4) * 0.25)
                                    * 100.0) / 100.0;
        const double h = std::max(o, c) + 0.5 + static_cast<double>(seed % 3) * 0.25;
        const double l = std::min(o, c) - 0.5 - static_cast<double>((seed >> 8) % 3) * 0.25;
        bars.push_back(Bar{o, h, l, c, 1.0 + static_cast<double>((seed >> 16) % 9),
                           kT0 + i * step});
        p = c;
    }
    return bars;
}

NativeRunSpec shape_spec(Shape shape, bool tolerant, NativeCalculationTrigger cadence) {
    NativeRunSpec s = base_spec();
    s.slot_label_policy = tolerant ? NativeSlotLabelPolicy::FeedTolerant
                                   : NativeSlotLabelPolicy::Canonical;
    s.calculation = cadence;
    switch (shape) {
    case Shape::Batch:
        s.report_policy = NativeReportPolicy::KernelRecorded;
        break;
    case Shape::Margin:
    case Shape::MarginCalc:
    case Shape::MarginFx: {
        s.initial_capital = 1000.0;
        s.fee_value = 0.0;
        NativeMarginModel m;
        m.initial_long = 0.5;
        m.initial_short = 0.5;
        m.maintenance_long = 0.5;
        m.maintenance_short = 0.5;
        if (shape == Shape::MarginCalc) m.check = NativeLiquidationCheck::CalculationOnly;
        s.margin = m;
        s.close_execution = NativeCloseExecution::AfterCalculation;
        break;
    }
    case Shape::EveryPoint:
        s.calculation = NativeCalculationTrigger::EveryModeledPoint;
        s.max_recalculations_per_point = 2;
        break;
    case Shape::Synthesized:
    case Shape::SynthesizedEveryPoint:
    case Shape::VolumeWeighted: {
        IntrabarPath::synthesized path;
        path.samples = shape == Shape::VolumeWeighted ? 5 : 4;
        path.distribution = shape == Shape::Synthesized ? MagnifierDistribution::ENDPOINTS
                                                        : MagnifierDistribution::UNIFORM;
        path.volume_weighted = shape == Shape::VolumeWeighted;
        s.intrabar.value = path;
        if (shape == Shape::SynthesizedEveryPoint)
            s.calculation = NativeCalculationTrigger::EveryModeledPoint;
        break;
    }
    case Shape::Lower: {
        s.input_tf = "5";
        s.script_tf = "5";
        IntrabarPath::lower_tf path;
        path.tf = "1";
        path.bars = walk_bars(20 * 5, kMinute, 0x1234567ULL);
        s.intrabar.value = path;
        break;
    }
    case Shape::Aggregated:
        s.script_tf = "5";
        break;
    case Shape::Subscribed: {
        NativeTimeframeSubscription five;
        five.tf = "5";
        s.subscriptions.push_back(five);
        break;
    }
    case Shape::Anchored:
        s.report_policy = NativeReportPolicy::KernelRecorded;
        break;
    case Shape::Risk: {
        NativeRiskLimits risk;
        NativeLossLimit loss;
        loss.value = 3.0;
        risk.max_intraday_loss = loss;
        risk.max_fills_per_day = 12;
        risk.action = NativeRiskAction::FlattenAndBlock;
        s.risk = risk;
        break;
    }
    case Shape::Ticks:
        s.calculation = NativeCalculationTrigger::BarClose;
        break;
    default:
        break;
    }
    return s;
}

Outcome run_shape(Shape shape, bool tolerant, NativeCalculationTrigger cadence,
                  std::uint64_t seed, bool guards, int bars_n) {
    LeanHost host;
    host.shape = shape;
    host.seed = seed;
#ifndef PF_L1_HARVEST
    host.consumer().set_point_guards(guards);
    // Witness 1 compares the event record a run ends with; read it back whole
    // (V19-B's default Window keeps none of it by then). A readback choice
    // alone: the spec, its digest and every pinned value are the default's.
    host.consumer().set_retention_override(NativeEventRetention::Full);
#else
    (void)guards;
#endif
    Outcome out;
    if (host.configure_native(shape_spec(shape, tolerant, cadence)).status
        != NativeSetupStatus::Applied) {
        out.kind = -1;
        return out;
    }
    if (shape == Shape::MarginFx) {
        NativeFxCurve curve;
        curve.effective_from_ms = {kT0 - 24 * 60 * kMinute, kT0 + 3 * kMinute,
                                   kT0 + 9 * kMinute};
        curve.account_per_quote = {1.0, 1.4, 0.9};
        (void)host.configure_native_fx_curve(curve);
    }
    host.set_broker_state_hash_recording(seed % 2 == 0);
    switch (shape) {
    case Shape::Ticks: {
        const auto warmup = walk_bars(3, kMinute, seed);
        if (!host.stream_begin(warmup.data(), 3, "1", "1")) break;
        std::uint64_t sequence = 0;
        for (int k = 0; k < bars_n; ++k) {
            const TradeTick tick{kT0 + 3 * kMinute + k * 20000, ++sequence,
                                 100.0 + 0.25 * static_cast<double>(k % 7), 1.0};
            if (!host.stream_push_tick(tick)) break;
        }
        (void)host.stream_end(true);
        break;
    }
    case Shape::StreamBars: {
        const auto bars = walk_bars(bars_n, kMinute, seed);
        if (!host.stream_begin(bars.data(), 4, "1", "1")) break;
        for (int i = 4; i < bars_n; ++i) {
            if (!host.stream_push_bar(bars[i])) break;
        }
        (void)host.stream_end(false);
        break;
    }
    case Shape::Lower: {
        const auto bars = walk_bars(20, 5 * kMinute, seed);
        host.run(bars.data(), static_cast<int>(bars.size()));
        break;
    }
    default: {
        const auto bars = walk_bars(bars_n, kMinute, seed);
        host.run(bars.data(), static_cast<int>(bars.size()));
        break;
    }
    }
    out.fold = host.fold.h;
    for (int i = 0; i < HookCount; ++i) out.calls[i] = host.calls[i];
    host.reading = true;
    out.continuation = host.native_continuation_hash();
    out.broker = host.broker_state_hash();
    out.events = host.native_events(0).size();
    const auto state = host.native_state();
    out.kind = static_cast<int>(state.kind);
    out.failure = static_cast<int>(state.failure.code);
    Fnv trades;
    for (std::size_t i = 0; i < host.closed_trade_count(); ++i) {
        const Trade& t = host.closed_trade(i);
        trades.v(t.entry_time); trades.v(t.exit_time); trades.v(t.entry_price);
        trades.v(t.exit_price); trades.v(t.qty); trades.v(t.pnl);
        ++out.trades;
    }
    out.trade_digest = trades.h;
    return out;
}

bool same(const Outcome& a, const Outcome& b) {
    if (a.fold != b.fold || a.continuation != b.continuation || a.broker != b.broker
        || a.trades != b.trades || a.trade_digest != b.trade_digest || a.events != b.events
        || a.kind != b.kind || a.failure != b.failure) {
        return false;
    }
    for (int i = 0; i < HookCount; ++i) {
        if (a.calls[i] != b.calls[i]) return false;
    }
    return true;
}

struct Pin {
    const char* name;
    std::uint64_t fold;
    std::uint64_t continuation;
    std::uint64_t broker;
    std::uint64_t trades;
};

// Harvested at f71cd820 (the lane's base): -DPF_L1_HARVEST, PF_LEAN_PATH_DUMP=1.
// expectation corrected (90 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19) -- the fold moved only through the continuation and broker-state values it reads (with those two reads masked it is identical on 6976a808 and here); every hook's call count and the trades did not move; re-harvested the same way on INT19's tree; V19-A's tip (6211dc94) gives the same rows:
//   batch/canonical: fold 5166037941821544647ULL -> 8417350598053238959ULL, continuation 14809352016120894185ULL -> 3930831923810011709ULL, broker 11338747070571302820ULL -> 11999400313963825447ULL
//   batch/tolerant: fold 18189486081941112598ULL -> 14844934503495743671ULL, continuation 14388238089025711186ULL -> 13486072370782740075ULL, broker 7862196356352290449ULL -> 714139398657209591ULL
//   margin/canonical: fold 11987783106259734011ULL -> 17458675826522343092ULL, continuation 3455256496964847716ULL -> 2991704189571748528ULL, broker 7725613184755129846ULL -> 13780954738578495924ULL
//   margin/tolerant: fold 4409189285804866438ULL -> 15357271826299544132ULL, continuation 855085601544096740ULL -> 7845006491016447433ULL, broker 5160401203850839130ULL -> 14848973100437479489ULL
//   margin_calculation/canonical: fold 1568470444282449322ULL -> 12225698990055261516ULL, continuation 13861699917022548257ULL -> 17852094020611711180ULL, broker 3289041035191503419ULL -> 13636565062500767501ULL
//   margin_calculation/tolerant: fold 14823551462041516976ULL -> 15618343851819169665ULL, continuation 11533820964462831656ULL -> 12657325099533710982ULL, broker 13853377775653101846ULL -> 15810633237895378470ULL
//   margin_fx_roll/canonical: fold 13645453736898832911ULL -> 2789533630001178538ULL, continuation 2272937872551974761ULL -> 17178754079582823760ULL, broker 14995707425517547873ULL -> 209420367109385368ULL
//   margin_fx_roll/tolerant: fold 928661954089809302ULL -> 14338402113188947682ULL, continuation 14611393619647988727ULL -> 1793934981347633737ULL, broker 9639648990238122086ULL -> 12065124535081589535ULL
//   every_point/canonical: fold 15169653257185161393ULL -> 7562764044102581192ULL, continuation 11942220871818969230ULL -> 3737699987008739230ULL, broker 9620246032232391656ULL -> 4383124431781758246ULL
//   every_point/tolerant: fold 11710645089551942874ULL -> 14141982756948460013ULL, continuation 16516145559658558435ULL -> 9846509637745922907ULL, broker 15993892195546568613ULL -> 14678512838653533407ULL
//   synthesized/canonical: fold 13721396498718821255ULL -> 2376681450298217164ULL, continuation 16252911878732103900ULL -> 8919327222620353451ULL, broker 1032857913756253235ULL -> 3600925079421309162ULL
//   synthesized/tolerant: fold 4331154914870688083ULL -> 4035245647427445567ULL, continuation 16490472740586268474ULL -> 2693662612566588702ULL, broker 15966330760823216464ULL -> 119147027709136062ULL
//   synthesized_every_point/canonical: fold 11971132682800678447ULL -> 13963831207202754247ULL, continuation 6753026284485815074ULL -> 1411759699604227935ULL, broker 2776925473445912126ULL -> 8220237254062191568ULL
//   synthesized_every_point/tolerant: fold 3146256244272210816ULL -> 4089704344096729236ULL, continuation 10083350186892347974ULL -> 12877338782012258332ULL, broker 640394820882642634ULL -> 445075360625743117ULL
//   volume_weighted/canonical: fold 17207772673036829567ULL -> 17081627527962347612ULL, continuation 5231083461152657006ULL -> 10482737734476373322ULL, broker 17134903625378598223ULL -> 8103680506999774488ULL
//   volume_weighted/tolerant: fold 10180024423461507044ULL -> 6813541438777655137ULL, continuation 7895603099092190300ULL -> 4189454090113514211ULL, broker 17629149376284097724ULL -> 11870207615699990715ULL
//   lower/canonical: fold 11313059837849793711ULL -> 4641827469802785645ULL, continuation 12219685438646842918ULL -> 376923421924023966ULL, broker 13991740092282611901ULL -> 2995238052981659152ULL
//   lower/tolerant: fold 7346939494279371322ULL -> 8536129199662207228ULL, continuation 14377851102187122165ULL -> 9174537903413103601ULL, broker 4706589708976022611ULL -> 5095969895919361099ULL
//   aggregated/canonical: fold 9967633632096001297ULL -> 8850485432917411920ULL, continuation 13776066499644887863ULL -> 4961257679657254387ULL, broker 9170645520517013395ULL -> 1799761924515711285ULL
//   aggregated/tolerant: fold 5278699956602830351ULL -> 15875277156285188567ULL, continuation 13569450870148861372ULL -> 8914616736812346008ULL, broker 16119994911092813777ULL -> 15133071495860523233ULL
//   subscribed/canonical: fold 10304051538385989722ULL -> 390894263916183541ULL, continuation 5016811788325936431ULL -> 12462316195371213713ULL, broker 12476082044112793053ULL -> 7973845562128009868ULL
//   subscribed/tolerant: fold 18199390937065225687ULL -> 7361324576483339450ULL, continuation 7478342822764879742ULL -> 9264427645489656194ULL, broker 1102107428635476833ULL -> 2831646342730737553ULL
//   anchored/canonical: fold 1592318052540792533ULL -> 17197439533615939981ULL, continuation 3538934304779449462ULL -> 5136987709397563060ULL, broker 1198877457996285863ULL -> 6929047812020472439ULL
//   anchored/tolerant: fold 9100924387544486212ULL -> 8397173971337408442ULL, continuation 4088918694907758187ULL -> 4669775647537569183ULL, broker 9162067365282817362ULL -> 14274470403008160849ULL
//   risk/canonical: fold 6804201206108766303ULL -> 5095627687336300866ULL, continuation 10299737440489900284ULL -> 2785871838757152508ULL, broker 981689054909477125ULL -> 8320168402934656314ULL
//   risk/tolerant: fold 2654864785376993282ULL -> 13227263656148245620ULL, continuation 16752229890685358566ULL -> 10822680993924448723ULL, broker 5783115819434271414ULL -> 16354953049491256245ULL
//   ticks/canonical: fold 16224648660468853933ULL -> 9487218993916862366ULL, continuation 11902141403921900111ULL -> 10310962926004479498ULL, broker 4879187569078925087ULL -> 11647353557686017076ULL
//   ticks/tolerant: fold 10848192097659751783ULL -> 2625227532442913688ULL, continuation 5512713152239396396ULL -> 4061674630377631542ULL, broker 11227996984517915967ULL -> 3207582933107796684ULL
//   stream_bars/canonical: fold 14414642560926173568ULL -> 7718834609732373965ULL, continuation 7506352038632773653ULL -> 5464448038614246844ULL, broker 10926254795834410113ULL -> 18150893572169595191ULL
//   stream_bars/tolerant: fold 6192619848942785326ULL -> 12737355743496944695ULL, continuation 5530173888507380536ULL -> 5118755846516500329ULL, broker 5034083613376457261ULL -> 64347809724535028ULL
constexpr Pin kPins[] = {
    {"batch/canonical", 8417350598053238959ULL, 3930831923810011709ULL, 11999400313963825447ULL, 6ULL},
    {"batch/tolerant", 14844934503495743671ULL, 13486072370782740075ULL, 714139398657209591ULL, 7ULL},
    {"margin/canonical", 17458675826522343092ULL, 2991704189571748528ULL, 13780954738578495924ULL, 0ULL},
    {"margin/tolerant", 15357271826299544132ULL, 7845006491016447433ULL, 14848973100437479489ULL, 4ULL},
    {"margin_calculation/canonical", 12225698990055261516ULL, 17852094020611711180ULL, 13636565062500767501ULL, 4ULL},
    {"margin_calculation/tolerant", 15618343851819169665ULL, 12657325099533710982ULL, 15810633237895378470ULL, 1ULL},
    {"margin_fx_roll/canonical", 2789533630001178538ULL, 17178754079582823760ULL, 209420367109385368ULL, 0ULL},
    {"margin_fx_roll/tolerant", 14338402113188947682ULL, 1793934981347633737ULL, 12065124535081589535ULL, 5ULL},
    {"every_point/canonical", 7562764044102581192ULL, 3737699987008739230ULL, 4383124431781758246ULL, 9ULL},
    {"every_point/tolerant", 14141982756948460013ULL, 9846509637745922907ULL, 14678512838653533407ULL, 8ULL},
    {"synthesized/canonical", 2376681450298217164ULL, 8919327222620353451ULL, 3600925079421309162ULL, 9ULL},
    {"synthesized/tolerant", 4035245647427445567ULL, 2693662612566588702ULL, 119147027709136062ULL, 13ULL},
    {"synthesized_every_point/canonical", 13963831207202754247ULL, 1411759699604227935ULL, 8220237254062191568ULL, 10ULL},
    {"synthesized_every_point/tolerant", 4089704344096729236ULL, 12877338782012258332ULL, 445075360625743117ULL, 9ULL},
    {"volume_weighted/canonical", 17081627527962347612ULL, 10482737734476373322ULL, 8103680506999774488ULL, 6ULL},
    {"volume_weighted/tolerant", 6813541438777655137ULL, 4189454090113514211ULL, 11870207615699990715ULL, 7ULL},
    {"lower/canonical", 4641827469802785645ULL, 376923421924023966ULL, 2995238052981659152ULL, 6ULL},
    {"lower/tolerant", 8536129199662207228ULL, 9174537903413103601ULL, 5095969895919361099ULL, 8ULL},
    {"aggregated/canonical", 8850485432917411920ULL, 4961257679657254387ULL, 1799761924515711285ULL, 2ULL},
    {"aggregated/tolerant", 15875277156285188567ULL, 8914616736812346008ULL, 15133071495860523233ULL, 0ULL},
    {"subscribed/canonical", 390894263916183541ULL, 12462316195371213713ULL, 7973845562128009868ULL, 10ULL},
    {"subscribed/tolerant", 7361324576483339450ULL, 9264427645489656194ULL, 2831646342730737553ULL, 10ULL},
    {"anchored/canonical", 17197439533615939981ULL, 5136987709397563060ULL, 6929047812020472439ULL, 7ULL},
    {"anchored/tolerant", 8397173971337408442ULL, 4669775647537569183ULL, 14274470403008160849ULL, 9ULL},
    {"risk/canonical", 5095627687336300866ULL, 2785871838757152508ULL, 8320168402934656314ULL, 4ULL},
    {"risk/tolerant", 13227263656148245620ULL, 10822680993924448723ULL, 16354953049491256245ULL, 3ULL},
    {"ticks/canonical", 9487218993916862366ULL, 10310962926004479498ULL, 11647353557686017076ULL, 10ULL},
    {"ticks/tolerant", 2625227532442913688ULL, 4061674630377631542ULL, 3207582933107796684ULL, 5ULL},
    {"stream_bars/canonical", 7718834609732373965ULL, 5464448038614246844ULL, 18150893572169595191ULL, 8ULL},
    {"stream_bars/tolerant", 12737355743496944695ULL, 5118755846516500329ULL, 64347809724535028ULL, 11ULL},
};

struct Scenario {
    Shape shape;
    bool tolerant;
    NativeCalculationTrigger cadence;
    std::uint64_t seed;
    int bars;
};

std::vector<Scenario> fixed_scenarios() {
    std::vector<Scenario> out;
    for (int shape = 0; shape < kShapes; ++shape) {
        for (int tolerant = 0; tolerant < 2; ++tolerant) {
            const auto cadence = shape % 3 == 0 ? NativeCalculationTrigger::BarCloseAndFills
                                                : NativeCalculationTrigger::BarClose;
            out.push_back({static_cast<Shape>(shape), tolerant == 1, cadence,
                           0x9E3779B97F4A7C15ULL + 7919ULL * static_cast<std::uint64_t>(shape)
                               + static_cast<std::uint64_t>(tolerant),
                           24});
        }
    }
    return out;
}

std::string scenario_name(const Scenario& s) {
    return std::string(kShapeNames[static_cast<int>(s.shape)])
        + (s.tolerant ? "/tolerant" : "/canonical");
}

void pinned_values_hold() {
    const bool dump = std::getenv("PF_LEAN_PATH_DUMP") != nullptr;
    const auto scenarios = fixed_scenarios();
    std::uint64_t hooks[HookCount] = {};
    std::size_t index = 0;
    for (const auto& s : scenarios) {
        const Outcome out = run_shape(s.shape, s.tolerant, s.cadence, s.seed, true, s.bars);
        for (int i = 0; i < HookCount; ++i) hooks[i] += out.calls[i];
        const std::string name = scenario_name(s);
        if (dump) {
            std::printf("    {\"%s\", %lluULL, %lluULL, %lluULL, %lluULL},\n", name.c_str(),
                        static_cast<unsigned long long>(out.fold),
                        static_cast<unsigned long long>(out.continuation),
                        static_cast<unsigned long long>(out.broker),
                        static_cast<unsigned long long>(out.trades));
            std::fprintf(stderr, "%-34s", name.c_str());
            for (int i = 0; i < HookCount; ++i)
                std::fprintf(stderr, " %llu", static_cast<unsigned long long>(out.calls[i]));
            std::fprintf(stderr, "\n");
            continue;
        }
        CHECK(index < sizeof(kPins) / sizeof(kPins[0]));
        if (index >= sizeof(kPins) / sizeof(kPins[0])) break;
        const Pin& pin = kPins[index++];
        const bool ok = name == pin.name && out.fold == pin.fold
            && out.continuation == pin.continuation && out.broker == pin.broker
            && out.trades == pin.trades;
        if (!ok) std::fprintf(stderr, "  pinned value moved: %s\n", name.c_str());
        CHECK(ok);
    }
    if (dump) return;
    CHECK(index == sizeof(kPins) / sizeof(kPins[0]));
    // Not vacuous: every hook of the host was reached by the fixed scenarios.
    for (int i = 0; i < HookCount; ++i) {
        if (hooks[i] == 0) std::fprintf(stderr, "  hook never reached: %s\n", kHookNames[i]);
        CHECK(hooks[i] > 0);
    }
    std::printf("  %zu pinned scenarios hold\n", index);
}

#ifndef PF_L1_HARVEST
void guards_are_their_bodies() {
    int scenarios = 0;
    int trades = 0;
    for (const auto& s : fixed_scenarios()) {
        const Outcome on = run_shape(s.shape, s.tolerant, s.cadence, s.seed, true, s.bars);
        const Outcome off = run_shape(s.shape, s.tolerant, s.cadence, s.seed, false, s.bars);
        if (!same(on, off))
            std::fprintf(stderr, "  guards moved a value: %s\n", scenario_name(s).c_str());
        CHECK(same(on, off));
        ++scenarios;
    }
    std::mt19937_64 rng(0xC0FFEE11D00DULL);
    const NativeCalculationTrigger cadences[] = {
        NativeCalculationTrigger::BarClose, NativeCalculationTrigger::BarCloseAndFills,
        NativeCalculationTrigger::EveryModeledPoint};
    for (int round = 0; round < 420; ++round) {
        const Shape shape = static_cast<Shape>(rng() % kShapes);
        const bool tolerant = rng() % 2 == 0;
        const auto cadence = cadences[rng() % 3];
        const std::uint64_t seed = rng() | 1;
        const int n = 12 + static_cast<int>(rng() % 40);
        const Outcome on = run_shape(shape, tolerant, cadence, seed, true, n);
        const Outcome off = run_shape(shape, tolerant, cadence, seed, false, n);
        if (!same(on, off)) {
            std::fprintf(stderr, "  guards moved a value: %s seed %llu\n",
                         kShapeNames[static_cast<int>(shape)],
                         static_cast<unsigned long long>(seed));
        }
        CHECK(same(on, off));
        trades += static_cast<int>(on.trades);
        ++scenarios;
    }
    std::printf("  %d scenarios agree with the guards on and off (%d trades)\n", scenarios,
                trades);
    CHECK(trades > 1000);
}
#endif

}  // namespace

int main() {
    pinned_values_hold();
    if (std::getenv("PF_LEAN_PATH_DUMP") != nullptr) return 0;
#ifndef PF_L1_HARVEST
    guards_are_their_bodies();
#endif
    std::printf("test_native_lean_path: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
