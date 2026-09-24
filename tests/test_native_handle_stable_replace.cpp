// R5 lane V19-D witness: a replace that keeps the handle
// (ReplaceOptions::keep_handle) or carries a close's book binding
// (ReplaceOptions::keep_binding) changes what the run records, never what it
// matches, nor any ordinal of its timeline.
//
// 1. The differential. One seeded bare host plays randomized re-issue scripts
//    on gapped tapes -- exits of every closing intent (explicit units,
//    flatten, host-sized) on limit, stop and trail triggers re-priced every
//    bar, trails re-priced with their running best retained, OCA siblings of
//    both effects, brackets whose legs wait for their entry under either arm
//    scope (the legs re-priced while they wait), cohort closes, entries
//    re-priced (with a plain replace whenever they have children waiting),
//    reversal market orders placed before and after the re-prices in the same
//    callback, ties -- a new exit at exactly the level a re-priced one takes,
//    placed before or after the re-price -- cancels, and commands from the fill
//    callback. Every script runs under four option sets (a plain replace,
//    keep_handle, keep_binding, both) and on the direct and the staged core
//    paths. Across the four the run must match identically: every fill (the
//    request that filled, its event and point ordinals, cursor, price and
//    units, in order), every journal event with its ordinal mapped to the
//    host's slots (CloseBoundEvents aside: a carried binding takes the
//    ordinal of the event it spares, unrecorded), the book's queue order and
//    the position every bar, the replace answers, and every closed trade
//    field for field -- entry incarnations included, because a re-price takes
//    the number a successor would have been issued.
//    The two paths of one option set must match bit for bit, hashes too.
//    A second pass re-prices openings with keep_handle too: their later fills
//    carry the kept handle, so every trade field but the entry incarnation
//    must match.
// 2. Directed scenarios, one mechanism each: a tie between a re-priced
//    request and one placed before its re-price (the re-priced one ranks
//    newest); a reversal placed before the re-price in the same callback (the
//    kept binding is not the book's any more, so the leg binds to the new
//    book with a CloseBoundEvent, exactly as a successor does); a book the
//    fill leaves unchanged (no CloseBoundEvent at all); children kept by a
//    parent re-priced with keep_handle (a plain replace ends them); a trail's
//    arm ordinal (kept by keep_handle, zeroed by a retaining successor); a
//    number a re-price took answers UnknownOrigin to a cohort command and
//    NotWorking to a command.
// 3. The continuation over a run that keeps handles every bar costs the same
//    per read at 4x the bars (the issued ranges fold as a running digest).
//
// Fail-before, this TU compiled against the lane's base 6c081f5d (a git
// archive): the options do not exist --
//   tests/test_native_handle_stable_replace.cpp:388:21: error: no member named
//   'keep_handle' in 'pineforge::native_order::ReplaceOptions'
//
// Source-free: kernel-only builds register it.
#include "../src/native_execution_consumer.hpp"
#include "native_match_book_fixture.hpp"
#include "native_order_full_fold.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace pineforge;
namespace no = pineforge::native_order;
using k3_book::Rng;
using k3_book::ticks;

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

// ---- the option sets -------------------------------------------------------
enum class Mode : int { Replace = 0, KeepHandle = 1, KeepBinding = 2, KeepBoth = 3 };
constexpr Mode kModes[] = {Mode::Replace, Mode::KeepHandle, Mode::KeepBinding, Mode::KeepBoth};
const char* mode_name(Mode mode) {
    switch (mode) {
    case Mode::Replace: return "replace";
    case Mode::KeepHandle: return "keep_handle";
    case Mode::KeepBinding: return "keep_binding";
    case Mode::KeepBoth: return "keep_handle+keep_binding";
    }
    return "?";
}
bool keeps_handle(Mode mode) { return mode == Mode::KeepHandle || mode == Mode::KeepBoth; }
bool keeps_binding(Mode mode) { return mode == Mode::KeepBinding || mode == Mode::KeepBoth; }

struct Config {
    std::uint64_t seed = 1;
    k3_book::Path path = k3_book::Path::None;
    bool calc_on_fills = false;
    bool quantize = false;
    // keep_handle on openings too (pass 2).
    bool keep_openings = false;
    int bars = 160;
};

NativeRunSpec spec_for(const Config& config, const k3_book::Tape& tape) {
    k3_book::BookConfig book;
    book.seed = config.seed;
    book.bars = config.bars;
    book.path = config.path;
    book.calc_on_fills = config.calc_on_fills;
    book.quantize = config.quantize;
    NativeRunSpec spec = k3_book::make_spec(book, tape);
    spec.identity = {"v19d-reissue", 1};
    return spec;
}

// ---- what a run shows, host-slot for host-slot -----------------------------
struct Record {
    std::vector<std::uint64_t> fills;      // one word per fill
    std::vector<std::uint64_t> events;     // one word per terminal/trigger event
    std::vector<std::uint64_t> bars;       // queue order + position per bar
    std::vector<std::uint64_t> answers;    // replace statuses
    std::vector<std::uint64_t> trades;     // one word per closed trade
    std::vector<std::uint64_t> trades_no_incarnation;
    std::vector<std::uint64_t> hashes;     // continuation per bar (paths only)
    std::uint64_t broker = 0;
    std::string error;
    long close_bound = 0;                  // CloseBoundEvents
    long close_bound_kept = 0;             // ... of a definition that carried a binding
    long kept_definitions = 0;             // definitions that carried a binding
    long repriced_in_place = 0;            // replaces answered with the target itself
    long ties = 0;                         // fills sharing a cursor with a re-priced request
    long reversals_before = 0;
    long children_repriced = 0;
    long mid_path_replaces = 0;
};

// ---- the scripted host -----------------------------------------------------
enum class Kind : std::uint8_t { Entry, Exit, Child, Cohort };

struct Slot {
    int id = 0;
    Kind kind = Kind::Exit;
    std::optional<no::RequestHandle> handle;
    int parent = -1;
    int trigger = 0;          // 0 limit, 1 stop, 2 trail, 3 market
    int intent = 0;           // exits: 0 explicit, 1 flatten, 2 host-sized
    bool buy = true;          // entries
    std::uint64_t group = 0;  // OCA group, 0 none
    no::GroupEffect effect = no::GroupEffect::Cancel;
    long level = 0;           // quarter ticks, the last one placed
    bool repriced = false;    // replaced at least once with the target kept
};

class ReissueHost final : public NativeStrategyHost {
public:
    ReissueHost(const Config& config, Mode mode, bool direct)
        : config_(config), mode_(mode), rng_(config.seed * 7919 + 17) {
        as_native_consumer(execution_consumer()).set_direct_mutation(direct);
    }
    Record record;

    void on_native_run_begin() override {
        rng_ = Rng(config_.seed * 7919 + 17);
        slots_.clear();
        by_incarnation_.clear();
        cohort_.reset();
        cohort_members_ = 0;
        bar_ = 0;
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ++bar_;
        ref_ = static_cast<long>(bar.close * 4.0);
        observe_bar();
        refresh();
        if (!cohort_ && rng_.percent(40)) cohort_ = cohort_open();
        const bool reverse_first = rng_.percent(12);
        if (reverse_first) {
            reverse();
            ++record.reversals_before;
        }
        // Every live exit, child and cohort close is re-priced every bar.
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            const Kind kind = slots_[index].kind;
            if (kind == Kind::Entry || !slots_[index].handle) continue;
            reprice(index);
        }
        // Some live entries too.
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (slots_[index].kind != Kind::Entry || !slots_[index].handle) continue;
            if (slots_[index].trigger == 3 || !rng_.percent(35)) continue;
            reprice(index);
        }
        // A flat book is re-entered at the market half the time, so exits
        // spend most bars bound to a position.
        if (physical_position().signed_units == 0.0 && rng_.percent(50)) {
            Slot market;
            market.kind = Kind::Entry;
            market.buy = rng_.percent(50);
            market.trigger = 3;
            place(market);
        }
        const int actions = rng_.between(0, 3);
        for (int i = 0; i < actions; ++i) act();
        if (!reverse_first && rng_.percent(10)) reverse();
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        const int slot = slot_of(event.handle());
        full_fold::Fold f;
        f.i(slot);
        // A carried binding takes the ordinal its CloseBoundEvent would have,
        // so every ordinal matches too.
        f.u(event.ordinal);
        f.u(event.cursor.point.ordinal);
        f.i(event.cursor.point.interval_index);
        f.i(event.cursor.point.effective_time_ms);
        f.e(event.cursor.point.path_phase);
        f.d(event.cursor.t);
        f.d(event.raw_price);
        f.d(event.resolved_price);
        f.d(event.closed_units);
        f.d(event.opened_units);
        f.d(event.filled_working);
        f.b(event.terminal);
        f.i(event.cycle_before);
        f.i(event.cycle_after);
        record.fills.push_back(f.h);
        // A fill that shares its cursor with a re-priced request's fill.
        const bool repriced = slot >= 0 && slots_[static_cast<std::size_t>(slot)].repriced;
        if (last_fill_ && last_fill_->interval == event.cursor.point.interval_index
            && last_fill_->t == event.cursor.t && last_fill_->phase
                   == static_cast<int>(event.cursor.point.path_phase)
            && (repriced || last_fill_->repriced)) {
            ++record.ties;
        }
        last_fill_ = LastFill{event.cursor.point.interval_index,
                              static_cast<int>(event.cursor.point.path_phase), event.cursor.t,
                              repriced};
        if (cohort_ && event.opened_units != 0.0 && rng_.percent(50)) {
            cohort_add(*cohort_, event.handle());
            ++cohort_members_;
        }
        // Commands from the fill callback: a re-price on the rest of the path.
        if (rng_.percent(25)) {
            const int pick = pick_live(Kind::Exit);
            if (pick >= 0) {
                reprice(static_cast<std::size_t>(pick));
                ++record.mid_path_replaces;
            }
        }
        if (rng_.percent(10)) act();
    }

    no::ExecutionTerms resolve_execution_terms(const NativeExecutionTermsFacts& facts) const override {
        no::ExecutionTerms terms{facts.default_resolved_price, std::nullopt,
                                 no::OpeningShape::Transact};
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            const double scope = facts.scope_exposure_units;
            terms.units = scope > 0.0 ? (scope < 2.0 ? scope : 2.0) : 1.0;
        }
        return terms;
    }

    const no::WorkingRequestCore& core() const {
        return as_native_consumer(execution_consumer()).request_core();
    }

    int slot_of(const no::RequestHandle& handle) const {
        const auto found = by_incarnation_.find(handle.incarnation);
        return found == by_incarnation_.end() ? -1 : found->second;
    }

private:
    struct LastFill {
        int interval = 0;
        int phase = 0;
        double t = 0.0;
        bool repriced = false;
    };

    void observe_bar() {
        full_fold::Fold f;
        f.i(bar_);
        for (const auto& live : core().live()) f.i(slot_of(live.handle()));
        f.d(physical_position().signed_units);
        f.u(physical_position().lot_count);
        record.bars.push_back(f.h);
        record.hashes.push_back(native_continuation_hash());
    }

    void refresh() {
        for (auto& slot : slots_) {
            if (slot.handle && !core().find_live(*slot.handle)) slot.handle.reset();
        }
    }

    int pick_live(Kind kind) {
        std::vector<int> live;
        for (const auto& slot : slots_) {
            if (slot.kind == kind && slot.handle) live.push_back(slot.id);
        }
        if (live.empty()) return -1;
        return live[static_cast<std::size_t>(rng_.below(static_cast<int>(live.size())))];
    }

    bool has_waiting_children(int parent) const {
        for (const auto& slot : slots_) {
            if (slot.kind != Kind::Child || slot.parent != parent || !slot.handle) continue;
            const auto* live = core().find_live(*slot.handle);
            if (live && std::holds_alternative<no::Wait>(live->authority)) return true;
        }
        return false;
    }

    no::Request request_for(const Slot& slot, bool retain_hint) {
        no::Request request;
        if (slot.kind == Kind::Entry) {
            request.intent = no::Transact{slot.buy ? 1.0 : -1.0};
            request.label = "e" + std::to_string(slot.id);
        } else if (slot.kind == Kind::Cohort) {
            request.intent = no::HostSized{no::HostSizedKind::Close, std::nullopt};
            request.owner = no::BindCohort{*cohort_};
            request.label = "c" + std::to_string(slot.id);
        } else {
            switch (slot.intent) {
            case 0: request.intent = no::Reduce{no::ExplicitUnits{1.0}}; break;
            case 1: request.intent = no::Flatten{}; break;
            default: request.intent = no::HostSized{no::HostSizedKind::Close, std::nullopt}; break;
            }
            request.label = "x" + std::to_string(slot.id);
        }
        if (slot.kind == Kind::Child) {
            const auto& parent = slots_[static_cast<std::size_t>(slot.parent)];
            no::WaitForApplied wait;
            wait.parent = parent.handle ? *parent.handle : no::RequestHandle{};
            wait.scope = slot.intent == 1 ? no::NativeArmScope::Book : no::NativeArmScope::OwnerLot;
            request.owner = wait;
            if (slot.intent != 1) request.intent = no::Reduce{no::OwnerOpenedUnits{}};
        }
        switch (slot.trigger) {
        case 0: request.trigger = no::Limit{ticks(slot.level)}; break;
        case 1: request.trigger = no::Stop{ticks(slot.level)}; break;
        case 2: {
            no::Trail trail;
            trail.offset = ticks(2 + (slot.id % 5));
            if (!retain_hint) trail.arm_price = ticks(slot.level);
            request.trigger = trail;
            break;
        }
        default: break;
        }
        if (slot.group != 0) {
            no::Member member;
            member.group = slot.group;
            member.cohort = slot.id;
            member.effect = slot.effect;
            request.group = member;
        }
        return request;
    }

    void place(Slot slot) {
        slot.id = static_cast<int>(slots_.size());
        const auto request = request_for(slot, false);
        const auto result = submit(request);
        if (result.handle) {
            slot.handle = *result.handle;
            by_incarnation_[result.handle->incarnation] = slot.id;
        }
        slots_.push_back(slot);
    }

    void reprice(std::size_t index) {
        Slot& slot = slots_[index];
        const int step = rng_.between(-3, 3);
        const bool retain = slot.trigger == 2 && rng_.percent(50);
        slot.level = ref_ + (slot.level - ref_) / 2 + step;
        if (slot.level < 8) slot.level = 8;
        no::ReplaceOptions options;
        options.retain_trigger_state = retain;
        // A request whose children wait on it would end them under a plain
        // replace and keep them under keep_handle: the host keeps its handle
        // only where the two agree, as a source adapter would.
        const bool opening = slot.kind == Kind::Entry;
        if (keeps_handle(mode_) && !has_waiting_children(slot.id)
            && (!opening || config_.keep_openings)) {
            options.keep_handle = true;
        }
        if (keeps_binding(mode_)) options.keep_binding = true;
        if (slot.kind == Kind::Child) ++record.children_repriced;
        const auto request = request_for(slot, retain);
        const auto result = replace(*slot.handle, request, options);
        full_fold::Fold f;
        f.i(slot.id);
        f.e(result.status);
        f.b(result.reason.has_value());
        if (result.reason) f.e(*result.reason);
        record.answers.push_back(f.h);
        if (result.status == no::ReplaceStatus::Replaced && result.successor) {
            if (*result.successor == *slot.handle) {
                ++record.repriced_in_place;
                slot.repriced = true;
            }
            slot.handle = *result.successor;
            by_incarnation_[result.successor->incarnation] = slot.id;
        } else if (result.status != no::ReplaceStatus::ReplaceRejected) {
            slot.handle.reset();
        }
    }

    void reverse() {
        const double position = physical_position().signed_units;
        no::Request request;
        request.intent = no::Transact{position > 0.0 ? -2.0 : (position < 0.0 ? 2.0 : 1.0)};
        request.label = "rv";
        Slot slot;
        slot.kind = Kind::Entry;
        slot.trigger = 3;
        slot.buy = position <= 0.0;
        slot.id = static_cast<int>(slots_.size());
        const auto result = submit(request);
        if (result.handle) {
            slot.handle = *result.handle;
            by_incarnation_[result.handle->incarnation] = slot.id;
        }
        slots_.push_back(slot);
    }

    void act() {
        const int pick = rng_.below(100);
        const bool buy = rng_.percent(50);
        // A bounded book: past two dozen live requests the script cancels one.
        const auto live = std::count_if(slots_.begin(), slots_.end(),
                                        [](const Slot& slot) { return slot.handle.has_value(); });
        if (live > 24) {
            const int target = pick_live(rng_.percent(50) ? Kind::Exit : Kind::Child);
            if (target >= 0) {
                Slot& slot = slots_[static_cast<std::size_t>(target)];
                (void)cancel(*slot.handle);
                slot.handle.reset();
            }
            return;
        }
        if (pick < 25) {
            // An entry, sometimes with a bracket waiting on it.
            Slot entry;
            entry.kind = Kind::Entry;
            entry.buy = buy;
            entry.trigger = rng_.percent(60) ? 3 : rng_.below(2);
            entry.level = ref_ + (buy ? -rng_.between(1, 6) : rng_.between(1, 6))
                * (entry.trigger == 1 ? -1 : 1);
            place(entry);
            const int parent = static_cast<int>(slots_.size()) - 1;
            if (slots_.back().handle && rng_.percent(50)) {
                const std::uint64_t group = static_cast<std::uint64_t>(rng_.between(1, 1 << 20));
                const auto effect = rng_.percent(50) ? no::GroupEffect::Cancel
                                                     : no::GroupEffect::Reduce;
                Slot take;
                take.kind = Kind::Child;
                take.parent = parent;
                take.trigger = 0;
                take.intent = rng_.percent(30) ? 1 : 0;
                take.level = ref_ + (buy ? 8 : -8);
                take.group = group;
                take.effect = effect;
                place(take);
                Slot stop = take;
                stop.trigger = rng_.percent(30) ? 2 : 1;
                stop.level = ref_ + (buy ? -8 : 8);
                place(stop);
            }
        } else if (pick < 75) {
            // An exit, sometimes an OCA pair, sometimes a tie with a live exit.
            Slot exit;
            exit.kind = Kind::Exit;
            exit.trigger = rng_.below(3);
            exit.intent = rng_.below(3);
            exit.level = ref_ + (rng_.percent(50) ? 1 : -1) * rng_.between(4, 16);
            const int tie = rng_.percent(30) ? pick_live(Kind::Exit) : -1;
            if (tie >= 0) {
                const Slot& other = slots_[static_cast<std::size_t>(tie)];
                exit.trigger = other.trigger == 2 ? 0 : other.trigger;
                exit.level = other.level;
                if (other.group != 0 && rng_.percent(50)) {
                    exit.group = other.group;
                    exit.effect = other.effect;
                }
            } else if (rng_.percent(40)) {
                exit.group = static_cast<std::uint64_t>(rng_.between(1, 1 << 20));
                exit.effect = rng_.percent(50) ? no::GroupEffect::Cancel : no::GroupEffect::Reduce;
                place(exit);
                Slot sibling = exit;
                sibling.trigger = exit.trigger == 0 ? 1 : 0;
                sibling.level = ref_ + (rng_.percent(50) ? 1 : -1) * rng_.between(4, 16);
                place(sibling);
                return;
            }
            place(exit);
        } else if (pick < 80 && cohort_ && cohort_members_ > 0) {
            Slot close;
            close.kind = Kind::Cohort;
            close.trigger = rng_.below(2);
            close.level = ref_ + rng_.between(-8, 8);
            place(close);
        } else if (pick < 90) {
            const int target = rng_.percent(50) ? pick_live(Kind::Exit) : pick_live(Kind::Entry);
            if (target >= 0) {
                Slot& slot = slots_[static_cast<std::size_t>(target)];
                (void)cancel(*slot.handle);
                slot.handle.reset();
            }
        } else {
            Slot market;
            market.kind = Kind::Entry;
            market.buy = buy;
            market.trigger = 3;
            place(market);
        }
    }

    Config config_;
    Mode mode_;
    Rng rng_;
    std::vector<Slot> slots_;
    std::map<std::uint64_t, int> by_incarnation_;
    std::optional<no::CohortHandle> cohort_;
    long cohort_members_ = 0;
    std::optional<LastFill> last_fill_;
    long ref_ = 400;
    int bar_ = 0;
};

// The journal's terminal and trigger events, each mapped to the host's slot:
// what the run decided. Handles, ordinals and the CloseBoundEvents (which is
// what keep_binding saves) are left out; the rest must agree.
void read_journal(const ReissueHost& host, Record& record) {
    const auto rows = host.native_events(0);
    auto cursor = [](full_fold::Fold& f, const no::MatchCursor& c) {
        f.u(c.point.ordinal);
        f.i(c.point.interval_index);
        f.i(c.point.effective_time_ms);
        f.e(c.point.path_phase);
        f.d(c.t);
    };
    auto state = [](full_fold::Fold& f, const no::TriggerState& s) {
        f.u(s.index());
        if (const auto* track = std::get_if<no::TrailTrack>(&s)) f.d(track->best);
        if (const auto* active = std::get_if<no::TrailActive>(&s)) f.d(active->best_at_trigger);
    };
    for (const auto& row : rows) {
        if (row.kind != NativeEventKind::Command || !row.command) continue;
        full_fold::Fold f;
        bool keep = true;
        std::visit([&](const auto& event) {
            using E = std::decay_t<decltype(event)>;
            f.u(row.command->index());
            f.u(event.ordinal);
            if constexpr (std::is_same_v<E, no::CloseBoundEvent>) {
                keep = false;
                ++record.close_bound;
                if (event.definition && event.definition->kept_binding) ++record.close_bound_kept;
            } else if constexpr (std::is_same_v<E, no::AcceptedEvent>) {
                f.i(host.slot_of(event.handle()));
            } else if constexpr (std::is_same_v<E, no::ReplacedEvent>) {
                f.i(host.slot_of(event.successor()));
                if (event.successor_definition->kept_binding) ++record.kept_definitions;
            } else if constexpr (std::is_same_v<E, no::CancelledEvent>) {
                f.i(host.slot_of(event.handle()));
                f.e(event.reason);
            } else if constexpr (std::is_same_v<E, no::NoEffectEvent>) {
                f.i(host.slot_of(event.handle()));
                cursor(f, event.cursor);
            } else if constexpr (std::is_same_v<E, no::MatchRejectedEvent>) {
                f.i(host.slot_of(event.handle()));
                f.e(event.reason);
                cursor(f, event.cursor);
            } else if constexpr (std::is_same_v<E, no::ActivatedEvent>) {
                f.i(host.slot_of(event.definition->handle));
                f.e(event.kind);
                state(f, event.before);
                state(f, event.after);
                f.d(event.reached_price);
                cursor(f, event.cursor);
            } else if constexpr (std::is_same_v<E, no::ArmedEvent>) {
                f.i(host.slot_of(event.definition->handle));
                f.u(event.after.index());
            } else if constexpr (std::is_same_v<E, no::ReservationReducedEvent>) {
                f.i(host.slot_of(event.recipient));
                f.d(event.requested_delta);
                f.d(event.actual_deduction);
            } else if constexpr (std::is_same_v<E, no::DeferredGroupAdjustmentEvent>) {
                f.i(host.slot_of(event.recipient));
                f.d(event.deferred_delta);
            } else if constexpr (std::is_same_v<E, no::ExecutionAppliedEvent>) {
                f.i(host.slot_of(event.handle()));
                f.d(event.resolved_price);
                cursor(f, event.cursor);
            } else if constexpr (std::is_same_v<E, no::TermsResolvedEvent>) {
                f.i(host.slot_of(event.handle()));
                cursor(f, event.cursor);
            } else if constexpr (std::is_same_v<E, no::QuantityBoundEvent>) {
                f.i(host.slot_of(event.definition->handle));
                f.d(event.source_units);
            } else {
                // Rejections, NotWorking / InvalidHandle answers, margin and
                // risk rows: the kind alone.
            }
        }, *row.command);
        if (keep) record.events.push_back(f.h);
    }
}

void read_trades(const ReissueHost& host, Record& record) {
    for (int index = 0; index < host.trade_count(); ++index) {
        const Trade& trade = host.get_trade(index);
        full_fold::Fold f;
        f.i(trade.entry_time);
        f.i(trade.exit_time);
        f.d(trade.entry_price);
        f.d(trade.exit_price);
        f.d(trade.qty);
        f.d(trade.pnl);
        f.d(trade.pnl_pct);
        f.b(trade.is_long);
        f.i(trade.entry_bar_index);
        f.i(trade.exit_bar_index);
        f.s(trade.entry_id);
        f.s(trade.exit_id);
        f.s(trade.entry_comment);
        f.s(trade.exit_comment);
        f.d(trade.max_runup);
        f.d(trade.max_drawdown);
        f.d(trade.commission);
        f.b(trade.open_at_end);
        record.trades_no_incarnation.push_back(f.h);
        f.u(trade.entry_incarnation);
        record.trades.push_back(f.h);
    }
}

Record run_one(const Config& config, const k3_book::Tape& tape, Mode mode, bool direct) {
    auto host = std::make_unique<ReissueHost>(config, mode, direct);
    const auto setup = host->configure_native(spec_for(config, tape));
    CHECK(setup.status == NativeSetupStatus::Applied);
    host->run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    Record record = std::move(host->record);
    record.error = host->last_error();
    record.broker = host->broker_state_hash();
    read_journal(*host, record);
    read_trades(*host, record);
    return record;
}

bool same_run(const Record& a, const Record& b, bool incarnations) {
    return a.fills == b.fills && a.events == b.events && a.bars == b.bars
        && a.answers == b.answers && a.error == b.error
        && (incarnations ? a.trades == b.trades
                         : a.trades_no_incarnation == b.trades_no_incarnation);
}

void run_differential() {
    long runs = 0, fills = 0, trades = 0, in_place = 0, ties = 0, kept = 0;
    long bound_default = 0, bound_kept = 0, bound_changed = 0, reversals = 0;
    long children = 0, mid_path = 0, events = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (std::uint64_t seed = 1; seed <= 12; ++seed) {
            Config config;
            config.seed = seed;
            config.path = static_cast<k3_book::Path>(seed % 3);
            config.calc_on_fills = seed % 4 == 1;
            config.quantize = seed % 5 == 2;
            config.keep_openings = pass == 1;
            k3_book::BookConfig tape_config;
            tape_config.seed = seed * 101 + 3;
            tape_config.bars = config.bars;
            const k3_book::Tape tape = k3_book::make_tape(tape_config);
            Record reference;
            for (const Mode mode : kModes) {
                const Record direct = run_one(config, tape, mode, true);
                const Record staged = run_one(config, tape, mode, false);
                // The two core paths of one option set: bit for bit.
                const bool paths = same_run(direct, staged, true) && direct.hashes == staged.hashes
                    && direct.broker == staged.broker && direct.close_bound == staged.close_bound;
                CHECK(paths);
                if (!paths) {
                    std::fprintf(stderr, "  seed=%llu pass=%d %s: direct != staged\n",
                                 static_cast<unsigned long long>(seed), pass, mode_name(mode));
                }
                if (mode == Mode::Replace) {
                    reference = direct;
                    bound_default += direct.close_bound;
                    ++runs;
                    fills += static_cast<long>(direct.fills.size());
                    trades += static_cast<long>(direct.trades.size());
                    events += static_cast<long>(direct.events.size());
                    reversals += direct.reversals_before;
                    children += direct.children_repriced;
                    mid_path += direct.mid_path_replaces;
                    continue;
                }
                // Every other option set: the same run. Pass 2 keeps the
                // handles of openings, whose later fills carry them.
                const bool same = same_run(direct, reference, pass == 0);
                CHECK(same);
                if (!same) {
                    std::fprintf(stderr,
                                 "  seed=%llu pass=%d %s differs from a plain replace: fills %d "
                                 "events %d bars %d answers %d trades %d error '%s'/'%s'\n",
                                 static_cast<unsigned long long>(seed), pass, mode_name(mode),
                                 direct.fills == reference.fills, direct.events == reference.events,
                                 direct.bars == reference.bars, direct.answers == reference.answers,
                                 pass == 0 ? direct.trades == reference.trades
                                           : direct.trades_no_incarnation
                                                 == reference.trades_no_incarnation,
                                 direct.error.c_str(), reference.error.c_str());
                }
                if (keeps_handle(mode)) {
                    in_place += direct.repriced_in_place;
                    ties += direct.ties;
                }
                if (mode == Mode::KeepBinding) {
                    kept += direct.kept_definitions;
                    bound_kept += direct.close_bound;
                    bound_changed += direct.close_bound_kept;
                }
            }
        }
    }
    // Not vacuous: re-prices kept their handles, bindings were carried and
    // saved their events, some carried bindings met a changed book and bound
    // with an event, and re-priced requests tied with others at one cursor.
    CHECK(in_place > 1000);
    CHECK(kept > 500);
    CHECK(bound_kept < bound_default);
    CHECK(bound_changed > 0);
    CHECK(ties > 0);
    CHECK(reversals > 0);
    CHECK(children > 0);
    CHECK(mid_path > 0);
    std::printf("differential: %ld runs x 4 option sets x 2 core paths, %ld fills, %ld trades, "
                "%ld journal events; %ld re-prices kept their handle, %ld ties with a re-priced "
                "request; %ld definitions carried a binding: %ld CloseBoundEvents under a plain "
                "replace, %ld under keep_binding (%ld where the carried binding was not the "
                "book's any more); %ld reversals before the re-prices, %ld children re-priced, "
                "%ld re-prices from the fill callback\n",
                runs, fills, trades, events, in_place, ties, kept, bound_default, bound_kept,
                bound_changed, reversals, children, mid_path);
}

// ---- directed scenarios ----------------------------------------------------
// A fixed tape and a host that runs one step list, bar by bar.
std::vector<Bar> tape_of(const std::vector<std::array<double, 4>>& ohlc) {
    std::vector<Bar> bars;
    std::int64_t t = 1736121600000LL;
    for (const auto& row : ohlc) {
        Bar bar{};
        bar.open = row[0];
        bar.high = row[1];
        bar.low = row[2];
        bar.close = row[3];
        bar.volume = 10.0;
        bar.timestamp = t;
        bars.push_back(bar);
        t += 5 * 60000;
    }
    return bars;
}

NativeRunSpec directed_spec() {
    NativeRunSpec spec;
    spec.identity = {"v19d-directed", 1};
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.tickerid = "TEST:V19D";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    spec.initial_capital = 1.0e6;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.25;
    spec.event_retention = NativeEventRetention::Full;
    return spec;
}

class StepHost final : public NativeStrategyHost {
public:
    using Step = std::function<void(StepHost&, int bar)>;
    explicit StepHost(Step step) : step_(std::move(step)) {}
    std::vector<std::string> fills;
    std::map<std::string, no::RequestHandle> handles;
    int bar = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { step_(*this, bar++); }
    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        char text[96];
        std::snprintf(text, sizeof text, "%s@%.2f:%g", event.request().label.c_str(),
                      event.resolved_price, event.closed_units + event.opened_units);
        fills.emplace_back(text);
    }
    no::ReplaceResult reprice(const std::string& name, no::Request request,
                              no::ReplaceOptions options) {
        const auto result = replace(handles.at(name), request, options);
        if (result.successor) handles[name] = *result.successor;
        return result;
    }
    void place(const std::string& name, const no::Request& request) {
        const auto result = submit(request);
        if (result.handle) handles[name] = *result.handle;
    }
    long count(std::size_t kind) const {
        long n = 0;
        for (const auto& row : native_events(0)) {
            if (row.command && row.command->index() == kind) ++n;
        }
        return n;
    }
    const no::WorkingRequestCore& core() const {
        return as_native_consumer(execution_consumer()).request_core();
    }

private:
    Step step_;
};

no::Request limit_exit(const std::string& label, double price, std::uint64_t group) {
    no::Request request{no::Reduce{no::ExplicitUnits{1.0}}, label, ""};
    request.trigger = no::Limit{price};
    if (group != 0) {
        no::Member member;
        member.group = group;
        member.cohort = static_cast<std::int64_t>(label[0]);
        member.effect = no::GroupEffect::Cancel;
        request.group = member;
    }
    return request;
}

std::unique_ptr<StepHost> run_steps(const std::vector<Bar>& bars, StepHost::Step step) {
    auto host = std::make_unique<StepHost>(std::move(step));
    const auto setup = host->configure_native(directed_spec());
    CHECK(setup.status == NativeSetupStatus::Applied);
    host->run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host->last_error().empty());
    return host;
}

constexpr std::size_t kCloseBound = 10;  // CommandEvent's CloseBoundEvent alternative
static_assert(std::is_same_v<std::variant_alternative_t<kCloseBound, no::CommandEvent>,
                             no::CloseBoundEvent>);

void directed() {
    // (a) The tie. "a" rests at 101 from bar 0; "b" at 101 from bar 1; "a" is
    // re-priced to 101 again at bar 2. Both are in one OCA group, so only the
    // first to fill at bar 3's open (a gap through 101) fills: under a plain
    // replace a's successor is the newest, so b fills. keep_handle ranks the
    // re-priced a newest too -- the order it had kept its incarnation for
    // would fill a.
    const auto tie_tape = tape_of({{100, 100.5, 99.5, 100}, {100, 100.5, 99.5, 100},
                                   {100, 100.5, 99.5, 100}, {102, 103, 101.5, 102.5},
                                   {102, 102.5, 101.5, 102}});
    auto tie = [&](bool keep) {
        return run_steps(tie_tape, [keep](StepHost& host, int bar) {
            if (bar == 0) {
                host.place("long", no::Request{no::Transact{2.0}, "long", ""});
                host.place("a", limit_exit("a", 101.0, 77));
            } else if (bar == 1) {
                host.place("b", limit_exit("b", 101.0, 77));
            } else if (bar == 2) {
                no::ReplaceOptions options;
                options.keep_handle = keep;
                const auto result = host.reprice("a", limit_exit("a", 101.0, 77), options);
                CHECK(result.status == no::ReplaceStatus::Replaced);
            }
        });
    };
    const auto tie_plain = tie(false);
    const auto tie_kept = tie(true);
    auto filled = [](const StepHost& host, const char* label) {
        return std::any_of(host.fills.begin(), host.fills.end(), [&](const std::string& fill) {
            return fill.rfind(std::string(label) + "@", 0) == 0;
        });
    };
    CHECK(tie_plain->fills == tie_kept->fills);
    CHECK(filled(*tie_kept, "b"));
    CHECK(!filled(*tie_kept, "a"));

    // (b) A reversal placed before the re-price in the same callback. The
    // exit was bound to the long book; at bar 2 a market reversal to short is
    // placed first, then the exit is re-priced with keep_binding. At bar 3's
    // open the reversal fills first (it is older), so the book the exit would
    // bind to is the short one: the carried binding is not the book's, and the
    // exit binds to the new book with a CloseBoundEvent -- where a plain
    // successor binds too -- and closes the short at its level.
    const auto rev_tape = tape_of({{100, 100.5, 99.5, 100}, {100, 100.5, 99.5, 100},
                                   {100, 100.5, 99.5, 100}, {100, 100.5, 99, 99.5},
                                   {99.5, 100, 98.5, 99}, {99, 99.5, 98.5, 99}});
    auto reversal = [&](bool keep) {
        return run_steps(rev_tape, [keep](StepHost& host, int bar) {
            if (bar == 0) {
                host.place("long", no::Request{no::Transact{1.0}, "long", ""});
                host.place("x", limit_exit("x", 110.0, 0));
            } else if (bar == 2) {
                host.place("flip", no::Request{no::Transact{-2.0}, "flip", ""});
                no::ReplaceOptions options;
                options.keep_binding = keep;
                options.keep_handle = keep;
                // A buy limit that closes the short at 99.
                (void)host.reprice("x", limit_exit("x", 99.0, 0), options);
            }
        });
    };
    const auto rev_plain = reversal(false);
    const auto rev_kept = reversal(true);
    CHECK(rev_plain->fills == rev_kept->fills);
    CHECK(rev_plain->trade_count() == rev_kept->trade_count());
    CHECK(rev_kept->count(kCloseBound) == rev_plain->count(kCloseBound));
    CHECK(std::find(rev_kept->fills.begin(), rev_kept->fills.end(), std::string("x@99.00:1"))
          != rev_kept->fills.end());

    // (c) A book the bars leave alone: an exit bound to the long book,
    // re-priced every bar with keep_binding, never binds again -- one
    // CloseBoundEvent in all, where a plain successor binds at every bar.
    const auto calm_tape = tape_of({{100, 100.5, 99.5, 100}, {100, 100.5, 99.5, 100},
                                    {100, 100.5, 99.5, 100}, {100, 100.5, 99.5, 100},
                                    {100, 100.5, 99.5, 100}, {100, 100.5, 99.5, 100}});
    auto calm = [&](bool keep) {
        return run_steps(calm_tape, [keep](StepHost& host, int bar) {
            if (bar == 0) {
                host.place("long", no::Request{no::Transact{1.0}, "long", ""});
                host.place("x", limit_exit("x", 110.0, 0));
                return;
            }
            no::ReplaceOptions options;
            options.keep_binding = keep;
            (void)host.reprice("x", limit_exit("x", 110.0 + bar * 0.25, 0), options);
        });
    };
    const auto calm_plain = calm(false);
    const auto calm_kept = calm(true);
    CHECK(calm_plain->fills == calm_kept->fills);
    CHECK(calm_plain->count(kCloseBound) == 5);
    CHECK(calm_kept->count(kCloseBound) == 1);

    // (d) A parent re-priced while its children wait: a plain replace ends
    // them (OwnerGone); keep_handle keeps them waiting on the same handle, and
    // the parent's fill arms them.
    const auto parent_tape = tape_of({{100, 100.5, 99.5, 100}, {100, 100.5, 99.5, 100},
                                      {100, 100.5, 98, 98.5}, {98.5, 99, 98, 98.5}});
    auto parent = [&](bool keep) {
        return run_steps(parent_tape, [keep](StepHost& host, int bar) {
            if (bar == 0) {
                no::Request entry{no::Transact{1.0}, "entry", ""};
                entry.trigger = no::Limit{97.0};
                host.place("entry", entry);
                no::Request take{no::Reduce{no::OwnerOpenedUnits{}}, "take", ""};
                take.trigger = no::Limit{120.0};
                take.owner = no::WaitForApplied{host.handles.at("entry")};
                host.place("take", take);
            } else if (bar == 1) {
                no::Request entry{no::Transact{1.0}, "entry", ""};
                entry.trigger = no::Limit{99.0};
                no::ReplaceOptions options;
                options.keep_handle = keep;
                (void)host.reprice("entry", entry, options);
            }
        });
    };
    const auto parent_plain = parent(false);
    const auto parent_kept = parent(true);
    CHECK(parent_plain->core().find_live(parent_plain->handles.at("take")) == nullptr);
    const auto* kept_take = parent_kept->core().find_live(parent_kept->handles.at("take"));
    CHECK(kept_take != nullptr);
    CHECK(kept_take && !std::holds_alternative<no::Wait>(kept_take->authority));
    CHECK(parent_kept->handles.at("entry").incarnation == 1);

    // (e) A trail's arm ordinal: a retaining successor armed nothing (0); a
    // retaining re-price that kept the handle is the request that armed.
    const auto trail_tape = tape_of({{100, 100.5, 99.5, 100}, {100, 102, 99.5, 101.5},
                                     {101.5, 102, 101, 101.5}, {101.5, 102, 101, 101.5}});
    auto trail = [&](bool keep) {
        return run_steps(trail_tape, [keep](StepHost& host, int bar) {
            no::Request request{no::Reduce{no::ExplicitUnits{1.0}}, "t", ""};
            no::Trail ride;
            ride.offset = 5.0;
            ride.arm_price = 101.0;
            request.trigger = ride;
            if (bar == 0) {
                host.place("long", no::Request{no::Transact{1.0}, "long", ""});
                host.place("t", request);
            } else if (bar == 2) {
                no::ReplaceOptions options;
                options.retain_trigger_state = true;
                options.keep_handle = keep;
                (void)host.reprice("t", request, options);
            }
        });
    };
    const auto trail_plain = trail(false);
    const auto trail_kept = trail(true);
    const auto plain_state = trail_plain->trail_state(trail_plain->handles.at("t"));
    const auto kept_state = trail_kept->trail_state(trail_kept->handles.at("t"));
    CHECK(plain_state && kept_state);
    if (plain_state && kept_state) {
        CHECK(plain_state->activated && kept_state->activated);
        CHECK(plain_state->best_price == kept_state->best_price);
        CHECK(plain_state->current_level == kept_state->current_level);
        CHECK(plain_state->activation_ordinal == 0);
        CHECK(kept_state->activation_ordinal != 0);
    }

    // (f) The number a re-price took is no handle: a command naming it is
    // NotWorking, and a cohort add of it is UnknownOrigin. The requests after
    // it are numbered as a replace would have numbered them.
    const auto numbers = run_steps(calm_tape, [](StepHost& host, int bar) {
        if (bar == 0) {
            host.place("long", no::Request{no::Transact{1.0}, "long", ""});
            host.place("x", limit_exit("x", 110.0, 0));
        } else if (bar == 1) {
            no::ReplaceOptions options;
            options.keep_handle = true;
            const auto result = host.reprice("x", limit_exit("x", 111.0, 0), options);
            CHECK(result.successor && result.successor->incarnation == 2);
            const auto& live = *host.core().find_live(*result.successor);
            CHECK(live.priority() == 3);
            const no::RequestHandle taken{host.handles.at("x").run, 3};
            CHECK(host.cancel(taken).status == no::CancelStatus::NotWorking);
            const auto cohort = host.cohort_open();
            host.cohort_add(cohort, taken);
            CHECK(host.core().cohort_receipts().back().status
                  == no::CohortReceiptStatus::UnknownOrigin);
            host.place("y", limit_exit("y", 112.0, 0));
            CHECK(host.handles.at("y").incarnation == 4);
        }
    });
    CHECK(numbers->core().issued_incarnations().size() == 2);

    std::printf("directed: tie %zu fills, reversal %zu fills (%ld CloseBound each), calm "
                "%ld / %ld CloseBound, children %s under keep_handle, trail arm ordinal %llu "
                "kept vs 0\n",
                tie_kept->fills.size(), rev_kept->fills.size(), rev_kept->count(kCloseBound),
                calm_plain->count(kCloseBound), calm_kept->count(kCloseBound),
                kept_take ? "kept" : "lost",
                static_cast<unsigned long long>(kept_state ? kept_state->activation_ordinal : 0));
}

// ---- the continuation over a keep_handle run -------------------------------
// Every bar both legs are re-priced keeping their handles, so the issued
// incarnations gain a range per re-price; the continuation read every bar
// must cost the same per bar at 4x the bars (a walk of the ranges would make
// it ~4x per read, ~16x in all).
class ContinuationHost final : public NativeStrategyHost {
public:
    std::uint64_t acc = 0;
    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        if (!entered_) {
            entered_ = true;
            (void)submit(no::Request{no::Transact{1.0}, "L", ""});
        }
        if (physical_position().signed_units > 0.0) {
            // One leg keeps its handle, the other is replaced by a successor:
            // every bar a number is taken for a priority and one is issued,
            // so the issued incarnations gain a range per bar.
            leg(tp_, no::Limit{std::round(bar.close * 1.6 * 4.0) / 4.0}, true);
            leg(sl_, no::Stop{std::round(bar.close * 0.4 * 4.0) / 4.0}, false);
        }
        acc ^= native_continuation_hash();
    }
    std::size_t issued_ranges() const {
        return as_native_consumer(execution_consumer()).request_core()
            .issued_incarnations().size();
    }

private:
    void leg(std::optional<no::RequestHandle>& handle, no::Trigger trigger, bool keep) {
        no::Request request{no::Reduce{no::ExplicitUnits{1.0}}, "x", ""};
        request.trigger = trigger;
        if (!handle) {
            const auto result = submit(request);
            if (result.handle) handle = *result.handle;
            return;
        }
        no::ReplaceOptions options;
        options.keep_handle = keep;
        options.keep_binding = true;
        const auto result = replace(*handle, request, options);
        if (result.successor) handle = *result.successor;
    }
    bool entered_ = false;
    std::optional<no::RequestHandle> tp_, sl_;
};

double continuation_seconds(int bars) {
    k3_book::BookConfig tape_config;
    tape_config.seed = 99;
    tape_config.bars = bars;
    const auto tape = k3_book::make_tape(tape_config);
    NativeRunSpec spec = directed_spec();
    ContinuationHost host;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    const auto start = std::chrono::steady_clock::now();
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    // A range per bar: a walk of them at every read would be quadratic.
    CHECK(host.issued_ranges() + 10 > static_cast<std::size_t>(bars));
    return seconds;
}

void scaling() {
    constexpr int kSmall = 2000;
    double best_small = 1e9, best_large = 1e9;
    for (int round = 0; round < 3; ++round) {
        best_small = std::min(best_small, continuation_seconds(kSmall));
        best_large = std::min(best_large, continuation_seconds(4 * kSmall));
    }
    const double ratio = best_large / best_small;
    CHECK(ratio < 8.0);
    std::printf("continuation read every bar, re-prices keeping handles: %d bars %.4f s, %d bars "
                "%.4f s (x%.2f, bound x8)\n",
                kSmall, best_small, 4 * kSmall, best_large, ratio);
}
}  // namespace

int main() {
    run_differential();
    directed();
    scaling();
    std::printf("%d checks\n", checks);
    if (failures == 0) {
        std::printf("test_native_handle_stable_replace: ok\n");
        return 0;
    }
    std::printf("test_native_handle_stable_replace: %d of %d checks failed\n", failures, checks);
    return 1;
}
