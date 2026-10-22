// LAB-O1SEAMS s3, round 1. Edit-only delivery against 87ba09a7013383847.
// Contract: the read-only o1s34/scratch-store.md, lines 357--767, SHA-256
// 71adb08d2a4308f961cc8f8f24a7c91fffd38e9b487d04cef558c133cac6ce5e.
// NOTHING HERE HAS BEEN COMPILED OR RUN.
//
// Lines 757--762 explicitly leave the C++ codec binding unspecified. The
// deliberately UNDECLARED calls bind_journal_codec, encode_journal_record,
// decode_journal_record, append_journal_record and journal_readback below name
// those missing seams; they are requirements, not production declarations or
// a proposed ABI. The implementing lane must settle that binding. Decode into
// an owning event is a diagnostic oracle only, never the production reader.
//
// Define PINEFORGE_O1S3_CLAUSE=1..10 to compile just one clause body, in the
// order of the case table at the end. Zero selects all ten. This lets the box
// record the first diagnostic separately, including the two assertion reds
// (6 and 8) that use the existing interfaces. There is no skipped-case success.
//
// Oracle vectors/strings and test_memory's provisioned backing are test
// infrastructure. They are prepared outside a measured interval and are never
// adopted by a consumer. Producer captures, observations, shared definitions,
// their control blocks, unused container capacity and transient allocations
// made INSIDE that interval are all counted by the real global replacements.
// W is already in that heap census and is never added a second time. Backing
// extent is reported separately; it is NOT physical-memory evidence for B_p.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/compat/pine/market_admission.hpp>
#include <pineforge/detail/scratch_errors.hpp>
#include <pineforge/detail/scratch_store.hpp>
#include <pineforge/native_order.hpp>
#include <pineforge/source/pine_adapter.hpp>
#include "../src/native_execution_consumer.hpp"
#include "../src/source/pine_command_scratch_production.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <unistd.h>

#ifndef PINEFORGE_O1S3_CLAUSE
#define PINEFORGE_O1S3_CLAUSE 0
#endif

namespace {
namespace no = pineforge::native_order;
namespace ad = pineforge::admission;
namespace pd = pineforge::detail;
using pd::ScratchBuffer;
using pd::ScratchBytes;
using pd::ScratchStatus;
using pd::CommandScratchStore;
using Bytes = std::vector<std::byte>;
constexpr std::uint64_t seed = 1469598103934665603ULL;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void status_is(ScratchStatus actual, ScratchStatus expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "%s: got %u, expected %u\n", message,
                     static_cast<unsigned>(actual), static_cast<unsigned>(expected));
        throw std::runtime_error(message);
    }
}
template<class F> void engine_invariant(F&& operation) {
    bool refused = false;
    try { operation(); }
    catch (const pineforge::RunFailureInfo& error) {
        refused = error.run_failure_code() == pineforge::RunFailureCode::engine_invariant;
    }
    require(refused, "the invalid admission operation must refuse with engine_invariant");
}

std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    std::memcpy(&result, &value, sizeof result);
    return result;
}
double binary64(std::uint64_t value) {
    double result = 0;
    std::memcpy(&result, &value, sizeof result);
    return result;
}
std::string text(std::size_t length, unsigned salt) {
    std::string result(length, '\0');
    for (std::size_t i = 0; i < length; ++i)
        result[i] = static_cast<char>((i * 37 + salt * 19) & 255);
    return result; // Includes NULs; the final byte need not be a terminator.
}
ScratchBytes view(const Bytes& bytes) { return {bytes.data(), bytes.size()}; }

// Test-written storage model. No production encoder, reflection, hash, object
// representation, padding, intern address or shared_ptr control block is used.
struct Wire {
    Bytes bytes;
    void u(std::uint64_t value) {
        for (unsigned i = 0; i != 8; ++i)
            bytes.push_back(static_cast<std::byte>((value >> (8 * i)) & 255));
    }
    void add(bool value) { bytes.push_back(value ? std::byte{1} : std::byte{0}); }
    template<class T, std::enable_if_t<std::is_integral_v<T>
                                     && !std::is_same_v<T, bool>, int> = 0>
    void add(T value) { u(static_cast<std::uint64_t>(value)); }
    template<class T, std::enable_if_t<std::is_enum_v<T>, int> = 0>
    void add(T value) { add(static_cast<std::underlying_type_t<T>>(value)); }
    void add(double value) { u(bits(value)); }
    void add(const std::string& value) {
        u(value.size());
        for (const unsigned char byte : value) bytes.push_back(static_cast<std::byte>(byte));
    }
    void add(const no::SessionKey& value) { add(value.string()); }
    template<class... T> void fields(const T&... values) { (add(values), ...); }
    template<class T> void add(const std::optional<T>& value) {
        add(value.has_value());
        if (value) add(*value);
    }
    template<class T> void add(const std::shared_ptr<T>& value) {
        add(static_cast<bool>(value));
        if (value) add(*value);
    }
    template<class T> void add(const std::vector<T>& values) {
        u(values.size());
        for (const auto& value : values) add(value);
    }
    template<class... T> void add(const std::variant<T...>& value) {
        u(value.index());
        std::visit([&](const auto& active) { add(active); }, value);
    }

#define EMPTY(T) void add(const T&) {}
#define VALUE(T, ...) void add(const T& x) { fields(__VA_ARGS__); }
    EMPTY(no::Flatten)
    EMPTY(no::OwnerOpenedUnits)
    EMPTY(no::Market)
    EMPTY(no::Absolute)
    EMPTY(no::ImmediateRemaining)
    EMPTY(no::Independent)
    EMPTY(no::NoGroup)
    EMPTY(no::RemainingUnbound)
    EMPTY(no::RemainingFlattenAll)
    EMPTY(no::RemainingDeferred)
    EMPTY(no::NoTarget)
    EMPTY(no::RemainingProjectionUnbound)
    EMPTY(no::RemainingProjectionFlattenAll)
    EMPTY(no::RemainingProjectionDeferred)
    EMPTY(no::RemainingProjectionNoTarget)
    EMPTY(no::BookTransaction)
    EMPTY(no::UnboundBookClose)
    EMPTY(no::MarketReady)
    EMPTY(no::LimitReady)
    EMPTY(no::StopIdle)
    EMPTY(no::StopActive)
    EMPTY(no::StopLimitPending)
    EMPTY(no::StopLimitLive)
    EMPTY(no::TrailWaitArm)
    EMPTY(no::AllowanceUnset)
    EMPTY(no::PendingNone)
    EMPTY(no::PositionFlat)
    EMPTY(pineforge::execution::Book)
    VALUE(no::RunIdentity, x.session_key, x.run_number)
    VALUE(no::RequestHandle, x.run, x.incarnation)
    VALUE(no::EventId, x.run, x.ordinal)
    VALUE(no::Birth, x.acceptance_ordinal, x.decision_time_lower_bound)
    VALUE(no::CohortHandle, x.value)
    VALUE(no::Transact, x.signed_units)
    VALUE(no::ReverseTo, x.signed_units)
    VALUE(no::HostSized, x.kind, x.side)
    VALUE(no::CashValue, x.cash)
    VALUE(no::EquityFraction, x.fraction)
    VALUE(no::Sized, x.side, x.basis, x.time, x.price, x.grid_policy, x.reserve_percent_fee)
    VALUE(no::ExplicitUnits, x.units)
    VALUE(no::ScopeFraction, x.fraction, x.claim, x.basis)
    VALUE(no::Reduce, x.size)
    VALUE(no::Limit, x.price, x.fill_through)
    VALUE(no::Stop, x.price)
    VALUE(no::StopLimit, x.stop, x.limit)
    VALUE(no::TrailTicks, x.ticks)
    VALUE(no::Trail, x.offset, x.arm_price, x.ticks, x.best_seed)
    VALUE(no::FromOwnerFill, x.offset, x.ticks, x.rounding)
    VALUE(no::PointBudget, x.units)
    VALUE(no::WaitForApplied, x.parent, x.visibility, x.first_match, x.scope)
    VALUE(no::BindOpening, x.opening, x.cycle)
    VALUE(no::BindOpenings, x.openings, x.cycle, x.external_openings)
    VALUE(no::BindCohort, x.cohort)
    VALUE(no::Member, x.group, x.cohort, x.effect)
    VALUE(no::Request, x.intent, x.label, x.comment, x.trigger, x.capacity,
          x.owner, x.group, x.anchor)
    VALUE(pineforge::NativeCoordinate, x.ordinal, x.interval_index, x.input_interval_index,
          x.open_ms, x.eligible_open_ms, x.last_traded_close_ms, x.next_period_open_ms,
          x.next_input_open_ms, x.effective_time_ms, x.source_price_time_ms,
          x.provenance, x.path_phase, x.completion)
    VALUE(no::MatchCursor, x.point, x.t)
    VALUE(no::RemainingUnits, x.q)
    VALUE(no::RemainingProjectionUnits, x.q)
    VALUE(no::Wait, x.parent)
    VALUE(no::ArmedTransaction, x.parent, x.cause, x.cause_cursor)
    VALUE(no::BookClose, x.cycle, x.side, x.binding_event, x.binding_cursor)
    VALUE(no::EnrollmentFromCommand, x.accepted)
    VALUE(no::EnrollmentFromApplied, x.cause, x.cursor)
    VALUE(no::OpeningClose, x.opening, x.cycle, x.side, x.enrollment)
    VALUE(no::OpeningsClose, x.openings, x.cycle, x.side, x.enrollment, x.external_openings)
    VALUE(no::CohortClose, x.cohort)
    VALUE(no::SelectedExposure, x.cycle, x.incarnations)
    VALUE(pineforge::execution::OpeningExposure, x.incarnation, x.cycle)
    VALUE(no::TrailTrack, x.best, x.activation_ordinal)
    VALUE(no::TrailActive, x.best_at_trigger, x.activation_ordinal)
    VALUE(no::AllowanceUnits, x.point_ordinal, x.initial, x.left)
    VALUE(no::AllowanceAllScope, x.point_ordinal)
    VALUE(no::AllowanceDeferred, x.point_ordinal)
    VALUE(no::PendingDeferred, x.total, x.count, x.tail_receipt)
    VALUE(no::PositionNonflat, x.cycle, x.side)
    VALUE(no::RequestDefinition, x.handle, x.request, x.birth, x.predecessor,
          x.origin, x.root, x.priority, x.kept_binding)
    VALUE(no::ExecutionTerms, x.resolved_price, x.units, x.shape, x.grid_policy)
    VALUE(no::TermsResolvedInput, x.price_kind, x.shared_cursor_collision, x.raw_price,
          x.default_resolved_price, x.terms)
    VALUE(no::AcceptedEvent, x.ordinal, x.definition, x.surface)
    VALUE(no::RejectedEvent, x.ordinal, x.request, x.reason, x.surface)
    VALUE(no::ReplacedEvent, x.ordinal, x.predecessor_definition, x.successor_definition, x.surface)
    VALUE(no::ReplaceRejectedEvent, x.ordinal, x.live_definition, x.attempted, x.reason, x.surface)
    VALUE(no::CancelledEvent, x.ordinal, x.definition, x.reason, x.cause,
          x.prior_authority, x.unexecuted, x.pending)
    VALUE(no::NotWorkingEvent, x.ordinal, x.target, x.attempted, x.surface)
    VALUE(no::InvalidHandleEvent, x.ordinal, x.target, x.attempted, x.surface)
    VALUE(no::NoEffectEvent, x.ordinal, x.definition, x.remaining, x.authority, x.cursor)
    VALUE(no::MatchRejectedEvent, x.ordinal, x.reason, x.definition, x.remaining,
          x.authority, x.cursor, x.attempted_terms)
    VALUE(no::ExecutionAppliedEvent, x.ordinal, x.definition, x.raw_price, x.resolved_price,
          x.current_ticket, x.first_trade_index, x.closed_trade_count, x.opened_lot_incarnation,
          x.closed_units, x.opened_units, x.filled_working, x.remaining_before, x.remaining_after,
          x.allowance_before, x.allowance_after, x.terminal, x.terminal_reason, x.cycle_before,
          x.cycle_after, x.scope, x.cursor)
    VALUE(no::CloseBoundEvent, x.ordinal, x.definition, x.cycle, x.side, x.cursor, x.before, x.after)
    VALUE(no::ActivatedEvent, x.ordinal, x.definition, x.kind, x.before, x.after,
          x.reached_price, x.cursor)
    VALUE(no::ReservationReducedEvent, x.ordinal, x.definition, x.cause, x.recipient,
          x.effect, x.requested_delta, x.actual_deduction, x.before, x.after)
    VALUE(no::DeferredGroupAdjustmentEvent, x.ordinal, x.definition, x.cause, x.recipient,
          x.effect, x.deferred_delta, x.pending_before, x.pending_after, x.previous_pending_receipt)
    VALUE(no::QuantityBoundEvent, x.ordinal, x.definition, x.source, x.source_units,
          x.prior_adjustment_ids, x.pending_total, x.effective_deduction, x.remaining)
    VALUE(no::ArmedEvent, x.ordinal, x.definition, x.before, x.after,
          x.enrollment, x.quantity_resolution)
    VALUE(no::TermsResolvedEvent, x.ordinal, x.definition, x.cursor, x.input,
          x.prior_adjustment_ids, x.pending_total, x.effective_deduction,
          x.remaining_before, x.remaining_after, x.allowance_after)
    VALUE(no::MarginCallEvent, x.ordinal, x.definition, x.applied, x.cursor, x.side,
          x.mark, x.equity, x.required, x.liquidation_price, x.units,
          x.position_before, x.position_after)
    VALUE(no::NativeRiskEvent, x.ordinal, x.kind, x.limit, x.observed, x.day_ordinal, x.cursor)
#undef VALUE
#undef EMPTY

    void add(const no::OpeningReadback& openings) {
        u(openings.count());
        for (std::uint64_t i = 0; i != openings.count(); ++i) {
            no::RequestHandle handle;
            status_is(openings.opening_at(i, handle), ScratchStatus::ok, "oracle opening input");
            add(handle);
        }
    }
};
Bytes native_value(const no::CommandEvent& event) {
    Wire result;
    result.add(event);
    return std::move(result.bytes);
}

// Independent admission field-path/order model, written from the declared
// values. In particular it NEVER calls admission::reflect/fields/sequence.
struct AdmissionModel {
    std::vector<ad::Field> rows;
    template<class T> void leaf(const std::string& path, const T& value) {
        if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, double>)
            rows.push_back({path, value});
        else if constexpr (std::is_same_v<T, bool> || std::is_unsigned_v<T>)
            rows.push_back({path, std::uint64_t(value)});
        else rows.push_back({path, std::int64_t(value)});
    }
    template<class T> void field(const std::string& path, const char* name, const T& value) {
        leaf(path + "." + name, value);
    }
    void birth(const std::string& p, const pineforge::OrderBirth& x) {
        field(p, "cause", x.cause()); field(p, "bar", x.bar()); field(p, "timestamp", x.timestamp());
        field(p, "cursor_domain", x.cursor().domain()); field(p, "cursor_position", x.cursor().position());
        field(p, "cursor_index", x.cursor().index()); field(p, "cursor_count", x.cursor().count());
        field(p, "cursor_price", x.cursor_price()); field(p, "first_fill", x.first_fill());
        field(p, "last_fill", x.last_fill()); field(p, "evaluation_ordinal", x.evaluation_ordinal());
    }
    void config(const std::string& p, const ad::Configuration& x) {
#define F(name) field(p, #name, x.name)
        F(process_on_close); F(calc_on_fills); F(magnifier); F(fill_recalculation); F(scheduler);
        F(slippage); F(pyramiding); F(default_quantity_type); F(default_quantity_value);
        F(long_margin); F(short_margin); F(commission_value); F(commission_type);
        F(pointvalue); F(fx); F(quantity_step); F(mintick); F(risk_direction); F(loss_days_limit);
        F(drawdown_limit); F(intraday_loss_limit); F(position_limit); F(fill_cap_active); F(risk_halted);
#undef F
    }
    void sizing(const std::string& p, const ad::SizingObservation& x) {
#define F(name) field(p, #name, x.name)
        F(quantity); F(equity); F(price); F(mark); F(fx);
#undef F
    }
    void observation(const std::string& p, const ad::CommandObservation& x) {
#define F(name) field(p, #name, x.name)
        F(command); F(kind); birth(p + ".birth", x.birth); F(id);
        F(requested_quantity); F(quantity_type); F(buy);
        field(p + ".prices", "limit", x.prices.limit); field(p + ".prices", "stop", x.prices.stop);
        F(oca_name); F(oca_type); config(p + ".configuration", x.configuration);
        F(bar); F(placement_side); F(placement_cycle); F(prior_close_quantity); F(held_quantity);
        F(held_entries); F(realized_equity); F(placement_equity); F(signal_close); F(quantized_fixed_quantity);
        field(p, "original_sizing_present", x.original_sizing.has_value());
        if (x.original_sizing) sizing(p + ".original_sizing", *x.original_sizing);
        F(explicit_equity); F(explicit_price);
#undef F
    }
    void receipt(const std::string& p, const ad::ReviewReceipt& x) {
        field(p, "sequence", x.sequence); field(p, "checkpoint", x.checkpoint);
        field(p, "bar", x.bar); field(p, "target_command", x.target_command);
    }
    void receipt(const std::string& p, const ad::SizingRevision& x) {
        field(p, "sequence", x.sequence); field(p, "cause_fill", x.cause_fill);
        field(p, "bar", x.bar); field(p, "target_command", x.target_command);
    }
    void draft(const std::string& p, const ad::Draft& x) {
        field(p, "observation_present", bool(x.observation()));
        if (x.observation()) observation(p + ".observation", *x.observation());
        field(p, "review_present", x.review().has_value());
        if (x.review()) receipt(p + ".review", *x.review());
        field(p, "sizing_revision_present", x.sizing_revision().has_value());
        if (x.sizing_revision()) receipt(p + ".sizing_revision", *x.sizing_revision());
    }
    void book(const std::string& p, const ad::BookObservation& x) {
#define F(name) field(p, #name, x.name)
        F(incarnation); F(priority); F(bar); F(type); F(placement_side); F(buy);
        F(id); F(oca_name); F(oca_type); birth(p + ".birth", x.birth);
        field(p + ".prices", "limit", x.prices.limit); field(p + ".prices", "stop", x.prices.stop);
        field(p + ".prices", "trail_points", x.prices.trail_points);
        field(p + ".prices", "trail_price", x.prices.trail_price);
        field(p + ".prices", "trail_offset", x.prices.trail_offset); draft(p + ".draft", x.draft);
#undef F
    }
    template<class T, class F> void array(const std::string& p, const std::vector<T>& values, F emit) {
        field(p, "size", std::uint64_t(values.size()));
        for (std::size_t i = 0; i != values.size(); ++i)
            emit(p + "[" + std::to_string(i) + "]", values[i]);
    }
    explicit AdmissionModel(const ad::Event& event) {
        const std::string p = "event";
        field(p, "kind", std::uint64_t(event.index()));
        std::visit([&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, ad::CommandEvent>) {
                observation(p + ".observation", *x.observation);
                field(p, "outcome", x.outcome); field(p, "admitted_incarnation", x.admitted_incarnation);
                array(p + ".before", x.before, [&](const auto& q, const auto& row) { book(q, row); });
                array(p + ".removed", x.removed,
                      [&](const auto& q, auto id) { field(q, "incarnation", id); });
            } else if constexpr (std::is_same_v<T, ad::ReviewEvent>) {
                receipt(p + ".receipt", x.receipt); config(p + ".configuration", x.configuration);
                field(p, "open_price", x.open_price); field(p, "position_side", x.position_side);
                field(p, "position_cycle", x.position_cycle);
                array(p + ".book", x.book, [&](const auto& q, const auto& row) { book(q, row); });
                array(p + ".reviewed", x.reviewed, [&](const auto& q, const auto& row) { book(q, row); });
                array(p + ".resolutions", x.resolutions, [&](const auto& q, const auto& row) {
                    field(q, "incarnation", row.incarnation); field(q, "kind", row.kind);
                    field(q, "peer_incarnation", row.peer_incarnation);
                    field(q, "own_priority", row.own_priority); field(q, "peer_priority", row.peer_priority);
                    field(q, "transaction_quantity", row.transaction_quantity);
                });
                array(p + ".causes", x.causes, [&](const auto& q, auto id) { field(q, "sequence", id); });
            } else {
                receipt(p + ".receipt", x.receipt); field(p, "incarnation", x.incarnation);
                sizing(p + ".before", x.before); sizing(p + ".after", x.after);
                field(p, "affordability_equity_before", x.affordability_equity_before);
                field(p, "affordability_equity_after", x.affordability_equity_after);
            }
        }, event);
    }
    Bytes storage() const {
        Wire result;
        result.u(rows.size()); // The ONLY outer framing: array of Field rows.
        for (const auto& row : rows) result.fields(row.path, row.value);
        return std::move(result.bytes);
    }
};

// Separate semantic models: storage retains exact double bits; v4 normalizes
// zeros/NaNs and has no storage row count, event tag or optional prefix bytes.
struct AdmissionFold {
    std::uint64_t h = seed;
    std::size_t count = 0;
    std::uint64_t last = 0;
    int rejected_bar = -1;
    void byte(unsigned value) { h = (h ^ value) * 1099511628211ULL; }
    void u(std::uint64_t value) {
        for (unsigned i = 0; i != 8; ++i) byte((value >> (8 * i)) & 255);
    }
    void s(const std::string& value) { u(value.size()); for (unsigned char c : value) byte(c); }
    void append(const ad::Event& event, std::uint64_t sequence) {
        for (const auto& row : AdmissionModel(event).rows) {
            s(row.path); u(row.value.index());
            std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::string>) s(value);
                else if constexpr (std::is_same_v<T, double>) {
                    const auto raw = bits(value);
                    const bool nan = (raw & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL
                                  && (raw & 0x000fffffffffffffULL) != 0;
                    u(value == 0 ? 0 : (nan ? 0x7ff8000000000000ULL : raw));
                } else u(static_cast<std::uint64_t>(value));
            }, row.value);
        }
        ++count; last = sequence;
        if (const auto* command = std::get_if<ad::CommandEvent>(&event)) {
            const auto outcome = command->outcome;
            if (outcome == ad::Outcome::RejectedIntradayCap
                || outcome == ad::Outcome::RejectedFrozenMarketCap
                || outcome == ad::Outcome::RejectedAffordability
                || outcome == ad::Outcome::RejectedPricedCap
                || outcome == ad::Outcome::OpeningRejectedReductionAdmitted)
                rejected_bar = command->observation->bar;
        }
    }
};
struct NativeFold {
    std::uint64_t h = seed;
    std::uint64_t count = 0;
    void word(std::uint64_t value) {
        h = (h ^ value) * 0x9e3779b97f4a7c15ULL;
        h ^= h >> 32;
    }
    void s(const std::string& value) {
        word(value.size());
        for (std::size_t at = 0; at < value.size(); at += 8) {
            std::uint64_t packed = 0;
            for (std::size_t i = 0; i != 8 && at + i < value.size(); ++i)
                packed |= std::uint64_t(static_cast<unsigned char>(value[at + i])) << (8 * i);
            word(packed);
        }
    }
    void event(std::uint64_t alternative, std::uint64_t compact_outcome) {
        word(alternative); word(compact_outcome); ++count;
    }
    void handle(const no::RequestHandle& value) {
        s(value.run.session_key.string()); word(0); word(value.incarnation);
    }
    void id(const no::EventId& value) {
        s(value.run.session_key.string()); word(0); word(value.ordinal);
    }
};

// Test-only private-member access, the pattern already used by the window
// tests. These observe existing folds/cursors; they add no production state.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
#define MEMBER(Tag, Owner, member) \
    struct Tag { friend auto access(Tag); }; \
    template struct Access<Tag, &Owner::member>
MEMBER(AdmissionRunFold, pineforge::source::PineExecutionAdapter, fold_admission_events);
MEMBER(AdmissionCount, pineforge::source::PineExecutionAdapter, admission_events_folded_);
MEMBER(AdmissionDigest, pineforge::source::PineExecutionAdapter, admission_events_digest_);
MEMBER(AdmissionLast, pineforge::source::PineExecutionAdapter, admission_events_last_);
MEMBER(NativeCore, pineforge::NativeExecutionConsumer, requests_);
MEMBER(NativeNote, pineforge::NativeExecutionConsumer, note_committed_events);
MEMBER(NativeNoteCohorts, pineforge::NativeExecutionConsumer, note_cohort_receipts);
MEMBER(NativeRetire, pineforge::NativeExecutionConsumer, retire_journal);
MEMBER(NativeRecords, pineforge::NativeExecutionConsumer, event_records_);
MEMBER(NativeRoots, pineforge::NativeExecutionConsumer, chain_roots_);
MEMBER(NativeGroups, pineforge::NativeExecutionConsumer, group_receipts_);
MEMBER(NativeCohorts, pineforge::NativeExecutionConsumer, cohort_receipts_);
MEMBER(NativeNotifications, pineforge::NativeExecutionConsumer, applied_notifications_);
MEMBER(NativePolicy, pineforge::NativeExecutionConsumer, retention_override_);
MEMBER(NativeLifecycleState, pineforge::NativeExecutionConsumer, state_);
MEMBER(NativeRunningCache, pineforge::NativeExecutionConsumer, cache_running_policy);
#undef MEMBER

void running_consumer(pineforge::NativeExecutionConsumer& consumer, const no::RunIdentity& run) {
    // This is a journal unit fixture, not a host/backtest. Give the existing
    // lifecycle its actual Running alternative and derived policy cache BEFORE
    // calling acknowledge_events: that method correctly ignores idle consumers.
    auto& running = (consumer.*access(NativeLifecycleState{})).emplace<pineforge::NativeRunning>();
    running.spec.identity = run;
    running.spec.event_retention = pineforge::NativeEventRetention::Window;
    (consumer.*access(NativeRunningCache{}))();
    (consumer.*access(NativeCore{})).reset(run);
    require(consumer.running(), "acknowledgement fixture has a real Running lifecycle alternative");
}

void fold(pineforge::source::PineExecutionAdapter& adapter) {
    (adapter.*access(AdmissionRunFold{}))();
}
void equal_fold(const pineforge::source::PineExecutionAdapter& adapter, const AdmissionFold& model) {
    require(adapter.*access(AdmissionCount{}) == model.count, "v4 lifetime count");
    require(adapter.*access(AdmissionDigest{}) == model.h, "independent v4 field fold");
    require(adapter.*access(AdmissionLast{}) == model.last, "v4 absolute sequence cursor");
}
void retire(pineforge::source::PineExecutionAdapter& adapter, const AdmissionFold& model) {
    adapter.admission_journal.retire_prefix(model.count, model.h, model.last);
}

// Observer accounting uses fixed counters, not an allocation per operation.
struct Charges {
    explicit Charges(pd::ScratchBackingKind kind = pd::ScratchBackingKind::test_memory)
        : expected_kind(kind) {}
    pd::ScratchBackingKind expected_kind;
    std::uint64_t reads = 0, writes = 0, syncs = 0, bytes = 0, errors = 0;
    std::uint64_t malformed = 0;
    static void charge(void* self, const pd::ScratchIoCharge& value) noexcept {
        auto& c = *static_cast<Charges*>(self);
        c.bytes += value.bytes;
        c.errors += value.status != ScratchStatus::ok;
        c.malformed += value.kind != c.expected_kind;
        if (value.operation == pd::ScratchIoOperation::read_page) ++c.reads;
        else if (value.operation == pd::ScratchIoOperation::write_page) ++c.writes;
        else ++c.syncs;
    }
    pd::ScratchIoObserver observer() { return {this, &charge}; }
};
class FaultBacking final : public pd::ScratchBacking {
public:
    FaultBacking(pd::ScratchBacking& inner, Charges& observer)
        : ScratchBacking(inner.kind(), inner.page_size(), observer.observer()), inner_(inner) {}
    std::uint64_t page_count() const noexcept override { return inner_.page_count(); }
    bool fail_reads = false, fail_writes = false, fail_syncs = false;
    std::uint64_t cap = std::numeric_limits<std::uint64_t>::max();
    mutable std::uint64_t read_calls = 0, read_faults = 0;
    std::uint64_t write_calls = 0, write_faults = 0, sync_calls = 0, sync_faults = 0;
protected:
    ScratchStatus do_read_page(pd::ScratchPageId id, ScratchBuffer out) const noexcept override {
        ++read_calls;
        if (fail_reads) { ++read_faults; return ScratchStatus::io_error; }
        return inner_.read_page(id, out);
    }
    ScratchStatus do_write_page(pd::ScratchPageId id, ScratchBytes in) noexcept override {
        ++write_calls;
        if (fail_writes || id >= cap) { ++write_faults; return ScratchStatus::io_error; }
        return inner_.write_page(id, in);
    }
    ScratchStatus do_sync(pd::ScratchDurability durability) noexcept override {
        ++sync_calls;
        if (fail_syncs) { ++sync_faults; return ScratchStatus::io_error; }
        return inner_.sync(durability);
    }
private:
    pd::ScratchBacking& inner_;
};
std::unique_ptr<pd::ScratchBacking> disk_backing(std::size_t page) {
    // The outer FaultBacking is the one observed boundary, including attempts
    // it refuses before reaching the medium. Avoid double-charging its inner
    // real disk provider; restore any previous thread registration on unwind.
    struct Registration {
        pd::ScratchIoObserver prior =
            pineforge::source::detail::set_command_scratch_io_observer({});
        ~Registration() { pineforge::source::detail::set_command_scratch_io_observer(prior); }
    } registration;
    return pineforge::source::detail::make_command_scratch_backing(page);
}
ScratchStatus numeric_order(void*, const pd::ScratchByteSource& a,
                            const pd::ScratchByteSource& b, int& order) noexcept {
    if (a.size() != 8 || b.size() != 8) return ScratchStatus::programming_error;
    std::array<std::byte, 8> left{}, right{};
    auto status = a.read(0, {left.data(), left.size()});
    if (status != ScratchStatus::ok) return status;
    status = b.read(0, {right.data(), right.size()});
    if (status != ScratchStatus::ok) return status;
    order = 0;
    for (std::size_t i = 8; i-- != 0;)
        if (left[i] != right[i]) {
            order = std::to_integer<unsigned>(left[i]) < std::to_integer<unsigned>(right[i]) ? -1 : 1;
            break;
        }
    return ScratchStatus::ok;
}
struct Rig {
    explicit Rig(std::size_t page_size = 512, bool use_disk = false, Bytes* provision = nullptr)
        : storage(use_disk || provision ? 0 : 32 * 1024 * 1024),
          charges(use_disk ? pd::ScratchBackingKind::disk : pd::ScratchBackingKind::test_memory),
          memory(provision ? ScratchBuffer{provision->data(), provision->size()}
                           : ScratchBuffer{storage.data(), storage.size()}, page_size),
          disk(use_disk ? disk_backing(page_size) : std::unique_ptr<pd::ScratchBacking>{}),
          backing(disk ? *disk : static_cast<pd::ScratchBacking&>(memory), charges), page(page_size) {}
    Bytes storage; // Fixture-only medium capacity, fixed BEFORE the heap census.
    Charges charges;
    pd::FixedMemoryScratchBacking memory;
    std::unique_ptr<pd::ScratchBacking> disk;
    FaultBacking backing;
    std::size_t page;
    pd::ScratchStoreConfig config(pd::ScratchOpenMode mode = pd::ScratchOpenMode::create_empty,
                                  std::size_t slots = 2) const {
        return {slots, page, 1, 1, {0x73336a6f75726e01ULL, nullptr, &numeric_order}, mode};
    }
    void audit(const CommandScratchStore& store) const {
        const auto s = store.stats();
        require(s.page_reads == charges.reads && s.page_writes == charges.writes
                && s.syncs == charges.syncs, "store counters equal every observed attempt");
        require(charges.reads == backing.read_calls && charges.writes == backing.write_calls
                && charges.syncs == backing.sync_calls, "one observer charge per actual attempt");
        require(charges.bytes == (charges.reads + charges.writes) * page,
                "requested page bytes, not logical payload bytes or cumulative extent");
        require(charges.malformed == 0, "observer reports the explicitly chosen real medium");
        require(s.backing_pages == backing.page_count()
                && s.backing_bytes == s.backing_pages * page, "actual backing extent proxy");
    }
};
struct StoreValue final : pd::ScratchByteSource {
    StoreValue(const pd::ScratchReadback& store, pd::ScratchHandle handle, std::uint64_t size)
        : store(store), handle(handle), length(size) {}
    const pd::ScratchReadback& store;
    pd::ScratchHandle handle;
    std::uint64_t length;
    std::uint64_t size() const noexcept override { return length; }
    ScratchStatus read(std::uint64_t offset, ScratchBuffer out) const noexcept override {
        return store.read(handle, pd::ScratchField::value, offset, out);
    }
};
// An arbitrarily long, independently spelled rejected attempt. Only its 48-byte
// prefix and 56-byte suffix exist in memory. The comment is a byte formula,
// so neither the producer nor its oracle owns a maximum-record-sized string.
class LongNativeAttempt final : public pd::ScratchByteSource {
public:
    LongNativeAttempt(std::uint64_t ordinal, std::uint64_t length) : length_(length) {
        require(length <= std::numeric_limits<std::uint64_t>::max() - 104,
                "procedural input length is representable");
        const std::array<std::uint64_t, 6> words{{1, ordinal, 2, bits(-0.0), 0, length}};
        for (std::size_t j = 0; j != words.size(); ++j)
            for (unsigned i = 0; i != 8; ++i)
                prefix_[8 * j + i] = static_cast<std::byte>((words[j] >> (8 * i)) & 255);
    }
    std::uint64_t size() const noexcept override { return 104 + length_; }
    ScratchStatus read(std::uint64_t offset, ScratchBuffer out) const noexcept override {
        if (offset > size() || out.size > size() - offset || (out.size && !out.data))
            return ScratchStatus::programming_error;
        for (std::size_t i = 0; i != out.size; ++i) {
            const auto at = offset + i;
            out.data[i] = at < prefix_.size() ? prefix_[static_cast<std::size_t>(at)]
                : at - prefix_.size() < length_
                    ? static_cast<std::byte>(((at - prefix_.size()) * 37 + 19) & 255)
                    : std::byte{0};
        }
        return ScratchStatus::ok;
    }
private:
    std::array<std::byte, 48> prefix_{};
    std::uint64_t length_;
};
void equals_bytes(const pd::ScratchByteSource& source, const Bytes& model) {
    require(source.size() == model.size(), "encoded logical length agrees with independent model");
    std::array<std::byte, 97> chunk{};
    for (std::size_t at = 0; at < model.size();) {
        const auto n = std::min(chunk.size(), model.size() - at);
        status_is(source.read(at, {chunk.data(), n}), ScratchStatus::ok, "cross-page value read");
        require(std::equal(chunk.begin(), chunk.begin() + n, model.begin() + at),
                "all active payload bytes, including exact zero/NaN bits and ordered arrays");
        at += n;
    }
}
pd::ScratchHandle put(CommandScratchStore& store, std::uint64_t position,
                      const pd::ScratchByteSource& value) {
    std::array<std::byte, 8> key{};
    for (unsigned i = 0; i != 8; ++i)
        key[i] = static_cast<std::byte>((position >> (8 * i)) & 255);
    const pd::ScratchSpanSource key_source({key.data(), key.size()});
    pd::ScratchHandle handle = pd::scratch_no_handle;
    status_is(store.begin(), ScratchStatus::ok, "begin record");
    status_is(store.put(key_source, value, handle), ScratchStatus::ok, "put codec record");
    status_is(store.commit(pd::ScratchDurability::buffered), ScratchStatus::ok, "buffered record commit");
    return handle;
}

std::size_t heap_peak = 0, heap_calls = 0, heap_requested = 0;
void record_bytes(std::size_t size) {
    heap_peak = std::max(heap_peak, global_allocation::live_bytes);
    ++heap_calls;
    heap_requested += size;
}
struct HeapCensus {
    HeapCensus() {
        require(!global_allocation::recording, "heap census windows must not overlap");
        global_allocation::live_bytes = 0;
        heap_peak = heap_calls = heap_requested = 0;
        global_allocation::on_recorded = &record_bytes;
        global_allocation::recording = true;
    }
    ~HeapCensus() {
        global_allocation::on_recorded = nullptr;
        global_allocation::recording = false;
    }
    std::size_t fixed = 0;
    void constructed() {
        fixed = global_allocation::live_bytes;
        heap_peak = std::max(heap_peak, fixed); // Never discard construction's transient peak.
    }
    void flat(const CommandScratchStore& a, const CommandScratchStore& b, std::size_t initial_w,
              std::size_t fixed_frames) const {
        require(a.stats().resident_bytes + b.stats().resident_bytes == initial_w,
                "both CommandScratchStore resident workspaces stay fixed");
        require(initial_w <= fixed, "W is included once in the measured construction heap");
        require(global_allocation::live_bytes <= fixed + fixed_frames
                && heap_peak <= fixed + fixed_frames,
                "every retained island and every transient fits the fixed byte census");
    }
};
struct AdmissionCursorWatch {
    inline static AdmissionCursorWatch* current = nullptr;
    const pineforge::source::PineExecutionAdapter& adapter;
    std::size_t lifetime;
    void (*previous)(std::size_t);
    bool zero_restart = false;
    std::size_t samples = 0;
    AdmissionCursorWatch(const pineforge::source::PineExecutionAdapter& value, std::size_t count)
        : adapter(value), lifetime(count), previous(global_allocation::on_recorded) {
        require(current == nullptr && global_allocation::recording, "one active real-allocation cursor probe");
        current = this;
        global_allocation::on_recorded = &allocated;
    }
    ~AdmissionCursorWatch() {
        current = nullptr;
        global_allocation::on_recorded = previous;
    }
    static void allocated(std::size_t size) {
        auto& watch = *current;
        if (watch.previous) watch.previous(size);
        ++watch.samples;
        if (watch.lifetime != 0 && watch.adapter.*access(AdmissionCount{}) == 0)
            watch.zero_restart = true;
    }
};

no::Request limit(double quantity = 1, double price = 75) {
    no::Request request{no::Transact{quantity}, "order", "input"};
    request.trigger = no::Limit{price};
    return request;
}
no::DefinitionRef definition(no::RunIdentity run, std::uint64_t incarnation,
                             no::Request request, std::uint64_t ordinal) {
    return std::make_shared<const no::RequestDefinition>(
        no::RequestDefinition{{run, incarnation}, std::move(request), {ordinal, -37},
                              std::nullopt, no::RequestOrigin::Host, std::nullopt, 0, std::nullopt});
}
std::vector<no::CommandEvent> native_examples(const no::RunIdentity& run) {
    no::Request request = limit();
    request.label = text(8209, 3);
    request.comment = text(12307, 5);
    auto before = definition(run, 17, request, 91);
    auto changed = std::make_shared<no::RequestDefinition>(*before);
    changed->request.trigger = no::Trail{0.0, -0.0, no::TrailTicks{3.25}, 102.5};
    changed->request.comment = text(12299, 7);
    changed->predecessor = no::RequestHandle{run, 9};
    changed->root = no::RequestHandle{run, 3};
    changed->priority = 29; // Two definition VALUES, with the SAME kept handle.
    changed->origin = no::RequestOrigin::KernelRisk;
    changed->kept_binding = no::PositionNonflat{-91, no::Side::Short};
    const no::DefinitionRef after = changed;
    const no::EventId cause{run, 83};
    const no::RequestHandle parent{run, 7};
    no::MatchCursor cursor;
    cursor.point = {997, -7, -3, -9000, -8999, 5001, 6007, 7009, 5000, 4999,
                    pineforge::NativePriceProvenance::ObservedPrint,
                    pineforge::NativePathPhase::None, pineforge::NativeCompletionKind::Confirmed};
    cursor.t = -0.0;
    const no::Authority book = no::BookClose{-8, no::Side::Short, cause, cursor};
    const no::PendingDeferred pending{12.5, 4099, {run, 79}};
    std::vector<no::EventId> adjustments;
    for (std::uint64_t i = 0; i != 1031; ++i) adjustments.push_back({run, 10000 + i * 3});

    std::vector<no::CommandEvent> events;
    events.emplace_back(no::AcceptedEvent{0, before, no::CommandSurface::MarketOnly});
    request.intent = no::Transact{binary64(0x7ff8000000000041ULL)};
    events.emplace_back(no::RejectedEvent{0, request,
        static_cast<no::RequestRejectReason>(233), no::CommandSurface::General});
    events.emplace_back(no::ReplacedEvent{0, before, after, no::CommandSurface::MarketOnly});
    request.intent = no::HostSized{no::HostSizedKind::Close, std::nullopt};
    events.emplace_back(no::ReplaceRejectedEvent{0, before, request,
        no::RequestRejectReason::InvalidOwner, no::CommandSurface::General});
    events.emplace_back(no::CancelledEvent{0, after, no::CancelReason::Group, cause, book,
                                         no::RemainingProjectionUnits{-0.0}, pending});
    events.emplace_back(no::NotWorkingEvent{0, parent, std::nullopt, no::CommandSurface::MarketOnly});
    events.emplace_back(no::InvalidHandleEvent{0, {{text(9001, 13), 999}, 17}, request,
                                             no::CommandSurface::General});
    events.emplace_back(no::NoEffectEvent{0, before, no::RemainingProjectionNoTarget{}, book, cursor});
    events.emplace_back(no::MatchRejectedEvent{0, no::MatchRejectReason::HostPrecommit, before,
        no::RemainingProjectionDeferred{}, no::Wait{parent}, cursor,
        no::ExecutionTerms{binary64(0x7ff8000000000083ULL), -0.0, no::OpeningShape::ReverseTo,
                           no::ExecutionGridPolicy::ExplicitUnits}});
    no::ExecutionAppliedEvent applied;
    applied.definition = after; applied.raw_price = -0.0; applied.resolved_price = 0.0;
    applied.current_ticket = 717.25; applied.first_trade_index = 9003;
    applied.closed_trade_count = 4097; applied.opened_lot_incarnation = 8193;
    applied.closed_units = 3.5; applied.opened_units = 1.25; applied.filled_working = 4.75;
    applied.remaining_before = no::RemainingProjectionUnits{91.5};
    applied.remaining_after = no::RemainingProjectionUnits{86.75};
    applied.allowance_before = no::AllowanceUnits{997, 12, 7.25};
    applied.allowance_after = no::AllowanceAllScope{998}; applied.terminal = false;
    applied.terminal_reason = no::AppliedTerminalReason::TargetExhausted;
    applied.cycle_before = -33; applied.cycle_after = 35;
    applied.scope = no::SelectedExposure{37, {31, 7, 901, 3}}; applied.cursor = cursor;
    events.emplace_back(applied);
    events.emplace_back(no::CloseBoundEvent{0, before, -71, no::Side::Short, cursor,
        no::UnboundBookClose{}, no::OpeningClose{parent, -71, no::Side::Short,
                                               no::EnrollmentFromCommand{cause}}});
    events.emplace_back(no::ActivatedEvent{0, after, no::ActivationKind::TrailTrigger,
        no::TrailTrack{201.75, 901}, no::TrailActive{199.5, 901}, 197.25, cursor});
    events.emplace_back(no::ReservationReducedEvent{0, before, cause, parent,
        no::GroupEffect::Reduce, 7.5, 6.25, {17.5}, no::RemainingProjectionUnits{11.25}});
    events.emplace_back(no::DeferredGroupAdjustmentEvent{0, after, cause, parent,
        no::GroupEffect::Reduce, 2.5, pending, {15, 4100, {run, 999}}, no::EventId{run, 79}});
    events.emplace_back(no::QuantityBoundEvent{0, after, cause, 100, adjustments, 12.5, 12.5,
                                              no::RemainingProjectionUnits{87.5}});
    events.emplace_back(no::ArmedEvent{0, before, no::Wait{parent},
        no::ArmedTransaction{parent, cause, cursor}, no::EnrollmentFromApplied{cause, cursor},
        no::EventId{run, 991}});
    events.emplace_back(no::TermsResolvedEvent{0, after, cursor,
        {no::NativeCandidatePriceKind::CurrentQuote, true, -0.0, 199.5,
         {201.75, 31.25, no::OpeningShape::CloseOpposite, no::ExecutionGridPolicy::ExplicitUnits}},
        adjustments, 12.5, 7.25, no::RemainingProjectionDeferred{},
        no::RemainingProjectionUnits{24}, no::AllowanceDeferred{997}});
    events.emplace_back(no::MarginCallEvent{0, after, cause, cursor, no::Side::Short,
        51.25, -77.5, 81.75, 57, 3.25, -11, -7.75});
    events.emplace_back(no::NativeRiskEvent{0, no::RiskLimitKind::MaxIntradayLoss,
        binary64(0x7ff8000000000041ULL), binary64(0xfff8000000000083ULL), -917, cursor});

    // Nested alternatives and optional absences are values too. Attempted
    // requests may contain unknown enum NUMBERS without being corrupt tags.
    const std::vector<no::OrderIntent> intents{
        no::Flatten{}, no::Reduce{no::OwnerOpenedUnits{}}, no::Reduce{no::ExplicitUnits{9}},
        no::Reduce{no::ScopeFraction{0.25, no::ScopeClaim::NetOfSiblings, no::ScopeBasis::AtAcceptance}},
        no::Transact{-0.0}, no::ReverseTo{-5},
        no::HostSized{no::HostSizedKind::Open, no::Side::Short},
        no::Sized{no::Side::Long, no::CashValue{321}, no::SizeTime::AtAcceptance,
                  no::SizePrice::SignalOnTick, no::ExecutionGridPolicy::ExplicitUnits, true},
        no::Sized{no::Side::Short, no::EquityFraction{0.125}}};
    const std::vector<no::Trigger> triggers{no::Market{}, no::Limit{31.5, true},
        no::Stop{33.75}, no::StopLimit{35, 34}, no::Trail{1.5, std::nullopt}};
    const std::vector<no::Owner> owners{no::Independent{},
        no::WaitForApplied{parent, no::NativeArmVisibility::PendingUntilArmed,
            no::NativeArmFirstMatch::AfterArmPrint, no::NativeArmScope::Book},
        no::BindOpening{parent, -3}, no::BindOpenings{{parent, {run, 19}}, -4, {}},
        no::BindCohort{{72}}};
    for (std::size_t i = 0; i != intents.size(); ++i) {
        auto attempted = request;
        attempted.intent = intents[i]; attempted.trigger = triggers[i % triggers.size()];
        attempted.owner = owners[i % owners.size()];
        attempted.capacity = i % 2 ? no::Capacity{no::PointBudget{3.25}}
                                   : no::Capacity{no::ImmediateRemaining{}};
        attempted.group = i % 2 ? no::Group{no::Member{41, -7, no::GroupEffect::Reduce}}
                                : no::Group{no::NoGroup{}};
        attempted.anchor = i % 2 ? no::TriggerAnchor{no::FromOwnerFill{-2.75, true,
                                                        no::NativeAnchorRounding::Directional}}
                                 : no::TriggerAnchor{no::Absolute{}};
        events.emplace_back(no::RejectedEvent{0, attempted,
            static_cast<no::RequestRejectReason>(199 + i), no::CommandSurface::General});
    }
    const std::vector<no::Authority> authorities{no::BookTransaction{}, no::Wait{parent},
        no::ArmedTransaction{parent, cause, cursor}, no::UnboundBookClose{}, book,
        no::OpeningClose{parent, -11, no::Side::Long, no::EnrollmentFromApplied{cause, cursor}},
        no::OpeningsClose{{parent, {run, 33}}, -12, no::Side::Short,
                           no::EnrollmentFromCommand{cause}, {}}, no::CohortClose{{73}}};
    const std::vector<no::RemainingProjection> remaining{no::RemainingProjectionUnbound{},
        no::RemainingProjectionFlattenAll{}, no::RemainingProjectionUnits{-0.0},
        no::RemainingProjectionDeferred{}, no::RemainingProjectionNoTarget{}};
    for (std::size_t i = 0; i != authorities.size(); ++i)
        events.emplace_back(no::CancelledEvent{0, before, no::CancelReason::OwnerGone,
            i % 2 ? std::optional<no::EventId>{cause} : std::nullopt, authorities[i],
            remaining[i % remaining.size()], i % 2 ? no::PendingAdjustments{pending}
                                                  : no::PendingAdjustments{no::PendingNone{}}});
    const std::vector<no::TriggerState> states{no::MarketReady{}, no::LimitReady{}, no::StopIdle{},
        no::StopActive{}, no::StopLimitPending{}, no::StopLimitLive{}, no::TrailWaitArm{},
        no::TrailTrack{101.5, 71}, no::TrailActive{103.75, 73}};
    for (std::size_t i = 0; i != states.size(); ++i)
        events.emplace_back(no::ActivatedEvent{0, before, no::ActivationKind::StopLimit,
            states[i], states[(i + 1) % states.size()], 97 + double(i), cursor});
    applied.definition.reset(); // Absent shared values and terminal optionals.
    applied.terminal = true; applied.terminal_reason.reset();
    applied.allowance_before = no::AllowanceUnset{};
    applied.scope = pineforge::execution::OpeningExposure{17, -71};
    events.emplace_back(applied);
    applied.scope = pineforge::execution::Book{};
    events.emplace_back(applied);
    for (std::size_t i = 0; i != events.size(); ++i)
        std::visit([&](auto& event) { event.ordinal = 101 + 3 * i; }, events[i]);
    return events;
}

ad::CommandEvent command(std::uint64_t sequence, int bar, std::size_t payload = 0,
                         ad::Outcome outcome = ad::Outcome::Admitted) {
    auto observation = std::make_shared<ad::CommandObservation>();
    observation->command = sequence; observation->kind = ad::CommandKind::Raw;
    observation->birth = pineforge::OrderBirth::chart_evaluation(bar, -9007 + bar);
    observation->id = text(payload, unsigned(sequence)); observation->requested_quantity = -0.0;
    observation->quantity_type = -17; observation->buy = true;
    observation->prices = {binary64(0x7ff8000000000041ULL), binary64(0xfff8000000000083ULL)};
    observation->oca_name = std::string("oca\0bytes", 9); observation->oca_type = -5;
    observation->configuration.process_on_close = true;
    observation->configuration.calc_on_fills = true; observation->configuration.slippage = -19;
    observation->configuration.default_quantity_value = 3.25;
    observation->configuration.mintick = 0.125; observation->configuration.fx = -0.0;
    observation->bar = bar; observation->placement_side = -3; observation->placement_cycle = -31;
    observation->prior_close_quantity = 7.5; observation->held_quantity = 8.75;
    observation->held_entries = -11; observation->realized_equity = 1013.25;
    observation->placement_equity = 997.5; observation->signal_close = 79.25;
    observation->quantized_fixed_quantity = 1.75;
    if (sequence % 2)
        observation->original_sizing = ad::SizingObservation{2.5, 991.25, 31.5, -0.0,
                                                            binary64(0x7ff80000000000a7ULL)};
    observation->explicit_equity = 711; observation->explicit_price = 93.75;
    return {std::move(observation), outcome, sequence * 7 + 1, {}, {}};
}
ad::Event admission_example(unsigned kind, std::uint64_t sequence, std::size_t book_count,
                             std::size_t payload = 0, int bar = 7) {
    auto original = command(1, bar, payload).observation;
    const auto book_row = [&](std::size_t i) {
        ad::BookObservation row;
        row.incarnation = 17 + 3 * i; row.priority = 73 - std::int64_t(i);
        row.bar = bar; row.type = -11; row.placement_side = -1; row.buy = i % 2;
        row.id = text(payload, unsigned(i + 17)); row.oca_name = text(19, unsigned(i));
        row.oca_type = -7; row.birth = pineforge::OrderBirth::direct_command(bar, -7001);
        row.prices = {-0.0, 0.0, binary64(0x7ff8000000000041ULL),
                      binary64(0x7ff8000000000083ULL), 5.25};
        if (i % 3 != 0) {
            row.draft.bind(original);
            if (i % 3 == 1)
                row.draft.reviewed({11, ad::Checkpoint::TerminalGross, bar, 1});
            row.draft.sizing_revised({13, 97, bar, 1});
        }
        return row;
    };
    if (kind == 0) {
        auto result = command(sequence, bar, payload, ad::Outcome::RejectedPricedCap);
        for (std::size_t i = 0; i != book_count; ++i) {
            result.before.push_back(book_row(i));
            result.removed.push_back(101 + 7 * (book_count - i));
        }
        return result;
    }
    if (kind == 1) {
        ad::ReviewEvent result;
        result.receipt = {sequence, ad::Checkpoint::TerminalGross, bar, 0};
        result.configuration = original->configuration;
        result.open_price = -0.0; result.position_side = -1; result.position_cycle = -71;
        for (std::size_t i = 0; i != book_count; ++i) {
            result.book.push_back(book_row(i));
            result.reviewed.push_back(book_row(book_count - i));
            result.resolutions.push_back({17 + i, static_cast<ad::ResolutionKind>(i % 3),
                71 + i, -std::int64_t(i + 1), std::int64_t(2 * i), 0.5 + double(i)});
            result.causes.push_back(3 + 11 * (book_count - i));
        }
        return result;
    }
    ad::SizingEvent result;
    result.receipt = {sequence, 103, bar, 1}; result.incarnation = 991;
    result.before = {3.5, -0.0, 0.0, binary64(0x7ff8000000000041ULL), 1.25};
    result.after = {2.75, 991, 71.5, binary64(0xfff8000000000083ULL), 1.5};
    result.affordability_equity_before = 983.25; result.affordability_equity_after = 977.5;
    return result;
}

struct NativeDriver {
    no::WorkingRequestCore& core;
    no::RunIdentity run;
    std::uint64_t incarnation = 1, ordinal = 1;
    no::RequestHandle submit(const no::Request& request) {
        const auto expected = no::RequestHandle{run, incarnation};
        const auto at = ordinal;
        const auto result = core.submit(request, 0, incarnation, ordinal);
        require(result.status == no::SubmitStatus::Accepted && result.handle
                && *result.handle == expected && result.event_ordinal == at,
                "submit preserves input identity and ordinal");
        return expected;
    }
    void install(no::Preparation<no::PreparedMutation> preparation) {
        require(std::holds_alternative<no::PreparedMutation>(preparation), "prepared mutation");
        auto installed = core.install_mutation(std::get<no::PreparedMutation>(std::move(preparation)));
        require(std::holds_alternative<no::Installed>(installed), "installed mutation");
        ordinal += std::get<no::Installed>(installed).events.count;
    }
    no::EventId fill(const no::RequestHandle& handle, double units) {
        no::EvaluationContext context;
        context.cursor.point.ordinal = ordinal + 1;
        context.cursor.point.effective_time_ms = 1000;
        context.cursor.point.provenance = pineforge::NativePriceProvenance::ObservedPrint;
        context.driver_class = no::DriverEligibilityClass::ObservedPrint;
        context.existing_matching_bit = true;
        no::TargetObservation target;
        target.current_position = no::PositionNonflat{1, no::Side::Long};
        auto evaluation = core.prepare_evaluation(handle, context, target, ordinal);
        if (std::holds_alternative<no::PreparedMutation>(evaluation)) install(std::move(evaluation));
        else require(std::holds_alternative<no::NoChange>(evaluation), "evaluation succeeds");
        no::ExecutionProposal proposal;
        proposal.cursor = context.cursor; proposal.raw_price = proposal.resolved_price = 100;
        proposal.physical_action = no::Transact{units};
        proposal.pre_fill = target.current_position; proposal.pre_target = target;
        proposal.inspected_opened_units = units; proposal.inspected_current_ticket = 1;
        auto prepared = core.prepare_execution(handle, proposal, ordinal);
        require(std::holds_alternative<no::PreparedExecution>(prepared), "prepared fill");
        no::CommittedExecutionFacts facts;
        facts.result.status = pineforge::execution::Status::Applied;
        facts.result.opened_units = units; facts.result.current_ticket = 1;
        facts.cycle_after = 1; facts.post_target = target; facts.committed_action = no::Transact{units};
        auto installed = core.install_execution(std::get<no::PreparedExecution>(std::move(prepared)), facts);
        require(std::holds_alternative<no::Installed>(installed), "installed fill");
        const auto range = std::get<no::Installed>(installed).events;
        ordinal += range.count;
        const auto& event = std::get<no::ExecutionAppliedEvent>(core.history_at(range.first_index));
        require(event.handle() == handle && event.filled_working == units
                && event.raw_price == 100 && event.resolved_price == 100 && event.terminal,
                "filled event agrees with submitted units and committed facts");
        return {run, event.ordinal};
    }
    std::uint64_t effect(const no::EventId& cause, const no::RequestHandle& recipient) {
        const auto at = ordinal;
        install(core.prepare_group_effect(cause, recipient, ordinal));
        require(ordinal == at + 1, "one ordered deferred receipt per effect");
        return at;
    }
    void resolve(const no::EventId& cause, const no::RequestHandle& parent,
                 const no::RequestHandle& child) {
        no::OpeningObservation observation{parent, 1, no::PositionNonflat{1, no::Side::Long}, true};
        install(core.prepare_owner_applied(cause, child, observation, ordinal));
    }
};

#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 1
// Spec: docs/scratch-store.md stage 2.3 (ecbc4d175e), LAB-O1SEAMS clause S3-O, PINEFORGE_O1S3_CLAUSE=1 — native/admission owners reset independently and handles remain run-qualified.
void s3_separate_journal_owners_and_reset_red() {
    // Ownership / S3-O. Input: colliding native/admission numbers, adapter
    // reset alone, then a fresh run reusing incarnation 1. Assertion: native
    // bytes/frontiers survive adapter reset; neither old prefix nor old-run
    // handle becomes current. Fails to compile: bind_journal_codec.
    Rig native_rig, admission_rig;
    CommandScratchStore native_store(native_rig.config(), &native_rig.backing);
    CommandScratchStore admission_store(admission_rig.config(), &admission_rig.backing);
    const no::RunIdentity first{"s3-owners", 41}, second{"s3-owners", 42};
    no::WorkingRequestCore core(first);
    pineforge::source::PineExecutionAdapter adapter;
    bind_journal_codec(core, native_store);
    bind_journal_codec(adapter.admission_journal, admission_store);
    NativeDriver driver{core, first};
    const auto input = limit(3.5, 71.25);
    const auto handle = driver.submit(input);
    no::RequestDefinition expected{handle, input, {1, 0}, {}, no::RequestOrigin::Host, {}, 0, {}};
    const no::CommandEvent native_model = no::AcceptedEvent{
        1, std::make_shared<const no::RequestDefinition>(expected), no::CommandSurface::General};
    const auto sequence = adapter.admission_journal.next_sequence();
    require(sequence == handle.incarnation, "fixture really collides in the two number domains");
    const ad::Event admitted = command(sequence, 17, 9001, ad::Outcome::RejectedAffordability);
    AdmissionFold admission_model;
    admission_model.append(admitted, sequence);
    adapter.admission_journal.append(admitted);
    fold(adapter); equal_fold(adapter, admission_model); retire(adapter, admission_model);
    const auto native_end = core.history_end();
    const auto native_base = core.history_base();
    adapter.reset_for_run();
    require(core.history_end() == native_end && core.history_base() == native_base,
            "source reset has no native ownership or retention authority");
    require(native_value(core.history_at(0)) == native_value(native_model),
            "source record and prefix never alias the native event");
    require(adapter.admission_journal.sequence_frontier() == 1
            && adapter.admission_journal.event_count() == 0
            && adapter.admission_journal.retired_prefix().count == 0,
            "adapter run reset removes admission sequence and prefix state");
    require(pineforge::compat::pine::last_rejected_command_bar(adapter.admission_journal) == -1,
            "old rejection gate is not current after reset");
    core.reset(second);
    NativeDriver next{core, second};
    const auto reused = next.submit(limit(8.25, 83.5));
    require(reused.incarnation == handle.incarnation && reused.run != handle.run,
            "incarnation reuse remains qualified by run identity");
    require(core.event_at({first, 1}) == nullptr && core.find_live(handle) == nullptr,
            "old-run record and handle are absent");
    require(core.history_base() == 0 && core.history_end() == 1,
            "new run owns a new absolute-position domain");
    const auto& current = std::get<no::AcceptedEvent>(core.history_at(0));
    require(current.handle() == reused && std::get<no::Limit>(current.request().trigger).price == 83.5,
            "new record retains the new input rather than a reused backing value");
    const ad::Event new_admission = command(adapter.admission_journal.next_sequence(), 3, 13);
    adapter.admission_journal.append(new_admission);
    const auto& actual = adapter.admission_journal.events();
    require(actual.size() == 1 && AdmissionModel(actual.front()).storage()
            == AdmissionModel(new_admission).storage(), "only current-run source value reads back");
    native_rig.audit(native_store); admission_rig.audit(admission_store);
    require(native_store.stats().page_writes != 0 && admission_store.stats().page_writes != 0,
            "both owners actually used their explicitly borrowed stores");
}
#endif

struct ModelWalk {
    explicit ModelWalk(const std::vector<Bytes>& input) : values(&input) {}
    explicit ModelWalk(const Bytes& input) : single(&input) {}
    const std::vector<Bytes>* values = nullptr;
    const Bytes* single = nullptr;
    std::size_t index = 0;
    bool bad = false;
    ScratchStatus error = ScratchStatus::ok;
    static bool record(void* self, const pd::ScratchRecordView& row) noexcept {
        auto& c = *static_cast<ModelWalk*>(self);
        const auto count = c.values ? c.values->size() : std::size_t{1};
        if (c.index == count) { c.bad = true; return false; }
        const auto& want = c.values ? (*c.values)[c.index] : *c.single;
        ++c.index;
        if (row.value.size() != want.size()) { c.bad = true; return false; }
        std::array<std::byte, 97> bytes{};
        for (std::size_t at = 0; at != want.size();) {
            const auto n = std::min(bytes.size(), want.size() - at);
            c.error = row.value.read(at, {bytes.data(), n});
            if (c.error != ScratchStatus::ok) return false;
            if (!std::equal(bytes.begin(), bytes.begin() + n, want.begin() + at)) {
                c.bad = true; return false;
            }
            at += n;
        }
        return true;
    }
};
struct SourceWalk {
    const pd::ScratchByteSource& expected;
    std::size_t calls = 0;
    bool bad = false;
    ScratchStatus error = ScratchStatus::ok;
    static bool record(void* self, const pd::ScratchRecordView& row) noexcept {
        auto& c = *static_cast<SourceWalk*>(self);
        ++c.calls;
        if (c.calls != 1 || row.value.size() != c.expected.size()) { c.bad = true; return false; }
        std::array<std::byte, 97> actual{}, want{};
        for (std::uint64_t at = 0; at != c.expected.size();) {
            const auto n = static_cast<std::size_t>(
                std::min<std::uint64_t>(actual.size(), c.expected.size() - at));
            c.error = row.value.read(at, {actual.data(), n});
            if (c.error != ScratchStatus::ok) return false;
            c.error = c.expected.read(at, {want.data(), n});
            if (c.error != ScratchStatus::ok) return false;
            if (!std::equal(actual.begin(), actual.begin() + n, want.begin())) {
                c.bad = true; return false;
            }
            at += n;
        }
        return true;
    }
};
void note(pineforge::NativeExecutionConsumer& consumer, std::size_t first);

#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 4
// Spec: docs/scratch-store.md stage 2.3 (ecbc4d175e), LAB-O1SEAMS clause S3-M, PINEFORGE_O1S3_CLAUSE=4 — fixed heap/workspace bounds hold as pinned journals, captures and record lengths grow.
void s3_journal_islands_fixed_under_pins_red() {
    // Fixed memory / S3-M. Input: fixed two-slot stores, separately increasing
    // J and text length, delayed acknowledgement, a live deferred chain, and
    // many real CommandCaptures on one bar. Assertion: EVERY allocation peak,
    // including captures/Drafts and cleared peak capacity, stays within the
    // enumerated fixed owners/frames while stats report backing spill. Fails
    // to compile: bind_journal_codec; streaming append_journal_record and
    // journal_readback are also missing.
    constexpr std::size_t population = 1025;
    const no::RunIdentity run{"s3-islands", 71}; // Intern before measuring.
    const auto parent_input = limit(4096, 50);
    std::vector<Bytes> admission_values;
    for (std::uint64_t i = 1; i <= population; ++i) {
        auto observation = std::make_shared<ad::CommandObservation>();
        observation->command = i; observation->bar = 7; observation->requested_quantity = 4;
        if (i == 1) {
            observation->original_sizing = ad::SizingObservation{4, 1000, 100, 100, 1};
            observation->explicit_equity = 1000; observation->explicit_price = 100;
        }
        admission_values.push_back(AdmissionModel(ad::CommandEvent{observation,
            i == 1 ? ad::Outcome::Admitted : ad::Outcome::NoAdmission, 0, {}, {}}).storage());
    }
    // Fixed test inputs for the length axis: source bytes are borrowed, never
    // transferred to the journal or allocated as a producer-side whole record.
    std::vector<Bytes> long_values;
    for (std::size_t length : {std::size_t{17}, std::size_t{4099}, std::size_t{65539}})
        long_values.push_back(AdmissionModel(command(1, 7, length)).storage());
    Rig native_rig, admission_rig;
    HeapCensus heap; // Starts before construction of EVERY consumer owner/store.
    {
        CommandScratchStore native_store(native_rig.config(), &native_rig.backing);
        CommandScratchStore admission_store(admission_rig.config(), &admission_rig.backing);
        pineforge::NativeExecutionConsumer consumer;
        auto& core = consumer.*access(NativeCore{});
        running_consumer(consumer, run);
        pineforge::source::PineExecutionAdapter adapter;
        bind_journal_codec(core, native_store);
        bind_journal_codec(adapter.admission_journal, admission_store);
        consumer.acknowledge_events(0);
        NativeDriver driver{core, run};
        ad::Draft held_draft;
        std::array<std::byte, 512> outer{}, inner{};
        // Requested-byte B_u ledger (allocator A and physical M are deliberately
        // NOT guessed): the observed construction blocks, plus these fixed
        // simultaneous frames. Four shared definitions/observations and their
        // control blocks, four command/live frames, two capture frames, fixed
        // short-text staging and two disjoint caller pages. No term uses J,
        // a maximum record length, a bar population or a retained peak.
        constexpr std::size_t frames =
            4 * (sizeof(no::RequestDefinition) + sizeof(ad::CommandObservation)
                 + 4 * sizeof(void*))
            + 4 * (sizeof(no::CommandEvent) + sizeof(no::LiveRequest))
            + 2 * sizeof(ad::CommandCapture) + 512;
        const auto w = native_store.stats().resident_bytes + admission_store.stats().resident_bytes;
        heap.constructed();
        const auto census = [&] {
            heap.flat(native_store, admission_store, w, frames);
        };
        const auto parent = driver.submit(parent_input);
        no::Request child_input{no::Reduce{no::OwnerOpenedUnits{}}, "child", ""};
        child_input.owner = no::WaitForApplied{parent};
        child_input.group = no::Member{1, 2, no::GroupEffect::Reduce};
        const auto child = driver.submit(child_input);
        std::optional<no::EventId> previous;
        std::size_t noted = 0;
        for (std::size_t i = 0; i != population; ++i) {
            auto source = limit(0.5, 60);
            source.group = no::Member{1, 1, no::GroupEffect::Reduce};
            const auto filler = driver.submit(source);
            const auto cause = driver.fill(filler, 0.5);
            const auto receipt = driver.effect(cause, child);
            const auto* event = std::get_if<no::DeferredGroupAdjustmentEvent>(
                core.event_at({run, receipt}));
            require(event && event->cause == cause && event->recipient == child
                    && event->previous_pending_receipt == previous
                    && event->pending_after.count == i + 1
                    && event->pending_after.total == 0.5 * double(i + 1),
                    "independent chain link, order, count and sum model");
            previous = no::EventId{run, receipt};
            ad::CommandObservation observation;
            observation.bar = 7; observation.requested_quantity = 4;
            auto allocation = adapter.admission_journal.reserve();
            observation.command = allocation.sequence();
            require(observation.command == i + 1, "capture allocation matches the independent model");
            {
                ad::CommandCapture capture(std::move(allocation), observation, {},
                    [&](ad::CommandEvent event) { adapter.admission_journal.append(std::move(event)); });
                if (i == 0)
                    capture.bind(held_draft, ad::SizingObservation{4, 1000, 100, 100, 1}, 1000, 100);
            }
            require(adapter.admission_journal.event_count() == i + 1,
                    "all same-bar admission captures completed without truncation");
            note(consumer, noted); noted = core.history_end();
            (consumer.*access(NativeRetire{}))();
            require(core.history_base() == 0 && core.history_end() == 2 + 3 * (i + 1),
                    "delayed acknowledgement pins the exact growing native population");
            if (i == 16 || i == 256 || i + 1 == population) census();
        }
        require(core.history_end() - core.history_base() >= population
                && core.group_effect_receipt_count() == population,
                "a long pinned native suffix really exists");
        require(held_draft.observation() && held_draft.observation()->bar == 7,
                "a still-referenced Draft allocation remains in the heap census");
        ModelWalk captured(admission_values);
        status_is(journal_readback(adapter.admission_journal).for_each(
                      {outer.data(), outer.size()}, &ModelWalk::record, &captured),
                  ScratchStatus::ok, "stream every completed capture without collecting it");
        require(!captured.bad && captured.index == population,
                "all same-bar observations match the independent value/order model");
        census();
        consumer.acknowledge_events(std::numeric_limits<std::uint64_t>::max());
        (consumer.*access(NativeRetire{}))();
        require(core.history_base() == 4, "acknowledgement releases only the prefix before the chain");
        census();
        fold(adapter); // Fold while the entire same-bar source tail is present.
        const auto count = adapter.*access(AdmissionCount{});
        const auto digest = adapter.*access(AdmissionDigest{});
        const auto last = adapter.*access(AdmissionLast{});
        adapter.admission_journal.retire_prefix(count, digest, last);
        census(); // Includes retained vector capacity after clear, not just size.
        const auto parent_fill = driver.fill(parent, 4096);
        driver.resolve(parent_fill, parent, child);
        note(consumer, noted);
        consumer.acknowledge_events(std::numeric_limits<std::uint64_t>::max());
        (consumer.*access(NativeRetire{}))();
        require(core.history_base() == core.history_end(), "resolved native suffix drains completely");
        census();
        held_draft = ad::Draft{};
        census(); // Releasing a shared observation cannot hide a retained island.
        adapter.reset_for_run();
        for (std::uint64_t i = 1; i <= population; ++i)
            require(adapter.admission_journal.next_sequence() == i,
                    "outstanding allocation model is a dense ascending frontier");
        require(adapter.admission_journal.has_unfinished_events(),
                "the outstanding-sequence population really exists before completion");
        census(); // Includes every outstanding node/bucket/capacity, not caller handles.
        for (std::uint64_t i = population; i != 0; --i) {
            auto observation = std::make_shared<ad::CommandObservation>();
            observation->command = i; observation->bar = 7; observation->requested_quantity = 4;
            if (i == 1) {
                observation->original_sizing = ad::SizingObservation{4, 1000, 100, 100, 1};
                observation->explicit_equity = 1000; observation->explicit_price = 100;
            }
            adapter.admission_journal.append(ad::CommandEvent{std::move(observation),
                i == 1 ? ad::Outcome::Admitted : ad::Outcome::NoAdmission, 0, {}, {}});
        }
        require(!adapter.admission_journal.has_unfinished_events(), "reverse completion consumes all allocations");
        ModelWalk reversed(admission_values);
        status_is(journal_readback(adapter.admission_journal).for_each(
                      {outer.data(), outer.size()}, &ModelWalk::record, &reversed),
                  ScratchStatus::ok, "outstanding population completes in sequence order");
        require(!reversed.bad && reversed.index == population,
                "reverse completion preserves every independent capture value");
        census();
        std::uint64_t new_run = run.run_number;
        for (const auto& expected : long_values) {
            adapter.reset_for_run();
            require(adapter.admission_journal.next_sequence() == 1, "length phase allocation");
            const pd::ScratchSpanSource input(view(expected));
            status_is(append_journal_record(adapter.admission_journal, input), ScratchStatus::ok,
                      "stream admission record larger than the entire cache");
            ModelWalk one(expected);
            status_is(journal_readback(adapter.admission_journal).for_each(
                          {outer.data(), outer.size()}, &ModelWalk::record, &one),
                      ScratchStatus::ok, "stream the long value in fixed caller pages");
            require(!one.bad && one.index == 1, "long value remains exact in backing");
            require(adapter.admission_journal.event_count() == 1,
                    "record-length axis does not change the population axis");
            core.reset({run.session_key, ++new_run});
            const LongNativeAttempt native_input(1, expected.size());
            status_is(append_journal_record(core, native_input), ScratchStatus::ok,
                      "stream native text independently of J");
            SourceWalk native_one{native_input};
            status_is(journal_readback(core).for_each({inner.data(), inner.size()},
                          &SourceWalk::record, &native_one),
                      ScratchStatus::ok, "stream native long value without a resident string");
            require(!native_one.bad && native_one.calls == 1 && core.history_end() == 1,
                    "native record-length axis preserves its independent value");
            census();
        }
        native_rig.audit(native_store); admission_rig.audit(admission_store);
        require(native_store.stats().backing_pages > 2 && admission_store.stats().backing_pages > 2,
                "fixed slots spill both populations to observed backing");
    }
}
#endif

#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 2
// Spec: docs/scratch-store.md stage 2.3 (ecbc4d175e), LAB-O1SEAMS clause S3-NR, PINEFORGE_O1S3_CLAUSE=2 — native values round-trip fieldwise in absolute order after eviction and malformed framing is corrupt.
void s3_native_codec_all_values_cross_page_red() {
    // Records and ordering / S3-NR. Input: all 19 alternatives, every nested
    // request/authority/state alternative, independent kept-handle definitions,
    // >cache arrays/text, NULs, unknown reason numbers, zeros and NaN payloads.
    // Assertion: storage equals Wire and diagnostic decode equals every active
    // value in absolute order after eviction; malformed framing is corrupt.
    // Fails to compile: encode_journal_record (also decode_journal_record).
    const no::RunIdentity run{text(2053, 23), 83};
    const auto events = native_examples(run);
    std::array<bool, 19> witnessed{};
    for (const auto& event : events) witnessed[event.index()] = true;
    require(std::all_of(witnessed.begin(), witnessed.end(), [](bool x) { return x; }),
            "fixture populates every native alternative");
    for (const auto page : {std::size_t{512}, std::size_t{2048}}) {
        Rig rig(page);
        CommandScratchStore store(rig.config(), &rig.backing);
        status_is(store.status(), ScratchStatus::ok, "native codec store");
        std::vector<pd::ScratchHandle> handles;
        std::vector<Bytes> model;
        for (std::size_t i = 0; i != events.size(); ++i) {
            model.push_back(native_value(events[i]));
            auto encoded = encode_journal_record(events[i]);
            equals_bytes(encoded, model.back());
            handles.push_back(put(store, 37 + i, encoded));
        }
        std::array<std::byte, 2048> page_bytes{};
        for (std::size_t i = 0; i != events.size(); ++i) {
            pd::ScratchRecordInfo info;
            status_is(store.point(handles[i], info), ScratchStatus::ok, "evicted native record");
            StoreValue bytes(store, handles[i], info.value_bytes);
            equals_bytes(bytes, model[i]);
            no::CommandEvent decoded;
            status_is(decode_journal_record(bytes, {page_bytes.data(), page}, decoded),
                      ScratchStatus::ok, "explicit diagnostic native decode");
            require(native_value(decoded) == model[i], "decoded native active values, not a compact digest");
            require(std::visit([](const auto& x) { return x.ordinal; }, decoded) == 101 + 3 * i,
                    "absolute journal order preserves gapped command ordinals");
        }
        // Changing sharing/control blocks cannot change a replacement's bytes.
        auto replacement = std::get<no::ReplacedEvent>(events[2]);
        replacement.predecessor_definition =
            std::make_shared<const no::RequestDefinition>(*replacement.predecessor_definition);
        replacement.successor_definition =
            std::make_shared<const no::RequestDefinition>(*replacement.successor_definition);
        const no::CommandEvent unshared = replacement;
        auto unshared_source = encode_journal_record(unshared);
        equals_bytes(unshared_source, model[2]);
        require(replacement.predecessor_definition->handle == replacement.successor_definition->handle
                && replacement.predecessor_definition->request.comment
                    != replacement.successor_definition->request.comment,
                "kept-handle definitions have distinct complete values");

        // Invalid structural tag, presence, truncation and overflowing length.
        std::vector<Bytes> corrupt;
        corrupt.push_back(Bytes(8, std::byte{0xff}));
        auto bad_presence = native_value(no::AcceptedEvent{71, {}, no::CommandSurface::General});
        bad_presence[16] = std::byte{2};
        corrupt.push_back(std::move(bad_presence));
        auto truncated = model[2]; truncated.pop_back(); corrupt.push_back(std::move(truncated));
        no::RejectedEvent simple{73, limit(), no::RequestRejectReason::InvalidTrigger,
                                 no::CommandSurface::General};
        auto impossible_length = native_value(simple);
        Wire prefix;
        prefix.u(1); prefix.u(simple.ordinal); prefix.add(simple.request.intent);
        std::fill_n(impossible_length.begin() + prefix.bytes.size(), 8, std::byte{0xff});
        corrupt.push_back(std::move(impossible_length));
        auto bad_bool = native_value(simple);
        Wire through_limit;
        through_limit.u(1); through_limit.u(simple.ordinal); through_limit.add(simple.request.intent);
        through_limit.fields(simple.request.label, simple.request.comment);
        through_limit.u(1); through_limit.add(std::get<no::Limit>(simple.request.trigger).price);
        bad_bool[through_limit.bytes.size()] = std::byte{3};
        corrupt.push_back(std::move(bad_bool));
        auto narrow_enum = native_value(simple);
        // Reason and surface are the final two LE64 fields. 256 cannot be
        // represented in RequestRejectReason's original uint8_t.
        std::fill_n(narrow_enum.end() - 16, 8, std::byte{0});
        narrow_enum[narrow_enum.size() - 15] = std::byte{1};
        corrupt.push_back(std::move(narrow_enum));
        for (const auto& invalid : corrupt) {
            const pd::ScratchSpanSource bytes(view(invalid));
            no::CommandEvent output;
            status_is(decode_journal_record(bytes, {page_bytes.data(), page}, output),
                      ScratchStatus::corrupt, "invalid stored native framing");
        }
        const pd::ScratchSpanSource valid(view(model[0]));
        no::CommandEvent output;
        status_is(decode_journal_record(valid, {nullptr, page}, output),
                  ScratchStatus::programming_error, "null codec caller page is misuse");
        require(store.stats().backing_pages > 2 && store.stats().page_reads != 0,
                "records and eviction exceed the resident cache");
        rig.audit(store);
    }
}
#endif

#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 3
// Spec: docs/scratch-store.md stage 2.3 (ecbc4d175e), LAB-O1SEAMS clause S3-AR, PINEFORGE_O1S3_CLAUSE=3 — allocated admission values retain every field in sequence order; abandoned, reused and unallocated ids are refused.
void s3_admission_codec_allocations_and_order_red() {
    // Records and ordering / S3-AR. Input: allocated sequences completed in
    // reverse, one abandoned allocation, all kinds and large nested Draft/book
    // arrays. Assertion: only allocated completed values occur, in ascending
    // sequence with the legal gap, and every field/path/array matches the
    // independent model. Fails to compile: bind_journal_codec (also the
    // admission encode_journal_record/decode_journal_record overloads).
    Rig rig;
    CommandScratchStore store(rig.config(), &rig.backing);
    ad::Journal journal;
    bind_journal_codec(journal, store);
    std::vector<ad::Allocation> allocations;
    std::vector<std::pair<std::uint64_t, ad::Event>> model;
    constexpr std::size_t count = 19, abandoned = 7;
    allocations.reserve(count);
    for (std::size_t i = 0; i != count; ++i) allocations.emplace_back(journal.reserve());
    for (std::size_t i = 0; i != count; ++i) {
        require(allocations[i].sequence() == i + 1, "independent allocated sequence model");
        if (i != abandoned)
            model.emplace_back(i + 1, admission_example(unsigned(i % 3), i + 1,
                                                       i % 3 == 2 ? 0 : 17, 1031));
    }
    journal.abandon(allocations[abandoned].sequence());
    for (auto at = model.rbegin(); at != model.rend(); ++at) journal.append(at->second);
    allocations.clear(); // Release capture ownership; completed ids stay consumed.
    require(!journal.has_unfinished_events(), "every reservation completed or was abandoned");
    const auto& actual = journal.events(); // Explicit, unmeasured diagnostic surface.
    require(actual.size() == model.size(), "no abandoned event or duplicate completion");
    for (std::size_t i = 0; i != model.size(); ++i) {
        require(AdmissionModel(actual[i]).storage() == AdmissionModel(model[i].second).storage(),
                "field values, paths, variant index and nested order in ascending allocation sequence");
        require(model[i].first != abandoned + 1, "the reference sequence has its deliberate gap");
    }
    std::array<std::byte, 512> page{};
    for (const auto& expected : model) {
        const auto bytes = AdmissionModel(expected.second).storage();
        auto encoded = encode_journal_record(expected.second);
        equals_bytes(encoded, bytes); // No duplicate outer event tag/presence byte.
        const pd::ScratchSpanSource source(view(bytes));
        ad::Event decoded;
        status_is(decode_journal_record(source, {page.data(), page.size()}, decoded),
                  ScratchStatus::ok, "explicit diagnostic admission decode");
        require(AdmissionModel(decoded).storage() == bytes,
                "every reflected active value and array order survives admission decoding");
    }
    std::vector<Bytes> malformed;
    auto bad_count = AdmissionModel(model.front().second).storage();
    std::fill_n(bad_count.begin(), 8, std::byte{0xff});
    malformed.push_back(std::move(bad_count));
    auto bad_value_tag = AdmissionModel(model.front().second).storage();
    Wire first_path;
    first_path.u(AdmissionModel(model.front().second).rows.size());
    first_path.add(std::string("event.kind"));
    std::fill_n(bad_value_tag.begin() + first_path.bytes.size(), 8, std::byte{0xff});
    malformed.push_back(std::move(bad_value_tag));
    auto short_value = AdmissionModel(model.back().second).storage();
    short_value.pop_back(); malformed.push_back(std::move(short_value));
    for (const auto& invalid : malformed) {
        const pd::ScratchSpanSource source(view(invalid));
        ad::Event decoded;
        status_is(decode_journal_record(source, {page.data(), page.size()}, decoded),
                  ScratchStatus::corrupt, "invalid admission field-array framing");
    }
    const auto frontier = journal.sequence_frontier();
    engine_invariant([&] { journal.append(model.front().second); });
    engine_invariant([&] { journal.append(command(frontier + 37, 7)); });
    engine_invariant([&] { journal.append(command(abandoned + 1, 7)); });
    require(journal.sequence_frontier() == frontier && journal.event_count() == model.size(),
            "unallocated/reused/abandoned ids change neither values nor sequence frontier");
    require(store.stats().backing_pages > 2 && store.stats().page_writes > model.size(),
            "admission values larger than the cache actually reached backing");
    rig.audit(store);
}
#endif

void note(pineforge::NativeExecutionConsumer& consumer, std::size_t first) {
    const auto& core = consumer.*access(NativeCore{});
    (consumer.*access(NativeNote{}))({first, core.history_end() - first});
}

#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 5
// Spec: docs/scratch-store.md stage 2.3 (ecbc4d175e), LAB-O1SEAMS clause S3-NW, PINEFORGE_O1S3_CLAUSE=5 — Window retirement respects acknowledgements, queued causes and pending chains without renumbering retained positions or totals.
void s3_native_window_dependency_frontier_red() {
    // Transactions and retain windows / S3-NW. Input: ack zero, above/below
    // high water, a queued applied cause, and an unresolved then resolved
    // chain, under all three policies. Assertion: Window's base is exactly
    // min(ack, queued-cause-1, chain-head-1); absolute order/totals and pinned
    // values survive each boundary. Fails to compile: bind_journal_codec.
    const no::RunIdentity run{"s3-native-window", 97};
    constexpr std::size_t links = 257;
    for (auto policy : {pineforge::NativeEventRetention::Full,
                        pineforge::NativeEventRetention::Commands,
                        pineforge::NativeEventRetention::Window}) {
        Rig rig;
        CommandScratchStore store(rig.config(), &rig.backing);
        pineforge::NativeExecutionConsumer consumer;
        consumer.set_retention_override(policy);
        auto& core = consumer.*access(NativeCore{});
        running_consumer(consumer, run);
        bind_journal_codec(core, store);
        NativeDriver driver{core, run};
        // Independent absolute row model: command inputs, not a captured copy
        // of history(). Every source commits Accepted, Applied, Deferred.
        std::vector<std::pair<std::uint64_t, unsigned>> rows;
        std::vector<no::EventId> receipts;
        const auto parent = driver.submit(limit(4096, 50)); rows.emplace_back(1, 0);
        no::Request child_input{no::Reduce{no::OwnerOpenedUnits{}}, "child", ""};
        child_input.owner = no::WaitForApplied{parent};
        child_input.group = no::Member{1, 2, no::GroupEffect::Reduce};
        const auto child = driver.submit(child_input); rows.emplace_back(2, 0);
        no::EventId first_cause;
        for (std::size_t i = 0; i != links; ++i) {
            auto source = limit(0.5, 60);
            source.group = no::Member{1, 1, no::GroupEffect::Reduce};
            const auto accepted = driver.ordinal;
            const auto filler = driver.submit(source); rows.emplace_back(accepted, 0);
            const auto expected_cause = driver.ordinal;
            const auto cause = driver.fill(filler, 0.5);
            require(cause.ordinal == expected_cause, "one applied event per explicit complete fill");
            rows.emplace_back(expected_cause, 9);
            const auto receipt = driver.effect(cause, child);
            rows.emplace_back(receipt, 13); receipts.push_back({run, receipt});
            if (i == 0) first_cause = cause;
        }
        note(consumer, 0);
        require(core.history_end() == rows.size(), "independent absolute event population");
        const auto verify = [&](std::size_t expected_base) {
            require(core.history_base() == expected_base && core.history_end() == rows.size(),
                    "only the dependency-free absolute prefix retires");
            for (std::size_t i = expected_base; i != rows.size(); ++i) {
                const auto& event = core.history_at(i);
                require(event.index() == rows[i].second
                        && std::visit([](const auto& x) { return x.ordinal; }, event) == rows[i].first,
                        "retained absolute positions do not become vector offsets");
            }
            if (expected_base != 0) {
                bool absent = false;
                try { (void)core.history_at(expected_base - 1); }
                catch (const std::out_of_range&) { absent = true; }
                require(absent && core.event_at({run, rows[expected_base - 1].first}) == nullptr,
                        "retired position throws and retired event lookup returns null");
            }
            const auto* head = core.event_at(receipts.front());
            if (expected_base <= 4) {
                require(head && std::get<no::DeferredGroupAdjustmentEvent>(*head).pending_after.total == 0.5,
                        "pinned chain head retains its original value");
            }
        };
        consumer.acknowledge_events(0); // Polling reader has read nothing.
        (consumer.*access(NativeRetire{}))();
        verify(0);
        auto& notifications = consumer.*access(NativeNotifications{});
        notifications.emplace_back();
        notifications.back().ordinal = first_cause.ordinal;
        notifications.back().history_index = 3;
        consumer.acknowledge_events(std::numeric_limits<std::uint64_t>::max());
        consumer.acknowledge_events(1); // Must not lower the acknowledged high water.
        (consumer.*access(NativeRetire{}))();
        verify(policy == pineforge::NativeEventRetention::Window ? 3 : 0);
        notifications.clear();
        (consumer.*access(NativeRetire{}))();
        verify(policy == pineforge::NativeEventRetention::Window ? 4 : 0);
        const auto before_resolution = core.history_end();
        const auto expected_parent_fill = driver.ordinal;
        const auto cause = driver.fill(parent, 4096);
        rows.emplace_back(expected_parent_fill, 9);
        const auto bound_ordinal = driver.ordinal;
        driver.resolve(cause, parent, child);
        rows.emplace_back(bound_ordinal, 14); rows.emplace_back(bound_ordinal + 1, 15);
        note(consumer, before_resolution);
        require(core.prior_adjustment_count({run, bound_ordinal}) == receipts.size(),
                "resolved chain has every original receipt");
        std::size_t visited = 0;
        const bool found = core.visit_prior_adjustments({run, bound_ordinal}, [&](const no::EventId& id) {
            require(visited < receipts.size() && id == receipts[visited],
                    "resolved adjustment ids retain original chain order");
            ++visited; return true;
        });
        require(found && visited == receipts.size(), "all pinned values remain readable until resolution");
        consumer.acknowledge_events(std::numeric_limits<std::uint64_t>::max());
        (consumer.*access(NativeRetire{}))();
        verify(policy == pineforge::NativeEventRetention::Window ? rows.size() : 0);
        require(core.last_ordinal() == rows.back().first
                && core.group_effect_receipt_count() == links, "retirement never renumbers lifetime totals");
        rig.audit(store);
    }
}
#endif

#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 6
// Spec: docs/scratch-store.md stage 2.3 (ecbc4d175e), LAB-O1SEAMS clause S3-AW, PINEFORGE_O1S3_CLAUSE=6 — unfinished captures and mismatched fold evidence refuse retirement; referenced Draft values and rejection summaries survive.
void s3_admission_window_requires_closed_prefix_red() {
    // Transactions and retain windows / S3-AW. Input: a terminal-bar command,
    // unfinished review and process-on-close sizing captures, rejection prefix
    // and tail, a live Draft, forged fold evidence and destructive retain.
    // Assertion red on this head: retire_prefix(count, digest^1, last) accepts
    // mismatched fold evidence instead of engine_invariant (nonempty tail).
    // No missing codec call is needed to expose this existing-interface red.
    pineforge::source::PineExecutionAdapter adapter;
    auto& journal = adapter.admission_journal;
    AdmissionFold model;
    ad::Draft held;
    ad::CommandObservation input;
    input.command = 1; input.bar = 7; input.id = text(8197, 31);
    input.requested_quantity = 9.25;
    const ad::SizingObservation sizing{9.25, 1000, 100, 99.5, 1};
    auto expected_input = input;
    expected_input.original_sizing = sizing;
    expected_input.explicit_equity = 1000; expected_input.explicit_price = 100;
    const ad::Event expected_command = ad::CommandEvent{
        std::make_shared<const ad::CommandObservation>(expected_input),
        ad::Outcome::RejectedPricedCap, 0, {}, {}};
    {
        ad::CommandCapture capture(journal.reserve(), input, {},
            [&](ad::CommandEvent event) { journal.append(std::move(event)); });
        capture.bind(held, sizing, 1000, 100);
        capture.outcome(ad::Outcome::RejectedPricedCap);
    }
    model.append(expected_command, 1); fold(adapter); equal_fold(adapter, model);
    auto review = std::get<ad::ReviewEvent>(admission_example(1, 2, 17, 1031, 8));
    {
        ad::ReviewCapture unfinished(journal.reserve(), review,
            [&](ad::ReviewEvent event) { journal.append(std::move(event)); });
        const auto receipt = unfinished.receipt_for(held);
        held.reviewed(receipt);
        engine_invariant([&] { retire(adapter, model); });
        require(journal.retired_prefix().count == 0 && journal.event_count() == 1,
                "unfinished terminal capture prevents retirement and keeps original values");
    }
    model.append(review, 2); fold(adapter); equal_fold(adapter, model);
    {
        auto unfinished = journal.reserve(); // Later process-on-close tail still open.
        engine_invariant([&] { retire(adapter, model); });
        const auto event = admission_example(2, unfinished.sequence(), 0, 0, 8);
        journal.append(event); model.append(event, 3);
        fold(adapter);
        engine_invariant([&] { retire(adapter, model); }); // Capture ownership has not unwound.
    }
    fold(adapter); equal_fold(adapter, model);
    retire(adapter, model);
    require(journal.retired_prefix().count == model.count
            && journal.retired_prefix().last_rejected_command_bar == model.rejected_bar,
            "closed, exactly folded terminal/process-on-close prefix");
    require(held.observation() && held.observation()->id == expected_input.id
            && held.review() && held.review()->sequence == 2,
            "a referenced Draft still answers its original observation and receipt after retirement");
    const auto rejected = command(journal.next_sequence(), 11, 17, ad::Outcome::RejectedAffordability);
    journal.append(rejected); model.append(rejected, 4); fold(adapter); equal_fold(adapter, model);
    require(pineforge::compat::pine::last_rejected_command_bar(journal) == 11,
            "tail rejection takes precedence over prefix in sequence order");
    engine_invariant([&] { journal.retire_prefix(model.count - 1, model.h, model.last); });
    engine_invariant([&] { journal.retire_prefix(model.count, model.h, model.last - 1); });
    // This is the predicted FIRST executed red on 87ba09a7.
    engine_invariant([&] { journal.retire_prefix(model.count, model.h ^ 1, model.last); });
    const auto prior = journal.retired_prefix();
    journal.retain({}); // A destructive diagnostic filter cannot certify a new prefix.
    engine_invariant([&] { retire(adapter, model); });
    require(journal.retired_prefix().count == prior.count
            && journal.retired_prefix().digest == prior.digest, "failed filter proof publishes no prefix");
}
#endif

#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 7
// Spec: docs/scratch-store.md stage 2.3 (ecbc4d175e), LAB-O1SEAMS clause S3-ND, PINEFORGE_O1S3_CLAUSE=7 — native continuation folds and lifetime counts match the independent model through nonzero-base retirement.
void s3_native_digest_survives_nonzero_base_red() {
    // Digests / S3-ND. Input: identical commands under two page/cache
    // geometries and three retire schedules, a replace root, cohort receipts,
    // and a deferred chain resolved after a nonzero base. Assertion: existing
    // event/root/group/cohort folds and lifetime counts equal the independent
    // word model AFTER EVERY commit; retirement adds no bytes. Fails to
    // compile: bind_journal_codec.
    const no::RunIdentity run{"s3-native-digest", 101};
    for (std::size_t page : {std::size_t{512}, std::size_t{2048}})
        for (unsigned schedule = 0; schedule != 3; ++schedule) {
            Rig rig(page);
            CommandScratchStore store(rig.config(pd::ScratchOpenMode::create_empty, schedule + 2), &rig.backing);
            pineforge::NativeExecutionConsumer consumer;
            auto& core = consumer.*access(NativeCore{});
            running_consumer(consumer, run); bind_journal_codec(core, store);
            NativeDriver driver{core, run};
            NativeFold events, roots, groups, cohorts;
            std::size_t noted = 0;
            const auto check = [&] {
                note(consumer, noted); noted = core.history_end();
                (consumer.*access(NativeNoteCohorts{}))();
                const auto equal = [](const auto& actual, const NativeFold& expected) {
                    require(actual.h == expected.h && actual.count == expected.count,
                            "independent native continuation leaf and lifetime count");
                };
                equal(consumer.*access(NativeRecords{}), events);
                equal(consumer.*access(NativeRoots{}), roots);
                equal(consumer.*access(NativeGroups{}), groups);
                equal(consumer.*access(NativeCohorts{}), cohorts);
                if (schedule == 1 || (schedule == 2 && events.count % 3 == 0)
                    || (schedule == 0 && events.count % 7 == 0)) {
                    const auto before = consumer.*access(NativeRecords{});
                    (consumer.*access(NativeRetire{}))();
                    equal(consumer.*access(NativeRecords{}), events);
                    require((consumer.*access(NativeRecords{})).h == before.h,
                            "retirement folds no new base, prefix, or storage bytes");
                }
            };
            auto root = driver.submit(limit()); events.event(0, 0); check();
            const auto cohort = core.cohort_open();
            core.cohort_add(cohort, root);
            cohorts.word(0); cohorts.word(0); cohorts.word(cohort.value);
            cohorts.handle(root); ++cohorts.count; check();
            const auto replacement = core.replace(root, limit(2, 73), 0, driver.incarnation, driver.ordinal);
            require(replacement.status == no::ReplaceStatus::Replaced && replacement.successor,
                    "normal replacement creates one successor-root row");
            roots.word(replacement.successor->incarnation); roots.word(root.incarnation); ++roots.count;
            events.event(2, 0); check();
            root = *replacement.successor;
            core.cohort_remove(cohort, root);
            cohorts.word(1); cohorts.word(0); cohorts.word(cohort.value);
            cohorts.handle(root); ++cohorts.count; check();
            require(core.cancel(root, driver.ordinal).status == no::CancelStatus::Cancelled,
                    "cancel the replaced request");
            // An explicit withdrawal's cancelled event folds its EXISTING compact
            // record: CancelReason::User (0) with the cause-present bit (1 << 32).
            // S3-ND (docs/scratch-store.md :411, :678) keeps those bytes; a digest
            // that does not survive a nonzero base still differs from this model.
            events.event(4, std::uint64_t{1} << 32); check();
            (void)core.retire_history(core.last_ordinal());
            require(core.history_base() != 0, "fixture establishes a real unavailable prefix");
            const auto parent = driver.submit(limit(4096, 50)); events.event(0, 0); check();
            no::Request child_input{no::Reduce{no::OwnerOpenedUnits{}}, "child", ""};
            child_input.owner = no::WaitForApplied{parent};
            child_input.group = no::Member{1, 2, no::GroupEffect::Reduce};
            const auto child = driver.submit(child_input); events.event(0, 0); check();
            for (std::size_t i = 0; i != 33; ++i) {
                auto source = limit(0.5, 60);
                source.group = no::Member{1, 1, no::GroupEffect::Reduce};
                const auto filler = driver.submit(source); events.event(0, 0); check();
                const auto cause_position = core.history_end();
                const auto cause = driver.fill(filler, 0.5);
                auto& notifications = consumer.*access(NativeNotifications{});
                notifications.emplace_back();
                notifications.back().ordinal = cause.ordinal;
                notifications.back().history_index = cause_position;
                events.event(9, 1 | (std::uint64_t{1} << 8)); check();
                const auto receipt = driver.effect(cause, child);
                events.event(13, 0);
                groups.id(cause); groups.handle(child);
                groups.word(1); groups.word(receipt); ++groups.count;
                check();
                notifications.clear();
            }
            const auto parent_position = core.history_end();
            const auto cause = driver.fill(parent, 4096);
            auto& notifications = consumer.*access(NativeNotifications{});
            notifications.emplace_back();
            notifications.back().ordinal = cause.ordinal;
            notifications.back().history_index = parent_position;
            events.event(9, 1 | (std::uint64_t{1} << 8)); check();
            driver.resolve(cause, parent, child);
            events.event(14, 0); events.event(15, 0); check();
            notifications.clear();
            (consumer.*access(NativeRetire{}))();
            require(core.group_effect_receipt_count() == groups.count
                    && core.history_end() == events.count, "absolute counters survive every retention schedule");
            rig.audit(store);
        }
}
#endif

#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 8
// Spec: docs/scratch-store.md stage 2.3 (ecbc4d175e), LAB-O1SEAMS clause S3-AD, PINEFORGE_O1S3_CLAUSE=8 — the v4 fold, lifetime count and cursor survive retirement/filtering without any zero refold outside a true run reset.
void s3_admission_digest_refuses_zero_refold_red() {
    // Digests / S3-AD. Input: all admission kinds, normalized zeros/NaNs,
    // rejection prefix, legal prefix retirement, perturbed cursor, then a
    // shortened/filtered folded tail and a true reset. Assertion red on this
    // head: the real-allocation cursor probe observes an illicit zero restart;
    // the lifetime count also decreases. The probe sees the intermediate zero
    // even if a later refold were to restore matching final bytes. Existing
    // APIs, assertion red; no production hash helper is used as an oracle.
    for (bool use_prefix : {false, true}) {
        pineforge::source::PineExecutionAdapter adapter;
        auto& journal = adapter.admission_journal;
        AdmissionFold model;
        for (unsigned kind = 0; kind != 3; ++kind) {
            const auto sequence = journal.next_sequence();
            const auto event = admission_example(kind, sequence, 3, 17, 7);
            journal.append(event); model.append(event, sequence);
            fold(adapter); equal_fold(adapter, model);
        }
        require(pineforge::compat::pine::last_rejected_command_bar(journal) == model.rejected_bar,
                "v4 model and command gate include the rejected-command value");
        if (use_prefix) retire(adapter, model);
        const auto validated = journal.retired_prefix();
        for (unsigned kind = 0; kind != 3; ++kind) {
            const auto sequence = journal.next_sequence();
            const auto event = admission_example(kind, sequence, 5, 31, 11);
            journal.append(event); model.append(event, sequence);
        }
        fold(adapter); equal_fold(adapter, model);
        if (use_prefix) {
            adapter.*access(AdmissionCount{}) = 0;
            adapter.*access(AdmissionDigest{}) = seed;
            adapter.*access(AdmissionLast{}) = 0;
            fold(adapter); equal_fold(adapter, model); // Validated NONZERO prefix recovery is legal.
            require(journal.retired_prefix().digest == validated.digest,
                    "cursor repair does not replace validated prefix bytes");
        }
        const auto lifetime = model.count;
        const auto final_sequence = model.last;
        bool refused = false;
        bool zero_restart = false;
        {
            HeapCensus probe;
            AdmissionCursorWatch watch(adapter, lifetime);
            try {
                journal.retain({final_sequence});
                fold(adapter);
            } catch (const pineforge::RunFailureInfo& error) {
                require(error.run_failure_code() == pineforge::RunFailureCode::engine_invariant,
                        "filter refusal retains the existing invariant vocabulary");
                refused = true;
            }
            zero_restart = watch.zero_restart;
        }
        // Predicted FIRST executed red: the current cursor mismatch path
        // sets zero, then allocates reflected paths while refolding the tail.
        require(!zero_restart, "no intermediate zero refold outside a true run reset");
        require(adapter.*access(AdmissionCount{}) == lifetime,
                "a destructive filter cannot silently refold and rewrite lifetime admission history");
        equal_fold(adapter, model);
        const auto& prefix = journal.retired_prefix();
        const bool unchanged_prefix = prefix.count == validated.count && prefix.digest == validated.digest
                                   && prefix.last_sequence == validated.last_sequence;
        const bool validated_complete_prefix = prefix.count == model.count && prefix.digest == model.h
                                            && prefix.last_sequence == model.last
                                            && prefix.last_rejected_command_bar == model.rejected_bar;
        require(refused || unchanged_prefix || validated_complete_prefix,
                "recovery cannot fabricate a new unvalidated prefix");
        require(pineforge::compat::pine::last_rejected_command_bar(journal) == model.rejected_bar,
                "filter refusal/recovery preserves the rejection gate as well as the v4 digest");
        adapter.reset_for_run(); // The only legitimate zero restart.
        const auto sequence = journal.next_sequence();
        const auto new_event = command(sequence, 2, 19);
        AdmissionFold fresh;
        fresh.append(new_event, 1); journal.append(new_event);
        fold(adapter); equal_fold(adapter, fresh);
        require(sequence == 1 && journal.retired_prefix().count == 0
                && pineforge::compat::pine::last_rejected_command_bar(journal) == -1,
                "true run reset discards the old fold and rejection summary");
    }
}
#endif

struct NestedWalk {
    const pd::ScratchReadback& reader;
    const std::vector<Bytes>& expected;
    ScratchBuffer inner;
    std::size_t index = 0, stop_after = std::numeric_limits<std::size_t>::max();
    std::size_t calls = 0, chunks = 0;
    std::uint64_t value_offset = 0;
    pd::ScratchHandle first = 0, last = 0;
    bool bad = false;
    ScratchStatus nested = ScratchStatus::ok;
    static bool chunk(void* self, const pd::ScratchChunkView& view) noexcept {
        auto& c = *static_cast<NestedWalk*>(self);
        ++c.chunks;
        if (view.field == pd::ScratchField::key) return true;
        const auto& want = c.expected[c.index];
        if (view.offset != c.value_offset || c.value_offset > want.size()
            || view.bytes.size > want.size() - c.value_offset) {
            c.bad = true; return false;
        }
        for (std::size_t i = 0; i != view.bytes.size; ++i)
            if (view.bytes.data[i] != want[c.value_offset + i]) { c.bad = true; return false; }
        c.value_offset += view.bytes.size;
        if (view.final != (c.value_offset == want.size())) { c.bad = true; return false; }
        return true;
    }
    static bool record(void* self, const pd::ScratchRecordView& row) noexcept {
        auto& c = *static_cast<NestedWalk*>(self);
        ++c.calls;
        if (c.index >= c.expected.size()) { c.bad = true; return false; }
        if (c.first == 0) c.first = row.info.handle;
        c.last = row.info.handle;
        pd::ScratchRecordInfo point;
        c.nested = c.reader.point(row.info.handle, point);
        if (c.nested != ScratchStatus::ok) return false;
        if (point.value_bytes != c.expected[c.index].size()) { c.bad = true; return false; }
        c.value_offset = 0;
        c.nested = c.reader.visit(row.info.handle, c.inner, &chunk, &c);
        if (c.nested != ScratchStatus::ok || c.bad) return false;
        if (c.value_offset != c.expected[c.index].size()) { c.bad = true; return false; }
        // Use the OUTER source AFTER a nested walk has evicted its pages.
        std::array<std::byte, 97> bytes{};
        for (std::size_t at = 0; at < c.expected[c.index].size();) {
            const auto n = std::min(bytes.size(), c.expected[c.index].size() - at);
            c.nested = row.value.read(at, {bytes.data(), n});
            if (c.nested != ScratchStatus::ok) return false;
            if (!std::equal(bytes.begin(), bytes.begin() + n, c.expected[c.index].begin() + at)) {
                c.bad = true; return false;
            }
            at += n;
        }
        ++c.index;
        return c.calls != c.stop_after;
    }
};
struct CountWalk {
    std::size_t calls = 0;
    static bool record(void* self, const pd::ScratchRecordView&) noexcept {
        ++static_cast<CountWalk*>(self)->calls;
        return true;
    }
};

#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 9
// Spec: docs/scratch-store.md stages 2.1 (f2c2fbb370) / 2.3 (ecbc4d175e) §Readback (S34-R), PINEFORGE_O1S3_CLAUSE=9 — nested streaming preserves exact values, status distinctions and fixed memory.
void s34_readback_never_collects_population_red() {
    // Readback / S34-R. Input: both journals, >cache records, eviction, nested
    // disjoint-page callbacks, retired positions, cancellation and backing read
    // faults. Assertion: ordered logical bytes and status distinctions remain
    // exact with a heap census over streaming reads; no owning collection or
    // callback/page address survives a call. Fails to compile:
    // bind_journal_codec (also append_journal_record and journal_readback).
    const no::RunIdentity run{"s34-readback", 107};
    std::vector<Bytes> native_values, admission_values;
    AdmissionFold admission_model;
    constexpr std::size_t count = 33;
    for (std::size_t i = 0; i != count; ++i) {
        auto request = limit();
        request.label = text(4099 + i, unsigned(i));
        request.intent = no::Transact{-0.0}; // A valid recorded rejected attempt.
        native_values.push_back(native_value(no::RejectedEvent{101 + 3 * i, request,
            no::RequestRejectReason::InvalidQuantity, no::CommandSurface::General}));
        const auto event = admission_example(unsigned(i % 3), i + 1, 7, 1031);
        admission_values.push_back(AdmissionModel(event).storage());
        admission_model.append(event, i + 1);
    }
    Rig native_rig, admission_rig;
    HeapCensus heap;
    {
        CommandScratchStore native_store(native_rig.config(), &native_rig.backing);
        CommandScratchStore admission_store(admission_rig.config(), &admission_rig.backing);
        no::WorkingRequestCore core(run);
        ad::Journal admission;
        bind_journal_codec(core, native_store); bind_journal_codec(admission, admission_store);
        std::array<std::byte, 512> outer{}, inner{};
        heap.constructed();
        const auto w = native_store.stats().resident_bytes + admission_store.stats().resident_bytes;
        constexpr std::size_t frames = 4 * sizeof(no::CommandEvent) + 4 * sizeof(ad::Event)
                                     + sizeof(NestedWalk) + 2 * 97;
        for (std::size_t i = 0; i != count; ++i) {
            const pd::ScratchSpanSource native_input(view(native_values[i]));
            status_is(append_journal_record(core, native_input), ScratchStatus::ok, "stream native input");
            require(admission.next_sequence() == i + 1, "stream admission allocation");
            const pd::ScratchSpanSource admission_input(view(admission_values[i]));
            status_is(append_journal_record(admission, admission_input),
                      ScratchStatus::ok, "stream admission input");
        }
        heap.flat(native_store, admission_store, w, frames);
        const pd::ScratchReadback& native = journal_readback(core);
        const pd::ScratchReadback& source = journal_readback(admission);
        const auto exercise = [&](const pd::ScratchReadback& reader, const std::vector<Bytes>& model,
                                  Rig& rig) {
            NestedWalk walk{reader, model, {inner.data(), inner.size()}};
            status_is(reader.for_each({outer.data(), outer.size()}, &NestedWalk::record, &walk),
                      ScratchStatus::ok, "nested production walk");
            require(!walk.bad && walk.index == model.size() && walk.chunks > model.size(),
                    "all requested values were streamed across cache/page boundaries");
            const auto first = walk.first, last = walk.last;
            NestedWalk cancelled{reader, model, {inner.data(), inner.size()}};
            cancelled.stop_after = model.size() / 2;
            status_is(reader.for_each({outer.data(), outer.size()}, &NestedWalk::record, &cancelled),
                      ScratchStatus::cancelled, "visitor cancellation is not end or error");
            require(cancelled.calls == cancelled.stop_after && !cancelled.bad,
                    "no callback follows cancellation");
            std::fill(outer.begin(), outer.end(), std::byte{0xa7});
            std::fill(inner.begin(), inner.end(), std::byte{0x3d});
            StoreValue last_value(reader, last, model.back().size());
            equals_bytes(last_value, model.back()); // Old callback buffers were NOT retained.
            pd::ScratchRecordInfo absent{71, 73, 79};
            status_is(reader.point(pd::scratch_no_handle, absent), ScratchStatus::not_found, "proved absence");
            require(absent.handle == 71 && absent.key_bytes == 73 && absent.value_bytes == 79,
                    "not_found leaves caller information untouched");
            const auto fault_hits = rig.backing.read_faults;
            rig.backing.fail_reads = true;
            CountWalk faulted;
            status_is(reader.for_each({outer.data(), outer.size()}, &CountWalk::record, &faulted),
                      ScratchStatus::io_error, "read fault never becomes an empty/cancelled answer");
            rig.backing.fail_reads = false;
            require(rig.backing.read_faults > fault_hits, "injected read fault was actually hit");
            heap.flat(native_store, admission_store, w, frames);
            return first;
        };
        const auto retired_handle = exercise(native, native_values, native_rig);
        (void)exercise(source, admission_values, admission_rig);
        // Store seeks distinguish end from absence; this scan holds one opaque
        // handle only and makes no assumption about codec index records.
        pd::ScratchHandle handle = 0;
        auto seek = native_store.first(handle);
        std::uint64_t walked = 0;
        while (seek == ScratchStatus::ok) {
            ++walked; seek = native_store.next(handle, handle);
        }
        status_is(seek, ScratchStatus::end, "ordered store exhaustion is end");
        require(handle == pd::scratch_no_handle && walked == native_store.record_count(),
                "streaming seek visited the entire backing relation");
        const auto removed = count / 2;
        (void)core.retire_history(101 + 3 * (removed - 1));
        require(core.history_base() == removed && core.history_end() == count,
                "native codec retirement preserves absolute end");
        bool below_base = false;
        try { (void)core.history_at(removed - 1); }
        catch (const std::out_of_range&) { below_base = true; }
        require(below_base && core.event_at({run, 101}) == nullptr, "retired native reader policy");
        pd::ScratchRecordInfo old;
        status_is(native.point(retired_handle, old), ScratchStatus::not_found, "retired logical record");
        admission.retire_prefix(admission_model.count, admission_model.h, admission_model.last);
        NestedWalk empty{source, admission_values, {inner.data(), inner.size()}};
        status_is(source.for_each({outer.data(), outer.size()}, &NestedWalk::record, &empty),
                  ScratchStatus::ok, "proved empty window is successful, not a fault");
        require(empty.calls == 0, "no callback for the retired admission tail");
        heap.flat(native_store, admission_store, w, frames);
        native_rig.audit(native_store); admission_rig.audit(admission_store);
    }
}
#endif

#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 10
// Spec: docs/scratch-store.md stages 2.1 (f2c2fbb370) / 2.3 (ecbc4d175e) §Backing and charged I/O (S34-B), PINEFORGE_O1S3_CLAUSE=10 — disk/test_memory charge every attempt, preserve committed reopen state and release borrowed owners; extent is a proxy.
void s34_backing_physical_charge_and_lifetime_red() {
    // Backing / S34-B. Input: identical large journal populations on explicitly
    // chosen test_memory AND the existing CommandScratchFile disk provider,
    // repeated I/O, an extent cap/medium fault, rollback,
    // buffered same-backing reopen, then backing-owner destruction. Assertion:
    // one real-kind charge per attempt, accurate extent separate from fixed
    // userspace, io_error on cap/fault, no adopted/global backing lifetime.
    // Fails to compile: bind_journal_codec (also append_journal_record and
    // journal_readback). There is no memfd/tmpfs provider in this tree.
    const no::RunIdentity run{"s34-backing", 109};
    std::vector<Bytes> native_values, admission_values;
    constexpr std::size_t count = 65;
    constexpr std::size_t frames = 4 * sizeof(no::CommandEvent) + 4 * sizeof(ad::Event)
                                 + sizeof(NestedWalk) + 2 * 97;
    for (std::size_t i = 0; i != count + 1; ++i) {
        auto attempted = limit(); attempted.comment = text(8209 + i, unsigned(i + 11));
        attempted.intent = no::Transact{-0.0};
        native_values.push_back(native_value(no::RejectedEvent{1 + i, attempted,
            no::RequestRejectReason::InvalidQuantity, no::CommandSurface::General}));
        admission_values.push_back(AdmissionModel(command(i + 1, 7, 8209 + i)).storage());
    }
    const long platform_page = ::sysconf(_SC_PAGESIZE);
    require(platform_page > 0, "disk provider requires the real platform page geometry");
    const auto page_size = static_cast<std::size_t>(platform_page);
    const auto exercise = [&](bool use_disk) {
        // Only the test_memory byte provision is outside the census. Real disk
        // objects, shared run owners and all store/codec allocations are recorded
        // from construction. Both media use IDENTICAL geometry and logical inputs.
        Bytes native_provision(use_disk ? 0 : 32 * 1024 * 1024);
        Bytes admission_provision(use_disk ? 0 : 32 * 1024 * 1024);
        Bytes fresh_provision(use_disk ? 0 : 32 * 1024 * 1024);
        HeapCensus heap;
        // The byte owners, extents and observers outlive BOTH codecs and stores.
        auto native_owner = std::make_shared<Rig>(page_size, use_disk, &native_provision);
        auto admission_owner = std::make_shared<Rig>(page_size, use_disk, &admission_provision);
        const std::weak_ptr<Rig> native_lifetime = native_owner, admission_lifetime = admission_owner;
        pd::ScratchHandle native_first = 0, admission_first = 0;
        std::uint64_t native_digest = 0, admission_digest = 0;
        std::uint64_t native_extent = 0, admission_extent = 0;
        std::size_t total_w = 0;
        Rig fresh(page_size, use_disk, &fresh_provision);
        {
            CommandScratchStore native_store(native_owner->config(), &native_owner->backing);
            CommandScratchStore admission_store(admission_owner->config(), &admission_owner->backing);
            no::WorkingRequestCore core(run);
            ad::Journal admission;
            bind_journal_codec(core, native_store); bind_journal_codec(admission, admission_store);
            require(native_owner.use_count() == 1 && admission_owner.use_count() == 1,
                    "codec/store borrow backing rather than adopting the run owner");
            Bytes outer(page_size), inner(page_size); // Fixed caller pages, already in the heap census.
            heap.constructed();
            const auto w = native_store.stats().resident_bytes + admission_store.stats().resident_bytes;
            total_w = w;
            for (std::size_t i = 0; i != count; ++i) {
                const pd::ScratchSpanSource native_input(view(native_values[i]));
                const pd::ScratchSpanSource admission_input(view(admission_values[i]));
                status_is(append_journal_record(core, native_input), ScratchStatus::ok, "backed native population");
                require(admission.next_sequence() == i + 1, "backed admission allocation");
                status_is(append_journal_record(admission, admission_input),
                          ScratchStatus::ok, "backed admission population");
                heap.flat(native_store, admission_store, w, frames);
            }
            // The expected walk excludes the extra fault input without constructing
            // another owning sequence inside the heap census.
            NestedWalk native_walk{journal_readback(core), native_values, {inner.data(), inner.size()}};
            status_is(journal_readback(core).for_each({outer.data(), outer.size()},
                      &NestedWalk::record, &native_walk), ScratchStatus::ok, "charged native readback");
            NestedWalk admission_walk{journal_readback(admission), admission_values,
                                     {inner.data(), inner.size()}};
            status_is(journal_readback(admission).for_each({outer.data(), outer.size()},
                      &NestedWalk::record, &admission_walk), ScratchStatus::ok, "charged admission readback");
            require(!native_walk.bad && !admission_walk.bad
                    && native_walk.index == count && admission_walk.index == count,
                    "large populations retain independent values and order");
            native_first = native_walk.first; admission_first = admission_walk.first;
            status_is(native_store.state_digest({outer.data(), outer.size()}, native_digest),
                      ScratchStatus::ok, "committed native storage digest");
            status_is(admission_store.state_digest({outer.data(), outer.size()}, admission_digest),
                      ScratchStatus::ok, "committed admission storage digest");
            const auto native_faults = native_owner->backing.write_faults;
            native_owner->backing.cap = native_owner->backing.page_count();
            const LongNativeAttempt native_over_cap(count + 1,
                native_owner->backing.page_count() * native_owner->page + native_owner->page);
            status_is(append_journal_record(core, native_over_cap), ScratchStatus::io_error,
                      "run backing extent cap is an I/O failure, not success or a workload limit");
            require(native_owner->backing.write_faults > native_faults, "extent cap was reached");
            native_owner->backing.cap = std::numeric_limits<std::uint64_t>::max();
            if (native_store.transaction_active())
                status_is(native_store.rollback(), ScratchStatus::ok, "explicit successful native rollback");
            const auto admission_faults = admission_owner->backing.write_faults;
            admission_owner->backing.fail_writes = true;
            require(admission.next_sequence() == count + 1, "faulted source allocation");
            const pd::ScratchSpanSource admission_fault(view(admission_values.back()));
            status_is(append_journal_record(admission, admission_fault), ScratchStatus::io_error,
                      "medium failure has its own I/O result");
            admission_owner->backing.fail_writes = false;
            admission.abandon(count + 1);
            require(admission_owner->backing.write_faults > admission_faults, "medium write fault was hit");
            if (admission_store.transaction_active())
                status_is(admission_store.rollback(), ScratchStatus::ok, "explicit successful source rollback");
            std::uint64_t after = 0;
            status_is(native_store.state_digest({outer.data(), outer.size()}, after),
                      ScratchStatus::ok, "native digest after cap failure cleanup");
            require(after == native_digest, "cap failure cannot publish a different committed relation");
            status_is(admission_store.state_digest({outer.data(), outer.size()}, after),
                      ScratchStatus::ok, "admission digest after medium failure cleanup");
            require(after == admission_digest, "failed medium cannot publish a different committed relation");
            heap.flat(native_store, admission_store, w, frames);
            native_owner->audit(native_store); admission_owner->audit(admission_store);
            native_extent = native_store.stats().backing_bytes;
            admission_extent = admission_store.stats().backing_bytes;
            require(native_extent > w && admission_extent > w,
                    "extent grows independently of fixed resident allocation; this is only the B_p proxy");
            require(native_owner->charges.errors != 0 && admission_owner->charges.errors != 0,
                    "observer includes failed attempts, not just successful traffic");
        }
        // End the accounting epoch only after all consumer/store destructors.
        // No operation uses a destroyed owner or a dangling borrowed readback.
        native_owner->charges = Charges(native_owner->backing.kind());
        admission_owner->charges = Charges(admission_owner->backing.kind());
        native_owner->backing.read_calls = native_owner->backing.write_calls = native_owner->backing.sync_calls = 0;
        admission_owner->backing.read_calls = admission_owner->backing.write_calls =
            admission_owner->backing.sync_calls = 0;
        {
            CommandScratchStore native(native_owner->config(pd::ScratchOpenMode::reopen), &native_owner->backing);
            CommandScratchStore admission(admission_owner->config(pd::ScratchOpenMode::reopen), &admission_owner->backing);
            status_is(native.status(), ScratchStatus::ok, "buffered native same-backing reopen");
            status_is(admission.status(), ScratchStatus::ok, "buffered source same-backing reopen");
            Bytes page(page_size);
            std::uint64_t reopened = 0;
            status_is(native.state_digest({page.data(), page.size()}, reopened),
                      ScratchStatus::ok, "reopened native relation");
            require(reopened == native_digest, "native buffered committed state survives orderly close");
            status_is(admission.state_digest({page.data(), page.size()}, reopened),
                      ScratchStatus::ok, "reopened admission relation");
            require(reopened == admission_digest, "source buffered committed state survives orderly close");
            StoreValue native_value_source(native, native_first, native_values.front().size());
            StoreValue admission_value_source(admission, admission_first, admission_values.front().size());
            equals_bytes(native_value_source, native_values.front());
            equals_bytes(admission_value_source, admission_values.front());
            require(native.stats().backing_bytes == native_extent
                    && admission.stats().backing_bytes == admission_extent,
                    "reopen reports actual existing extent, not write traffic or a fresh allowance");
            native_owner->audit(native); admission_owner->audit(admission);
            heap.flat(native, admission, total_w, frames);
        }
        require(native_owner.use_count() == 1 && admission_owner.use_count() == 1,
                "owner lifetime is still exclusive after both reopen readers close");
        native_owner.reset(); admission_owner.reset();
        require(native_lifetime.expired() && admission_lifetime.expired(),
                "no codec, observer or global cache keeps the backing owner alive");
        CommandScratchStore no_persistence(fresh.config(pd::ScratchOpenMode::reopen), &fresh.backing);
        status_is(no_persistence.status(), ScratchStatus::not_found,
                  "a fresh run-owned medium cannot reopen another owner's discarded state");
        require(heap_peak <= heap.fixed + frames,
                "reopen, failure cleanup and owner destruction stay in the same fixed heap census");
    };
    exercise(false);
    exercise(true);
}
#endif

struct Case { const char* name; void (*run)(); };
const Case cases[] = {
#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 1
    {"s3_separate_journal_owners_and_reset_red", &s3_separate_journal_owners_and_reset_red},
#endif
#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 2
    {"s3_native_codec_all_values_cross_page_red", &s3_native_codec_all_values_cross_page_red},
#endif
#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 3
    {"s3_admission_codec_allocations_and_order_red", &s3_admission_codec_allocations_and_order_red},
#endif
#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 4
    {"s3_journal_islands_fixed_under_pins_red", &s3_journal_islands_fixed_under_pins_red},
#endif
#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 5
    {"s3_native_window_dependency_frontier_red", &s3_native_window_dependency_frontier_red},
#endif
#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 6
    {"s3_admission_window_requires_closed_prefix_red", &s3_admission_window_requires_closed_prefix_red},
#endif
#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 7
    {"s3_native_digest_survives_nonzero_base_red", &s3_native_digest_survives_nonzero_base_red},
#endif
#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 8
    {"s3_admission_digest_refuses_zero_refold_red", &s3_admission_digest_refuses_zero_refold_red},
#endif
#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 9
    {"s34_readback_never_collects_population_red", &s34_readback_never_collects_population_red},
#endif
#if PINEFORGE_O1S3_CLAUSE == 0 || PINEFORGE_O1S3_CLAUSE == 10
    {"s34_backing_physical_charge_and_lifetime_red", &s34_backing_physical_charge_and_lifetime_red},
#endif
};
static_assert(PINEFORGE_O1S3_CLAUSE >= 0 && PINEFORGE_O1S3_CLAUSE <= 10,
              "select a real clause, never an empty/disabled test");
} // namespace

int main(int argc, char** argv) {
    unsigned executed = 0, failures = 0;
    for (const auto& test : cases) {
        if (argc == 2 && std::strcmp(argv[1], test.name) != 0) continue;
        ++executed;
        try { test.run(); }
        catch (const std::exception& error) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s\n", test.name, error.what());
        }
    }
    if (argc > 2 || executed == 0) {
        std::fprintf(stderr, "select one compiled clause by its exact name, or run all\n");
        return 2;
    }
    std::printf("o1s3 journal red: %u cases, %u failures\n", executed, failures);
    return failures == 0 ? 0 : 1;
}

// The supervisor handoff for this file (spec citation resolution, CMake registration lines,
// interface assumptions) is kept outside the source, in lab-o1s3.handoff.txt beside the brief.

