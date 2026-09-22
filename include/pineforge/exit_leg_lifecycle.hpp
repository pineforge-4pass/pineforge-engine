#pragma once
// First standalone lifecycle ABI; aggregate engine layout uses its own epoch.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <variant>
#include <type_traits>
#include <vector>

namespace pineforge::exit_legs {
inline namespace lifecycle_v1 {
inline double absent() { return std::numeric_limits<double>::quiet_NaN(); }
enum class Leg : uint8_t { Stop, Limit, Trail };
// The observation domain a frame comes from. `FillRecalc` is the
// fill-recalculation re-entry pass -- the host re-runs its script after a fill
// and observes the rest of the same bar; `MagnifierFillRecalc` is that pass on
// a magnified sub-bar. `Coof` / `MagnifierCoof` are the historical spellings
// (coof = calc-on-order-fills, the Pine adapter's name for the same pass);
// they are DEPRECATED aliases with identical values and are removed at
// lifecycle_v2. See ADR-0001 "Deprecated public spellings".
enum class Domain : uint8_t {
    Ordinary, FillRecalc, Magnifier, MagnifierFillRecalc, RawTicks,
    Coof [[deprecated("Coof is the historical spelling of FillRecalc; removed at lifecycle_v2")]]
        = FillRecalc,
    MagnifierCoof [[deprecated("MagnifierCoof is the historical spelling of "
                               "MagnifierFillRecalc; removed at lifecycle_v2")]]
        = MagnifierFillRecalc,
};
enum class Phase : uint8_t { Observation, AfterMargin };
enum class Fold : uint8_t { Prefix, Continue };
struct Frame {
    uint64_t event = 0;
    int64_t bar = -1;
    Domain domain = Domain::Ordinary;
    Phase phase = Phase::Observation;
};
struct Target { uint64_t incarnation = 0; int64_t owner = 0; };
struct Prices {
    double limit_price = absent();
    double stop_price = absent();
    double trail_points = absent();
    double trail_price = absent();
    double trail_offset = absent();
    double profit_ticks = absent();
    double loss_ticks = absent();
};
class Lifecycle;
struct Definition {
    uint64_t incarnation() const { return incarnation_; }
    uint64_t revision() const { return revision_; }
    bool has_value() const { return value_ != nullptr; }
    const Prices& prices() const { static const Prices empty{}; return value_ ? *value_ : empty; }
private:
    friend class Lifecycle;
    uint64_t incarnation_ = 0;
    uint64_t revision_ = 0;
    std::shared_ptr<const Prices> value_;
};
struct Barrier { Frame requested; Target target; uint64_t revision = 0; };
struct ObservationWindow {
    Frame excluded;
    double best = absent();
    double prefix = absent();
};
struct Retirement { uint64_t generation; Frame cause; };
struct Replacement {
    uint64_t queue_predecessor;
    Definition revival_definition;
    Barrier release;
};
struct Suspension {
    Frame cause;
    std::vector<Leg> legs;
    std::optional<Barrier> hold;
    std::optional<Definition> revival_definition;
    std::optional<Replacement> replacement;
    std::optional<ObservationWindow> window;
};
struct BindOwner { int64_t owner; };
struct Suspend {
    std::vector<Leg> legs;
    std::optional<Barrier> hold;
    std::optional<ObservationWindow> window;
    std::vector<Leg> retire;
};
struct StageReplacement { Replacement relation; };
struct CancelDeferredActivation {};
struct Restore { std::vector<Leg> legs; };
struct CompleteBarrier { Frame completed; std::optional<Barrier> requested; };
struct Observe { double high; double low; int direction; Fold fold; };
struct Cancel { std::vector<Leg> legs; };
using Operation = std::variant<BindOwner, Suspend, StageReplacement,
    CancelDeferredActivation, Restore, CompleteBarrier, Observe, Cancel>;
struct Action {
    Target target;
    uint64_t expected_revision;
    Frame cause;
    Operation operation;
};
enum class Result { Applied, Replay, StaleIdentity, StaleOwner, StaleRevision,
                    ExpiredEvent, ConflictingReplay, InvalidAction, Exhausted };

// One canonical definition and bounded current lifecycle. No external book,
// source-rule recognizer or map of historical action outcomes.
class Lifecycle {
public:
    const Prices& prices() const { return definition_.prices(); }
    const Definition& current_definition() const { return definition_; }
    Definition definition(uint64_t incarnation) const {
        if (!incarnation || (target_.incarnation && target_.incarnation != incarnation))
            throw std::logic_error("definition identity mismatch");
        Definition d = definition_; d.incarnation_ = incarnation; return d;
    }
    Target target() const { return target_; }
    uint64_t revision() const { return revision_; }
    uint64_t generation(Leg leg) const { return generations_[index(leg)]; }
    const std::optional<Suspension>& suspension() const { return suspension_; }
    const std::array<std::optional<Retirement>, 3>& retirements() const { return retired_; }
    const std::optional<Action>& last_action() const { return last_; }
    bool suspended(Leg leg) const {
        if (!suspension_) return false;
        for (Leg selected : suspension_->legs) if (selected == leg) return true;
        return false;
    }
    bool retired(Leg leg) const { return retired_[index(leg)].has_value(); }
    bool available(Leg leg, int64_t bar) const {
        if (suspended(leg) || retired(leg)) return false;
        return leg != Leg::Trail || !suspension_ || !suspension_->window
            || suspension_->window->excluded.bar != bar;
    }
    bool dormant() const { return suspension_.has_value(); }
    bool pending_replacement() const { return suspension_ && suspension_->replacement.has_value(); }
    double original_stop() const {
        return suspension_ && suspension_->revival_definition
            ? suspension_->revival_definition->prices().stop_price : absent();
    }
    std::optional<Barrier> release_barrier() const {
        if (!suspension_) return {};
        if (suspension_->replacement) return suspension_->replacement->release;
        return suspension_->hold;
    }
    int64_t hold_bar() const { return suspension_ && suspension_->hold ? suspension_->hold->requested.bar : -1; }
    int64_t excluded_bar() const { return suspension_ && suspension_->window ? suspension_->window->excluded.bar : -1; }
    double trail_best() const { return suspension_ && suspension_->window ? suspension_->window->best : absent(); }
    double trail_prefix() const { return suspension_ && suspension_->window ? suspension_->window->prefix : absent(); }

    // Construction/fresh copied quantity bindings, not a replayable action.
    // The owner supplies the new incarnation; no receipt survives a fork.
    void attach(uint64_t incarnation, int64_t owner) {
        if (!incarnation || owner < 0 || target_.incarnation) throw std::logic_error("exit lifecycle already attached");
        target_ = {incarnation, owner}; definition_.incarnation_ = incarnation;
    }
    void fork(uint64_t incarnation, int64_t owner) {
        if (!incarnation || owner < 0 || incarnation == target_.incarnation) throw std::logic_error("invalid exit lifecycle fork");
        target_ = {incarnation, owner}; definition_.incarnation_ = incarnation;
        revision_ = 0; last_.reset();
        // A new quantity-bound instruction has no queue predecessor, even
        // when its inherited old-stop definition remains causally relevant.
        if (suspension_ && suspension_->replacement)
            suspension_->replacement->queue_predecessor = 0;
        if (suspension_ && suspension_->hold) {
            suspension_->hold->target = target_; suspension_->hold->revision = revision_;
        }
        if (suspension_ && suspension_->replacement) {
            suspension_->replacement->release.target = target_;
            suspension_->replacement->release.revision = revision_;
        }
    }

    // Trusted local definition construction/materialization. The immutable
    // Prices object is replaced, so predecessor references cannot diverge.
    // Each change invalidates outstanding action revisions, even without fills.
    void set_prices(Prices value) {
        if (revision_ == UINT64_MAX || definition_.revision_ == UINT64_MAX)
            throw std::overflow_error("exit definition revision exhausted");
        auto next = std::make_shared<const Prices>(std::move(value));
        definition_.value_ = std::move(next); ++definition_.revision_; ++revision_;
    }
#define PF_LEG_PRICE_SETTER(name) \
    double set_##name(double value) { auto next = prices(); next.name = value; set_prices(std::move(next)); return value; }
    PF_LEG_PRICE_SETTER(limit_price)
    PF_LEG_PRICE_SETTER(stop_price)
    PF_LEG_PRICE_SETTER(trail_points)
    PF_LEG_PRICE_SETTER(trail_price)
    PF_LEG_PRICE_SETTER(trail_offset)
    PF_LEG_PRICE_SETTER(profit_ticks)
    PF_LEG_PRICE_SETTER(loss_ticks)
#undef PF_LEG_PRICE_SETTER

    Result apply(Target actual, const Action& action) {
        if (!actual.incarnation || actual.incarnation != target_.incarnation
            || action.target.incarnation != actual.incarnation) return Result::StaleIdentity;
        if (actual.owner != target_.owner || action.target.owner != actual.owner) return Result::StaleOwner;
        if (!valid_payload(action)) return Result::InvalidAction;
        if (last_ && action.cause.event == last_->cause.event) {
            if (!equal(action, *last_)) return Result::ConflictingReplay;
            return revision_ == last_->expected_revision + 1 ? Result::Replay : Result::StaleRevision;
        }
        if (!action.cause.event || (last_ && action.cause.event < last_->cause.event)) return Result::ExpiredEvent;
        if (action.expected_revision != revision_) return Result::StaleRevision;
        if (revision_ == UINT64_MAX) return Result::Exhausted;
        Lifecycle next = *this;
        if (!next.perform(action)) return Result::InvalidAction;
        ++next.revision_; next.last_ = action;
        *this = std::move(next);
        return Result::Applied;
    }

    // Every stored field is emitted, including the last bounded replay payload.
    // The same traversal supplies state hashing and exact replay comparison.
    template<class Sink> void visit(Sink& f) const {
        f.u(target_.incarnation); f.i(target_.owner); f.u(revision_);
        visit_definition(f, definition_);
        for (auto generation : generations_) f.u(generation);
        for (const auto& retirement : retired_) {
            f.b(retirement.has_value());
            if (retirement) { f.u(retirement->generation); visit_frame(f, retirement->cause); }
        }
        f.b(suspension_.has_value());
        if (suspension_) {
            visit_frame(f, suspension_->cause); visit_legs(f, suspension_->legs);
            visit_barrier(f, suspension_->hold);
            f.b(suspension_->revival_definition.has_value());
            if (suspension_->revival_definition) visit_definition(f, *suspension_->revival_definition);
            f.b(suspension_->replacement.has_value());
            if (suspension_->replacement) visit_replacement(f, *suspension_->replacement);
            visit_window(f, suspension_->window);
        }
        f.b(last_.has_value()); if (last_) visit_action(f, *last_);
    }
private:
    Definition definition_;
    Target target_;
    uint64_t revision_ = 0;
    std::array<uint64_t, 3> generations_{{1, 1, 1}};
    std::array<std::optional<Retirement>, 3> retired_;
    std::optional<Suspension> suspension_;
    std::optional<Action> last_;

    static size_t index(Leg leg) { return static_cast<size_t>(leg); }
    static bool valid_legs(const std::vector<Leg>& legs) {
        unsigned seen = 0; // transient validation, not stored policy
        for (Leg leg : legs) { const auto n = index(leg); if (n >= 3 || (seen & (1u << n))) return false; seen |= 1u << n; }
        return !legs.empty();
    }
    static bool valid_frame(const Frame& f) {
        return static_cast<unsigned>(f.domain) <= static_cast<unsigned>(Domain::RawTicks)
            && static_cast<unsigned>(f.phase) <= static_cast<unsigned>(Phase::AfterMargin);
    }
    static bool valid_payload(const Action& action) {
        if (!valid_frame(action.cause)) return false;
        return std::visit([](const auto& op) {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, Suspend>)
                return valid_legs(op.legs) && (op.retire.empty() || valid_legs(op.retire))
                    && (!op.hold || valid_frame(op.hold->requested))
                    && (!op.window || valid_frame(op.window->excluded));
            else if constexpr (std::is_same_v<T, Restore> || std::is_same_v<T, Cancel>) return valid_legs(op.legs);
            else if constexpr (std::is_same_v<T, BindOwner>) return op.owner >= 0;
            else if constexpr (std::is_same_v<T, Observe>) return (op.direction == 1 || op.direction == -1)
                && (op.fold == Fold::Prefix || op.fold == Fold::Continue);
            else if constexpr (std::is_same_v<T, CompleteBarrier>) return valid_frame(op.completed)
                && (!op.requested || valid_frame(op.requested->requested));
            else if constexpr (std::is_same_v<T, StageReplacement>) return valid_frame(op.relation.release.requested);
            else return true;
        }, action.operation);
    }
    static bool unissued(const Barrier& b) {
        return b.target.incarnation == 0 && b.target.owner == 0 && b.revision == 0;
    }
    Barrier issue(Barrier b) const {
        b.target = target_; b.revision = revision_ + 1; return b;
    }
    static bool same_barrier(const Barrier& a, const Barrier& b) {
        return a.target.incarnation == b.target.incarnation && a.target.owner == b.target.owner
            && a.revision == b.revision && a.requested.event == b.requested.event
            && a.requested.bar == b.requested.bar && a.requested.domain == b.requested.domain
            && a.requested.phase == b.requested.phase;
    }
    // Event identities share a causal sequence. Bar/phase coordinates can
    // also be compared when they belong to the same observation domain.
    static bool not_after(const Frame& occurrence, const Frame& processing) {
        return occurrence.event <= processing.event
            && (occurrence.domain != processing.domain
                || occurrence.bar < processing.bar
                || (occurrence.bar == processing.bar
                    && static_cast<unsigned>(occurrence.phase)
                        <= static_cast<unsigned>(processing.phase)));
    }
    bool restore(const std::vector<Leg>& legs) {
        if (!valid_legs(legs)) return false;
        for (Leg leg : legs) if (generations_[index(leg)] == UINT64_MAX) return false;
        for (Leg leg : legs) {
            ++generations_[index(leg)]; retired_[index(leg)].reset();
            if (suspension_) {
                auto& list = suspension_->legs;
                for (auto it = list.begin(); it != list.end();) {
                    if (*it == leg) it = list.erase(it); else ++it;
                }
            }
        }
        if (suspension_ && suspension_->legs.empty()) suspension_.reset();
        return true;
    }
    bool perform(const Action& action) {
        return std::visit([&](const auto& op) -> bool {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, BindOwner>) { target_.owner = op.owner; return true; }
            else if constexpr (std::is_same_v<T, Suspend>) {
                if (!valid_legs(op.legs) || (!op.retire.empty() && !valid_legs(op.retire))) return false;
                if (op.hold && !unissued(*op.hold)) return false;
                const auto hold = op.hold ? std::optional<Barrier>{issue(*op.hold)} : std::nullopt;
                suspension_ = Suspension{action.cause, op.legs, hold, {}, {}, op.window};
                for (Leg leg : op.retire) if (!retired_[index(leg)])
                    retired_[index(leg)] = Retirement{generations_[index(leg)], action.cause};
                return true;
            } else if constexpr (std::is_same_v<T, StageReplacement>) {
                if (!op.relation.revival_definition.incarnation() || !unissued(op.relation.release)) return false;
                // This component supports one outstanding completion target.
                // A new explicit Suspend may supersede an episode; staging a
                // second obligation on an existing hold/replacement is refused.
                if (suspension_ && (suspension_->hold || suspension_->replacement)) return false;
                if (!suspension_) suspension_ = Suspension{action.cause, {Leg::Stop, Leg::Limit}, {}, {}, {}, {}};
                suspension_->revival_definition = op.relation.revival_definition;
                suspension_->replacement = op.relation;
                suspension_->replacement->release = issue(op.relation.release);
                return true;
            } else if constexpr (std::is_same_v<T, CancelDeferredActivation>) {
                if (!suspension_) return false;
                suspension_->replacement.reset(); return true;
            } else if constexpr (std::is_same_v<T, Restore>) { return restore(op.legs); }
            else if constexpr (std::is_same_v<T, CompleteBarrier>) {
                const auto expected = release_barrier();
                if (!expected || !op.requested || !same_barrier(*expected, *op.requested)) return false;
                const auto& origin = expected->requested;
                // Coordinates from different domains are not interchangeable.
                // Cross-domain completion names the actual old obligation;
                // the caller selects that conversion, never a permission bit.
                // The completion occurrence may precede its processing receipt
                // (e.g. an intervening owner bind), but it cannot come from the
                // future. Check every comparable pair, including an origin and
                // receipt separated by a cross-domain completion.
                if (!not_after(origin, op.completed)
                    || !not_after(op.completed, action.cause)
                    || !not_after(origin, action.cause)) return false;
                if (suspension_->replacement) return restore({Leg::Stop, Leg::Limit, Leg::Trail});
                suspension_->hold.reset(); // only the named hold, no activation
                return true;
            } else if constexpr (std::is_same_v<T, Observe>) {
                if (!suspension_ || !suspension_->window || (op.direction != 1 && op.direction != -1)) return false;
                auto& w = *suspension_->window;
                // A window excludes its entire originating observation bar.
                // Other domains require explicit caller selection; Pine also
                // retains its legacy bar-only filter before issuing Observe.
                if (action.cause.event <= w.excluded.event
                    || (action.cause.domain == w.excluded.domain
                        && action.cause.bar <= w.excluded.bar)) return false;
                if (op.fold == Fold::Prefix) w.prefix = w.best;
                const double price = op.direction > 0 ? op.high : op.low;
                if (std::isnan(w.best) || (op.direction > 0 ? price > w.best : price < w.best)) w.best = price;
                return true;
            } else if constexpr (std::is_same_v<T, Cancel>) {
                if (!valid_legs(op.legs)) return false;
                for (Leg leg : op.legs) if (!retired_[index(leg)])
                    retired_[index(leg)] = Retirement{generations_[index(leg)], action.cause};
                return true;
            }
            return false;
        }, action.operation);
    }
    template<class S> static void visit_frame(S& f, const Frame& v) {
        f.u(v.event); f.i(v.bar); f.u(static_cast<uint64_t>(v.domain)); f.u(static_cast<uint64_t>(v.phase));
    }
    template<class S> static void visit_prices(S& f, const Prices& p) {
        f.d(p.limit_price); f.d(p.stop_price); f.d(p.trail_points); f.d(p.trail_price);
        f.d(p.trail_offset); f.d(p.profit_ticks); f.d(p.loss_ticks);
    }
    template<class S> static void visit_definition(S& f, const Definition& d) {
        f.u(d.incarnation_); f.u(d.revision_); f.b(d.value_ != nullptr); visit_prices(f, d.prices());
    }
    template<class S> static void visit_legs(S& f, const std::vector<Leg>& legs) {
        f.u(legs.size()); for (Leg leg : legs) f.u(static_cast<uint64_t>(leg));
    }
    template<class S> static void visit_barrier_value(S& f, const Barrier& b) {
        visit_frame(f, b.requested); f.u(b.target.incarnation); f.i(b.target.owner); f.u(b.revision);
    }
    template<class S> static void visit_barrier(S& f, const std::optional<Barrier>& b) {
        f.b(b.has_value()); if (b) visit_barrier_value(f, *b);
    }
    template<class S> static void visit_window(S& f, const std::optional<ObservationWindow>& w) {
        f.b(w.has_value()); if (w) { visit_frame(f, w->excluded); f.d(w->best); f.d(w->prefix); }
    }
    template<class S> static void visit_replacement(S& f, const Replacement& r) {
        f.u(r.queue_predecessor); visit_definition(f, r.revival_definition); visit_barrier_value(f, r.release);
    }
    // The broker's ordinary double fold canonicalizes NaN/zero. Replay is
    // bit-exact, so its bounded receipt payload must preserve those bits too.
    template<class S> struct ReplaySink {
        S& sink;
        void u(uint64_t v) { sink.u(v); }
        void i(int64_t v) { sink.i(v); }
        void b(bool v) { sink.b(v); }
        void d(double v) { uint64_t bits; std::memcpy(&bits, &v, sizeof(bits)); sink.u(bits); }
    };
    template<class S> static void visit_action(S& f, const Action& a) {
        ReplaySink<S> raw{f};
        visit_action_fields(raw, a);
    }
    template<class S> static void visit_action_fields(S& f, const Action& a) {
        f.u(a.target.incarnation); f.i(a.target.owner); f.u(a.expected_revision); visit_frame(f, a.cause);
        f.u(a.operation.index());
        std::visit([&](const auto& op) {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, BindOwner>) f.i(op.owner);
            else if constexpr (std::is_same_v<T, Suspend>) { visit_legs(f, op.legs); visit_barrier(f, op.hold); visit_window(f, op.window); visit_legs(f, op.retire); }
            else if constexpr (std::is_same_v<T, StageReplacement>) visit_replacement(f, op.relation);
            else if constexpr (std::is_same_v<T, Restore> || std::is_same_v<T, Cancel>) visit_legs(f, op.legs);
            else if constexpr (std::is_same_v<T, CompleteBarrier>) { visit_frame(f, op.completed); visit_barrier(f, op.requested); }
            else if constexpr (std::is_same_v<T, Observe>) { f.d(op.high); f.d(op.low); f.i(op.direction); f.u(static_cast<uint64_t>(op.fold)); }
        }, a.operation);
    }
    struct Exact {
        std::vector<uint64_t> words;
        void u(uint64_t v) { words.push_back(v); }
        void i(int64_t v) { u(static_cast<uint64_t>(v)); }
        void b(bool v) { u(v ? 1 : 0); }
        void d(double v) { uint64_t bits; std::memcpy(&bits, &v, sizeof(bits)); u(bits); }
    };
    static bool equal(const Action& a, const Action& b) { Exact x, y; visit_action(x, a); visit_action(y, b); return x.words == y.words; }
};
} // inline namespace lifecycle_v1
} // namespace pineforge::exit_legs
