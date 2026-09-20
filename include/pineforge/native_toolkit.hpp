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

// The handles the legs were accepted under. A leg that was absent or that the
// host rejected stays empty; the remaining legs are still submitted.
struct BracketReceipt {
    native_order::RequestHandle parent;
    std::optional<native_order::RequestHandle> take_profit;
    std::optional<native_order::RequestHandle> stop_loss;
    std::optional<native_order::RequestHandle> trail;
};

// Sibling cohorts, fixed so a hand-written twin can reproduce the shape.
enum class BracketLeg : std::int64_t {
    TakeProfit = 1,
    StopLoss = 2,
    Trail = 3,
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

// Submits the present legs. A parent handle that was never allocated has no
// fill to wait for, so nothing is submitted for it.
inline BracketReceipt submit_bracket(NativeStrategyHost& host, const BracketSpec& spec) {
    BracketReceipt receipt;
    receipt.parent = spec.parent;
    if (spec.parent.incarnation == 0) return receipt;
    const auto place = [&](const std::optional<native_order::Request>& leg, BracketLeg which)
            -> std::optional<native_order::RequestHandle> {
        if (!leg) return std::nullopt;
        const auto result = host.submit(bracket_leg(*leg, spec.parent, which, spec.sibling_effect,
                                                    spec.visibility, spec.anchor_rounding));
        if (result.status != native_order::SubmitStatus::Accepted || !result.handle) {
            return std::nullopt;
        }
        return *result.handle;
    };
    receipt.take_profit = place(spec.take_profit, BracketLeg::TakeProfit);
    receipt.stop_loss = place(spec.stop_loss, BracketLeg::StopLoss);
    receipt.trail = place(spec.trail, BracketLeg::Trail);
    return receipt;
}

// A host-side key to live-handle index: the bookkeeping a strategy would
// otherwise write to re-price "its" order by name. It owns no engine state,
// only the handles the host returned. Key must be ordered (a std::string id
// or an integral id are the usual choices).
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
    // previous request working and still bound to the key, and reports
    // nothing; a rejected submit leaves the key unbound.
    //
    // A key bound to a PendingUntilArmed child is not in the working
    // enumeration before its arm, so the book does not ask the enumeration
    // whether it is working: it replaces first and treats NotWorking as
    // "gone, submit afresh". Every key bound to a Working request keeps the
    // enumeration check, so its command footprint is unchanged.
    std::optional<native_order::RequestHandle> submit_or_replace(
            const Key& key, const native_order::Request& request,
            native_order::ReplaceOptions options = {}) {
        const auto found = entries_.find(key);
        if (found != entries_.end()) {
            if (found->second.pending_until_armed || working(found->second.handle)) {
                const auto result = host_->replace(found->second.handle, request, options);
                if (result.status == native_order::ReplaceStatus::Replaced && result.successor) {
                    found->second = Entry{*result.successor, pending_until_armed(request)};
                    return found->second.handle;
                }
                if (result.status == native_order::ReplaceStatus::ReplaceRejected) {
                    return std::nullopt;
                }
            }
            entries_.erase(found);
        }
        const auto submitted = host_->submit(request);
        if (submitted.status != native_order::SubmitStatus::Accepted || !submitted.handle) {
            return std::nullopt;
        }
        entries_.emplace(key, Entry{*submitted.handle, pending_until_armed(request)});
        return *submitted.handle;
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
