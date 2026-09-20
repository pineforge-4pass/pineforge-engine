/*
 * test_native_margin_model.cpp — R5 lane L4: the generic native margin model.
 *
 * Everything here is opt-in through NativeRunSpec::margin. The first scenario
 * pins that a spec WITHOUT a margin model still books the same fills, the same
 * book and the same run-spec fold as the pre-L4 tree, so the adapter — which
 * never sets the field — is byte-identical by construction.
 *
 * Hand arithmetic used throughout (point_value = 1, account fx = 1, no fee):
 *   equity(P)      = capital + realized + dir * (P - entry) * units
 *   required(P, m) = units * P * m
 *   liquidation L  : equity(L) == required(L, maintenance)
 *                  = (capital + realized - dir * units * entry)
 *                    / (units * (maintenance - dir))
 *   restore        = (required(mark) - equity(mark)) / (mark * maintenance)
 */
#include "native_current_fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace r4_test;

namespace {

constexpr const char* kLiquidationLabel = "__kernel_liquidation__";

// ── Portable spec-fold pins ─────────────────────────────────────────────
// A raw native_continuation_hash() constant is NOT portable: the consumer
// folds the run's resolved timezone identity — the zoneinfo root and the zone
// file paths of the machine that ran it — so the same run hashes differently
// here and on each CI runner. native_run_spec_digest() is exactly the
// consumer's run-spec fold and nothing else, so it is the same number
// everywhere. Both constants are observed on THIS tree for the margin-free
// specs of scenarios 1+10 and guard the fold's field list and order; the
// neutrality claim itself is the equalities beside them — `margin` folds
// nothing while it is unset, and declaring a model (even one whose every field
// is its default) is what moves the fold — plus the fills, the book and the
// trade counts, which are unchanged from the pre-L4 tree.
constexpr std::uint64_t kNeutralSpecDigest = 2166775980498865536ULL;
constexpr std::uint64_t kNeutralRichSpecDigest = 17505314342075340318ULL;

Bar ohlc(int index, double open, double high, double low, double close,
         double volume = 1.0) {
    return {open, high, low, close, volume, T + static_cast<int64_t>(index) * 60000};
}

NativeRunSpec margin_spec(const char* key) {
    NativeRunSpec s;
    s.identity = {key, 1};
    s.input_tf = "1";
    s.script_tf = "1";
    s.tickerid = "TEST:MARGIN";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 1000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    s.close_execution = NativeCloseExecution::AfterCalculation;
    return s;
}

NativeMarginModel model(double initial_long, double initial_short,
                        std::optional<double> maintenance,
                        NativeLiquidationSizing sizing = NativeLiquidationSizing::RestoreMinimum,
                        double multiple = 1.0) {
    NativeMarginModel m;
    m.initial_long = initial_long;
    m.initial_short = initial_short;
    m.maintenance_long = maintenance;
    m.maintenance_short = maintenance;
    m.sizing = sizing;
    m.shortfall_multiple = multiple;
    return m;
}

// One host that opens a literal position on the first calculation and records
// every applied fill, every margin receipt, and their relative order.
struct MarginHost final : Host {
    double open_units = 0.0;
    std::function<void(MarginHost&)> bar_open;
    std::function<std::optional<double>(MarginHost&, const NativeMarginCallView&)> sizer;
    std::vector<std::string> order;
    std::vector<no::MarginCallEvent> margin_calls;
    std::vector<NativeMarginCallView> sizer_views;
    std::vector<std::optional<double>> level_at_bar_open;
    std::vector<std::optional<double>> level_at_calculation;
    int bars = 0;

    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override {
        level_at_bar_open.push_back(native_liquidation_price());
        if (bar_open) bar_open(*this);
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        Host::on_native_bar(bar, context);
        if (bars++ == 0 && open_units != 0.0) (void)put(*this, tx(open_units, "entry"));
        level_at_calculation.push_back(native_liquidation_price());
    }
    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        Host::on_native_applied(event, context);
        order.push_back("applied:" + std::to_string(event.ordinal));
    }
    void on_native_margin_call(const no::MarginCallEvent& event) override {
        order.push_back("margin:" + std::to_string(event.applied.ordinal));
        margin_calls.push_back(event);
    }
    std::optional<double> resolve_margin_call_units(
            const NativeMarginCallView& view) const override {
        auto& self = const_cast<MarginHost&>(*this);
        self.sizer_views.push_back(view);
        if (sizer) return sizer(self, view);
        return std::nullopt;
    }
};

void drive(MarginHost& host, const NativeRunSpec& s, const std::vector<Bar>& bars) {
    REQUIRE(host.configure_native(s).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
}

std::vector<no::ExecutionAppliedEvent> liquidations(const MarginHost& host) {
    std::vector<no::ExecutionAppliedEvent> out;
    for (const auto& row : events<no::ExecutionAppliedEvent>(host)) {
        if (row.request().label == kLiquidationLabel) out.push_back(row);
    }
    return out;
}

// The twin tape: a fully margined 20-unit long opened at 100, then a bar whose
// adverse extreme (95) breaches a 50 % requirement.
std::vector<Bar> twin_tape() {
    return {ohlc(0, 100.0, 100.0, 100.0, 100.0), ohlc(1, 100.0, 101.0, 95.0, 96.0)};
}

// ---------------------------------------------------------------- 1 + 10
// A spec with no margin model books exactly what it booked before L4, and
// hashes to the constant recorded from the pre-L4 tree. Every request in this
// run is RequestOrigin::Host, so authorship folds nothing into the digest.
void neutral_without_margin() {
    auto s = margin_spec("l4-neutral");
    s.initial_margin_fraction = 0.5;
    MarginHost host;
    host.open_units = 20.0;
    drive(host, s, twin_tape());

    const auto fills = events<no::ExecutionAppliedEvent>(host);
    CHECK(fills.size() == 1);
    if (!fills.empty()) {
        near(fills[0].resolved_price, 100.0);
        near(fills[0].opened_units, 20.0);
        CHECK(fills[0].request().label == "entry");
    }
    CHECK(liquidations(host).empty());
    CHECK(host.margin_calls.empty());
    near(host.physical_position().signed_units, 20.0);
    // The portable half of the pre-L4 neutrality: the run-spec fold this run
    // applies is pinned, stating `margin` as absent keeps it exactly there,
    // and declaring a model is what moves it.
    const auto digest = native_run_spec_digest(s);
    if (digest != kNeutralSpecDigest) {
        std::printf("  neutral spec digest %llu, pinned %llu\n",
                    static_cast<unsigned long long>(digest),
                    static_cast<unsigned long long>(kNeutralSpecDigest));
    }
    CHECK(digest == kNeutralSpecDigest);
    auto stated = s;
    stated.margin.reset();
    CHECK(native_run_spec_digest(stated) == kNeutralSpecDigest);
    // Presence is the opt-in: a model left at every default still moves the
    // fold, against the same spec without one (no initial_margin_fraction
    // here, which the model would conflict with).
    auto bare = margin_spec("l4-neutral");
    auto declared = bare;
    declared.margin = NativeMarginModel{};
    CHECK(native_run_spec_digest(declared) != native_run_spec_digest(bare));

    // The run-level half, compared between two runs in this process rather
    // than against a constant: the same margin-free spec with `margin` spelled
    // out as absent books the same fill and reaches the same continuation.
    MarginHost restated;
    restated.open_units = 20.0;
    drive(restated, stated, twin_tape());
    CHECK(events<no::ExecutionAppliedEvent>(restated).size() == 1);
    CHECK(liquidations(restated).empty());
    near(restated.physical_position().signed_units, 20.0);
    CHECK(restated.native_continuation_hash() == host.native_continuation_hash());
    // No maintenance fraction anywhere: the accessor stays silent.
    CHECK(!host.native_liquidation_price().has_value());
}

// A richer margin-free population — market, resting limit, resting stop,
// cancel, pyramided add, flatten, slippage, percent fee, lot cap — so every
// RequestOrigin::Host definition and every CommandEvent tag takes part in the
// digest, which stays where the pre-L4 tree left it: authorship folds nothing
// for a host request.
struct RichHost final : Host {
    int bars = 0;
    std::optional<no::RequestHandle> resting;
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        Host::on_native_bar(bar, context);
        const int index = bars++;
        if (index == 0) {
            (void)put(*this, tx(10.0, "entry"));
            no::Request limit = reduce(2.0, "take");
            limit.trigger = no::Limit{104.0, false};
            const auto out = submit(limit);
            if (out.handle) resting = *out.handle;
        } else if (index == 1) {
            no::Request stop = reduce(3.0, "protect");
            stop.trigger = no::Stop{97.0};
            (void)put(*this, stop);
        } else if (index == 2) {
            if (resting) (void)cancel(*resting);
            (void)put(*this, tx(4.0, "add"));
        } else if (index == 3) {
            (void)put(*this, flat("exit"));
        }
    }
};

void neutral_rich_population() {
    auto s = margin_spec("l4-neutral-rich");
    s.initial_capital = 100000.0;
    s.slippage_ticks = 2;
    s.fee_kind = NativeFeeKind::Percent;
    s.fee_value = 0.001;
    s.initial_margin_fraction = 0.25;
    s.max_open_lots = 4;
    RichHost host;
    REQUIRE(host.configure_native(s).status == NativeSetupStatus::Applied);
    const std::vector<Bar> bars = {
        ohlc(0, 100.0, 102.0, 99.0, 101.0, 5.0),
        ohlc(1, 101.0, 105.0, 96.0, 98.0, 7.0),
        ohlc(2, 98.0, 99.5, 93.0, 94.0, 3.0),
        ohlc(3, 94.0, 107.0, 94.0, 106.0, 9.0),
        ohlc(4, 106.0, 106.5, 103.0, 104.0, 2.0),
    };
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 4);
    near(host.physical_position().signed_units, 0.0);

    const auto digest = native_run_spec_digest(s);
    if (digest != kNeutralRichSpecDigest) {
        std::printf("  rich spec digest %llu, pinned %llu\n",
                    static_cast<unsigned long long>(digest),
                    static_cast<unsigned long long>(kNeutralRichSpecDigest));
    }
    CHECK(digest == kNeutralRichSpecDigest);
    auto stated = s;
    stated.margin.reset();
    CHECK(native_run_spec_digest(stated) == kNeutralRichSpecDigest);

    // In process, not against a constant: the same population under the same
    // spec with `margin` stated as absent books the same trades, the same flat
    // book and the same continuation identity.
    RichHost restated;
    REQUIRE(restated.configure_native(stated).status == NativeSetupStatus::Applied);
    restated.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(restated.last_error().empty());
    CHECK(restated.trade_count() == 4);
    near(restated.physical_position().signed_units, 0.0);
    CHECK(restated.native_continuation_hash() == host.native_continuation_hash());
}

// ------------------------------------------------------------------- 2
void per_side_initial_margin() {
    // initial_long 0.5 admits a 10-unit long (500 <= 1000); initial_short 2.0
    // refuses the same short (2000 > 1000).
    for (const double units : {10.0, -10.0}) {
        auto s = margin_spec(units > 0 ? "l4-init-long" : "l4-init-short");
        s.margin = model(0.5, 2.0, std::nullopt);
        MarginHost host;
        host.open_units = units;
        drive(host, s, twin_tape());
        const auto fills = events<no::ExecutionAppliedEvent>(host);
        const auto rejects = events<no::MatchRejectedEvent>(host);
        if (units > 0) {
            CHECK(fills.size() == 1);
            CHECK(rejects.empty());
            near(host.physical_position().signed_units, 10.0);
        } else {
            CHECK(fills.empty());
            CHECK(!rejects.empty());
            if (!rejects.empty()) {
                CHECK(rejects[0].reason == no::MatchRejectReason::InitialMargin);
            }
            near(host.physical_position().signed_units, 0.0);
        }
    }
}

// ------------------------------------------------------------------- 3
void maintenance_breach_sizes_by_policy() {
    // 20 @ 100, capital 1000, maintenance 0.5.
    //   L        = (1000 - 20*100) / (20 * (0.5 - 1))            = 100
    //   mark     = adverse extreme of {101, 95, 96}              = 95
    //   equity   = 1000 + 20 * (95 - 100)                        = 900
    //   required = 20 * 95 * 0.5                                 = 950
    //   restore  = (950 - 900) / (95 * 0.5)                      = 1.0526315789473684
    const double restore = 50.0 / 47.5;
    struct Case {
        const char* key;
        NativeLiquidationSizing sizing;
        double multiple;
        double units;
    };
    const Case cases[] = {
        {"l4-restore", NativeLiquidationSizing::RestoreMinimum, 1.0, restore},
        {"l4-mult1", NativeLiquidationSizing::ShortfallMultiple, 1.0, restore},
        {"l4-mult4", NativeLiquidationSizing::ShortfallMultiple, 4.0, 4.0 * restore},
        {"l4-flatten", NativeLiquidationSizing::Flatten, 1.0, 20.0},
    };
    for (const auto& row : cases) {
        auto s = margin_spec(row.key);
        s.margin = model(0.5, 0.5, 0.5, row.sizing, row.multiple);
        MarginHost host;
        host.open_units = 20.0;
        drive(host, s, twin_tape());
        const auto rows = liquidations(host);
        CHECK(rows.size() == 1);
        if (rows.size() != 1) continue;
        // The reduction fills AT the liquidation level, not at the extreme it
        // was sized against: the account runs out of margin at 100.
        near(rows[0].resolved_price, 100.0);
        near(rows[0].closed_units, row.units);
        near(host.physical_position().signed_units, 20.0 - row.units);
        CHECK(host.margin_calls.size() == 1);
        if (host.margin_calls.size() == 1) {
            const auto& call = host.margin_calls[0];
            CHECK(call.side == no::Side::Long);
            near(call.mark, 100.0);
            near(call.units, row.units);
            near(call.position_before, 20.0);
            near(call.position_after, 20.0 - row.units);
        }
    }
}

// ------------------------------------------------------------------- 4
void liquidation_price_accessor() {
    auto s = margin_spec("l4-level");
    s.margin = model(0.5, 0.5, 0.5);
    MarginHost host;
    host.open_units = 20.0;
    drive(host, s, twin_tape());
    // Bar 0 opens flat; bar 1 opens on the untouched 20-unit book.
    CHECK(host.level_at_bar_open.size() == 2);
    if (host.level_at_bar_open.size() == 2) {
        CHECK(!host.level_at_bar_open[0].has_value());
        REQUIRE(host.level_at_bar_open[1].has_value());
        near(*host.level_at_bar_open[1], 100.0);
    }
    // After the restore-minimum slice the book is 18.947368421052634 @ 100:
    //   L = (1000 - 18.947368421052634 * 100)
    //       / (18.947368421052634 * (0.5 - 1)) = 94.44444444444444
    const double left = 20.0 - 50.0 / 47.5;
    CHECK(host.level_at_calculation.size() == 2);
    if (host.level_at_calculation.size() == 2) {
        REQUIRE(host.level_at_calculation[1].has_value());
        near(*host.level_at_calculation[1], (1000.0 - left * 100.0) / (left * -0.5));
        CHECK(*host.level_at_calculation[1] < 100.0);
    }
}

// ------------------------------------------------------------------- 5
// A host fill between the arming and the trigger moves both the level and the
// units: the first kernel request is withdrawn under Superseded and exactly
// one kernel request is live at any moment.
void repriced_under_superseded() {
    auto s = margin_spec("l4-reprice");
    s.margin = model(0.5, 0.5, 0.5);
    MarginHost host;
    host.open_units = 20.0;
    host.bar_open = [](MarginHost& self) {
        if (self.bars != 1) return;   // only the bar after the entry
        const auto handle = put(self, reduce(1.0, "host-trim"));
        (void)handle;
    };
    drive(host, s, {ohlc(0, 100.0, 100.0, 100.0, 100.0), ohlc(1, 100.0, 101.0, 90.0, 91.0)});

    std::size_t accepted = 0;
    std::size_t terminal = 0;
    std::size_t superseded = 0;
    std::size_t peak_live = 0;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* e = std::get_if<no::AcceptedEvent>(&*row.command)) {
            if (e->request().label != kLiquidationLabel) continue;
            ++accepted;
            peak_live = std::max(peak_live, accepted - terminal);
        } else if (const auto* e = std::get_if<no::CancelledEvent>(&*row.command)) {
            if (e->request().label != kLiquidationLabel) continue;
            ++terminal;
            if (e->reason == no::CancelReason::Superseded) ++superseded;
        } else if (const auto* e = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
            if (e->request().label != kLiquidationLabel || !e->terminal) continue;
            ++terminal;
        }
    }
    CHECK(accepted == 2);
    CHECK(superseded == 1);
    CHECK(peak_live == 1);
    // The surviving request is the re-priced one: 19 @ 100 breaches at 90 by
    // 19*90*0.5 - (1000 - 190) = 855 - 810 = 45, restoring 45/(90*0.5) = 1.0
    // unit at L = (1000 - 1900) / (19 * -0.5) = 94.73684210526316.
    const auto rows = liquidations(host);
    CHECK(rows.size() == 1);
    if (rows.size() == 1) {
        near(rows[0].resolved_price, (1000.0 - 1900.0) / (19.0 * -0.5));
        near(rows[0].closed_units, 1.0);
    }
}

// ------------------------------------------------------------------- 6
void host_override_wins() {
    auto s = margin_spec("l4-override");
    s.margin = model(0.5, 0.5, 0.5, NativeLiquidationSizing::ShortfallMultiple, 4.0);
    MarginHost host;
    host.open_units = 20.0;
    host.sizer = [](MarginHost&, const NativeMarginCallView& view) -> std::optional<double> {
        near(view.position.signed_units, 20.0);
        near(view.mark, 95.0);
        near(view.equity, 900.0);
        near(view.required, 950.0);
        return 7.0;
    };
    drive(host, s, twin_tape());
    const auto rows = liquidations(host);
    CHECK(rows.size() == 1);
    if (rows.size() == 1) near(rows[0].closed_units, 7.0);
    near(host.physical_position().signed_units, 13.0);
    CHECK(!host.sizer_views.empty());
}

// ------------------------------------------------------------------- 7
void margin_call_follows_applied() {
    auto s = margin_spec("l4-order");
    s.margin = model(0.5, 0.5, 0.5, NativeLiquidationSizing::Flatten);
    MarginHost host;
    host.open_units = 20.0;
    drive(host, s, twin_tape());
    const auto rows = liquidations(host);
    REQUIRE(rows.size() == 1);
    REQUIRE(host.margin_calls.size() == 1);
    const auto applied_tag = "applied:" + std::to_string(rows[0].ordinal);
    const auto margin_tag = "margin:" + std::to_string(rows[0].ordinal);
    std::size_t applied_at = host.order.size();
    std::size_t margin_at = host.order.size();
    for (std::size_t i = 0; i < host.order.size(); ++i) {
        if (host.order[i] == applied_tag) applied_at = i;
        if (host.order[i] == margin_tag) margin_at = i;
    }
    CHECK(applied_at < host.order.size());
    CHECK(margin_at == applied_at + 1);
    CHECK(host.margin_calls[0].cursor == rows[0].cursor);
    CHECK(host.margin_calls[0].applied.ordinal == rows[0].ordinal);
}

// ------------------------------------------------------------------- 8
void calculation_only_never_fills_mid_path() {
    auto s = margin_spec("l4-calc-only");
    auto m = model(0.5, 0.5, 0.5);
    m.check = NativeLiquidationCheck::CalculationOnly;
    s.margin = m;
    MarginHost host;
    host.open_units = 20.0;
    drive(host, s, twin_tape());
    const auto rows = liquidations(host);
    CHECK(rows.size() == 1);
    if (rows.size() == 1) {
        // The calculation mark is bar 1's close, not the level and not the
        // path's adverse extreme:
        //   equity(96) = 920, required = 20*96*0.5 = 960,
        //   restore    = 40 / (96 * 0.5) = 0.8333333333333334
        near(rows[0].resolved_price, 96.0);
        near(rows[0].closed_units, 40.0 / 48.0);
        CHECK(rows[0].cursor.point.provenance == NativePriceProvenance::CurrentExecution);
    }
    // Nothing rested: no kernel request was ever cancelled or left working.
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* e = std::get_if<no::AcceptedEvent>(&*row.command)) {
            if (e->request().label != kLiquidationLabel) continue;
            CHECK(std::holds_alternative<no::Market>(e->request().trigger));
        }
    }
}

// ------------------------------------------------------------------- 9
void mutually_exclusive_configuration() {
    auto s = margin_spec("l4-conflict");
    s.initial_margin_fraction = 0.5;
    s.margin = model(0.5, 0.5, 0.5);
    MarginHost host;
    const auto setup = host.configure_native(s);
    CHECK(setup.status == NativeSetupStatus::Failed);
    CHECK(setup.validation.error == NativeRunSpecError::MarginModelConflict);
    CHECK(setup.validation.field == NativeRunSpecField::MarginModel);

    // Each spelling alone stays valid.
    auto only_scalar = margin_spec("l4-scalar");
    only_scalar.initial_margin_fraction = 0.5;
    MarginHost a;
    CHECK(a.configure_native(only_scalar).status == NativeSetupStatus::Applied);
    auto only_model = margin_spec("l4-model");
    only_model.margin = model(0.5, 0.5, 0.5);
    MarginHost b;
    CHECK(b.configure_native(only_model).status == NativeSetupStatus::Applied);

    // A margin model with a nonpositive per-side fraction is refused.
    auto bad = margin_spec("l4-bad");
    auto bad_model = model(0.5, 0.5, std::nullopt);
    bad_model.initial_long = 0.0;
    bad.margin = bad_model;
    MarginHost c;
    const auto refused = c.configure_native(bad);
    CHECK(refused.status == NativeSetupStatus::Failed);
    CHECK(refused.validation.error == NativeRunSpecError::NotFinitePositive);
    CHECK(refused.validation.field == NativeRunSpecField::MarginInitial);
}

// ------------------------------------------------------------------ TWIN
// tests/test_margin_call_l4a.cpp::test_leveraged_long_adverse_low drives the
// SAME tape through the Pine adapter with margin_long = 50 %: 20 units opened
// at 100 on a process-orders-on-close bar, then O100 H101 L95 C96. The adapter
// books 4.2105263157894735 units of "Margin call" at 95.
//
// The native model with initial = maintenance = 0.5 and ShortfallMultiple 4.0
// liquidates on the SAME bar, on the same side, for the SAME units. The only
// difference is the booked price, and it is a named TV quirk (MG15, fill
// pricing pine_adapter.cpp:11588-11597): the adapter pins its slice to the
// bar's adverse extreme it sized against, while the generic kernel rests the
// reduction at the liquidation level and books it where the account actually
// runs out of margin.
void twin_of_adapter_margin_call() {
    auto s = margin_spec("l4-twin");
    s.margin = model(0.5, 0.5, 0.5, NativeLiquidationSizing::ShortfallMultiple, 4.0);
    MarginHost host;
    host.open_units = 20.0;
    drive(host, s, twin_tape());
    const auto rows = liquidations(host);
    REQUIRE(rows.size() == 1);
    // Same bar as the adapter's row (entry bar + 1).
    CHECK(rows[0].cursor.point.interval_index == 1);
    // Same units, to the adapter's own 1e-6 tolerance and beyond.
    near(rows[0].closed_units, 4.2105263157894735);
    near(host.physical_position().signed_units, 15.789473684210526);
    // Itemized difference: adapter 95.0 (adverse-extreme fill pricing, MG15),
    // kernel 100.0 (the liquidation level).
    near(rows[0].resolved_price, 100.0);
    CHECK(host.margin_calls.size() == 1);
    if (host.margin_calls.size() == 1) near(host.margin_calls[0].mark, 100.0);
}

}  // namespace

int main() {
    test("neutral-without-margin", neutral_without_margin);
    test("neutral-rich-population", neutral_rich_population);
    test("per-side-initial-margin", per_side_initial_margin);
    test("maintenance-breach-sizing", maintenance_breach_sizes_by_policy);
    test("liquidation-price", liquidation_price_accessor);
    test("repriced-superseded", repriced_under_superseded);
    test("host-override", host_override_wins);
    test("margin-call-order", margin_call_follows_applied);
    test("calculation-only", calculation_only_never_fills_mid_path);
    test("configuration-conflict", mutually_exclusive_configuration);
    test("twin-adapter-margin-call", twin_of_adapter_margin_call);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
