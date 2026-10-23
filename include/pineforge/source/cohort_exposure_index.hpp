#pragma once

#include <pineforge/source/cohort_exposure_arith.hpp>
#include <pineforge/source/cohort_unit_owner.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace pineforge::source {

// One table attachment, shared by all cohorts. Roots are caller-owned plain
// values, never cached here. The caller publishes Edit's three descriptor
// fields through CohortUnitOwner in the SAME transaction as the edit.
// Mutations join an outer transaction or own one, like CohortUnitOwner.
// Failed operations discard frames and poison the attachment: explicitly
// roll back any active transaction, then attach again. An outer rollback
// after successful inner work also requires attach; discard provisional
// Edit/Root/reclamation cursors and reload the committed descriptor.
//
// A compressed MSB-first binary radix tree: only leaves carry operands;
// internal crit-bit indices strictly increase along a path. There are 2V-1
// records and at most 64 internal ancestors. A replacement writes h+1 nodes;
// the caller's descriptor publication makes E1 P6's h+2 puts.
class CohortExposureIndex {
public:
    using Store = ::pineforge::detail::ScratchStore;
    using Handle = ::pineforge::detail::ScratchHandle;
    static constexpr std::size_t node_bytes = 936;
    static constexpr std::size_t maximum_height = 64;
    static constexpr std::uint64_t record_kind = 6;
    static constexpr std::uint64_t branch_format = UINT64_C(0x706665696e640100);
    static constexpr std::uint64_t leaf_format = branch_format + 1;

    struct Root {
        Handle root = 0;
        std::uint64_t count = 0;
        std::uint64_t table_epoch = 0, cohort = 0, unit_epoch = 0;
    };
    struct Edit {
        Root root{};
        std::uint64_t answer_bits = 0, puts = 0, loads = 0;
    };
    struct Metrics {
        std::uint64_t puts = 0, loads = 0, key_searches = 0;
        std::uint64_t path_length = 0, node_visits = 0, real_additions = 0;
    };

    CohortExposureIndex() = default;
    CohortExposureIndex(const CohortExposureIndex&) = delete;
    CohortExposureIndex& operator=(const CohortExposureIndex&) = delete;
    void attach(Store&, std::uint64_t table_owner = 1); // read-only; no records created
    bool recovery_required() const noexcept { return failed_; }
    Edit insert(Root, std::uint64_t position, std::uint64_t raw_unit_bits);
    Edit replace(Root, std::uint64_t position, std::uint64_t raw_unit_bits);
    Edit erase(Root, std::uint64_t position); // absent: unchanged root, zero puts
    std::uint64_t answer(Root) const;
    // A reference makes both results explicit: return the old root and clear
    // the caller's live root. No node is read, rewritten or freed.
    Root detach(Root& current);
    // Detached summaries/counts/heights are no longer arithmetic metadata.
    // Splice one leftmost leaf + its parent, changing at most one grandparent;
    // the caller persists this plain cursor in its reclamation job transaction.
    bool reclaim_step(Root& detached);
    void validate(Root) const; // test-only O(V*h) rebuild checks, fixed workspace
    Metrics metrics() const noexcept { return metrics_; }
    std::size_t resident_bytes() const noexcept { return sizeof(*this); }

private:
    using Status = ::pineforge::detail::ScratchStatus;
    using Field = ::pineforge::detail::ScratchField;
    using Summary = cohort_exposure_arith::Summary;
    struct Node {
        std::uint64_t tag = leaf_format, id = 0;
        Handle left = 0, right = 0;
        std::uint64_t position_or_bit = 0, raw = 0, count = 1, height = 0;
        Summary sum{};
        bool leaf() const noexcept { return tag == leaf_format; }
    };
    static_assert(sizeof(Node) == node_bytes && offsetof(Node, sum) == 64
                      && std::is_trivially_copyable_v<Node> && std::is_standard_layout_v<Node>,
                  "eight structural words plus the 872-byte summary");
    class RecordKey;
    class NodeSource;
    enum class Operation { insert, replace, erase };
    [[noreturn]] static void invalid();
    [[noreturn]] static void exhausted();
    static bool right_of(std::uint64_t position, std::uint64_t bit);
    static std::uint64_t differing_bit(std::uint64_t a, std::uint64_t b);
    static void check_root_shape(Root);
    void ready() const;
    void poison() const noexcept;
    void start() const;
    void check(Status) const;
    void load(Root, Handle, Node&) const;
    Handle save(Root, const Node&, Handle expected = 0);
    void pull(Root);
    Handle descend(Root, std::uint64_t position, std::size_t& depth) const;
    void repair(Root&, std::size_t depth, Handle old_child, Handle new_child);
    bool any_id_from(Root, std::uint64_t first) const;
    std::uint64_t allocate_ids(Root, std::uint64_t number) const;
    std::uint64_t evaluate_tree(Root) const;
    Edit mutate(Root, std::uint64_t position, std::uint64_t raw, Operation);
    Edit result(Root);

    Store* store_ = nullptr;
    std::uint64_t table_owner_ = 1;
    mutable bool failed_ = false, busy_ = false;
    mutable Metrics metrics_{};
    // Three frames total. Frame 2 also serves as save()'s wire buffer once
    // pull() has consumed its right child. No frame is retained per ancestor.
    mutable std::array<Node, 3> frames_{};
    mutable std::array<Handle, 128> path_{};
};

} // namespace pineforge::source

#include <pineforge/source/cohort_exposure_index_impl.hpp>
