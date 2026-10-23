#pragma once

#include <pineforge/detail/id_pair_scratch_file.hpp>
#include <pineforge/engine.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace pineforge::detail {

// Exact action of a byte sequence on ANY incoming BrokerStateHashSink::h.
// XOR changes only the low byte. Induction on bytes gives
// F(256*q+r) = F(r) + q*(F(256)-F(0)), modulo 2^64.
// Store F(0)..F(256); concatenation X,Y is Y(X(r)) at all 257 seeds.
class BrokerStateByteFold {
public:
    BrokerStateByteFold() noexcept { reset(); }
    void reset() noexcept {
        for (std::size_t r = 0; r < basis_.size(); ++r) basis_[r] = r;
    }
    std::uint64_t apply(std::uint64_t seed) const noexcept {
        return basis_[seed & 255] + (seed >> 8) * (basis_[256] - basis_[0]);
    }
    void bytes(const void* data, std::size_t length) {
        for (auto& seed : basis_) {
            BrokerStateHashSink sink;
            sink.h = seed;
            sink.bytes(data, length);
            seed = sink.h;
        }
    }
    // May replace the left operand in place; after must be a different map.
    void then(const BrokerStateByteFold& after) noexcept {
        for (auto& seed : basis_) seed = after.apply(seed);
    }

private:
    std::array<std::uint64_t, 257> basis_{};
};
static_assert(sizeof(BrokerStateByteFold) == 2056, "exact fold has 257 words");

// A map file must not impose a lower key-count ceiling than the existing
// hash index. Its largest power-of-two slot array has 2^(B-6) slots, where B
// is the number of value bits of off_t, at half load: at most (OFF_MAX/64)+1
// distinct rows. Segment large
// records across a FIXED descriptor array covering that entire population.
// Normal runs open just one segment of each file. No resident directory
// grows with keys; no fold-specific capacity/exhaustion error is introduced.
// MaxOffset is a compile-time seam for exercising segment boundaries in tests.
template<class Record, std::uint64_t MaxOffset =
    static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())>
class IdPairFoldFile {
    static constexpr std::uint64_t kRowsPerSegment = MaxOffset / sizeof(Record);
    static constexpr std::uint64_t kMaximumRows = MaxOffset / 64 + 1;
    static_assert(kRowsPerSegment != 0, "a fold record fits in an off_t file");
    static constexpr std::size_t kSegments = static_cast<std::size_t>(
        (kMaximumRows + kRowsPerSegment - 1) / kRowsPerSegment);
    std::array<IdPairScratchFile, kSegments> files_;
public:
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
    void diagnostic_tag(const char* tag) noexcept {
        for (auto& file : files_) file.diagnostic_tag(tag);
    }
#endif
    bool opened() const noexcept {
        for (const auto& file : files_) if (file.opened()) return true;
        return false;
    }
    void clear() noexcept { for (auto& file : files_) file.reset(); }
    void fixture_count_io(IdPairScratchFile::FixtureIO* io) noexcept {
        for (auto& file : files_) file.fixture_count_io(io);
    }
    void read(std::uint64_t row, void* bytes, std::size_t size = sizeof(Record),
              std::size_t within = 0) const {
        files_[static_cast<std::size_t>(row / kRowsPerSegment)].read(
            (row % kRowsPerSegment) * sizeof(Record) + within, bytes, size);
    }
    void write(std::uint64_t row, const void* bytes, const std::string& directory) {
        auto& file = files_[static_cast<std::size_t>(row / kRowsPerSegment)];
        file.open(directory);
        file.write((row % kRowsPerSegment) * sizeof(Record), bytes, sizeof(Record));
    }
    static std::uint64_t disk_bytes(std::uint64_t rows) noexcept {
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        return rows > maximum / sizeof(Record) ? maximum : rows * sizeof(Record);
    }
};

// Ordered auxiliary index ONLY; IdPairStore still owns lookup, first-seen
// iteration, keys and values. End-of-key is symbol 0, byte b is symbol b+1.
// MSB-first 9-bit symbols give exactly std::string's unsigned-byte order,
// with every prefix before its extensions, including NUL / 0x1f / 0xff.
// Compressed radix pivots strictly increase along a path: for maximum key
// length L there are <=9*(L+1) branches, independently of key population.
// Leaves have <=16 original row offsets and NO map. Only branches have maps.
// No recursion, RAM path stack, key roster, capacity knob or per-key heap.
class IdPairOrderedFold {
public:
    struct Position {
        std::uint64_t byte = 0;
        std::uint64_t mask = 0;
    };
    static bool before(Position a, Position b) noexcept {
        return a.byte < b.byte || (a.byte == b.byte && a.mask > b.mask);
    }
    static unsigned symbol(std::string_view key, std::uint64_t at) noexcept {
        return at < key.size() ? static_cast<unsigned char>(key[static_cast<std::size_t>(at)]) + 1U : 0U;
    }
    static Position differing_symbol(std::uint64_t at, unsigned a, unsigned b) noexcept {
        unsigned mask = 256;
        while ((mask & (a ^ b)) == 0) mask >>= 1;
        return {at, mask}; // callers establish a != b
    }

#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
    IdPairOrderedFold() {
        leaves_.diagnostic_tag("consumed_partial_exit_cycles_.fold.leaves");
        branches_.diagnostic_tag("consumed_partial_exit_cycles_.fold.branches");
        undo_.diagnostic_tag("consumed_partial_exit_cycles_.fold.undo");
    }
#else
    IdPairOrderedFold() = default;
#endif
    IdPairOrderedFold(const IdPairOrderedFold&) = delete;
    IdPairOrderedFold& operator=(const IdPairOrderedFold&) = delete;
    IdPairOrderedFold(IdPairOrderedFold&& other) noexcept { swap(other); }
    IdPairOrderedFold& operator=(IdPairOrderedFold&& other) noexcept {
        if (this != &other) {
            IdPairOrderedFold moved(std::move(other));
            swap(moved);
        }
        return *this;
    }
    void swap(IdPairOrderedFold& other) noexcept {
        using std::swap;
        swap(leaves_, other.leaves_); swap(branches_, other.branches_); swap(undo_, other.undo_);
        swap(root_fold_, other.root_fold_); swap(root_, other.root_);
        swap(leaf_count_, other.leaf_count_); swap(branch_count_, other.branch_count_);
        swap(undo_rows_, other.undo_rows_); swap(undo_count_, other.undo_count_);
        swap(recovering_, other.recovering_);
    }
    void clear() noexcept {
        leaves_.clear(); branches_.clear(); undo_.clear();
        root_fold_.reset();
        root_ = leaf_count_ = branch_count_ = undo_rows_ = undo_count_ = 0;
        recovering_ = false;
    }
    bool opened() const noexcept { return leaves_.opened() || branches_.opened() || undo_.opened(); }
    std::uint64_t disk_bytes() const noexcept {
        return saturated_add(saturated_add(decltype(leaves_)::disk_bytes(leaf_count_),
                                          decltype(branches_)::disk_bytes(branch_count_)),
                             decltype(undo_)::disk_bytes(undo_rows_));
    }
    std::uint64_t apply(std::uint64_t seed) const noexcept { return root_fold_.apply(seed); }
    void fixture_count_io(IdPairScratchFile::FixtureIO* io) noexcept {
        leaves_.fixture_count_io(io); branches_.fixture_count_io(io); undo_.fixture_count_io(io);
    }

private:
    static constexpr std::size_t kBucket = 16;
    struct Leaf {
        std::uint64_t count = 0;
        std::array<std::uint64_t, kBucket> rows{};
    };
    struct Branch {
        Position pivot;
        std::array<std::uint64_t, 2> child{};
        BrokerStateByteFold fold;
    };
    // Also the disk path stack. A complete before-image is written BEFORE
    // touching any published node. Failed journal writes touch no live node.
    struct Snapshot {
        std::uint64_t ref = 0;
        std::array<unsigned char, sizeof(Branch)> bytes{};
    };
    struct Workspace {
        Snapshot snapshot;
        Branch branch;
        Leaf leaf, left, right;
        std::array<std::uint64_t, kBucket + 1> rows{};
        BrokerStateByteFold fold, other;
    };
    static_assert(sizeof(Leaf) == 136 && sizeof(Branch) == 2088 && sizeof(Snapshot) == 2096,
                  "ordered scratch layouts have no padding");
    static_assert(std::is_trivially_copyable_v<Leaf> && std::is_trivially_copyable_v<Branch>,
                  "journalled nodes preserve their complete byte representations");

    static bool is_leaf(std::uint64_t ref) noexcept { return (ref & 1) != 0; }
    static std::uint64_t row_index(std::uint64_t ref) noexcept {
        return (ref >> 1) - 1;
    }
    void read_node(std::uint64_t ref, void* bytes) const {
        if (is_leaf(ref)) leaves_.read(row_index(ref), bytes);
        else branches_.read(row_index(ref), bytes);
    }
    void write_node(std::uint64_t ref, const void* bytes, const std::string& directory) {
        if (is_leaf(ref)) leaves_.write(row_index(ref), bytes, directory);
        else branches_.write(row_index(ref), bytes, directory);
    }
    template<class Node>
    std::uint64_t append_node(const Node& node, std::uint64_t& count, bool leaf,
                              const std::string& directory) {
        const auto ref = ((count + 1) << 1) | (leaf ? 1U : 0U);
        write_node(ref, &node, directory);
        ++count;
        return ref;
    }
    void read_snapshot(std::uint64_t at, Snapshot& snapshot) const {
        undo_.read(at, &snapshot);
    }
    void recover(Snapshot& snapshot, const std::string& directory) {
        if (!recovering_) return;
        // Retryable even if restoration itself has a short write. Lookups
        // use the original table; hashes use the still-committed RAM root.
        for (std::uint64_t at = 0; at < undo_count_; ++at) {
            read_snapshot(at, snapshot);
            write_node(snapshot.ref, snapshot.bytes.data(), directory);
        }
        recovering_ = false;
    }
    template<class Reader>
    static void leaf_fold(const Leaf& leaf, Reader& reader, BrokerStateByteFold& fold) {
        fold.reset();
        for (std::uint64_t i = 0; i < leaf.count; ++i) reader.append(leaf.rows[i], fold);
    }
    template<class Reader>
    void load_fold(std::uint64_t ref, Reader& reader, BrokerStateByteFold& fold) const {
        if (is_leaf(ref)) {
            Leaf leaf;
            read_node(ref, &leaf);
            leaf_fold(leaf, reader, fold);
        } else {
            // Read just the map, without another map-sized stack temporary.
            branches_.read(row_index(ref), &fold, sizeof(fold), offsetof(Branch, fold));
        }
    }

public:
    // Reader provides bounded-buffer comparisons, first differing symbols,
    // and the exact prospective row bytes (s(key), i(value); dead = empty).
    template<class Reader>
    void update(std::string_view key, std::uint64_t row, const std::string& directory, Reader& reader) {
        Workspace work;
        recover(work.snapshot, directory);

        std::uint64_t ref = root_, depth = 0;
        while (ref != 0) {
            work.snapshot.ref = ref;
            read_node(ref, work.snapshot.bytes.data());
            // Offsets are checked by IdPairScratchFile; no fixed depth cap.
            undo_.write(depth, &work.snapshot, directory);
            ++depth;
            undo_rows_ = std::max(undo_rows_, depth);
            if (is_leaf(ref)) {
                std::memcpy(&work.leaf, work.snapshot.bytes.data(), sizeof(Leaf));
                break;
            }
            std::memcpy(&work.branch, work.snapshot.bytes.data(), sizeof(Branch));
            ref = work.branch.child[(symbol(key, work.branch.pivot.byte) & work.branch.pivot.mask) != 0];
        }
        const std::uint64_t ancestors = depth == 0 ? 0 : depth - 1;
        std::uint64_t parents = ancestors;
        bool present = false;
        for (std::uint64_t i = 0; i < work.leaf.count; ++i)
            present = present || work.leaf.rows[i] == row;

        // A new key may differ in a compressed-away prefix ABOVE the leaf
        // we reached. Insert there, preserving the entire existing subtree.
        Position divergence;
        if (!present && work.leaf.count != 0) {
            divergence = reader.difference(work.leaf.rows[0], key);
            for (std::uint64_t i = 0; i < ancestors; ++i) {
                read_snapshot(i, work.snapshot);
                std::memcpy(&work.branch, work.snapshot.bytes.data(), sizeof(Branch));
                if (before(divergence, work.branch.pivot)) { parents = i; break; }
            }
        }

        // From here any exception leaves the cached root and table unchanged.
        // The complete journal is retained for the next mutation's recovery.
        undo_count_ = depth;
        recovering_ = true;
        auto next_leaves = leaf_count_, next_branches = branch_count_;
        if (parents < ancestors) {
            read_snapshot(parents, work.snapshot);
            const auto old_subtree = work.snapshot.ref;
            work.left.count = 1;
            work.left.rows[0] = row;
            const auto new_leaf = append_node(work.left, next_leaves, true, directory);
            leaf_fold(work.left, reader, work.fold);
            load_fold(old_subtree, reader, work.other);
            const unsigned side = (symbol(key, divergence.byte) & divergence.mask) != 0;
            if (side == 0) work.fold.then(work.other);
            else { work.other.then(work.fold); work.fold = work.other; }
            work.branch.pivot = divergence;
            work.branch.child[side] = new_leaf;
            work.branch.child[1 - side] = old_subtree;
            work.branch.fold = work.fold;
            ref = append_node(work.branch, next_branches, false, directory);
        } else {
            std::size_t count = 0;
            bool inserted = present;
            for (std::uint64_t i = 0; i < work.leaf.count; ++i) {
                if (!inserted && reader.compare(work.leaf.rows[i], key) > 0) {
                    work.rows[count++] = row;
                    inserted = true;
                }
                work.rows[count++] = work.leaf.rows[i];
            }
            if (!inserted) work.rows[count++] = row;
            if (count <= kBucket) {
                work.left.count = count;
                std::copy_n(work.rows.begin(), count, work.left.rows.begin());
                if (ref == 0) ref = append_node(work.left, next_leaves, true, directory);
                else write_node(ref, &work.left, directory);
                leaf_fold(work.left, reader, work.fold);
            } else {
                const auto pivot = reader.difference(work.rows[0], work.rows[count - 1]);
                std::size_t split = 0;
                while (split < count && (reader.symbol(work.rows[split], pivot.byte) & pivot.mask) == 0)
                    ++split;
                work.left.count = split; work.right.count = count - split;
                std::copy_n(work.rows.begin(), split, work.left.rows.begin());
                std::copy_n(work.rows.begin() + split, count - split, work.right.rows.begin());
                write_node(ref, &work.left, directory); // reuses the journalled old leaf
                const auto right = append_node(work.right, next_leaves, true, directory);
                leaf_fold(work.left, reader, work.fold);
                leaf_fold(work.right, reader, work.other);
                work.fold.then(work.other);
                work.branch.pivot = pivot; work.branch.child = {ref, right};
                work.branch.fold = work.fold;
                ref = append_node(work.branch, next_branches, false, directory);
            }
        }
        while (parents != 0) {
            read_snapshot(--parents, work.snapshot);
            std::memcpy(&work.branch, work.snapshot.bytes.data(), sizeof(Branch));
            const unsigned side = (symbol(key, work.branch.pivot.byte) & work.branch.pivot.mask) != 0;
            work.branch.child[side] = ref;
            load_fold(work.branch.child[1 - side], reader, work.other);
            if (side == 0) work.fold.then(work.other);
            else { work.other.then(work.fold); work.fold = work.other; }
            work.branch.fold = work.fold;
            write_node(work.snapshot.ref, &work.branch, directory);
            ref = work.snapshot.ref;
        }
        // No throwing operation between publishing this root and the caller
        // publishing its pending table header/slot and live count.
        root_ = ref; root_fold_ = work.fold;
        leaf_count_ = next_leaves; branch_count_ = next_branches;
        recovering_ = false;
    }

    // Resident root + update workspace + the reader's two 512-byte buffers,
    // table rehash buffer, headers and scalar call frames fit under 32 KiB.
    // All variable-depth storage is in undo_, never on the heap or stack.
    static constexpr std::size_t workspace_bytes() noexcept { return sizeof(Workspace); }

private:
    static std::uint64_t saturated_add(std::uint64_t a, std::uint64_t b) noexcept {
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        return b > maximum - a ? maximum : a + b;
    }
    IdPairFoldFile<Leaf> leaves_;
    IdPairFoldFile<Branch> branches_;
    IdPairFoldFile<Snapshot> undo_;
    BrokerStateByteFold root_fold_;
    std::uint64_t root_ = 0, leaf_count_ = 0, branch_count_ = 0;
    std::uint64_t undo_rows_ = 0, undo_count_ = 0;
    bool recovering_ = false;
};
static_assert(sizeof(IdPairOrderedFold) + IdPairOrderedFold::workspace_bytes() + 8192 <= 32768,
              "ordered fold resident root, fixed buffers and call-frame allowance <=32 KiB");

} // namespace pineforge::detail
