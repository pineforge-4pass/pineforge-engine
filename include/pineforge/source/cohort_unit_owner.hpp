#pragma once

#include <pineforge/detail/scratch_domains.hpp>
#include <pineforge/detail/working_store.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace pineforge::source {

enum class ExposureOwnerError {
    duplicate_origin_incarnation,
    invalid_state,
    exhausted,
    recovery_required,
};

// Diagnostic integers are typed members, not undeclared run-failure arguments.
// engine_invariant has the existing empty argument vocabulary.
class ExposureOwnerFailure final : public coded<std::logic_error> {
public:
    ExposureOwnerFailure(ExposureOwnerError reason, std::uint64_t cohort = 0,
                         std::uint64_t incarnation = 0, std::uint64_t old_count = 0,
                         std::uint64_t attempted_position = 0)
        : coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                  "cohort unit owner invariant"),
          reason(reason), cohort(cohort), incarnation(incarnation),
          old_count(old_count), attempted_position(attempted_position) {}
    ExposureOwnerError reason;
    std::uint64_t cohort, incarnation, old_count, attempted_position;
};

// Standalone canonical backend for B's future mandatory publisher. This is not
// a second publisher or a callback. Round 2 must make its mutation entry points
// private to that publisher; the read facade deliberately has no mutators.
//
// One instance per table, shared by every cohort. Facades contain only the
// attachment and cohort identity, never a root or a per-cohort workspace.
// Parent links replace a traversal stack; every ascent/search is bounded by
// 128. All transfer frames are <=512 bytes and their number is fixed. Neither
// the owner nor an operation allocates population-dependent resident state.
//
// Each operation reloads persisted header/descriptor fields. Transactions join
// the caller's transaction (or own one using working_store::transaction).
// Failure discards provisional frames/handles and poisons the attachment.
// Recovery is explicit store.rollback(), then attach(). After an outer rollback
// the caller must also attach(), even if our inner operation succeeded.
//
// Kind 6 belongs to EI1. position_root/count are opaque, preserved fields here;
// the round-2 publisher must detach/reclaim EI1's position tree in its same
// transaction. This round's GC owns only kind-4 nodes and kind-7 unit jobs.
class CohortUnitOwner {
public:
    using Store = ::pineforge::detail::ScratchStore;
    using Handle = ::pineforge::detail::ScratchHandle;
    using value_type = std::pair<std::uint64_t, double>;
    static constexpr std::size_t maximum_height = 128;
    // A new insertion creates one node. Clear creates one job. Two service
    // work units per mutation pay for one node plus its eventual job retirement.
    static constexpr unsigned reclamation_budget = 2;
    static constexpr std::uint64_t format = 0x706663756e697401ULL;

    struct Descriptor {
        Handle record = 0; // transient direct descriptor handle, not encoded
        std::uint64_t table_epoch = 0, cohort = 0;
        std::uint64_t unit_epoch = 1, origin_epoch = 1;
        Handle unit_root = 0, unit_first = 0, unit_last = 0;
        std::uint64_t unit_count = 0;
        Handle position_root = 0;
        std::uint64_t position_count = 0, answer_bits = 0, duplicate_slots = 0;
    };
    struct Header {
        std::uint64_t table_epoch = 1, run_identity_reference = 0;
        std::uint64_t duplicate_slots = 0, cohort_count = 0;
        Handle reclaim_first = 0, reclaim_last = 0;
        std::uint64_t reclaim_jobs = 0, reclaim_nodes = 0;
    };
    struct OriginSlot {
        std::uint64_t count = 0, single_position = 0;
    };

    class ReadFacade;
    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = CohortUnitOwner::value_type;
        using difference_type = std::ptrdiff_t;
        using reference = const value_type&;
        using pointer = const value_type*;
        const_iterator() = default;
        reference operator*() const;
        pointer operator->() const { return &**this; }
        const_iterator& operator++();
        const_iterator operator++(int) { auto old = *this; ++*this; return old; }
        std::uint64_t raw_bits() const;
        Handle node_handle() const noexcept { return id_; }
        friend bool operator==(const const_iterator& a, const const_iterator& b) {
            return a.owner_ == b.owner_ && a.cohort_ == b.cohort_ && a.id_ == b.id_;
        }
        friend bool operator!=(const const_iterator& a, const const_iterator& b) {
            return !(a == b);
        }
    private:
        friend class CohortUnitOwner;
        friend class ReadFacade;
        const_iterator(const CohortUnitOwner*, const Descriptor&, Handle);
        void refresh() const;
        const CohortUnitOwner* owner_ = nullptr;
        std::uint64_t cohort_ = 0, table_epoch_ = 0, unit_epoch_ = 0;
        std::uint64_t attachment_ = 0;
        Handle id_ = 0;
        mutable Handle next_ = 0;
        mutable std::uint64_t revision_ = 0, bits_ = 0;
        mutable value_type row_{};
    };
    using iterator = const_iterator;
    // Erase uses a stable node identity, never a dense rank.
    using ErasedIterator = const_iterator;

    class ReadFacade {
    public:
        std::uint64_t size() const;
        bool empty() const { return size() == 0; }
        const_iterator begin() const;
        const_iterator end() const;
        const_iterator cbegin() const { return begin(); }
        const_iterator cend() const { return end(); }
        const_iterator find(std::uint64_t incarnation) const;
        std::uint64_t count(std::uint64_t key) const { return contains(key) ? 1 : 0; }
        bool contains(std::uint64_t key) const { return find(key) != end(); }
        double at(std::uint64_t key) const;
        // Counted selection is also useful to validate the subtree counts.
        const_iterator nth(std::uint64_t rank) const;
    private:
        friend class CohortUnitOwner;
        ReadFacade(const CohortUnitOwner* owner, std::uint64_t cohort)
            : owner_(owner), cohort_(cohort) {}
        const CohortUnitOwner* owner_;
        std::uint64_t cohort_;
    };

    CohortUnitOwner() = default;
    CohortUnitOwner(const CohortUnitOwner&) = delete;
    CohortUnitOwner& operator=(const CohortUnitOwner&) = delete;
    CohortUnitOwner(CohortUnitOwner&&) = delete;
    CohortUnitOwner& operator=(CohortUnitOwner&&) = delete;

    void create(Store&, std::uint64_t table_owner = 1, std::uint64_t table_epoch = 1);
    void attach(Store&, std::uint64_t table_owner = 1); // read-only, never creates
    bool recovery_required() const noexcept { return failed_; }
    Header header() const;
    Descriptor descriptor(std::uint64_t cohort) const;
    std::optional<Descriptor> lookup(std::string_view full_source_id) const;
    bool ensure_cohort(std::string_view full_source_id, std::uint64_t cohort);
    ReadFacade units(std::uint64_t cohort) const { return {this, cohort}; }

    bool insert_bits(std::uint64_t cohort, std::uint64_t incarnation, std::uint64_t bits);
    void set_bits(std::uint64_t cohort, std::uint64_t incarnation, std::uint64_t bits);
    void set(std::uint64_t cohort, std::uint64_t incarnation, double value) {
        std::uint64_t bits = 0;
        static_assert(sizeof(value) == sizeof(bits), "binary64 unit payload");
        std::memcpy(&bits, &value, sizeof(bits));
        set_bits(cohort, incarnation, bits);
    }
    std::uint64_t erase(std::uint64_t cohort, std::uint64_t incarnation);
    ErasedIterator erase(std::uint64_t cohort, const_iterator);
    void clear(std::uint64_t cohort);

    OriginSlot origin_slot(std::uint64_t cohort, std::uint64_t incarnation) const;
    void require_production() const; // O(1) duplicate-total admission
    void append_origin(std::uint64_t cohort, std::uint64_t incarnation,
                       std::uint64_t position); // typed refusal before any write
    void fixture_append_origin(std::uint64_t cohort, std::uint64_t incarnation,
                               std::uint64_t position);
    // Canonical domain-50 positions remain authoritative. On 2 -> 1 the caller
    // supplies the surviving position from that relation, not from unit rows.
    void retire_origin(std::uint64_t cohort, std::uint64_t incarnation,
                       std::uint64_t retiring_position,
                       std::optional<std::uint64_t> sole_survivor = {});
    template<class It>
    void fixture_append_origins(std::uint64_t cohort, It first, It last);
    template<class It>
    void bootstrap_from(std::uint64_t cohort, It first, It last);
    // EI1 alone supplies these plain fields; no arithmetic or kind-6 traversal.
    void publish_position_fields(std::uint64_t cohort, Handle position_root,
                                 std::uint64_t position_count, std::uint64_t answer_bits);

private:
    using Status = ::pineforge::detail::ScratchStatus;
    using Field = ::pineforge::detail::ScratchField;
    class RecordKey;
    struct Node {
        Handle id = 0;
        std::uint64_t table_epoch = 0, cohort = 0, epoch = 0, incarnation = 0, bits = 0;
        Handle left = 0, right = 0, parent = 0;
        std::uint64_t height = 1, count = 1;
        Handle previous = 0, next = 0;
    };
    struct Job {
        Handle id = 0;
        std::uint64_t table_epoch = 0, cohort = 0, epoch = 0;
        Handle cursor = 0, next = 0;
        std::uint64_t remaining = 0;
    };
    // Exactly one workspace, owned by the table rather than its cohort rows.
    struct Workspace { Header header{}; Descriptor descriptor{}; };
    void check_attached() const;
    [[noreturn]] static void invalid();
    [[noreturn]] static void exhausted();
    static std::uint64_t increment(std::uint64_t);
    Handle find_record(const RecordKey&) const;
    template<std::size_t N> std::array<std::uint64_t, N>
    read_words(Handle, std::uint64_t key_bytes = 0) const;
    template<std::size_t N> Handle put_words(const RecordKey&,
                                            const std::array<std::uint64_t, N>&, Handle = 0);
    Header load_header() const;
    void save_header(const Header&);
    Descriptor load_descriptor(std::uint64_t epoch, std::uint64_t cohort) const;
    Descriptor read_descriptor(Handle, std::uint64_t epoch, std::uint64_t cohort) const;
    void save_descriptor(Descriptor&);
    Node load_node(Handle, std::uint64_t table_epoch, std::uint64_t cohort,
                   std::uint64_t unit_epoch) const;
    Node load_node(Handle id, const Descriptor& d) const {
        return load_node(id, d.table_epoch, d.cohort, d.unit_epoch);
    }
    void save_node(Node&);
    Handle search(const Descriptor&, std::uint64_t incarnation) const;
    void pull(Node&);
    void parent_link(Descriptor&, Handle parent, Handle old_child, Handle new_child);
    Handle rotate_left(Descriptor&, Handle);
    Handle rotate_right(Descriptor&, Handle);
    void balance(Descriptor&, Handle);
    bool upsert(Descriptor&, std::uint64_t incarnation, std::uint64_t bits, bool replace);
    Handle erase_node(Descriptor&, Handle);
    OriginSlot load_slot(const Descriptor&, std::uint64_t incarnation, Handle* = nullptr) const;
    void append_slot(Descriptor&, Header&, std::uint64_t, std::uint64_t, bool fixture);
    Job load_job(Handle) const;
    void save_job(Job&);
    void enqueue(Descriptor&, Header&);
    void reclaim(Header&);
    template<class Edit> void edit(std::uint64_t cohort, Edit&&);
    void poison() noexcept;

    Store* store_ = nullptr;
    std::uint64_t table_owner_ = 1, attachment_ = 0, revision_ = 1;
    bool failed_ = false, editing_ = false;
    Workspace workspace_{};
};

} // namespace pineforge::source

#include <pineforge/source/cohort_unit_owner_impl.hpp>
