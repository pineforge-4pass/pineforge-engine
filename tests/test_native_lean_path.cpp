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
constexpr Pin kPins[] = {
    {"batch/canonical", 5166037941821544647ULL, 14809352016120894185ULL, 11338747070571302820ULL, 6ULL},
    {"batch/tolerant", 18189486081941112598ULL, 14388238089025711186ULL, 7862196356352290449ULL, 7ULL},
    {"margin/canonical", 11987783106259734011ULL, 3455256496964847716ULL, 7725613184755129846ULL, 0ULL},
    {"margin/tolerant", 4409189285804866438ULL, 855085601544096740ULL, 5160401203850839130ULL, 4ULL},
    {"margin_calculation/canonical", 1568470444282449322ULL, 13861699917022548257ULL, 3289041035191503419ULL, 4ULL},
    {"margin_calculation/tolerant", 14823551462041516976ULL, 11533820964462831656ULL, 13853377775653101846ULL, 1ULL},
    {"margin_fx_roll/canonical", 13645453736898832911ULL, 2272937872551974761ULL, 14995707425517547873ULL, 0ULL},
    {"margin_fx_roll/tolerant", 928661954089809302ULL, 14611393619647988727ULL, 9639648990238122086ULL, 5ULL},
    {"every_point/canonical", 15169653257185161393ULL, 11942220871818969230ULL, 9620246032232391656ULL, 9ULL},
    {"every_point/tolerant", 11710645089551942874ULL, 16516145559658558435ULL, 15993892195546568613ULL, 8ULL},
    {"synthesized/canonical", 13721396498718821255ULL, 16252911878732103900ULL, 1032857913756253235ULL, 9ULL},
    {"synthesized/tolerant", 4331154914870688083ULL, 16490472740586268474ULL, 15966330760823216464ULL, 13ULL},
    {"synthesized_every_point/canonical", 11971132682800678447ULL, 6753026284485815074ULL, 2776925473445912126ULL, 10ULL},
    {"synthesized_every_point/tolerant", 3146256244272210816ULL, 10083350186892347974ULL, 640394820882642634ULL, 9ULL},
    {"volume_weighted/canonical", 17207772673036829567ULL, 5231083461152657006ULL, 17134903625378598223ULL, 6ULL},
    {"volume_weighted/tolerant", 10180024423461507044ULL, 7895603099092190300ULL, 17629149376284097724ULL, 7ULL},
    {"lower/canonical", 11313059837849793711ULL, 12219685438646842918ULL, 13991740092282611901ULL, 6ULL},
    {"lower/tolerant", 7346939494279371322ULL, 14377851102187122165ULL, 4706589708976022611ULL, 8ULL},
    {"aggregated/canonical", 9967633632096001297ULL, 13776066499644887863ULL, 9170645520517013395ULL, 2ULL},
    {"aggregated/tolerant", 5278699956602830351ULL, 13569450870148861372ULL, 16119994911092813777ULL, 0ULL},
    {"subscribed/canonical", 10304051538385989722ULL, 5016811788325936431ULL, 12476082044112793053ULL, 10ULL},
    {"subscribed/tolerant", 18199390937065225687ULL, 7478342822764879742ULL, 1102107428635476833ULL, 10ULL},
    {"anchored/canonical", 1592318052540792533ULL, 3538934304779449462ULL, 1198877457996285863ULL, 7ULL},
    {"anchored/tolerant", 9100924387544486212ULL, 4088918694907758187ULL, 9162067365282817362ULL, 9ULL},
    {"risk/canonical", 6804201206108766303ULL, 10299737440489900284ULL, 981689054909477125ULL, 4ULL},
    {"risk/tolerant", 2654864785376993282ULL, 16752229890685358566ULL, 5783115819434271414ULL, 3ULL},
    {"ticks/canonical", 16224648660468853933ULL, 11902141403921900111ULL, 4879187569078925087ULL, 10ULL},
    {"ticks/tolerant", 10848192097659751783ULL, 5512713152239396396ULL, 11227996984517915967ULL, 5ULL},
    {"stream_bars/canonical", 14414642560926173568ULL, 7506352038632773653ULL, 10926254795834410113ULL, 8ULL},
    {"stream_bars/tolerant", 6192619848942785326ULL, 5530173888507380536ULL, 5034083613376457261ULL, 11ULL},
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
