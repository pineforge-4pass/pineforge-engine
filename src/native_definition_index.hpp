#pragma once

// Internal: an incarnation -> definition index over one WorkingRequestCore's
// command history (R5 lane PERF-P7, P7a). Not a public native API: it lives
// in src/, WorkingRequestCore gains no member (native_order_v6's layout is
// frozen until V19-B) and the public native_order.hpp is unchanged.
//
// WorkingRequestCore::definition_for answers a request that is no longer
// working by scanning the history backwards for the NEWEST event naming it
// -- an AcceptedEvent's definition, or a ReplacedEvent's predecessor or
// successor, the predecessor first -- and cohort_add canonicalizes an origin
// through its whole replace chain with one such scan per link. A request
// re-issued for a long time made every cohort_add walk ever further back.
//
// This index keeps, per incarnation, where that scan stops: the position of
// the newest Accepted/Replaced event naming the incarnation and which of its
// definitions names it. It is folded in history order, a ReplacedEvent's
// successor before its predecessor, so a later fold overwrites an earlier
// one exactly as the scan's precedence ranks them (the newest event wins;
// inside one replace the predecessor wins). It only ever extends over the
// events appended since the last sync: the history only grows between the
// core's resets, and the owner resets the index with the core. A lookup is
// verified against the history -- the entry must name an event whose
// definition carries that very handle -- and answers only while the index
// covers the whole history; anything it cannot vouch for, it leaves to the
// scan (the caller's fallback), so its answer is the scan's by construction.
//
// The core reads it only while its owner publishes it around one call
// (Publication: a thread-local pointer naming the core it indexes, restored
// when the call returns), so a core the owner is not calling into, on any
// thread, scans as before. One word per incarnation the history names, never
// more words than the history has events plus a constant.

#include <pineforge/native_order.hpp>

#include <cstddef>
#include <cstdint>
#include <new>
#include <variant>
#include <vector>

namespace pineforge::native_order {
inline namespace native_order_v6 {

class DefinitionIndex {
public:
    // Folds the events appended since the last sync; a different core, or a
    // history shorter than what was folded, starts over. Never throws: when
    // the index cannot grow it stops short of the history's end and answers
    // nothing until a later sync reaches it.
    void sync(const WorkingRequestCore& core) noexcept {
        const auto& history = core.history();
        if (core_ != &core || history.size() < folded_) {
            reset();
            core_ = &core;
        }
        try {
            for (; folded_ < history.size(); ++folded_) {
                const auto& event = history[folded_];
                if (const auto* accepted = std::get_if<AcceptedEvent>(&event)) {
                    if (accepted->definition) note(*accepted->definition, kAccepted);
                } else if (const auto* replaced = std::get_if<ReplacedEvent>(&event)) {
                    if (replaced->successor_definition)
                        note(*replaced->successor_definition, kSuccessor);
                    if (replaced->predecessor_definition)
                        note(*replaced->predecessor_definition, kPredecessor);
                }
            }
        } catch (const std::bad_alloc&) {
            // folded_ still names the first event not folded.
        }
    }

    void reset() noexcept {
        core_ = nullptr;
        folded_ = 0;
        slots_.clear();
    }

    // The definition the backward scan returns for `handle`, or null when
    // the index cannot vouch for it.
    const RequestDefinition* find(const WorkingRequestCore& core,
                                  const RequestHandle& handle) const noexcept {
        const auto& history = core.history();
        if (&core != core_ || folded_ != history.size()) return nullptr;
        if (handle.incarnation == 0 || handle.incarnation > slots_.size()) return nullptr;
        const std::uint64_t slot = slots_[static_cast<std::size_t>(handle.incarnation - 1)];
        if (slot == 0) return nullptr;
        const std::uint64_t position = (slot >> 2) - 1;
        if (position >= history.size()) return nullptr;
        const auto& event = history[static_cast<std::size_t>(position)];
        const RequestDefinition* definition = nullptr;
        if ((slot & 3) == kAccepted) {
            if (const auto* accepted = std::get_if<AcceptedEvent>(&event))
                definition = accepted->definition.get();
        } else if (const auto* replaced = std::get_if<ReplacedEvent>(&event)) {
            definition = (slot & 3) == kPredecessor ? replaced->predecessor_definition.get()
                                                    : replaced->successor_definition.get();
        }
        if (!definition || !(definition->handle == handle)) return nullptr;
        ++answered_;
        return definition;
    }

    // How many lookups the index answered in place of the scan.
    std::uint64_t answered() const noexcept { return answered_; }

    // Publishes an index to the core it indexes for the lifetime of this
    // object, on this thread; a null index publishes nothing. Nests.
    class Publication {
    public:
        explicit Publication(const DefinitionIndex* index) noexcept
            : previous_(published()) {
            published() = index;
        }
        ~Publication() { published() = previous_; }
        Publication(const Publication&) = delete;
        Publication& operator=(const Publication&) = delete;

    private:
        const DefinitionIndex* previous_;
    };

    // The published index's answer for `handle` in `core`, or null when no
    // index is published for that core or it cannot vouch for the answer.
    static const RequestDefinition* published_lookup(const WorkingRequestCore& core,
                                                     const RequestHandle& handle) noexcept {
        const DefinitionIndex* index = published();
        return index ? index->find(core, handle) : nullptr;
    }

private:
    static constexpr std::uint64_t kAccepted = 1;
    static constexpr std::uint64_t kPredecessor = 2;
    static constexpr std::uint64_t kSuccessor = 3;

    static const DefinitionIndex*& published() noexcept {
        static thread_local const DefinitionIndex* index = nullptr;
        return index;
    }

    void note(const RequestDefinition& definition, std::uint64_t kind) {
        const std::uint64_t incarnation = definition.handle.incarnation;
        // Incarnations come from the run's own counter, one per request, so
        // they stay within the events that name requests. One far past the
        // history is left to the scan; the bound grows with the position, so
        // an incarnation once indexed is indexed at every later event too.
        if (incarnation == 0 || incarnation > 4 * (folded_ + 1) + 1024) return;
        if (incarnation > slots_.size()) slots_.resize(static_cast<std::size_t>(incarnation), 0);
        slots_[static_cast<std::size_t>(incarnation - 1)] = ((folded_ + 1) << 2) | kind;
    }

    const WorkingRequestCore* core_ = nullptr;
    std::uint64_t folded_ = 0;
    // incarnation - 1 -> ((position + 1) << 2) | kind, or 0 when no event
    // names the incarnation.
    std::vector<std::uint64_t> slots_;
    mutable std::uint64_t answered_ = 0;
};

}  // inline namespace native_order_v6
}  // namespace pineforge::native_order
