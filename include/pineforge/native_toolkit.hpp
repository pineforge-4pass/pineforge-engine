#pragma once

// Header-only conveniences over the public native request surface. Nothing
// here is kernel behaviour: every function composes submit / replace / cancel
// and the owner, group and anchor values a host could write out by hand, so a
// host that prefers the primitives loses nothing by ignoring this header. No
// Pine, no source layer, no engine internals.

#include <pineforge/native_host.hpp>
#include <pineforge/native_order.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>

namespace pineforge::native_toolkit {
inline namespace native_toolkit_v1 {

// One filled entry and the exit legs that live off it. Each present leg is
// submitted as the parent's `WaitForApplied` child in a single OCA group, so
// a leg cannot rest before the entry has a fill and a completed leg ends its
// siblings. The legs carry the caller's own intent, trigger and anchor; the
// builder owns only the owner and group fields, plus the two anchored-leg
// knobs below, both defaulting to "leave the leg as written".
//
// Legs are submitted in take_profit, stop_loss, trail order. The group id is
// the parent's incarnation: every leg of one entry shares one group, and two
// brackets built on the same parent deliberately join that same group.
//
// anchor_rounding, when set, is written onto every present leg whose anchor
// is FromOwnerFill (a leg with an absolute level is untouched); unset keeps
// each leg's own rounding. visibility is written into the owner relation the
// builder creates for every leg: PendingUntilArmed keeps the legs out of
// native_working_requests() until the parent's fill arms them.
struct BracketSpec {
    native_order::RequestHandle parent;
    std::optional<native_order::Request> take_profit;
    std::optional<native_order::Request> stop_loss;
    std::optional<native_order::Request> trail;
    native_order::GroupEffect sibling_effect = native_order::GroupEffect::Cancel;
    std::optional<native_order::NativeAnchorRounding> anchor_rounding;
    native_order::NativeArmVisibility visibility = native_order::NativeArmVisibility::Working;
};

// Sibling cohorts, fixed so a hand-written twin can reproduce the shape.
enum class BracketLeg : std::int64_t {
    TakeProfit = 1,
    StopLoss = 2,
    Trail = 3,
};

// What became of one leg. An empty handle is four different things, and a
// host that cannot tell them apart runs on with a leg it believes it placed:
//
//   NotRequested  the spec left this leg unset; nothing was submitted.
//   NotSubmitted  the spec asked for the leg, but `spec.parent` was never
//                 allocated, so there is no fill to wait on and the builder
//                 sent nothing. The kernel never saw the leg and gave no
//                 verdict; this is the builder's own refusal, not a rejection.
//   Accepted      the kernel accepted the leg.
//   Rejected      the kernel refused the leg, with its own reason.
enum class BracketLegState : std::uint8_t {
    NotRequested = 0,
    NotSubmitted = 1,
    Accepted = 2,
    Rejected = 3,
};

// One leg's submit outcome. `result` is the kernel's own SubmitResult,
// verbatim and whole (status, event ordinal, handle, RequestRejectReason):
// the toolkit adds no refusal vocabulary of its own, and `state` is that
// status widened by the two cases a submit never produces because no submit
// happened. It is present exactly when the leg reached the kernel, so for
// Accepted and Rejected and for neither of the others.
struct BracketLegOutcome {
    BracketLegState state = BracketLegState::NotRequested;
    std::optional<native_order::SubmitResult> result;

    bool requested() const noexcept { return state != BracketLegState::NotRequested; }
    bool accepted() const noexcept { return state == BracketLegState::Accepted; }
    bool rejected() const noexcept { return state == BracketLegState::Rejected; }

    // The handle the leg rests under, empty unless the kernel accepted it.
    std::optional<native_order::RequestHandle> handle() const {
        if (!accepted() || !result) return std::nullopt;
        return result->handle;
    }
    // Why the kernel refused the leg, empty unless it refused one.
    std::optional<native_order::RequestRejectReason> reason() const {
        if (!rejected() || !result) return std::nullopt;
        return result->reason;
    }
};

// What the builder did with each leg. The three optional handles are the
// original surface and keep their meaning exactly: each holds the handle its
// leg was accepted under and is empty otherwise. They cannot say why it is
// empty -- a leg the caller never asked for, a leg the builder could not
// submit and a leg the kernel refused are all the same empty optional -- so
// every leg also carries its outcome, and a host reads the refusal and its
// reason there.
struct BracketReceipt {
    native_order::RequestHandle parent;
    std::optional<native_order::RequestHandle> take_profit;
    std::optional<native_order::RequestHandle> stop_loss;
    std::optional<native_order::RequestHandle> trail;
    BracketLegOutcome take_profit_outcome;
    BracketLegOutcome stop_loss_outcome;
    BracketLegOutcome trail_outcome;

    const BracketLegOutcome& outcome(BracketLeg which) const noexcept {
        switch (which) {
        case BracketLeg::TakeProfit: return take_profit_outcome;
        case BracketLeg::StopLoss: return stop_loss_outcome;
        case BracketLeg::Trail: break;
        }
        return trail_outcome;
    }

    // The one question a host must ask before it treats the bracket as
    // placed: is every leg I asked for resting? A spec with no legs at all is
    // trivially true.
    bool every_requested_leg_accepted() const noexcept {
        for (const BracketLeg which : {BracketLeg::TakeProfit, BracketLeg::StopLoss,
                                       BracketLeg::Trail}) {
            const BracketLegOutcome& leg = outcome(which);
            if (leg.requested() && !leg.accepted()) return false;
        }
        return true;
    }
};

// The owner/group pair the builder writes onto every leg, plus the two
// anchored-leg knobs: the visibility goes into the owner relation, the
// rounding (when given) onto a FromOwnerFill anchor only.
inline native_order::Request bracket_leg(
        const native_order::Request& leg,
        const native_order::RequestHandle& parent,
        BracketLeg which,
        native_order::GroupEffect sibling_effect,
        native_order::NativeArmVisibility visibility = native_order::NativeArmVisibility::Working,
        std::optional<native_order::NativeAnchorRounding> anchor_rounding = std::nullopt) {
    native_order::Request out = leg;
    out.owner = native_order::WaitForApplied{parent, visibility};
    out.group = native_order::Member{parent.incarnation, static_cast<std::int64_t>(which),
                                     sibling_effect};
    if (anchor_rounding) {
        if (auto* anchor = std::get_if<native_order::FromOwnerFill>(&out.anchor)) {
            anchor->rounding = *anchor_rounding;
        }
    }
    return out;
}

// Submits the present legs and reports what became of each. A parent handle
// that was never allocated has no fill to wait for, so nothing is submitted
// for it and every requested leg reads NotSubmitted.
inline BracketReceipt submit_bracket(NativeStrategyHost& host, const BracketSpec& spec) {
    BracketReceipt receipt;
    receipt.parent = spec.parent;
    const auto place = [&](const std::optional<native_order::Request>& leg, BracketLeg which,
                           BracketLegOutcome& outcome)
            -> std::optional<native_order::RequestHandle> {
        if (!leg) return std::nullopt;
        if (spec.parent.incarnation == 0) {
            outcome.state = BracketLegState::NotSubmitted;
            return std::nullopt;
        }
        auto result = host.submit(bracket_leg(*leg, spec.parent, which, spec.sibling_effect,
                                              spec.visibility, spec.anchor_rounding));
        const bool accepted = result.status == native_order::SubmitStatus::Accepted;
        outcome.state = accepted ? BracketLegState::Accepted : BracketLegState::Rejected;
        outcome.result = std::move(result);
        return outcome.handle();
    };
    receipt.take_profit =
            place(spec.take_profit, BracketLeg::TakeProfit, receipt.take_profit_outcome);
    receipt.stop_loss = place(spec.stop_loss, BracketLeg::StopLoss, receipt.stop_loss_outcome);
    receipt.trail = place(spec.trail, BracketLeg::Trail, receipt.trail_outcome);
    return receipt;
}

// What the book did for one key. `submit_or_replace` may issue TWO commands
// for one call -- a replace the kernel answers NotWorking, then a fresh
// submit -- and the single optional handle it returns names neither of them:
// a host that reads a handle back believes it re-priced its named order when
// it may in fact hold a brand new request, born later, behind everything
// already resting, and an empty handle is a refused replacement and a refused
// submit at once, with no reason attached to either.
//
// This word names the command whose verdict the book acted on, and each half
// of it is that command's own kernel status:
//
//   Replaced         ReplaceStatus::Replaced with a successor; the key now
//                    holds it and no submit happened.
//   ReplaceRejected  ReplaceStatus::ReplaceRejected; nothing was submitted,
//                    the previous request is untouched and still bound.
//   SubmitAccepted   SubmitStatus::Accepted for the fresh submit; the key
//                    holds the new handle.
//   SubmitRejected   SubmitStatus::Rejected for it; the key is left unbound.
//
// ReplaceStatus::NotWorking and ReplaceStatus::InvalidHandle are not here
// because the book does not stop on them: it forgets the key and submits, so
// the deciding verdict is the submit's. The abandoned replace is still
// reported, whole, in OrderBookOutcome::replace.
enum class OrderBookAction : std::uint8_t {
    Replaced = 0,
    ReplaceRejected = 1,
    SubmitAccepted = 2,
    SubmitRejected = 3,
};

// One submit_or_replace outcome. `replace` and `submit` are the kernel's own
// ReplaceResult and SubmitResult verbatim and whole -- status, event ordinal,
// successor/handle, RequestRejectReason -- and each is present exactly when
// that command reached the kernel. The toolkit invents no refusal vocabulary
// of its own: `action` is the only word it adds, and it is the disjoint union
// of the two kernel statuses restricted to the four the book stops on.
//
// At least one of the two is always present: submit_or_replace always
// commands the host, so every outcome carries the verdict that decided it.
struct OrderBookOutcome {
    OrderBookAction action = OrderBookAction::SubmitRejected;
    std::optional<native_order::ReplaceResult> replace;
    std::optional<native_order::SubmitResult> submit;

    // The key is bound to a live request this call produced.
    bool bound() const noexcept {
        return action == OrderBookAction::Replaced
            || action == OrderBookAction::SubmitAccepted;
    }
    // The key's own request was amended in place; no new request was born.
    bool replaced() const noexcept { return action == OrderBookAction::Replaced; }
    // The kernel refused the call, on whichever command decided it.
    bool refused() const noexcept { return !bound(); }

    // Exactly what submit_or_replace returns: the handle the key now holds,
    // empty when the kernel refused.
    std::optional<native_order::RequestHandle> handle() const {
        if (action == OrderBookAction::Replaced && replace) return replace->successor;
        if (action == OrderBookAction::SubmitAccepted && submit) return submit->handle;
        return std::nullopt;
    }
    // Why the kernel refused, empty unless it refused.
    std::optional<native_order::RequestRejectReason> reason() const {
        if (action == OrderBookAction::ReplaceRejected && replace) return replace->reason;
        if (action == OrderBookAction::SubmitRejected && submit) return submit->reason;
        return std::nullopt;
    }
    // The timeline ordinal of the command that decided the call.
    std::uint64_t event_ordinal() const noexcept {
        switch (action) {
        case OrderBookAction::Replaced:
        case OrderBookAction::ReplaceRejected:
            return replace ? replace->event_ordinal : 0;
        case OrderBookAction::SubmitAccepted:
        case OrderBookAction::SubmitRejected:
            break;
        }
        return submit ? submit->event_ordinal : 0;
    }
};

// A host-side key to live-handle index: the bookkeeping a strategy would
// otherwise write to re-price "its" order by name. It owns no engine state,
// only the handles the host returned. Key must be ordered (a std::string id
// or an integral id are the usual choices).
//
// This book is for a host that wants to REACH one named order again --
// replace it, re-price it, cancel it, ask whether it is still live. It is not
// the only way to address an order by name: a host that only ever withdraws
// them in bulk needs no book at all, because
// NativeStrategyHost::cancel_where(text, NativeRequestField::Label) cancels
// exactly the live requests whose Request::label is that text (and
// NativeRequestField::Comment does the same for the comment) in one call,
// walking the live book once. The trade is the usual one: the predicate
// keeps nothing in step and reads the truth the kernel already holds; this
// book is O(log n) to a handle and survives the request's own terminal
// states only as the key it forgets.
template <class Key>
class OrderBook {
public:
    explicit OrderBook(NativeStrategyHost& host) noexcept : host_(&host) {}

    std::optional<native_order::RequestHandle> handle(const Key& key) const {
        const auto found = entries_.find(key);
        if (found == entries_.end()) return std::nullopt;
        return found->second.handle;
    }
    bool contains(const Key& key) const { return entries_.find(key) != entries_.end(); }
    std::size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }

    // Replaces the key's request while it is still working, otherwise submits
    // a fresh one under that key. A replacement the host rejects leaves the
    // previous request working and still bound to the key; a rejected submit
    // leaves the key unbound.
    //
    // A key bound to a PendingUntilArmed child is not in the working
    // enumeration before its arm, so the book does not ask the enumeration
    // whether it is working: it replaces first and treats NotWorking as
    // "gone, submit afresh". Every key bound to a Working request keeps the
    // enumeration check, so its command footprint is unchanged.
    //
    // This spelling answers the handle alone, which cannot say which command
    // produced it nor why it is empty; submit_or_replace_outcome() is the
    // same call reporting the kernel's own results. It is the handle of that
    // outcome, exactly.
    std::optional<native_order::RequestHandle> submit_or_replace(
            const Key& key, const native_order::Request& request,
            native_order::ReplaceOptions options = {}) {
        return submit_or_replace_outcome(key, request, options).handle();
    }

    // submit_or_replace with the kernel's verdicts attached: which command
    // decided the call, the ReplaceResult of a replace that reached the
    // kernel (including one abandoned as NotWorking), the SubmitResult of a
    // submit that did, and with them the refusal reason and the ordinal the
    // handle alone cannot carry. Same commands, same order, same book state.
    OrderBookOutcome submit_or_replace_outcome(
            const Key& key, const native_order::Request& request,
            native_order::ReplaceOptions options = {}) {
        OrderBookOutcome outcome;
        const auto found = entries_.find(key);
        if (found != entries_.end()) {
            if (found->second.pending_until_armed || working(found->second.handle)) {
                const auto result = host_->replace(found->second.handle, request, options);
                outcome.replace = result;
                if (result.status == native_order::ReplaceStatus::Replaced && result.successor) {
                    found->second = Entry{*result.successor, pending_until_armed(request)};
                    outcome.action = OrderBookAction::Replaced;
                    return outcome;
                }
                if (result.status == native_order::ReplaceStatus::ReplaceRejected) {
                    outcome.action = OrderBookAction::ReplaceRejected;
                    return outcome;
                }
            }
            entries_.erase(found);
        }
        const auto submitted = host_->submit(request);
        outcome.submit = submitted;
        if (submitted.status != native_order::SubmitStatus::Accepted || !submitted.handle) {
            outcome.action = OrderBookAction::SubmitRejected;
            return outcome;
        }
        entries_.emplace(key, Entry{*submitted.handle, pending_until_armed(request)});
        outcome.action = OrderBookAction::SubmitAccepted;
        return outcome;
    }

    // Cancels the key's request and forgets the key. An unknown key is not a
    // command: nothing reaches the host. A key bound to a PendingUntilArmed
    // child is cancelled by handle without consulting the enumeration.
    native_order::CancelStatus cancel(const Key& key) {
        const auto found = entries_.find(key);
        if (found == entries_.end()) return native_order::CancelStatus::NotWorking;
        const auto target = found->second;
        entries_.erase(found);
        if (!target.pending_until_armed && !working(target.handle)) {
            return native_order::CancelStatus::NotWorking;
        }
        return host_->cancel(target.handle).status;
    }

    // Drops the key without commanding the host.
    void forget(const Key& key) { entries_.erase(key); }

private:
    struct Entry {
        native_order::RequestHandle handle;
        bool pending_until_armed = false;
    };

    static bool pending_until_armed(const native_order::Request& request) noexcept {
        const auto* wait = std::get_if<native_order::WaitForApplied>(&request.owner);
        return wait != nullptr
            && wait->visibility == native_order::NativeArmVisibility::PendingUntilArmed;
    }

    bool working(const native_order::RequestHandle& target) const {
        for (const auto& row : host_->native_working_requests()) {
            if (row.definition && row.definition->handle == target) return true;
        }
        return false;
    }

    NativeStrategyHost* host_ = nullptr;
    std::map<Key, Entry> entries_;
};

}  // inline namespace native_toolkit_v1
}  // namespace pineforge::native_toolkit
