#pragma once

// TEMPORARY ENGINE-SIDE PINE COMPATIBILITY. Native engines start detached.
// New generated sources attach this component before host metadata arrives;
// old protected limit assignments remain an explicit legacy-source opt-in.
// The source facade and physical package location remain migration boundaries.
#include "intraday_order_budget.hpp"
#include "../../position_close_obligation.hpp"
#include "../../session_time.hpp"
#include "../../timeframe.hpp"
#include <cmath>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace pineforge::compat::pine {

enum class CapAttachment { LegacySource, None };
enum class Placement { Allow, Deny };
enum class Dispatch { Allow, Decline };
enum class FillOutcome { NoEffect, Committed };
enum class AttemptOrigin { Independent, CloseQuotaBeneficiary };
enum class OrderKind { Market, Entry, Other };
enum class Side { Flat, Long, Short };
enum class DirectCloseRouting { Ignore, Observe };

struct CapConfiguration {
    int limit = 0;
    bool skip_noop_market = false;
    bool defer_pooc_close = false;
    bool count_pooc_full_close = false;
};
struct CapClock {
    int64_t timestamp = 0;
    std::string session;
    std::string timezone;
    int chart_day = 0;
    int chart_month = 0;
};
struct Calculation {
    bool process_on_close;
    bool calc_on_fills;
    bool coof_scheduler;
    bool magnifier;
    bool stream_warmup;
    bool stream_idle;
    bool fifo;
    int bar;
};
struct MatchedAttempt {
    OrderKind kind;
    uint64_t incarnation;
    int created_bar;
    bool is_long;
    Side live_side;
    int live_entries;
    int pyramiding;
};
struct QuotaTrigger { OrderRiskDay day; int charged_slots; };
struct Admission {
    Dispatch dispatch = Dispatch::Allow;
    std::optional<QuotaTrigger> trigger;
};
struct ContinuationCandidate {
    OrderKind kind;
    int created_bar;
    bool is_long;
    int64_t created_seq;
    uint64_t incarnation;
};
struct CloseCause {
    uint64_t action_id;
    OrderRiskDay charged_day;
    int charged_slots;
    int trigger_bar;
    uint64_t trigger_order;
};
struct Prices { double fill; double open; double high; double low; };
struct CloseNow { broker::PositionCloseRequest request; double price; };
struct CloseNextOpen { broker::PositionCloseRequest request; };
using CloseDecision = std::variant<std::monostate, CloseNow, CloseNextOpen>;

class IntradayCap {
public:
    static constexpr uint64_t schema_version = 1;
    explicit IntradayCap(CapAttachment attachment = CapAttachment::None)
        : attachment_(attachment) {}

    void attach() { attachment_ = CapAttachment::LegacySource; }

    // The old generated assignment is an explicit request for Pine behavior,
    // even from an opted-out native fixture. It never silently drops a rule.
    // Only the limit changes: statement-time updates do not reset the ledger.
    IntradayCap& operator=(int limit) {
        attach();
        configuration_.limit = limit;
        return *this;
    }
    CapAttachment attachment() const { return attachment_; }
    const CapConfiguration& configuration() const { return configuration_; }
    const IntradayOrderBudget& budget() const { return budget_; }
    const std::optional<CloseCause>& due_cause() const { return due_cause_; }
    uint64_t next_action() const { return next_action_; }
    bool active() const {
        return attachment_ != CapAttachment::None && configuration_.limit > 0;
    }
    bool needs_clock() const {
        return active() || (configuration_.count_pooc_full_close && budget_.transfer());
    }
    // Existing shortcut guards distinguish negative from exactly zero. This
    // is a Pine facade predicate, not a generic "any risk policy" switch.
    bool legacy_limit_is_zero() const {
        return attachment_ == CapAttachment::None || configuration_.limit == 0;
    }
    void metadata(const std::string& key, double value) {
        // Retain declaration values in the ONE configuration owner even when
        // detached. Metadata alone never attaches or activates the policy.
        // A later explicit legacy assignment therefore preserves metadata sent
        // before the first risk statement, without a second buffer or replay.
        const bool enabled = std::isfinite(value) && value > 0.0;
        if (key == "intraday_cap_skip_noop_market_fills") {
            configuration_.skip_noop_market = enabled;
            declared_ |= kDeclaredSkipNoopMarket;
        } else if (key == "intraday_cap_defer_pooc_close") {
            configuration_.defer_pooc_close = enabled;
            declared_ |= kDeclaredDeferPoocClose;
        } else if (key == "intraday_cap_count_pooc_full_close_fills") {
            configuration_.count_pooc_full_close = enabled;
            declared_ |= kDeclaredCountPoocFullClose;
        }
    }
    // A Pine script's own strategy.risk.max_intraday_filled_orders statement
    // counts as TradingView does: every candidate switch its host did not
    // declare is on (lane W10-DIAG-UNKNOWN rule CAP-ON,
    // tests/fixtures/intraday_cap_tv). A declared switch keeps its value, and
    // a component selected any other way keeps the defaults above.
    void adopt_script_count() {
        if (!(declared_ & kDeclaredSkipNoopMarket)) configuration_.skip_noop_market = true;
        if (!(declared_ & kDeclaredDeferPoocClose)) configuration_.defer_pooc_close = true;
        if (!(declared_ & kDeclaredCountPoocFullClose))
            configuration_.count_pooc_full_close = true;
    }
    std::uint8_t declared() const { return declared_; }
    static bool uses_chart_clock(const std::string& session) {
        return !(session.size() >= 9 && session[4] == '-'
            && hhmm_to_minutes(session.substr(0, 4)) >= 0
            && hhmm_to_minutes(session.substr(5, 4)) >= 0);
    }
    static OrderRiskDay risk_day(const CapClock& clock) {
        if (!uses_chart_clock(clock.session)) {
            return {internal::session_trading_day_index(
                clock.timestamp, clock.timezone, clock.session)};
        }
        // Preserve the existing fallback, including its omitted year.
        return {clock.chart_day * 100 + clock.chart_month};
    }
    Placement placement(const CapClock& clock) {
        if (!active()) return Placement::Allow;
        budget_.enter_day(risk_day(clock));
        return budget_.latched() ? Placement::Deny : Placement::Allow;
    }
    void decline(uint64_t incarnation) { budget_.decline(incarnation); }
    AttemptOrigin origin(const CapClock& clock, const Calculation& c,
                         uint64_t incarnation, uint64_t latest_fill) const {
        return configuration_.count_pooc_full_close
            && budget_.can_inherit(risk_day(clock), c.bar, incarnation, latest_fill)
            ? AttemptOrigin::CloseQuotaBeneficiary : AttemptOrigin::Independent;
    }
    Admission pre_dispatch(const CapClock& clock, const Calculation& context,
                           const MatchedAttempt& attempt, uint64_t latest_fill) {
        if (!active()) return {};
        if (configuration_.skip_noop_market && attempt.kind == OrderKind::Market
            && attempt.live_side != Side::Flat
            && (attempt.live_side == Side::Long) == attempt.is_long
            && attempt.live_entries >= attempt.pyramiding) {
            decline(attempt.incarnation);
            return {Dispatch::Decline, std::nullopt};
        }
        const auto day = risk_day(clock);
        const auto result = budget_.admit_matched_attempt(
            day, configuration_.limit, context.bar,
            configuration_.count_pooc_full_close ? attempt.incarnation : 0,
            latest_fill);
        if (result == QuotaAdmission::Blocked)
            return {Dispatch::Decline, std::nullopt};
        if (result == QuotaAdmission::ReachedLimit)
            return {Dispatch::Allow, QuotaTrigger{day, budget_.charged_slots()}};
        return {};
    }
    void outcome(FillOutcome outcome, AttemptOrigin origin) {
        if (outcome == FillOutcome::Committed && origin == AttemptOrigin::Independent)
            budget_.expire_transfer();
    }
    DirectCloseRouting direct_close_routing(const Calculation& c, bool full) const {
        return active() && configuration_.count_pooc_full_close && full
            && ordinary(c) && !c.coof_scheduler && c.fifo
            ? DirectCloseRouting::Observe : DirectCloseRouting::Ignore;
    }
    void committed_close(const CapClock& clock, const Calculation& c, Side before,
                         uint64_t fill, const std::vector<ContinuationCandidate>& candidates) {
        const ContinuationCandidate* selected = nullptr;
        for (const auto& candidate : candidates) {
            if (candidate.kind != OrderKind::Market || candidate.created_bar != c.bar
                || candidate.is_long == (before == Side::Long)) continue;
            if (!selected || candidate.created_seq < selected->created_seq)
                selected = &candidate;
        }
        budget_.count_committed_close(risk_day(clock), configuration_.limit,
            fill, c.bar, selected ? selected->incarnation : 0);
    }
    CloseDecision post_dispatch(const Admission& admission, const Calculation& c,
                                const MatchedAttempt& attempt, Side side,
                                int64_t cycle, Prices prices) {
        if (!admission.trigger) return {};
        if (side == Side::Flat) { budget_.latch(); return {}; }
        const uint64_t action = next_action_++;
        broker::PositionCloseRequest request{action, cycle, c.bar,
            "Close Position (Max number of filled orders in one day)"};
        if (configuration_.defer_pooc_close && ordinary(c)
            && attempt.kind == OrderKind::Market && attempt.created_bar == c.bar) {
            budget_.latch();
            due_cause_ = CloseCause{action, admission.trigger->day,
                admission.trigger->charged_slots, c.bar, attempt.incarnation};
            return CloseNextOpen{std::move(request)};
        }
        double price = prices.fill;
        if (attempt.kind == OrderKind::Market || attempt.kind == OrderKind::Entry) {
            if (side == Side::Long && prices.fill > prices.open) price = prices.high;
            else if (side == Side::Short && prices.fill < prices.open) price = prices.low;
        }
        return CloseNow{std::move(request), price};
    }
    // The synchronous simulator attempted the requested close. The existing
    // cap latches even if no economic effect occurred; flatness is read from
    // actual position state, never inferred from this notification.
    void after_immediate_close_attempt() { budget_.latch(); }
    void ordinary_open(int bar) {
        budget_.expire_transfer();
        if (due_cause_ && bar > due_cause_->trigger_bar) due_cause_.reset();
    }
    void source_batch_end() { budget_.expire_transfer(); }
    void reset_run() {
        budget_ = {};
        due_cause_.reset();
        next_action_ = 1;
    }
private:
    static bool ordinary(const Calculation& c) {
        return c.process_on_close && !c.calc_on_fills && !c.magnifier
            && !c.stream_warmup && c.stream_idle;
    }
    static constexpr std::uint8_t kDeclaredSkipNoopMarket = 1;
    static constexpr std::uint8_t kDeclaredDeferPoocClose = 2;
    static constexpr std::uint8_t kDeclaredCountPoocFullClose = 4;
    CapAttachment attachment_;
    CapConfiguration configuration_;
    std::uint8_t declared_ = 0;
    IntradayOrderBudget budget_;
    std::optional<CloseCause> due_cause_;
    uint64_t next_action_ = 1;
};

} // namespace pineforge::compat::pine
