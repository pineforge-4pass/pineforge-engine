// R5 lane PERF-P23: the ProjectionMismatch contract, write by write.
//
// A host that writes one of the engine fields the kernel projects from the run
// spec (initial capital, point value, the account FX scalar and curve, the
// tick, the commission and the ten instrument and zone strings) fails its run
// with NativeFailureCode::ProjectionMismatch at the first input or callback
// boundary after the write. The lane trims that check twice: pump_batch no
// longer re-compares the fields before an input that the previous input's
// closing check has just compared -- no host code runs between the two -- and
// the strings compare inline. Neither may move where a write is caught.
//
// This witness writes each projected field from each host hook -- every
// NativeStrategyHost virtual and BacktestEngine::hash_host_extension -- at the
// hook's first call and at its third, over batch, aggregated, margin,
// anchored, lower-timeframe, subscription, tick and confirmed-bar stream runs,
// and pins everything the write leaves behind: whether the hook ran, the
// lifecycle, and the whole NativeFailure (code, operation, ordinal,
// discriminator, context kind) with the error text. kExpected was harvested at
// the lane's base (fc7aad62, PF_PROJECTION_WITNESS_DUMP=1) and holds unchanged
// after it.
//
// Source-free: this TU runs in the kernel-only profile.
#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

enum class Hook : int {
    PrepareBegin, RunBegin, Input, Tick, TimeframeBar, BarOpen, Bar, Recalculate,
    SubBar, Applied, Terms, Precommit, MarginRequirement, MarginAllowed, MarginUnits,
    MarginCall, AnchoredLevel, OwnsExcursions, ClosedExcursion, HashExtension, Count
};
constexpr const char* kHookNames[] = {
    "prepare_native_begin", "on_native_run_begin", "on_native_input", "on_native_tick",
    "on_native_timeframe_bar", "on_native_bar_open", "on_native_bar", "on_native_recalculate",
    "on_native_sub_bar", "on_native_applied", "resolve_execution_terms",
    "validate_execution_precommit", "resolve_margin_requirement", "margin_check_allowed",
    "resolve_margin_call_units", "on_native_margin_call", "resolve_anchored_level",
    "owns_lot_excursions", "closed_lot_excursion", "hash_host_extension"};
constexpr int kHooks = static_cast<int>(Hook::Count);
static_assert(sizeof(kHookNames) / sizeof(kHookNames[0]) == kHooks, "one name per hook");

// Every field NativeExecutionConsumer::projection_ok compares.
enum class Field : int {
    InitialCapital, PointValue, AccountFx, FxTimestamps, FxRates, Mintick, CommissionType,
    CommissionValue, Ticker, TickerId, Type, Currency, BaseCurrency, Description,
    VolumeType, Timezone, Session, ChartTimezone, Count
};
constexpr const char* kFieldNames[] = {
    "initial_capital", "pointvalue", "account_fx", "fx_timestamps", "fx_rates", "mintick",
    "commission_type", "commission_value", "ticker", "tickerid", "type", "currency",
    "basecurrency", "description", "volumetype", "timezone", "session", "chart_timezone"};
constexpr int kFields = static_cast<int>(Field::Count);
static_assert(sizeof(kFieldNames) / sizeof(kFieldNames[0]) == kFields, "one name per field");

// The run shapes, each chosen so its hooks are reached.
enum class Profile : int {
    Batch,       // entry + flatten, fills recalculated, kernel-recorded hashes, owned excursions
    Margin,      // a margin model whose breach books a kernel liquidation
    Anchored,    // an anchored stop leg armed at its owner's fill
    Lower,       // a retained lower-timeframe path
    Subscribed,  // a declared 5-minute series over a 1-minute input
    Ticks,       // a realtime stream of prints
    Aggregated,  // a 1-minute input under a 5-minute script
    StreamBars,  // a confirmed-bar stream after its warmup
    Count
};
constexpr const char* kProfileNames[] = {"batch", "margin", "anchored", "lower",
                                         "subscribed", "ticks", "aggregated", "stream_bars"};
constexpr int kProfiles = static_cast<int>(Profile::Count);
static_assert(sizeof(kProfileNames) / sizeof(kProfileNames[0]) == kProfiles,
              "one name per profile");

struct Cell {
    Profile profile;
    Hook hook;
};

constexpr Cell kCells[] = {
    {Profile::Batch, Hook::PrepareBegin},
    {Profile::Batch, Hook::RunBegin},
    {Profile::Batch, Hook::Input},
    {Profile::Batch, Hook::BarOpen},
    {Profile::Batch, Hook::Bar},
    {Profile::Batch, Hook::Recalculate},
    {Profile::Batch, Hook::Applied},
    {Profile::Batch, Hook::Terms},
    {Profile::Batch, Hook::Precommit},
    {Profile::Batch, Hook::OwnsExcursions},
    {Profile::Batch, Hook::ClosedExcursion},
    {Profile::Batch, Hook::HashExtension},
    {Profile::Margin, Hook::MarginRequirement},
    {Profile::Margin, Hook::MarginAllowed},
    {Profile::Margin, Hook::MarginUnits},
    {Profile::Margin, Hook::MarginCall},
    {Profile::Anchored, Hook::AnchoredLevel},
    {Profile::Lower, Hook::SubBar},
    {Profile::Subscribed, Hook::TimeframeBar},
    {Profile::Ticks, Hook::Tick},
    {Profile::Aggregated, Hook::Input},
    {Profile::Aggregated, Hook::BarOpen},
    {Profile::Aggregated, Hook::Bar},
    {Profile::StreamBars, Hook::Input},
    {Profile::StreamBars, Hook::Bar},
};

constexpr int kTriggers[] = {1, 3};

// One host that answers every hook with the kernel's own default and counts
// each call. At the target hook's `trigger`-th call it writes `field` once.
struct WitnessHost final : NativeStrategyHost {
    Profile profile = Profile::Batch;
    bool arm = false;
    Hook target = Hook::Count;
    Field field = Field::Count;
    int trigger = 0;
    int calls[kHooks] = {};
    int closes = 0;
    NativeCalculationReason reason = NativeCalculationReason::BarClose;

    void poke(Hook hook) {
        const int n = ++calls[static_cast<int>(hook)];
        if (arm && hook == target && n == trigger) write(field);
    }
    void poke(Hook hook) const { const_cast<WitnessHost*>(this)->poke(hook); }

    void write(Field f) {
        switch (f) {
        case Field::InitialCapital: initial_capital_ += 1.0; break;
        case Field::PointValue: syminfo_.pointvalue += 1.0; break;
        case Field::AccountFx: account_currency_fx_ += 1.0; break;
        // The two curve arrays always keep one length: the kernel may read
        // them between the write and the check that catches it.
        case Field::FxTimestamps:
            if (!account_currency_fx_timestamps_.empty()) {
                account_currency_fx_timestamps_[0] -= kMinute;
            } else {
                account_currency_fx_timestamps_.push_back(kT0 - kMinute);
                account_currency_fx_rates_.push_back(1.0);
            }
            break;
        case Field::FxRates:
            if (!account_currency_fx_rates_.empty()) {
                account_currency_fx_rates_[0] *= 2.0;
            } else {
                account_currency_fx_timestamps_.push_back(kT0 - kMinute);
                account_currency_fx_rates_.push_back(2.0);
            }
            break;
        case Field::Mintick: syminfo_.mintick += 1.0; break;
        case Field::CommissionType:
            commission_type_ = commission_type_ == CommissionType::PERCENT
                ? CommissionType::CASH_PER_ORDER : CommissionType::PERCENT;
            break;
        case Field::CommissionValue: commission_value_ += 1.0; break;
        case Field::Ticker: syminfo_.ticker += "x"; break;
        case Field::TickerId: syminfo_.tickerid += "x"; break;
        case Field::Type: syminfo_.type += "x"; break;
        case Field::Currency: syminfo_.currency += "x"; break;
        case Field::BaseCurrency: syminfo_.basecurrency += "x"; break;
        case Field::Description: syminfo_.description += "x"; break;
        case Field::VolumeType: syminfo_.volumetype += "x"; break;
        case Field::Timezone: syminfo_.timezone += "x"; break;
        case Field::Session: syminfo_.session += "x"; break;
        case Field::ChartTimezone: chart_timezone_ += "x"; break;
        case Field::Count: break;
        }
    }

    void prepare_native_begin(const NativeBeginArgs&) override { poke(Hook::PrepareBegin); }
    void on_native_run_begin() override {
        poke(Hook::RunBegin);
        if (profile != Profile::Anchored) return;
        const auto parent = submit({no::Transact{1.0}, "entry", ""});
        if (!parent.handle) return;
        no::Request leg{no::Reduce{no::OwnerOpenedUnits{}}, "leg", "bracket"};
        leg.trigger = no::Stop{0.0};
        leg.owner = no::WaitForApplied{*parent.handle};
        leg.anchor = no::FromOwnerFill{-10.0, true};
        (void)submit(leg);
    }
    void on_native_input(const Bar&, const NativeInputContext&) override { poke(Hook::Input); }
    void on_native_tick(const Bar&, const NativeTickContext&) override { poke(Hook::Tick); }
    void on_native_timeframe_bar(const Bar&, const NativeTimeframeBarContext&) override {
        poke(Hook::TimeframeBar);
    }
    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override {
        poke(Hook::BarOpen);
    }
    void on_native_recalculate(const Bar& bar, const NativeDecisionContext& ctx,
                               NativeCalculationReason why,
                               const no::ExecutionAppliedEvent* cause) override {
        poke(Hook::Recalculate);
        reason = why;
        NativeStrategyHost::on_native_recalculate(bar, ctx, why, cause);
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        poke(Hook::Bar);
        if (reason != NativeCalculationReason::BarClose) return;
        const int index = closes++;
        switch (profile) {
        case Profile::Margin:
            if (index == 0) (void)submit({no::Transact{20.0}, "entry", ""});
            break;
        case Profile::Anchored:
            break;
        default:
            if (index == 0) (void)submit({no::Transact{2.0}, "entry", ""});
            if (index == 3) (void)submit({no::Flatten{}, "exit", ""});
            break;
        }
    }
    void on_native_sub_bar(const Bar&, const NativeDecisionContext&) override {
        poke(Hook::SubBar);
    }
    void on_native_applied(const no::ExecutionAppliedEvent&,
                           const NativeDecisionContext&) override {
        poke(Hook::Applied);
    }
    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        poke(Hook::Terms);
        return NativeStrategyHost::resolve_execution_terms(facts);
    }
    NativePrecommitVerdict validate_execution_precommit(
            const NativePrecommitView& view) const override {
        poke(Hook::Precommit);
        return NativeStrategyHost::validate_execution_precommit(view);
    }
    std::optional<NativeMarginDecision> resolve_margin_requirement(
            const NativeMarginRequirementView& view) const override {
        poke(Hook::MarginRequirement);
        return NativeStrategyHost::resolve_margin_requirement(view);
    }
    bool margin_check_allowed(const NativeMarginCheckPoint& point) const override {
        poke(Hook::MarginAllowed);
        return NativeStrategyHost::margin_check_allowed(point);
    }
    std::optional<double> resolve_margin_call_units(
            const NativeMarginCallView& view) const override {
        poke(Hook::MarginUnits);
        return NativeStrategyHost::resolve_margin_call_units(view);
    }
    void on_native_margin_call(const no::MarginCallEvent&) override {
        poke(Hook::MarginCall);
    }
    std::optional<double> resolve_anchored_level(
            const NativeAnchoredLevelView& view) const override {
        poke(Hook::AnchoredLevel);
        return NativeStrategyHost::resolve_anchored_level(view);
    }
    bool owns_lot_excursions() const noexcept override {
        poke(Hook::OwnsExcursions);
        return profile == Profile::Batch;
    }
    ClosedLotExcursion closed_lot_excursion(const ClosedLotExcursionFacts& facts) const override {
        poke(Hook::ClosedExcursion);
        return NativeStrategyHost::closed_lot_excursion(facts);
    }
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        poke(Hook::HashExtension);
        NativeStrategyHost::hash_host_extension(sink);
    }
};

NativeRunSpec base_spec(const char* key) {
    NativeRunSpec s;
    s.identity = {key, 1};
    s.input_tf = "1";
    s.script_tf = "1";
    s.ticker = "MOCK";
    s.tickerid = "TEST:MOCK";
    s.type = "crypto";
    s.currency = "USD";
    s.basecurrency = "BTC";
    s.description = "witness";
    s.volumetype = "base";
    s.timezone = "UTC";
    s.session = "24x7";
    s.chart_timezone = "UTC";
    s.initial_capital = 10000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 1.0;
    return s;
}

Bar ranged(std::int64_t open_ms, double p, double volume = 5.0) {
    return Bar{p, p + 1.0, p - 1.0, p + 0.5, volume, open_ms};
}

std::vector<Bar> minute_bars(int n) {
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i) bars.push_back(ranged(kT0 + i * kMinute, 100.0 + i));
    return bars;
}

NativeRunSpec profile_spec(Profile profile) {
    NativeRunSpec s = base_spec("perf-p23-projection");
    switch (profile) {
    case Profile::Batch:
        s.report_policy = NativeReportPolicy::KernelRecorded;
        s.calculation = NativeCalculationTrigger::BarCloseAndFills;
        break;
    case Profile::Margin: {
        s.initial_capital = 1000.0;
        s.fee_value = 0.0;
        s.close_execution = NativeCloseExecution::AfterCalculation;
        NativeMarginModel m;
        m.initial_long = 0.5;
        m.initial_short = 0.5;
        m.maintenance_long = 0.5;
        m.maintenance_short = 0.5;
        s.margin = m;
        break;
    }
    case Profile::Anchored:
        s.report_policy = NativeReportPolicy::KernelRecorded;
        break;
    case Profile::Lower: {
        s.input_tf = "5";
        s.script_tf = "5";
        IntrabarPath::lower_tf path;
        path.tf = "1";
        for (int i = 0; i < 4; ++i) {
            for (int m = 0; m < 5; ++m) {
                const double q = 100.0 + i + 0.2 * m;
                path.bars.push_back(Bar{q, q + 0.3, q - 0.3, q + 0.1, 1.0,
                                        kT0 + (i * 5 + m) * kMinute});
            }
        }
        s.intrabar.value = path;
        break;
    }
    case Profile::Subscribed: {
        NativeTimeframeSubscription five;
        five.tf = "5";
        s.subscriptions.push_back(five);
        break;
    }
    case Profile::Ticks:
    case Profile::StreamBars:
        break;
    case Profile::Aggregated:
        s.script_tf = "5";
        break;
    case Profile::Count:
        break;
    }
    return s;
}

// Every profile but the tick stream declares a one-step FX curve, so the two
// curve fields are compared against a staged curve; a stream of prints refuses
// one, and compares them against the empty arrays instead.
bool stages_fx_curve(Profile profile) { return profile != Profile::Ticks; }

void drive(WitnessHost& host, Profile profile) {
    switch (profile) {
    case Profile::Batch:
    case Profile::Anchored:
    case Profile::Subscribed:
    case Profile::Aggregated: {
        const int n = profile == Profile::Anchored ? 4
                    : profile == Profile::Batch ? 8 : 16;
        const auto bars = minute_bars(n);
        host.run(bars.data(), static_cast<int>(bars.size()));
        break;
    }
    case Profile::Margin: {
        const std::vector<Bar> bars = {Bar{100.0, 100.0, 100.0, 100.0, 1.0, kT0},
                                       Bar{100.0, 101.0, 95.0, 96.0, 1.0, kT0 + kMinute},
                                       Bar{96.0, 97.0, 95.0, 96.0, 1.0, kT0 + 2 * kMinute}};
        host.run(bars.data(), static_cast<int>(bars.size()));
        break;
    }
    case Profile::Lower: {
        std::vector<Bar> bars;
        for (int i = 0; i < 4; ++i) bars.push_back(ranged(kT0 + i * 5 * kMinute, 100.0 + i));
        host.run(bars.data(), static_cast<int>(bars.size()));
        break;
    }
    case Profile::Ticks: {
        const auto warmup = minute_bars(2);
        if (!host.stream_begin(warmup.data(), 2, "1", "1")) break;
        std::uint64_t sequence = 0;
        for (int k = 0; k < 6; ++k) {
            const TradeTick tick{kT0 + 2 * kMinute + k * 20000, ++sequence,
                                 102.0 + 0.1 * k, 1.0};
            if (!host.stream_push_tick(tick)) break;
        }
        (void)host.stream_end(true);
        break;
    }
    case Profile::StreamBars: {
        const auto bars = minute_bars(7);
        if (!host.stream_begin(bars.data(), 3, "1", "1")) break;
        for (int i = 3; i < 7; ++i) {
            if (!host.stream_push_bar(bars[i])) break;
        }
        (void)host.stream_end(false);
        break;
    }
    case Profile::Count:
        break;
    }
}

bool configure(WitnessHost& host, Profile profile) {
    host.profile = profile;
    if (host.configure_native(profile_spec(profile)).status != NativeSetupStatus::Applied)
        return false;
    if (stages_fx_curve(profile)) {
        NativeFxCurve curve;
        curve.effective_from_ms = {kT0 - 24 * 60 * kMinute};
        curve.account_per_quote = {1.0};
        if (host.configure_native_fx_curve(curve).status != NativeSetupStatus::Applied)
            return false;
    }
    if (profile == Profile::Batch) host.set_broker_state_hash_recording(true);
    return true;
}

std::string outcome(const WitnessHost& host, Hook hook, int trigger) {
    const auto state = host.native_state();
    char text[256];
    std::snprintf(text, sizeof text, "fired=%d kind=%d code=%d op=%d ordinal=%llu disc=%u ctx=%d",
                  host.calls[static_cast<int>(hook)] >= trigger ? 1 : 0,
                  static_cast<int>(state.kind), static_cast<int>(state.failure.code),
                  static_cast<int>(state.failure.operation),
                  static_cast<unsigned long long>(state.failure.ordinal),
                  static_cast<unsigned>(state.failure.discriminator),
                  static_cast<int>(state.failure.context.kind));
    return std::string(text) + " error='" + host.last_error() + "'";
}

struct Expected {
    const char* key;
    const char* outcome;
};

#include "test_native_projection_witness_data.hpp"

std::string cell_of(const Cell& cell, int trigger) {
    return std::string(kProfileNames[static_cast<int>(cell.profile)]) + "/"
        + kHookNames[static_cast<int>(cell.hook)] + "/" + std::to_string(trigger);
}

// The pinned outcome of one write: its field's own row when the write is one
// whose outcome depends on the field, else its cell's.
const char* expected_outcome(const std::string& cell, int field) {
    const std::string key = cell + "/" + kFieldNames[field];
    for (const Expected& row : kFieldExceptions)
        if (key == row.key) return row.outcome;
    for (const Expected& row : kExpected)
        if (cell == row.key) return row.outcome;
    return nullptr;
}

// The unwritten run of every profile completes, and every hook a cell names is
// reached in its profile: a cell whose hook never runs would witness nothing.
void every_cell_reaches_its_hook() {
    for (int p = 0; p < kProfiles; ++p) {
        WitnessHost host;
        const bool ok = configure(host, static_cast<Profile>(p));
        CHECK(ok);
        if (!ok) continue;
        drive(host, static_cast<Profile>(p));
        const auto state = host.native_state();
        if (state.kind != NativeLifecycleKind::Completed) {
            std::fprintf(stderr, "  control run %s: kind=%d code=%d error='%s'\n",
                         kProfileNames[p], static_cast<int>(state.kind),
                         static_cast<int>(state.failure.code), host.last_error().c_str());
        }
        CHECK(state.kind == NativeLifecycleKind::Completed);
        for (const Cell& cell : kCells) {
            if (static_cast<int>(cell.profile) != p) continue;
            const int n = host.calls[static_cast<int>(cell.hook)];
            if (n < 1) {
                std::fprintf(stderr, "  %s never reaches %s\n", kProfileNames[p],
                             kHookNames[static_cast<int>(cell.hook)]);
            }
            CHECK(n >= 1);
        }
    }
}

void every_write_fails_where_it_did() {
    const bool dump = std::getenv("PF_PROJECTION_WITNESS_DUMP") != nullptr;
    int writes = 0;
    int caught = 0;
    for (const Cell& cell : kCells) {
        for (int trigger : kTriggers) {
            const std::string name = cell_of(cell, trigger);
            for (int f = 0; f < kFields; ++f) {
                WitnessHost host;
                const bool ok = configure(host, cell.profile);
                CHECK(ok);
                if (!ok) continue;
                host.arm = true;
                host.target = cell.hook;
                host.field = static_cast<Field>(f);
                host.trigger = trigger;
                drive(host, cell.profile);
                const std::string observed = outcome(host, cell.hook, trigger);
                ++writes;
                if (observed.find("code=10 ") != std::string::npos) ++caught;
                if (dump) {
                    std::printf("    {\"%s/%s\", \"%s\"},\n", name.c_str(), kFieldNames[f],
                                observed.c_str());
                }
                const char* expected = expected_outcome(name, f);
                const bool same = expected != nullptr && observed == expected;
                if (!same) {
                    std::fprintf(stderr, "  %s/%s\n    expected: %s\n    observed: %s\n",
                                 name.c_str(), kFieldNames[f],
                                 expected ? expected : "(no row)", observed.c_str());
                }
                CHECK(same);
            }
        }
    }
    // Every row of the table names a write the matrix makes.
    CHECK(sizeof(kExpected) / sizeof(kExpected[0])
          == sizeof(kCells) / sizeof(kCells[0]) * (sizeof(kTriggers) / sizeof(kTriggers[0])));
    std::printf("  %d writes, %d caught as ProjectionMismatch\n", writes, caught);
}

}  // namespace

int main() {
    every_cell_reaches_its_hook();
    every_write_fails_where_it_did();
    std::printf("test_native_projection_witness: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
