// Actual native/source settlement boundaries. No run(), tape or copied observer.
#include <pineforge/engine.hpp>
#include <pineforge/execution_projection.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pineforge;
namespace x = pineforge::execution;
namespace {
int checks = 0, failures = 0;
const char* scenario = "setup";
struct Abort {};
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::printf("FAIL %s:%d %s\n", scenario, __LINE__, #value); } } while (0)
#define REQUIRE(value) do { ++checks; if (!(value)) { ++failures; \
    std::printf("FAIL %s:%d %s\n", scenario, __LINE__, #value); throw Abort{}; } } while (0)

enum class Seam { NativeBook, NativeOpening, NativeSelected, NativeReverse, Context,
                  SourceBook, SourceSelected, SourceReverse };
constexpr Seam natives[] = {Seam::NativeBook, Seam::NativeOpening, Seam::NativeSelected,
                            Seam::NativeReverse, Seam::Context};
constexpr Seam sources[] = {Seam::SourceBook, Seam::SourceSelected, Seam::SourceReverse};
bool reversal(Seam s) { return s == Seam::NativeReverse || s == Seam::SourceReverse; }
bool selected(Seam s) { return s == Seam::NativeSelected || s == Seam::SourceSelected; }
bool source(Seam s) { return s == Seam::SourceBook || s == Seam::SourceSelected || s == Seam::SourceReverse; }
Seam native_peer(Seam s) {
    return s == Seam::SourceReverse ? Seam::NativeReverse
        : s == Seam::SourceSelected ? Seam::NativeSelected : Seam::NativeBook;
}
uint64_t bits(double v) { uint64_t u; std::memcpy(&u, &v, sizeof(u)); return u; }
void exact(double actual, double expected) {
    if (bits(actual) != bits(expected))
        std::printf(" actual=%.17g expected=%.17g\n", actual, expected);
    CHECK(bits(actual) == bits(expected));
}
void near(double actual, double expected) {
    const bool ok = std::isfinite(actual) && std::isfinite(expected)
        && std::abs(actual - expected) <= 1e-12 * std::max(1.0, std::abs(expected));
    if (!ok) std::printf(" actual=%.17g expected=%.17g\n", actual, expected);
    CHECK(ok);
}
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double huge = std::numeric_limits<double>::max();
constexpr int maximum = std::numeric_limits<int>::max();
constexpr int64_t chart_time = 1743436800000LL; // 2025-04-01 00:00 Taipei, day104
x::Fill fill(double price = 100, double fee = 0) { return {price, "effect", "literal", 90, fee}; }

struct Book final : BacktestEngine {
    std::vector<uint64_t> members{11};
    double target = -.1;
    x::PhysicalExecutionContext native_context{999000, 9, {}, {}};
    Book() {
        initial_capital_ = 1000;
        commission_type_ = CommissionType::CASH_PER_ORDER;
        commission_value_ = 0;
        syminfo_.pointvalue = 1;
        account_currency_fx_ = 1;
        stream_observe_actions_ = true;
        clock(chart_time, "Asia/Taipei");
        bar_index_ = 7;
    }
    void on_bar(const Bar&) override {}
    void clock(int64_t time, const char* timezone) {
        current_bar_ = {100, 100, 100, 100, 1, time};
        set_chart_timezone(timezone);
    }
    void open(double price = 100, uint64_t id = 11, double quantity = 1, double paid = 0) {
        REQUIRE(settle_native_execution_at(order_action::Transact{quantity},
            x::Fill{price, "seed", "historical", id, paid}, native_context).status == x::Status::Applied);
    }
    x::Result effect(Seam s, const x::Action& a, const x::Fill& f,
                     const x::LifecycleEffects& lifecycle = {}) {
        switch (s) {
        case Seam::NativeBook: return settle_native_execution_at(a, f, native_context);
        case Seam::NativeOpening: return settle_native_execution_scoped_at(a, f, native_context, x::OpeningExposure{11, cycle()});
        case Seam::NativeSelected: return settle_native_execution_selected_at(a, f, native_context, x::SelectedOpeningSet{cycle(), members});
        case Seam::NativeReverse: return settle_native_reversal_at_v1(x::ReverseTo{target}, f, native_context);
        case Seam::Context: return settle_with_context(a, f, lifecycle, native_context);
        case Seam::SourceBook: return settle_execution_with_lifecycle(a, f, lifecycle);
        case Seam::SourceSelected: return settle_execution_selected_with_lifecycle(a, f, lifecycle, x::SelectedOpeningSet{cycle(), members});
        case Seam::SourceReverse: return settle_reversal_with_lifecycle_v1(x::ReverseTo{target}, f, lifecycle);
        }
        throw std::logic_error("unreachable seam");
    }
    x::SettlementInspection inspect(Seam s, const x::Action& a, const x::Fill& f) const {
        if (reversal(s)) return inspect_native_reversal_v1(x::ReverseTo{target}, f);
        if (selected(s)) return inspect_native_settlement_selected(a, f, x::SelectedOpeningSet{cycle(), members});
        if (s == Seam::NativeOpening) return inspect_native_settlement_scoped(a, f, x::OpeningExposure{11, cycle()});
        return inspect_native_settlement(a, f);
    }
    x::AccountEffectProjection project(Seam s, const x::Action& a, const x::Fill& f) const {
        if (reversal(s)) return project_native_reversal_v1(x::ReverseTo{target}, f);
        if (selected(s)) return project_native_settlement_selected_v1(a, f, x::SelectedOpeningSet{cycle(), members});
        if (s == Seam::NativeOpening) return project_native_settlement_scoped_v1(a, f, x::OpeningExposure{11, cycle()});
        return project_native_settlement_v1(a, f);
    }
    void fields(int days, int last, double pnl, int unused = 42) {
        cons_loss_day_count_ = days; last_loss_day_ = last; intraday_pnl_ = pnl; intraday_pnl_day_ = unused;
    }
    int days() const { return cons_loss_day_count_; }
    int last_day() const { return last_loss_day_; }
    double intraday() const { return intraday_pnl_; }
    int unused_day() const { return intraday_pnl_day_; }
    int64_t cycle() const { return position_cycle_seq_; }
    int64_t next_cycle() const { return next_position_cycle_seq_; }
    void exhaust_cycle() { next_position_cycle_seq_ = std::numeric_limits<int64_t>::max(); }
    void exhaust_stream() { stream_action_sequence_ = UINT64_MAX; }
    void financial_counter(int kind, int remaining = 0) {
        if (kind == 0) win_trades_count_ = maximum - remaining;
        if (kind == 1) loss_trades_count_ = maximum - remaining;
        if (kind == 2) eventrades_count_ = maximum - remaining;
    }
    void net(double value) { net_profit_sum_ = value; }
    double net() const { return net_profit_sum_; }
    double marked(double price) const { return marked_equity(price); }
    double gross_profit() const { return gross_profit_sum_; }
    double gross_loss() const { return gross_loss_sum_; }
    int wins() const { return win_trades_count_; }
    int losses() const { return loss_trades_count_; }
    int evens() const { return eventrades_count_; }
    const auto& rows() const { return trades_; }
    const auto& lots() const { return pyramid_entries_; }
    size_t actions() const { return stream_order_actions_.size(); }
    uint64_t stream_sequence() const { return stream_action_sequence_; }
    uint64_t lifecycle_sequence() const { return exit_leg_event_seq_; }
    uint64_t next_order() const { return next_order_incarnation_; }
    void pending_exit() {
        PendingOrder order;
        order.id = "pending"; order.type = OrderType::EXIT; order.incarnation = 700; order.created_seq = 700;
        order.legs.attach(order.incarnation, position_cycle_seq_);
        pending_orders_.push_back(std::move(order));
    }
    size_t pending() const { return pending_orders_.size(); }
    const PendingOrder* pending_data() const { return pending_orders_.data(); }
};
struct Fields {
    int days, last, unused; uint64_t intraday;
    explicit Fields(const Book& b) : days(b.days()), last(b.last_day()), unused(b.unused_day()), intraday(bits(b.intraday())) {}
    void unchanged(const Book& b) const {
        CHECK(b.days() == days && b.last_day() == last && b.unused_day() == unused);
        CHECK(bits(b.intraday()) == intraday);
    }
};
struct Snapshot {
    Fields fields;
    uint64_t broker, stream, order, stream_sequence, lifecycle;
    int64_t cycle, next_cycle;
    size_t rows, lots, actions, pending;
    const PendingOrder* pending_data;
    double net, profit, loss;
    int wins, losses, evens;
    explicit Snapshot(const Book& b) : fields(b), broker(b.broker_state_hash()), stream(b.stream_state_hash()),
        order(b.next_order()), stream_sequence(b.stream_sequence()), lifecycle(b.lifecycle_sequence()),
        cycle(b.cycle()), next_cycle(b.next_cycle()), rows(b.rows().size()), lots(b.lots().size()),
        actions(b.actions()), pending(b.pending()), pending_data(b.pending_data()), net(b.net()),
        profit(b.gross_profit()), loss(b.gross_loss()), wins(b.wins()), losses(b.losses()), evens(b.evens()) {}
    void unchanged(const Book& b) const {
        fields.unchanged(b);
        CHECK(b.broker_state_hash() == broker && b.stream_state_hash() == stream);
        CHECK(b.next_order() == order && b.stream_sequence() == stream_sequence && b.lifecycle_sequence() == lifecycle);
        CHECK(b.cycle() == cycle && b.next_cycle() == next_cycle);
        CHECK(b.rows().size() == rows && b.lots().size() == lots && b.actions() == actions);
        CHECK(b.pending() == pending && b.pending_data() == pending_data);
        CHECK(b.wins() == wins && b.losses() == losses && b.evens() == evens);
        exact(b.net(), net); exact(b.gross_profit(), profit); exact(b.gross_loss(), loss);
    }
};
void seed(Book& b, Seam s, double price = 100) {
    b.open(price);
    if (selected(s) || s == Seam::NativeOpening) b.open(price, 99);
}
void quote_is_read_only(Book& b, Seam s, const x::Action& a, const x::Fill& f,
                        x::Status expected = x::Status::Applied) {
    const Snapshot before(b);
    const auto inspect = b.inspect(s, a, f);
    const auto project = b.project(s, a, f);
    CHECK(inspect.status == expected && project.status == expected);
    before.unchanged(b);
    if (expected == x::Status::Applied && reversal(s)) {
        exact(inspect.opened_units, b.target); exact(project.opened_units, b.target);
        exact(project.signed_units_after, b.target);
        CHECK(project.cycle_after == b.next_cycle());
    }
}
void status_atomic(Book& b, Seam s, const x::Action& a, const x::Fill& f, x::Status status,
                   const x::LifecycleEffects& lifecycle = {}) {
    const Snapshot before(b);
    const auto r = b.effect(s, a, f, lifecycle);
    CHECK(r.status == status);
    CHECK(r.closed_units == 0 && r.opened_units == 0 && r.current_ticket == 0);
    CHECK(r.first_trade_index == 0 && r.closed_trade_count == 0 && r.opened_lot_incarnation == 0);
    before.unchanged(b);
}
void overflow_atomic(Book& b, Seam s, const x::Fill& f, const char* message) {
    const Snapshot before(b); bool threw = false;
    try { (void)b.effect(s, x::Flatten{}, f); }
    catch (const std::overflow_error& e) { threw = true; CHECK(std::string(e.what()) == message); }
    CHECK(threw); before.unchanged(b);
}

void native_inert(Seam s, bool default_context, bool nan_intraday) {
    scenario = "all native/context routes ignore exhausted or poisoned source state";
    Book b; seed(b, s);
    if (default_context) b.native_context = {};
    b.fields(nan_intraday ? 2 : maximum, -1, nan_intraday ? nan : 17.25);
    const Fields fields(b); const auto next = b.next_cycle();
    quote_is_read_only(b, s, x::Flatten{}, fill(90));
    const auto r = b.effect(s, x::Flatten{}, fill(90));
    REQUIRE(r.status == x::Status::Applied && r.closed_trade_count == 1);
    fields.unchanged(b); exact(r.closed_units, 1); exact(b.rows().back().pnl, -10);
    CHECK(b.rows().back().exit_time == b.native_context.effective_time_ms);
    CHECK(b.rows().back().exit_bar_index == b.native_context.interval_index);
    if (reversal(s)) { REQUIRE(b.lots().size() == 1); exact(b.lots()[0].qty, .1);
        CHECK(b.cycle() == next && b.next_cycle() == next + 1); }
    else if (selected(s) || s == Seam::NativeOpening) {
        REQUIRE(b.lots().size() == 1); CHECK(b.lots()[0].entry_incarnation == 99); exact(b.lots()[0].qty, 1);
    } else CHECK(b.lots().empty());
}

void giant_intraday(Seam s) {
    scenario = "finite source intraday overflow is not native cash overflow";
    Book b; seed(b, s, 0); b.fields(2, -1, huge);
    quote_is_read_only(b, s, x::Flatten{}, fill(huge / 2));
    if (source(s)) status_atomic(b, s, x::Flatten{}, fill(huge / 2), x::Status::InvalidAccounting);
    else {
        const Fields fields(b); const auto r = b.effect(s, x::Flatten{}, fill(huge / 2));
        REQUIRE(r.status == x::Status::Applied); fields.unchanged(b);
        exact(b.rows()[0].pnl, huge / 2); CHECK(std::isfinite(b.net()));
    }
}

void opening_and_noeffect() {
    scenario = "Ready opening-only separates source finite-state preflight from native";
    for (Seam s : {Seam::NativeBook, Seam::Context, Seam::SourceBook}) {
        Book b; b.fields(maximum, -1, nan);
        const Fields fields(b); const auto f = fill(100, 6);
        quote_is_read_only(b, s, order_action::Transact{1}, f);
        if (source(s)) status_atomic(b, s, order_action::Transact{1}, f, x::Status::InvalidAccounting);
        else {
            const auto r = b.effect(s, order_action::Transact{1}, f);
            REQUIRE(r.status == x::Status::Applied && b.lots().size() == 1 && b.rows().empty());
            fields.unchanged(b); exact(b.lots()[0].entry_commission_account, 6); exact(b.marked(100), 994);
        }
    }
    Book finite; finite.fields(maximum, -1, huge); const Fields fields(finite);
    REQUIRE(finite.effect(Seam::SourceBook, order_action::Transact{1}, fill()).status == x::Status::Applied);
    fields.unchanged(finite); CHECK(finite.rows().empty());
    scenario = "NoEffect skips source preflight observer and late stream checks";
    for (Seam s : {Seam::NativeBook, Seam::NativeOpening, Seam::NativeSelected, Seam::Context,
                   Seam::SourceBook, Seam::SourceSelected}) {
        Book b; seed(b, s); b.fields(maximum, -1, nan); b.exhaust_stream();
        quote_is_read_only(b, s, order_action::Reduce{0}, fill(), x::Status::NoEffect);
        status_atomic(b, s, order_action::Reduce{0}, fill(), x::Status::NoEffect);
    }
    Book flat; flat.fields(maximum, -1, nan); flat.exhaust_stream();
    status_atomic(flat, Seam::SourceBook, x::Flatten{}, fill(), x::Status::NoEffect);
}

void source_failures(Seam s) {
    scenario = "all three source wrappers retain pre-mutation source refusal";
    Book day; seed(day, s); day.pending_exit(); day.fields(maximum, -1, 17.25);
    quote_is_read_only(day, s, x::Flatten{}, fill(90));
    overflow_atomic(day, s, fill(90), "closed trade counter exhausted");
    Book poison; seed(poison, s); poison.fields(maximum, -1, nan);
    status_atomic(poison, s, x::Flatten{}, fill(90), x::Status::InvalidAccounting);
    giant_intraday(s);
    scenario = "full intraday pass wins over first-row day overflow";
    Book two; two.open(1, 11); two.open(-huge / 2, 12); two.members = {11, 12};
    if (selected(s)) two.open(100, 99);
    two.fields(maximum, -1, huge);
    quote_is_read_only(two, s, x::Flatten{}, fill(0));
    status_atomic(two, s, x::Flatten{}, fill(0), x::Status::InvalidAccounting);
    scenario = "source intraday InvalidAccounting precedes financial counter exception";
    Book cash; seed(cash, s); cash.fields(2, -1, nan); cash.financial_counter(0);
    status_atomic(cash, s, x::Flatten{}, fill(110), x::Status::InvalidAccounting);
    Book native_cash; seed(native_cash, native_peer(s)); native_cash.fields(2, -1, nan); native_cash.financial_counter(0);
    quote_is_read_only(native_cash, native_peer(s), x::Flatten{}, fill(110));
    overflow_atomic(native_cash, native_peer(s), fill(110), "closed trade counter exhausted");
    scenario = "source day overflow precedes stream while native reaches stream";
    Book late; seed(late, s); late.fields(maximum, -1, 17.25); late.exhaust_stream();
    overflow_atomic(late, s, fill(90), "closed trade counter exhausted");
    Book native_late; seed(native_late, native_peer(s)); native_late.fields(maximum, -1, 17.25); native_late.exhaust_stream();
    overflow_atomic(native_late, native_peer(s), fill(90), "stream action sequence overflow");
    scenario = "stage invalid lifecycle precedes source poison";
    Book invalid; seed(invalid, s); invalid.fields(maximum, -1, nan);
    x::LifecycleEffects lifecycle; lifecycle.removals.push_back({999, 999, {}, 0});
    status_atomic(invalid, s, x::Flatten{}, fill(), x::Status::InvalidLifecycle, lifecycle);
}

void cycle_and_generic_cash_precedence() {
    scenario = "cycle exhaustion stays before source intraday finite validation";
    for (Seam s : {Seam::NativeReverse, Seam::SourceReverse}) {
        Book b; seed(b, s); b.fields(maximum, -1, nan); b.exhaust_cycle();
        overflow_atomic(b, s, fill(), "position cycle sequence exhausted");
    }
    scenario = "generic cash overflow stays before source day counter";
    for (Seam s : {Seam::NativeBook, Seam::SourceBook}) {
        Book b; b.open(huge / 2); b.net(-huge); b.fields(maximum, -1, 0);
        status_atomic(b, s, x::Flatten{}, fill(0), x::Status::InvalidAccounting);
    }
}

void source_row_order(Seam s) {
    scenario = "same-day source loss cannot overflow the existing day count";
    Book same; seed(same, s); same.fields(maximum, 104, 17.25);
    REQUIRE(same.effect(s, x::Flatten{}, fill(90)).status == x::Status::Applied);
    CHECK(same.days() == maximum && same.last_day() == 104); exact(same.intraday(), 7.25);
    scenario = "source committed loss zero win rows keep exact observation order";
    Book mixed; mixed.open(110, 11); mixed.open(100, 12); mixed.open(90, 13); mixed.members = {11, 12, 13};
    if (selected(s)) mixed.open(100, 99);
    mixed.fields(4, -1, 17.25);
    const auto r = mixed.effect(s, x::Flatten{}, fill());
    REQUIRE(r.status == x::Status::Applied && r.closed_trade_count == 3);
    CHECK(mixed.days() == 0 && mixed.last_day() == 104 && mixed.unused_day() == 42);
    exact(mixed.intraday(), 17.25); exact(mixed.net(), 0);
    CHECK(mixed.wins() == 1 && mixed.losses() == 1 && mixed.evens() == 1);
    for (const auto& row : mixed.rows()) {
        CHECK(row.exit_time == chart_time && row.exit_bar_index == 7);
        CHECK(row.exit_id == "effect" && row.exit_comment == "literal");
    }
    if (reversal(s)) {
        REQUIRE(mixed.lots().size() == 1); exact(mixed.lots()[0].qty, .1);
        CHECK(mixed.lots()[0].time == chart_time && mixed.lots()[0].entry_bar_index == 7);
    }
    scenario = "win preserves last-loss-day through a second same-day loss";
    Book quirk; quirk.open(110, 11); quirk.open(90, 12); quirk.open(110, 13); quirk.members = {11, 12, 13};
    if (selected(s)) quirk.open(100, 99);
    quirk.fields(4, -1, 17.25);
    REQUIRE(quirk.effect(s, x::Flatten{}, fill()).status == x::Status::Applied);
    CHECK(quirk.days() == 0 && quirk.last_day() == 104); exact(quirk.intraday(), 7.25);
    scenario = "winning first row frees exhausted source day count before loss";
    Book recovery; recovery.open(90, 11); recovery.open(110, 12); recovery.members = {11, 12};
    if (selected(s)) recovery.open(100, 99);
    recovery.fields(maximum, -1, 17.25);
    REQUIRE(recovery.effect(s, x::Flatten{}, fill()).status == x::Status::Applied);
    CHECK(recovery.days() == 1 && recovery.last_day() == 104); exact(recovery.intraday(), 17.25);
    scenario = "later winning row cannot erase earlier source preflight overflow";
    Book refusal; refusal.open(110, 11); refusal.open(90, 12); refusal.members = {11, 12};
    if (selected(s)) refusal.open(100, 99);
    refusal.fields(maximum, -1, 17.25);
    overflow_atomic(refusal, s, fill(), "closed trade counter exhausted");
    scenario = "zero PnL source row changes neither day identity nor intraday value";
    Book zero; seed(zero, s); zero.fields(4, 3103, 17.25); const Fields fields(zero);
    REQUIRE(zero.effect(s, x::Flatten{}, fill()).status == x::Status::Applied);
    fields.unchanged(zero); CHECK(zero.evens() == 1);
}

void committed_slice(Seam s) {
    scenario = "source observer consumes only newly committed rows once";
    Book b; b.open(100, 1);
    REQUIRE(b.effect(Seam::NativeBook, x::Flatten{}, fill(107)).status == x::Status::Applied);
    b.open(101, 11); b.open(102, 12); b.members = {11, 12};
    if (selected(s)) b.open(100, 99);
    b.fields(1, 3103, 100); const auto actions = b.actions();
    const auto r = b.effect(s, x::Flatten{}, fill());
    REQUIRE(r.status == x::Status::Applied && r.first_trade_index == 1 && r.closed_trade_count == 2);
    CHECK(b.rows().size() == 3 && b.actions() == actions + 2 + (reversal(s) ? 1 : 0));
    exact(b.rows()[1].pnl, -1); exact(b.rows()[2].pnl, -2); exact(b.net(), 4);
    exact(b.intraday(), 97); CHECK(b.days() == 2 && b.last_day() == 104);
}

void generic_counter_guards() {
    scenario = "financial win loss even counters remain generic and projections ignore commit capacity";
    for (Seam s : {Seam::NativeBook, Seam::NativeReverse, Seam::SourceBook, Seam::SourceReverse}) {
        for (int kind : {0, 1, 2}) {
            Book b; seed(b, s); b.fields(2, 104, source(s) ? 17.25 : nan); b.financial_counter(kind);
            const auto f = fill(kind == 0 ? 110 : kind == 1 ? 90 : 100);
            quote_is_read_only(b, s, x::Flatten{}, f);
            overflow_atomic(b, s, f, "closed trade counter exhausted");
        }
        Book batch; batch.open(100, 11); batch.open(100, 12); batch.financial_counter(0, 1);
        overflow_atomic(batch, s, fill(110), "closed trade counter exhausted");
    }
}

void invalid_projections_and_source_stage() {
    scenario = "invalid native projections remain zeroed and never trigger source validation";
    for (Seam s : {Seam::NativeBook, Seam::NativeOpening, Seam::NativeSelected, Seam::NativeReverse,
                   Seam::Context, Seam::SourceBook, Seam::SourceSelected, Seam::SourceReverse}) {
        Book b; seed(b, s); b.fields(maximum, -1, nan); const Snapshot before(b);
        const auto p = b.project(s, x::Flatten{}, fill(nan));
        CHECK(p.status == x::Status::InvalidPrice);
        CHECK(p.closed_units == 0 && p.opened_units == 0 && p.current_ticket == 0 && p.resulting_lot_count == 0);
        CHECK(p.realized_balance == 0 && p.remaining_entry_cost == 0 && p.marked_equity == 0 && p.cycle_after == 0);
        before.unchanged(b);
        status_atomic(b, s, x::Flatten{}, fill(nan), x::Status::InvalidPrice);
    }
    for (Seam s : {Seam::NativeReverse, Seam::SourceReverse}) {
        Book flat; flat.fields(maximum, -1, nan);
        status_atomic(flat, s, x::Flatten{}, fill(), x::Status::InvalidCloseTarget);
        Book zero; seed(zero, s); zero.fields(maximum, -1, nan); zero.target = 0;
        status_atomic(zero, s, x::Flatten{}, fill(), x::Status::InvalidQuantity);
    }
    Book missing; seed(missing, Seam::SourceSelected); missing.fields(maximum, -1, nan); missing.members = {777};
    status_atomic(missing, Seam::SourceSelected, x::Flatten{}, fill(), x::Status::InvalidCloseTarget);
}

void one_fee_one_owner() {
    scenario = "native and source selected close share one fee and one financial commit";
    Book native, legacy;
    for (Book* b : {&native, &legacy}) {
        b->open(100, 11, 1, 2); b->open(100, 12, 3, 6); b->open(100, 99, 1, 17);
        b->members = {11, 12}; b->fields(4, -1, 17.25);
    }
    const Fields fields(native);
    const auto n = native.effect(Seam::NativeSelected, x::Flatten{}, fill(110, 6));
    const auto l = legacy.effect(Seam::SourceSelected, x::Flatten{}, fill(110, 6));
    REQUIRE(n.status == x::Status::Applied && l.status == x::Status::Applied);
    REQUIRE(native.rows().size() == 2 && legacy.rows().size() == 2);
    exact(n.current_ticket, 6); exact(l.current_ticket, 6); exact(native.net(), 26); exact(legacy.net(), 26);
    for (size_t i = 0; i < 2; ++i) {
        exact(native.rows()[i].qty, legacy.rows()[i].qty);
        exact(native.rows()[i].pnl, legacy.rows()[i].pnl);
        exact(native.rows()[i].commission, legacy.rows()[i].commission);
    }
    near(legacy.rows()[0].commission + legacy.rows()[1].commission, 14);
    REQUIRE(native.lots().size() == 1 && legacy.lots().size() == 1);
    CHECK(native.lots()[0].entry_incarnation == 99 && legacy.lots()[0].entry_incarnation == 99);
    exact(native.lots()[0].entry_commission_account, 17); exact(legacy.lots()[0].entry_commission_account, 17);
    fields.unchanged(native); exact(legacy.intraday(), 43.25); CHECK(legacy.days() == 0 && legacy.last_day() == -1);
}

void source_hash_domains() {
    scenario = "native isolation retains source trio and unused-day fingerprint domains";
    Book b; b.fields(2, 3103, 17.25); const auto original = b.broker_state_hash();
    for (int field : {0, 1, 2, 3}) {
        b.fields(field == 0 ? 3 : 2, field == 1 ? 104 : 3103, field == 2 ? 18.25 : 17.25, field == 3 ? 43 : 42);
        CHECK(b.broker_state_hash() != original);
        b.fields(2, 3103, 17.25); CHECK(b.broker_state_hash() == original);
    }
}

struct DayPoint { int64_t utc_ms; int key; int count; };
void calendar_pair(const char* zone, const std::vector<DayPoint>& points) {
    scenario = "production source chart day and DST rules remain separate from native closes";
    Book legacy, native; legacy.fields(0, -1, 0); native.fields(8, 777, 23.5); const Fields fields(native);
    size_t index = 0;
    for (const auto& point : points) {
        legacy.clock(point.utc_ms, zone); native.clock(point.utc_ms, zone);
        native.native_context = {point.utc_ms + 123, 9, {}, {}};
        legacy.open(); native.open();
        REQUIRE(legacy.effect(Seam::SourceBook, x::Flatten{}, fill(90)).status == x::Status::Applied);
        REQUIRE(native.effect(Seam::NativeBook, x::Flatten{}, fill(90)).status == x::Status::Applied);
        ++index;
        CHECK(legacy.last_day() == point.key && legacy.days() == point.count);
        exact(legacy.intraday(), -10 * static_cast<double>(index));
        exact(legacy.net(), native.net()); fields.unchanged(native);
        CHECK(native.rows().back().exit_time == point.utc_ms + 123);
        CHECK(legacy.rows().back().exit_time == point.utc_ms);
    }
}

template<class F> void run(F fn) {
    try { fn(); } catch (const Abort&) {}
    catch (const std::exception& e) { ++failures; std::printf("FAIL %s exception %s\n", scenario, e.what()); }
}
} // namespace

int main() {
    for (Seam s : natives) for (bool defaults : {false, true}) for (bool poison : {false, true})
        run([&] { native_inert(s, defaults, poison); });
    for (Seam s : {Seam::NativeSelected, Seam::NativeReverse}) run([&] { giant_intraday(s); });
    run(opening_and_noeffect);
    for (Seam s : sources) { run([&] { source_failures(s); }); run([&] { source_row_order(s); }); run([&] { committed_slice(s); }); }
    run(cycle_and_generic_cash_precedence); run(generic_counter_guards); run(invalid_projections_and_source_stage);
    run(one_fee_one_owner); run(source_hash_domains);
    run([] { calendar_pair("Asia/Taipei", {{1743435000000LL, 3103, 1}, {1743435600000LL, 3103, 1}, {1743436800000LL, 104, 2}}); });
    run([] { calendar_pair("America/New_York", {{1741503540000LL, 903, 1}, {1741503600000LL, 903, 1}, {1741579140000LL, 903, 1}, {1741579200000LL, 1003, 2}}); });
    run([] { calendar_pair("America/New_York", {{1762061400000LL, 211, 1}, {1762065000000LL, 211, 1}, {1762145940000LL, 211, 1}, {1762146000000LL, 311, 2}}); });
    run([] { calendar_pair("UTC", {{1736121600000LL, 601, 1}, {1767657600000LL, 601, 1}}); }); // Existing day key omits year.
    std::printf("%s settlement observation boundary: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
