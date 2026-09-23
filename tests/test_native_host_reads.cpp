// R5 lane PERF-L1: the NativeStrategyHost reads against the lookups they
// replaced, bit for bit.
//
// Every public NativeStrategyHost read -- physical_position(), native_state(),
// current_execution_point() and the rest of the forwarding block -- used to
// reach the kernel through BacktestEngine::execution_consumer(), an
// out-of-line call (stack-protected on hardened toolchains) that the Pine
// adapter pays some forty times a bar. The lane reads the consumer straight
// off the engine's slot (NativeExecutionConsumer::bound), computes the book
// aggregate inline, and answers native_state() for a running run without
// probing the lifecycle variant. Each answer must be the one the pre-lane code
// gave at every moment a host can observe it.
//
// This witness drives randomized hosts through batch, aggregated, margin,
// anchored, lower-timeframe, synthesized-path, subscribed, tick-stream and
// confirmed-bar-stream runs, with pyramided long and short books, cooperative
// aborts, callback exceptions and reuse, and compares -- inside every
// NativeStrategyHost hook and BacktestEngine::hash_host_extension, and around
// every public call -- each read with its reference: the consumer
// execution_consumer() answers, the pre-lane book loop and lifecycle probe
// restated verbatim below, and the consumer's own answers for every other
// read of the block.
//
// Fail-before: at the lane's base the consumer has no bound(), so this TU does
// not compile there (the lane report records the first diagnostic).
//
// Source-free: this TU runs in the kernel-only profile.
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v18 {

// The pre-lane lifecycle probes, verbatim, reading the consumer's own state.
struct NativeExecutionConsumerProbe {
    static const NativeRunSpec* reference_spec(const NativeExecutionConsumer& c) {
        if (const auto* n = std::get_if<NativeRunning>(&c.state_)) return &n->spec;
        if (const auto* r = std::get_if<NativeReady>(&c.state_)) return &r->spec;
        if (const auto* d = std::get_if<NativeCompleted>(&c.state_)) return &d->spec;
        if (const auto* f = std::get_if<NativeFailed>(&c.state_)) {
            if (f->spec) return &*f->spec;
        }
        return nullptr;
    }
    // NativeExecutionConsumer::view() at the lane's base.
    static NativeStateView reference_view(const NativeExecutionConsumer& c) {
        NativeStateView v;
        v.consumed_high_water = c.consumed_high_water_;
        v.decision_floor_ms = c.has_floor_ ? c.decision_floor_ms_
                                           : std::numeric_limits<int64_t>::min();
        v.spec = reference_spec(c);
        if (std::holds_alternative<NativeUnconfigured>(c.state_)) {
            v.kind = NativeLifecycleKind::Unconfigured;
        } else if (auto* r = std::get_if<NativeReady>(&c.state_)) {
            v.kind = NativeLifecycleKind::Ready;
            v.spec = &r->spec;
        } else if (auto* n = std::get_if<NativeRunning>(&c.state_)) {
            v.kind = NativeLifecycleKind::Running;
            v.spec = &n->spec;
            v.phase = n->phase;
        } else if (auto* d = std::get_if<NativeCompleted>(&c.state_)) {
            v.kind = NativeLifecycleKind::Completed;
            v.spec = &d->spec;
            v.completion = d->completion;
        } else if (auto* f = std::get_if<NativeFailed>(&c.state_)) {
            v.kind = NativeLifecycleKind::Failed;
            v.failure = f->failure;
            if (f->spec) v.spec = &*f->spec;
        }
        return v;
    }
    // NativeExecutionConsumer::current_execution_point() at the lane's base.
    static std::optional<NativeCurrentPointView> reference_point(const NativeExecutionConsumer& c) {
        if (!c.in_callback_ || !c.current_frame_ || !std::holds_alternative<NativeRunning>(c.state_))
            return std::nullopt;
        return c.current_frame_->point;
    }
};

}  // inline namespace engine_script_run_v18
}  // namespace pineforge

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int passed = 0;
int failed = 0;
std::uint64_t comparisons = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++passed;                                                            \
        } else {                                                                 \
            ++failed;                                                            \
            std::fprintf(stderr, "CHECK FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

// A failed comparison is reported with where it happened; a passing one is
// only counted, so the matrix stays quiet.
#define SAME(where, ok)                                                          \
    do {                                                                         \
        ++comparisons;                                                           \
        if (ok) {                                                                \
            ++passed;                                                            \
        } else {                                                                 \
            ++failed;                                                            \
            std::fprintf(stderr, "MISMATCH at %s: %s\n", where, #ok);            \
        }                                                                        \
    } while (0)

using Probe = NativeExecutionConsumerProbe;

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }

bool same_coordinate(const NativeCoordinate& a, const NativeCoordinate& b) {
    return a.ordinal == b.ordinal && a.interval_index == b.interval_index
        && a.open_ms == b.open_ms && a.eligible_open_ms == b.eligible_open_ms
        && a.last_traded_close_ms == b.last_traded_close_ms
        && a.next_period_open_ms == b.next_period_open_ms
        && a.next_input_open_ms == b.next_input_open_ms
        && a.effective_time_ms == b.effective_time_ms
        && a.source_price_time_ms == b.source_price_time_ms
        && a.provenance == b.provenance && a.path_phase == b.path_phase
        && a.completion == b.completion;
}

bool same_interval(const native_calendar::NativeInterval& a,
                   const native_calendar::NativeInterval& b) {
    return a.open_ms == b.open_ms && a.eligible_open_ms == b.eligible_open_ms
        && a.last_traded_close_ms == b.last_traded_close_ms
        && a.next_period_open_ms == b.next_period_open_ms
        && a.next_input_open_ms == b.next_input_open_ms;
}

bool same_decision(const NativeDecisionContext& a, const NativeDecisionContext& b) {
    return same_coordinate(a.coordinate, b.coordinate)
        && a.decision_floor_ms == b.decision_floor_ms
        && same_interval(a.input_interval, b.input_interval)
        && same_interval(a.script_interval, b.script_interval)
        && a.sub_index == b.sub_index && a.sub_count == b.sub_count
        && a.is_terminal_sub_bar == b.is_terminal_sub_bar
        && a.in_session == b.in_session && a.opens_session_day == b.opens_session_day
        && a.closes_session_day == b.closes_session_day
        && a.closes_session_day_open_ended == b.closes_session_day_open_ended
        && a.sub_bar_open_ms == b.sub_bar_open_ms
        && a.script_bar_open_ms == b.script_bar_open_ms
        && a.driver_statistics.intrabar_path_enabled == b.driver_statistics.intrabar_path_enabled
        && a.driver_statistics.sub_bars_per_script_bar
               == b.driver_statistics.sub_bars_per_script_bar
        && a.driver_statistics.samples_per_sub_bar == b.driver_statistics.samples_per_sub_bar
        && a.driver_statistics.sub_bars_processed == b.driver_statistics.sub_bars_processed
        && a.driver_statistics.sample_ticks_processed
               == b.driver_statistics.sample_ticks_processed;
}

bool same_point(const std::optional<NativeCurrentPointView>& a,
                const std::optional<NativeCurrentPointView>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return same_decision(a->decision, b->decision) && same_bits(a->price, b->price)
        && a->quote_kind == b->quote_kind && a->quote_origin_ordinal == b->quote_origin_ordinal;
}

bool same_failure(const NativeFailure& a, const NativeFailure& b) {
    if (a.code != b.code || a.operation != b.operation || a.ordinal != b.ordinal
        || a.discriminator != b.discriminator || a.context.kind != b.context.kind) {
        return false;
    }
    return a.context.cause.ordinal == b.context.cause.ordinal
        && a.context.recipient.incarnation == b.context.recipient.incarnation
        && same_coordinate(a.context.cursor.point, b.context.cursor.point)
        && same_bits(a.context.cursor.t, b.context.cursor.t);
}

bool same_state(const NativeStateView& a, const NativeStateView& b) {
    return a.kind == b.kind && a.spec == b.spec && a.phase == b.phase
        && a.completion == b.completion && same_failure(a.failure, b.failure)
        && a.consumed_high_water == b.consumed_high_water
        && a.decision_floor_ms == b.decision_floor_ms;
}

constexpr std::int64_t kT0 = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000;

// The run shapes, each chosen so its hooks are reached.
enum class Profile : int {
    Batch, Margin, Anchored, Lower, Synthesized, Subscribed, Ticks, Aggregated, StreamBars, Count
};
constexpr const char* kProfileNames[] = {"batch", "margin", "anchored", "lower", "synthesized",
                                         "subscribed", "ticks", "aggregated", "stream_bars"};
constexpr int kProfiles = static_cast<int>(Profile::Count);

enum Hook : int {
    PrepareBegin, RunBegin, Input, Tick, TimeframeBar, BarOpen, Bar_, Recalculate, SubBar,
    Applied, Terms, Precommit, MarginRequirement, MarginAllowed, MarginUnits, MarginCall,
    AnchoredLevel, OwnsExcursions, ClosedExcursion, HashExtension, Outside, HookCount
};
constexpr const char* kHookNames[] = {
    "prepare_native_begin", "on_native_run_begin", "on_native_input", "on_native_tick",
    "on_native_timeframe_bar", "on_native_bar_open", "on_native_bar", "on_native_recalculate",
    "on_native_sub_bar", "on_native_applied", "resolve_execution_terms",
    "validate_execution_precommit", "resolve_margin_requirement", "margin_check_allowed",
    "resolve_margin_call_units", "on_native_margin_call", "resolve_anchored_level",
    "owns_lot_excursions", "closed_lot_excursion", "hash_host_extension", "outside"};
static_assert(sizeof(kHookNames) / sizeof(kHookNames[0]) == HookCount, "one name per hook");

std::uint64_t visits[HookCount] = {};
int max_lots = 0;
int short_books = 0;

struct ReadsHost : NativeStrategyHost {
    Profile profile = Profile::Batch;
    std::mt19937_64* rng = nullptr;
    int closes = 0;
    int abort_at = -1;
    int throw_at = -1;
    NativeCalculationReason reason = NativeCalculationReason::BarClose;

    NativeExecutionConsumer& consumer() const {
        return as_native_consumer(const_cast<ReadsHost*>(this)->execution_consumer());
    }

    // The book aggregate at the lane's base (NativeExecutionConsumer::position).
    NativePhysicalPosition reference_position() const {
        NativePhysicalPosition out;
        const auto n = pyramid_entries_.size();
        out.lot_count = n;
        if (n == 0) return out;
        if (n == 1) {
            const auto& lot = pyramid_entries_[0];
            out.signed_units = position_side_ == PositionSide::SHORT ? -lot.qty : lot.qty;
            out.average_price = lot.qty > 0.0 ? lot.price : 0.0;
            return out;
        }
        double qty = 0.0;
        double weighted = 0.0;
        for (const auto& lot : pyramid_entries_) {
            qty += lot.qty;
            weighted += lot.qty * lot.price;
        }
        out.signed_units = position_side_ == PositionSide::SHORT ? -qty : qty;
        out.average_price = qty > 0.0 ? weighted / qty : 0.0;
        return out;
    }

    void verify(Hook hook) const {
        ++visits[hook];
        const char* where = kHookNames[hook];
        NativeExecutionConsumer& c = consumer();
        // The consumer itself: bound() is execution_consumer()'s own answer.
        SAME(where, &NativeExecutionConsumer::bound(*this) == &c);
        const BacktestEngine& engine = *this;
        SAME(where, &NativeExecutionConsumer::bound(engine) == &c);
        // The book aggregate, bit for bit.
        const NativePhysicalPosition position = physical_position();
        const NativePhysicalPosition expected = reference_position();
        SAME(where, position.lot_count == expected.lot_count);
        SAME(where, same_bits(position.signed_units, expected.signed_units));
        SAME(where, same_bits(position.average_price, expected.average_price));
        if (static_cast<int>(position.lot_count) > max_lots)
            max_lots = static_cast<int>(position.lot_count);
        if (position.signed_units < 0.0) ++short_books;
        // The lifecycle view and the current point.
        SAME(where, same_state(native_state(), Probe::reference_view(c)));
        SAME(where, same_point(current_execution_point(), Probe::reference_point(c)));
        // The rest of the forwarding block answers what the consumer answers.
        SAME(where, native_decision_floor() == c.decision_floor());
        SAME(where, native_consumed_high_water() == c.high_water());
        SAME(where, native_continuation_hash() == c.continuation_hash());
        SAME(where, native_recalculation_count() == c.recalculation_count());
        SAME(where, native_recalculations_skipped() == c.recalculations_skipped());
        const double mark = 100.0 + static_cast<double>(closes % 7);
        SAME(where, same_bits(native_marked_equity(mark), c.marked(*this, mark)));
        SAME(where, native_open_lots(mark).size() == c.open_lots(*this, mark).size());
        const auto liquidation = native_liquidation_price();
        const auto kernel_liquidation = c.host_liquidation_price(*this);
        SAME(where, liquidation.has_value() == kernel_liquidation.has_value()
                        && (!liquidation || same_bits(*liquidation, *kernel_liquidation)));
        const auto risk = native_risk_state();
        const auto kernel_risk = c.risk_state();
        SAME(where, risk.blocked == kernel_risk.blocked && risk.has_day == kernel_risk.has_day
                        && risk.day_ordinal == kernel_risk.day_ordinal
                        && risk.fills_today == kernel_risk.fills_today);
        const auto partial = current_partial_bar();
        const auto kernel_partial = c.partial_bar();
        SAME(where, partial.has_value() == kernel_partial.has_value()
                        && (!partial || (same_bits(partial->close, kernel_partial->close)
                                         && partial->timestamp == kernel_partial->timestamp)));
        const auto working = native_working_requests();
        SAME(where, working.size() == c.working_requests().size());
        for (const auto& row : working) {
            const auto trail = trail_state(row.definition->handle);
            const auto kernel_trail = c.trail_state(*this, row.definition->handle);
            SAME(where, trail.has_value() == kernel_trail.has_value());
        }
        const uint64_t after = native_consumed_high_water() > 0 && closes % 3 == 0
            ? c.event_high_water() / 2 : 0;
        const auto events = native_events(after);
        const auto kernel_events = c.events_after(after);
        SAME(where, events.size() == kernel_events.size()
                        && (events.empty() || events.back().ordinal == kernel_events.back().ordinal));
    }

    void prepare_native_begin(const NativeBeginArgs&) override { verify(PrepareBegin); }
    void on_native_run_begin() override {
        verify(RunBegin);
        if (profile != Profile::Anchored) return;
        const auto parent = submit({no::Transact{1.0}, "entry", ""});
        if (!parent.handle) return;
        no::Request leg{no::Reduce{no::OwnerOpenedUnits{}}, "leg", "bracket"};
        leg.trigger = no::Stop{0.0};
        leg.owner = no::WaitForApplied{*parent.handle};
        leg.anchor = no::FromOwnerFill{-10.0, true};
        (void)submit(leg);
    }
    void on_native_input(const Bar&, const NativeInputContext&) override { verify(Input); }
    void on_native_tick(const Bar&, const NativeTickContext&) override {
        verify(Tick);
        trade();
    }
    void on_native_timeframe_bar(const Bar&, const NativeTimeframeBarContext&) override {
        verify(TimeframeBar);
    }
    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override {
        verify(BarOpen);
        if ((*rng)() % 4 == 0) trade();
    }
    void on_native_recalculate(const Bar& bar, const NativeDecisionContext& ctx,
                               NativeCalculationReason why,
                               const no::ExecutionAppliedEvent* cause) override {
        verify(Recalculate);
        reason = why;
        NativeStrategyHost::on_native_recalculate(bar, ctx, why, cause);
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        verify(Bar_);
        if (reason != NativeCalculationReason::BarClose) return;
        const int index = closes++;
        if (index == abort_at) request_abort();
        if (index == throw_at) throw std::runtime_error("host reads witness callback exception");
        // The broker fold calls hash_host_extension, whose own verify() runs
        // inside this callback.
        if (index % 4 == 1) (void)broker_state_hash();
        if (profile == Profile::Margin) {
            if (index == 0) (void)submit({no::Transact{20.0}, "entry", ""});
            return;
        }
        trade();
    }
    // A random market order: pyramid a long or a short book, reverse it, or
    // flatten it.
    void trade() {
        switch ((*rng)() % 6) {
        case 0: case 1: (void)submit({no::Transact{1.0 + static_cast<double>((*rng)() % 3)}, "l", ""}); break;
        case 2: case 3: (void)submit({no::Transact{-1.0 - static_cast<double>((*rng)() % 3)}, "s", ""}); break;
        case 4: (void)submit({no::Flatten{}, "x", ""}); break;
        default: break;
        }
    }
    void on_native_sub_bar(const Bar&, const NativeDecisionContext&) override { verify(SubBar); }
    void on_native_applied(const no::ExecutionAppliedEvent&,
                           const NativeDecisionContext&) override {
        verify(Applied);
    }
    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        verify(Terms);
        return NativeStrategyHost::resolve_execution_terms(facts);
    }
    NativePrecommitVerdict validate_execution_precommit(
            const NativePrecommitView& view) const override {
        verify(Precommit);
        return NativeStrategyHost::validate_execution_precommit(view);
    }
    std::optional<NativeMarginDecision> resolve_margin_requirement(
            const NativeMarginRequirementView& view) const override {
        verify(MarginRequirement);
        return NativeStrategyHost::resolve_margin_requirement(view);
    }
    bool margin_check_allowed(const NativeMarginCheckPoint& point) const override {
        verify(MarginAllowed);
        return NativeStrategyHost::margin_check_allowed(point);
    }
    std::optional<double> resolve_margin_call_units(
            const NativeMarginCallView& view) const override {
        verify(MarginUnits);
        return NativeStrategyHost::resolve_margin_call_units(view);
    }
    void on_native_margin_call(const no::MarginCallEvent&) override { verify(MarginCall); }
    std::optional<double> resolve_anchored_level(
            const NativeAnchoredLevelView& view) const override {
        verify(AnchoredLevel);
        return NativeStrategyHost::resolve_anchored_level(view);
    }
    bool owns_lot_excursions() const noexcept override {
        verify(OwnsExcursions);
        return profile == Profile::Batch;
    }
    ClosedLotExcursion closed_lot_excursion(const ClosedLotExcursionFacts& facts) const override {
        verify(ClosedExcursion);
        return NativeStrategyHost::closed_lot_excursion(facts);
    }
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        verify(HashExtension);
        NativeStrategyHost::hash_host_extension(sink);
    }
};

// A deeper, final host class: the reads are the same whatever the most-derived
// type.
struct DeeperHost final : ReadsHost {
    int extra = 0;
    void on_native_input(const Bar& bar, const NativeInputContext& context) override {
        ++extra;
        ReadsHost::on_native_input(bar, context);
    }
};

NativeRunSpec base_spec(std::uint64_t run_number) {
    NativeRunSpec s;
    s.identity = {"perf-l1-host-reads", run_number};
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

Bar ranged(std::int64_t open_ms, double p, double volume = 5.0) {
    return Bar{p, p + 1.0, p - 1.0, p + 0.5, volume, open_ms};
}

std::vector<Bar> minute_bars(int n, std::mt19937_64& rng) {
    std::vector<Bar> bars;
    double p = 100.0;
    for (int i = 0; i < n; ++i) {
        p += static_cast<double>(static_cast<int>(rng() % 5) - 2) * 0.5;
        bars.push_back(ranged(kT0 + i * kMinute, p, 1.0 + static_cast<double>(rng() % 9)));
    }
    return bars;
}

NativeRunSpec profile_spec(Profile profile, std::mt19937_64& rng, std::uint64_t run_number) {
    NativeRunSpec s = base_spec(run_number);
    s.slot_label_policy = rng() % 2 == 0 ? NativeSlotLabelPolicy::Canonical
                                         : NativeSlotLabelPolicy::FeedTolerant;
    switch (rng() % 4) {
    case 0: s.calculation = NativeCalculationTrigger::BarCloseAndFills; break;
    case 1: s.calculation = NativeCalculationTrigger::EveryModeledPoint; break;
    default: break;
    }
    if (rng() % 3 == 0) s.report_policy = NativeReportPolicy::KernelRecorded;
    if (rng() % 3 == 0) s.close_execution = NativeCloseExecution::AfterCalculation;
    if (rng() % 4 == 0) s.open_bar_view = NativeOpenBarView::OpenOnly;
    switch (profile) {
    case Profile::Margin: {
        s.initial_capital = 1000.0;
        s.fee_value = 0.0;
        NativeMarginModel m;
        m.initial_long = 0.5;
        m.initial_short = 0.5;
        m.maintenance_long = 0.5;
        m.maintenance_short = 0.5;
        if (rng() % 2 == 0) m.check = NativeLiquidationCheck::CalculationOnly;
        s.margin = m;
        break;
    }
    case Profile::Lower: {
        s.input_tf = "5";
        s.script_tf = "5";
        IntrabarPath::lower_tf path;
        path.tf = "1";
        for (int i = 0; i < 12; ++i) {
            for (int m = 0; m < 5; ++m) {
                const double q = 100.0 + i + 0.2 * m;
                path.bars.push_back(Bar{q, q + 0.3, q - 0.3, q + 0.1, 1.0,
                                        kT0 + (i * 5 + m) * kMinute});
            }
        }
        s.intrabar.value = path;
        break;
    }
    case Profile::Synthesized: {
        IntrabarPath::synthesized path;
        path.samples = 4 + static_cast<int>(rng() % 3);
        s.intrabar.value = path;
        break;
    }
    case Profile::Subscribed: {
        NativeTimeframeSubscription five;
        five.tf = "5";
        s.subscriptions.push_back(five);
        break;
    }
    case Profile::Aggregated:
        s.script_tf = "5";
        break;
    case Profile::Batch:
        if (rng() % 2 == 0) {
            NativeRiskLimits risk;
            NativeLossLimit loss;
            loss.value = 50.0;
            risk.max_intraday_loss = loss;
            risk.max_fills_per_day = 40;
            s.risk = risk;
        }
        break;
    default:
        break;
    }
    // A tick stream calculates on its closes only.
    if (profile == Profile::Ticks) {
        s.calculation = NativeCalculationTrigger::BarClose;
        s.intrabar.value = IntrabarPath::none{};
    }
    return s;
}

void drive(ReadsHost& host, Profile profile, std::mt19937_64& rng) {
    switch (profile) {
    case Profile::Ticks: {
        const auto warmup = minute_bars(2, rng);
        if (!host.stream_begin(warmup.data(), 2, "1", "1")) break;
        host.verify(Outside);
        std::uint64_t sequence = 0;
        for (int k = 0; k < 12; ++k) {
            const TradeTick tick{kT0 + 2 * kMinute + k * 20000, ++sequence,
                                 102.0 + 0.1 * static_cast<double>(rng() % 9), 1.0};
            if (!host.stream_push_tick(tick)) break;
            host.verify(Outside);
        }
        (void)host.stream_end(true);
        break;
    }
    case Profile::StreamBars: {
        const auto bars = minute_bars(12, rng);
        if (!host.stream_begin(bars.data(), 3, "1", "1")) break;
        host.verify(Outside);
        for (int i = 3; i < 12; ++i) {
            if (!host.stream_push_bar(bars[i])) break;
            host.verify(Outside);
        }
        (void)host.stream_end(false);
        break;
    }
    case Profile::Lower: {
        std::vector<Bar> bars;
        for (int i = 0; i < 12; ++i) bars.push_back(ranged(kT0 + i * 5 * kMinute, 100.0 + i));
        host.run(bars.data(), static_cast<int>(bars.size()));
        break;
    }
    case Profile::Margin: {
        const std::vector<Bar> bars = {Bar{100.0, 100.0, 100.0, 100.0, 1.0, kT0},
                                       Bar{100.0, 101.0, 95.0, 96.0, 1.0, kT0 + kMinute},
                                       Bar{96.0, 97.0, 95.0, 96.0, 1.0, kT0 + 2 * kMinute},
                                       Bar{96.0, 99.0, 90.0, 91.0, 1.0, kT0 + 3 * kMinute}};
        host.run(bars.data(), static_cast<int>(bars.size()));
        break;
    }
    default: {
        const auto bars = minute_bars(10 + static_cast<int>(rng() % 20), rng);
        host.run(bars.data(), static_cast<int>(bars.size()));
        break;
    }
    }
    host.verify(Outside);
    (void)host.broker_state_hash();
}

struct Tally {
    int runs = 0;
    int completed = 0;
    int failed = 0;
};

void lifecycle(ReadsHost& host, std::mt19937_64& rng, Tally& tally) {
    host.rng = &rng;
    host.verify(Outside);
    const Profile profile = static_cast<Profile>(rng() % kProfiles);
    const int runs = 1 + static_cast<int>(rng() % 3);
    for (int run = 0; run < runs; ++run) {
        host.profile = profile;
        const auto setup = host.configure_native(
            profile_spec(profile, rng, static_cast<std::uint64_t>(run + 1)));
        host.verify(Outside);
        if (setup.status != NativeSetupStatus::Applied) break;
        if (profile == Profile::Margin) {
            NativeFxCurve curve;
            curve.effective_from_ms = {kT0 - 24 * 60 * kMinute, kT0 + 2 * kMinute};
            curve.account_per_quote = {1.0, 1.25};
            (void)host.configure_native_fx_curve(curve);
            host.verify(Outside);
        }
        host.closes = 0;
        host.reason = NativeCalculationReason::BarClose;
        // Kernel-recorded rows fold the host extension at every report point.
        host.set_broker_state_hash_recording(rng() % 3 == 0);
        host.abort_at = rng() % 6 == 0 ? static_cast<int>(rng() % 8) : -1;
        host.throw_at = host.abort_at < 0 && rng() % 8 == 0 ? static_cast<int>(rng() % 8) : -1;
        ++tally.runs;
        drive(host, profile, rng);
        const auto kind = host.native_state().kind;
        if (kind == NativeLifecycleKind::Completed) ++tally.completed;
        if (kind == NativeLifecycleKind::Failed) {
            ++tally.failed;
            if (host.native_state().failure.code != NativeFailureCode::Aborted) break;
        }
    }
    host.verify(Outside);
}

void reads_answer_as_the_lookups_did() {
    std::mt19937_64 rng(0x5EEDF00DBA5EBA11ULL);
    Tally tally;
    for (int round = 0; round < 180; ++round) {
        if (round % 2 == 0) {
            ReadsHost a;
            DeeperHost b;
            lifecycle(a, rng, tally);
            lifecycle(b, rng, tally);
        } else {
            DeeperHost a;
            lifecycle(a, rng, tally);
        }
    }
    std::printf("  %d runs: %d completed, %d failed; %llu comparisons; books up to %d lots,"
                " %d short-book reads\n",
                tally.runs, tally.completed, tally.failed,
                static_cast<unsigned long long>(comparisons), max_lots, short_books);
    // Not vacuous: every hook was reached, books were pyramided both ways,
    // and runs ended every way a run ends.
    for (int hook = 0; hook < HookCount; ++hook) {
        if (visits[hook] == 0) std::fprintf(stderr, "  hook never reached: %s\n", kHookNames[hook]);
        CHECK(visits[hook] > 0);
    }
    CHECK(max_lots >= 3);
    CHECK(short_books > 100);
    CHECK(tally.completed > 50);
    CHECK(tally.failed > 5);
    CHECK(comparisons > 100000);
}

}  // namespace

int main() {
    reads_answer_as_the_lookups_did();
    std::printf("test_native_host_reads: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
