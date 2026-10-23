#pragma once

#include <pineforge/detail/scratch_domains.hpp>
#include <pineforge/detail/working_members.hpp>
#include <array>
#include <exception>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace pineforge::source {

// C/B integration contract (LAB-O1S4C).
//
// This owner replaces ONLY the adapter's live roster. Identity is the FULL
// RequestHandle: incarnation, actual run_number, and all session-key bytes.
// Membership does not imply that a placement exists. All aliases of an
// incarnation, including names with no placement, belong to this owner.
//
// An occurrence id is stable through moves; erase invalidates it, and naming
// the same handle again creates a new id. Order labels compare front before
// back, front tickets decreasing, back tickets increasing. Append creates a
// back label; moves issue fresh labels without relabelling any survivor.
// Labels/ids/tree links encode the already-hashed handle order, not new source
// facts; the existing two roster hash folds must remain unchanged.
//
// B attaches one publisher with set_observer. Each successful insert, erase,
// or move calls it synchronously AFTER canonical application, BEFORE root
// publication/commit, inside the SAME borrowed-store transaction. No event is
// emitted for duplicate append, absent erase, or an already-terminal move.
// A move is ONE event with the same id and before/after labels, never transient
// erase/insert events. A clear is an explicit batch of erase events.
// The callback may update B's derived store records; it must not mutate this
// roster, retain references, commit, roll back, or observe a partial batch
// reentrantly through the host. Failure propagates; no implicit rollback/retry.
//
// visit_incarnation owns the incarnation -> ALL occurrences relation. It
// visits exactly the full-handle directory's matching prefix (not the roster),
// including holes. Its visitor receives Occurrence and returns bool to
// continue. Order of that visit is identity order, NOT roster order. B must
// compare Order labels when needed. A placement edit costs with its k aliases;
// no incarnation-only deduplication or second identity relation is needed.
//
// First bind explicitly migrates memory or domain_pending_roster_legacy rows. Normal
// rebind/reopen only loads the descriptor, discards read frames and reattaches:
// it performs no writes/rebuild. After external rollback, rebind is REQUIRED,
// as for WorkingSequence's cached count. B must similarly recover its roots.
// Attach/rebuild B explicitly at that lifecycle boundary, never during a query.
// First migration may replace unbound occurrence ids; B attaches afterwards.
//
// Private domains: domain_pending_roster_directory = full identity directory;
// domain_pending_roster_tree = counted AVL/root, with item UINT64_MAX reserved
// for the format marker (never an issued node key). domain_pending_projection_index
// is reserved for B. Only two handle frames and fixed node
// locals reside in RAM when bound; the vector is unbound compatibility only.
// Structural traversal/publication is O(log roster); each key-based store
// publication also pays the store's ordered-index lookup. There is no scan
// of names/history and no survivor compaction. Rank selection is counted;
// forward/reverse iteration follows direct previous/next handles.
class PendingRoster {
public:
    using value_type = native_order::RequestHandle;
    using size_type = std::size_t;
    using OccurrenceId = ::pineforge::detail::ScratchHandle;

    struct Order {
        bool front = false;
        std::uint64_t ticket = 0;
        friend bool operator==(Order a, Order b) {
            return a.front == b.front && a.ticket == b.ticket;
        }
        friend bool operator!=(Order a, Order b) { return !(a == b); }
        friend bool operator<(Order a, Order b) {
            if (a.front != b.front) return a.front;
            return a.front ? a.ticket > b.ticket : a.ticket < b.ticket;
        }
    };
    struct Occurrence {
        OccurrenceId id = 0;
        value_type handle{};
        Order order{};
    };
    enum class ChangeKind { insert, erase, move };
    struct Change {
        ChangeKind kind;
        Occurrence before{}, after{};
    };
    using Observer = void (*)(void*, const Change&);
    void set_observer(void* context, Observer observer) noexcept {
        observer_context_ = context; observer_ = observer;
    }

    class const_iterator {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = PendingRoster::value_type;
        using difference_type = std::ptrdiff_t;
        using reference = const value_type&;
        using pointer = const value_type*;
        const_iterator() = default;
        reference operator*() const;
        pointer operator->() const { return &**this; }
        const_iterator& operator++();
        const_iterator operator++(int) { auto old = *this; ++*this; return old; }
        const_iterator& operator--();
        const_iterator operator--(int) { auto old = *this; --*this; return old; }
        friend bool operator==(const const_iterator& a, const const_iterator& b) {
            return a.owner_ == b.owner_ && a.id_ == b.id_ && a.index_ == b.index_;
        }
        friend bool operator!=(const const_iterator& a, const const_iterator& b) {
            return !(a == b);
        }
    private:
        friend class PendingRoster;
        const_iterator(const PendingRoster* owner, OccurrenceId id, std::size_t index)
            : owner_(owner), id_(id), index_(index) {}
        const PendingRoster* owner_ = nullptr;
        OccurrenceId id_ = 0;
        std::size_t index_ = 0;
    };
    using iterator = const_iterator; // Deliberately no writable iterator.
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    PendingRoster() = default;
    PendingRoster(const PendingRoster&) = delete;
    PendingRoster& operator=(const PendingRoster&) = delete;
    PendingRoster(PendingRoster&&) = delete;
    PendingRoster& operator=(PendingRoster&&) = delete;

    void bind(::pineforge::detail::ScratchStore&, std::uint64_t owner = 1);
    bool bound() const noexcept { return store_ != nullptr; }
    std::size_t size() const noexcept {
        return store_ ? static_cast<std::size_t>(state_.count) : memory_.size();
    }
    bool empty() const noexcept { return size() == 0; }
    bool contains(const value_type&) const;
    bool append(const value_type&);
    bool erase(const value_type&);
    bool move_to_front(const value_type& h) { return move(h, true); }
    bool move_to_back(const value_type& h) { return move(h, false); }
    void clear();
    const value_type& operator[](std::size_t) const;
    const value_type& at(std::size_t i) const;
    const value_type& front() const { return at(0); }
    const value_type& back() const { return at(size() - 1); }
    value_type observe_at(std::size_t i) const { return at(i); }
    value_type value_at(std::size_t i) const { return at(i); }
    const_iterator begin() const { validate(); return {this, state_.first, 0}; }
    const_iterator end() const { return {this, 0, size()}; }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }
    const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
    const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }
    void flush() const { validate(); } // No deferred writable frames.

    template<class Visit>
    void visit_incarnation(std::uint64_t incarnation, Visit&& visit) const;
    std::optional<Occurrence> occurrence(const value_type&) const;

    // The five existing detached worksets need independent, store-backed
    // copies while the owner changes. Their explicit lifetime also reclaims
    // temporary rows/count records instead of leaking one owner per copy.
    class Snapshot {
    public:
        explicit Snapshot(const PendingRoster&);
        Snapshot(const Snapshot&) = delete;
        Snapshot& operator=(const Snapshot&) = delete;
        ~Snapshot() noexcept(false);
        auto begin() const { return rows_.cbegin(); }
        auto end() const { return rows_.cend(); }
        std::size_t size() const noexcept { return rows_.size(); }
    private:
        ::pineforge::detail::WorkingSequence<value_type> rows_;
        ::pineforge::detail::ScratchStore* store_ = nullptr;
        std::uint64_t owner_ = 0;
    };
    Snapshot snapshot() const { return Snapshot(*this); }

private:
    using Store = ::pineforge::detail::ScratchStore;
    using Handle = ::pineforge::detail::ScratchHandle;
    using Status = ::pineforge::detail::ScratchStatus;
    using Field = ::pineforge::detail::ScratchField;
    using Key = ::pineforge::detail::working_store::Key;
    struct Node {
        Handle id = 0; // Not encoded: the direct record handle.
        std::uint64_t key = 0;
        Handle left = 0, right = 0, parent = 0;
        std::uint64_t count = 1, height = 1, left_count = 0;
        Handle identity = 0, previous = 0, next = 0;
        Order order{};
    };
    struct State {
        Handle root = 0, first = 0, last = 0;
        std::uint64_t count = 0, next_key = 1, next_ticket = 1;
    };
    struct MemoryRow { Occurrence occurrence; };
    struct Frame { Handle id = 0; value_type handle{}; };
    class IdentityKey;
    static constexpr std::uint64_t format = 0x7066726f73740001ULL;
    static constexpr std::size_t node_bytes = 96, state_bytes = 56;

    void validate() const;
    void drop_frames() const { for (auto& frame : frames_) frame.id = 0; }
    Key key(std::uint64_t item) const {
        return {::pineforge::detail::working_store::domain_pending_roster_tree, owner_, item};
    }
    Node load(Handle) const;
    void save(Node&);
    void load_state();
    void save_state();
    Handle find_identity(const value_type&) const;
    Handle identity_node(Handle) const;
    value_type read_identity(Handle) const;
    const value_type& read_handle(Handle) const;
    Handle select(std::size_t) const;
    void pull(Node&);
    void parent_link(Handle parent, Handle old_child, Handle new_child);
    Handle rotate_left(Handle);
    Handle rotate_right(Handle);
    void balance(Handle);
    void link(Node&, bool front);
    void unlink(const Node&);
    void remove_tree(const Node&);
    bool move(const value_type&, bool front);
    Occurrence describe(const Node& n, const value_type& h) const {
        return {n.id, h, n.order};
    }
    std::uint64_t issue(std::uint64_t& counter) {
        if (counter == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("pending roster identity domain exhausted");
        return counter++;
    }
    void publish(const Change& change) {
        if (observer_) observer_(observer_context_, change);
    }
    template<class Edit> void edit(Edit&& apply);

    Store* store_ = nullptr;
    std::uint64_t owner_ = 1;
    Handle descriptor_ = 0;
    State state_{};
    std::vector<MemoryRow> memory_;
    mutable std::array<Frame, 2> frames_{};
    mutable std::size_t next_frame_ = 0;
    void* observer_context_ = nullptr;
    Observer observer_ = nullptr;
};

} // namespace pineforge::source

#include <pineforge/source/pending_roster_impl.hpp>
