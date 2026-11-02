#pragma once

#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/detail/id_table_read_cache.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace pineforge::detail {

// An AVL tree of byte-string keys and fixed-size values in positional scratch.
// A node is 56 + sizeof(Value) bytes followed by the key bytes. Parent links
// allow rotations and sorted walks with O(1) RAM, including adversarial ids.
// The independent next link preserves first insertion order. Updating a value
// overwrites its slot: repeated ids cost no additional disk or resident heap.
// File offsets/tree links are storage metadata; callers fold logical values.
template<class Value>
class DiskIdTable {
    static_assert(std::is_trivially_copyable_v<Value>, "disk id values are fixed records");
    struct Links {
        std::uint64_t left = 0, right = 0, parent = 0, next = 0;
        std::uint64_t key_size = 0, height = 1, reserved = 0;
    };
    static_assert(sizeof(Links) == 56, "scratch id node layout");

public:
    using Offset = std::uint64_t;
    DiskIdTable() = default;
    explicit DiskIdTable(std::size_t cache_bytes) : read_cache_(cache_bytes) {}
    explicit DiskIdTable(IdTableReadCache::Policy cache) : read_cache_(cache) {}
    explicit DiskIdTable(CacheCapacity capacity)
        : DiskIdTable(IdTableReadCache::policy_for_capacity(capacity)) {}
    void set_cache_capacity(CacheCapacity capacity) {
        read_cache_.set_cache_capacity(capacity);
    }
    DiskIdTable(const DiskIdTable&) = delete;
    DiskIdTable& operator=(const DiskIdTable&) = delete;
    DiskIdTable(DiskIdTable&& other) noexcept { *this = std::move(other); }
    DiskIdTable& operator=(DiskIdTable&& other) noexcept {
        if (this != &other) {
            read_cache_ = std::move(other.read_cache_); // Both owners lose every valid line.
            file_ = std::move(other.file_);
            root_ = std::exchange(other.root_, 0);
            first_ = std::exchange(other.first_, 0);
            last_ = std::exchange(other.last_, 0);
            end_ = std::exchange(other.end_, 1);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }
    void set_store(ScratchStore store) noexcept { file_.set_store(store); }
    bool arena_backed() const noexcept { return file_.arena_backed(); }
    bool bounded_cohorts() const noexcept {
        return file_.scratch_owner() && file_.scratch_resource() == ScratchResource::cohort;
    }
    void set_directory(const std::string& directory) { file_.set_directory(directory); }
    void bind_scratch(const std::shared_ptr<ScratchSlab>& slab, ScratchResource resource) {
        if (size_ != 0)
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                         "scratch id backend changed after use");
        if (slab && resource != ScratchResource::cohort && resource != ScratchResource::exit_rank)
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                         "scratch id table requires a tree region");
        read_cache_.release();
        file_.bind_scratch(slab, resource);
    }
    void clear() noexcept {
        read_cache_.release();
        file_.clear();
        root_ = first_ = last_ = size_ = 0;
        end_ = 1; // offset zero is the null link
    }
    std::uint64_t size() const noexcept { return size_; }
    std::uint64_t file_bytes() const noexcept { return size_ == 0 ? 0 : end_; }
    Offset first_inserted() const noexcept { return first_; }
    Offset next_inserted(Offset at) const { return links(at).next; }
    std::string key(Offset at) const { return read_key(at, links(at)); }
    Offset first_sorted() const { return leftmost(root_); }
    Offset next_sorted(Offset at) const {
        auto row = links(at);
        if (row.right != 0) return leftmost(row.right);
        Offset child = at;
        at = row.parent;
        while (at != 0) {
            row = links(at);
            if (row.left == child) break;
            child = at;
            at = row.parent;
        }
        return at;
    }

    std::optional<Offset> find(const std::string& key) const {
        Offset at = root_;
        while (at != 0) {
            const auto row = links(at);
            const int order = key.compare(read_key(at, row));
            if (order == 0) return at;
            at = order < 0 ? row.left : row.right;
        }
        return std::nullopt;
    }
    // Read-only whole-node ticket, also used before a caller spills resident
    // facts or accepts a kernel command. Duplicates need no count/byte growth.
    // Disk mode deliberately retains its existing search and I/O behaviour.
    void require_insert(const std::string& key) const {
        if (!file_.scratch_owner() || find(key)) return;
        require_new_key(key.size());
    }
    // enumerate(visit) replays a stable, borrowed workset. Count each absent
    // key once without a growing side index: O(workset^2) comparisons, O(1)
    // retained scratch. No caller may mutate the workset during this ticket.
    template<class Enumerate> void require_inserts(Enumerate&& enumerate) const {
        if (!file_.scratch_owner()) return;
        std::uint64_t ordinal = 0, extra_rows = 0, extra_bytes = 0;
        enumerate([&](const std::string& key) {
            require_id_size(key.size());
            const auto current = ordinal++;
            if (find(key)) return;
            bool repeated = false;
            std::uint64_t prior = 0;
            enumerate([&](const std::string& other) {
                if (prior++ < current && other == key) repeated = true;
            });
            if (repeated) return;
            const auto maximum = std::numeric_limits<std::uint64_t>::max();
            if (extra_rows == maximum || key.size() > maximum - extra_bytes) {
                const auto resource = file_.scratch_resource();
                const auto& manifest = file_.scratch_owner()->manifest();
                throw ScratchCapacityError(ids_total_label(resource), extra_rows == maximum
                    ? scratch_count_limit_message(resource,
                        resource == ScratchResource::cohort ? manifest.U : manifest.V)
                    : scratch_byte_limit_message(resource,
                        resource == ScratchResource::cohort ? manifest.B_U : manifest.B_V));
            }
            ++extra_rows;
            extra_bytes += key.size();
            require_growth(extra_rows, extra_bytes);
        });
    }
    Value get(Offset at) const {
        validate(at);
        Value value{};
        read(at + sizeof(Links), &value, sizeof value);
        return value;
    }
    void set(Offset at, const Value& value) {
        validate(at);
        write(at + sizeof(Links), &value, sizeof value);
    }
    std::pair<Offset, bool> insert(const std::string& key, const Value& value) {
        Offset parent = 0;
        Offset at = root_;
        bool left = false;
        while (at != 0) {
            const auto row = links(at);
            const int order = key.compare(read_key(at, row));
            if (order == 0) return {at, false};
            parent = at;
            left = order < 0;
            at = left ? row.left : row.right;
        }
        require_new_key(key.size());
        constexpr std::size_t header = sizeof(Links) + sizeof(Value);
        const bool arena = file_.opened() ? file_.arena_backed() : ScratchArena::attached() != nullptr;
        ScratchFile::check_extent(end_, header, arena, ScratchFile::IoRaiser::auxiliary);
        ScratchFile::check_extent(end_ + header, key.size(), arena, ScratchFile::IoRaiser::auxiliary);
        if (size_ == std::numeric_limits<std::uint64_t>::max())
            throw AuxiliaryScratchIoError(EFBIG, "scratch id count exhausted");
        file_.open("pineforge-ids");
        at = end_;
        Links row;
        row.parent = parent;
        row.key_size = key.size();
        write(at, &row, sizeof row);
        write(at + sizeof(Links), &value, sizeof value);
        write(at + header, key.data(), key.size());
        // All data bytes exist before linking the row into the logical table.
        // Any partial link/rotation I/O error fails the run; it is never
        // converted to a successful absent lookup or retried as another id.
        end_ += header + key.size();
        if (parent != 0) {
            auto old = links(parent);
            (left ? old.left : old.right) = at;
            put_links(parent, old);
        } else root_ = at;
        if (last_ != 0) {
            auto previous = links(last_);
            previous.next = at;
            put_links(last_, previous);
        } else first_ = at;
        last_ = at;
        ++size_;
        rebalance(parent);
        return {at, true};
    }
    template<class F> void for_each_sorted(F&& visit) const {
        for (Offset at = first_sorted(); at != 0; at = next_sorted(at))
            visit(key(at), get(at));
    }
    template<class F> void for_each_inserted(F&& visit) const {
        for (Offset at = first_; at != 0;) {
            const auto row = links(at);
            visit(read_key(at, row), get(at));
            at = row.next;
        }
    }

private:
    static constexpr ScratchResource ids_total_label(ScratchResource resource) noexcept {
        return resource == ScratchResource::cohort ? ScratchResource::cohort_ids_total : resource;
    }

    void read(Offset offset, void* data, std::size_t length) const {
        // disk_backed() is false for any arena, even an unjailed file arena,
        // and for a slab. Never use the attached arena's inherited fd here.
        read_cache_.read(file_, file_.disk_backed(), end_, 0, offset, data, length);
    }
    void write(Offset offset, const void* data, std::size_t length) {
        if (file_.disk_backed()) read_cache_.before_write(0, end_, offset, length);
        file_.write(offset, data, length); // Always immediate, including errors.
    }
    void require_id_size(std::size_t key_size) const {
        if (file_.scratch_owner() && file_.scratch_resource() == ScratchResource::cohort
            && key_size > 64)
            throw ScratchCapacityError(ScratchResource::cohort, "id longer than 64 bytes");
    }
    void require_new_key(std::size_t key_size) const {
        require_id_size(key_size);
        require_growth(1, key_size);
    }
    void require_growth(std::uint64_t extra_rows, std::uint64_t extra_bytes) const {
        const auto& slab = file_.scratch_owner();
        if (!slab) return;
        const auto resource = file_.scratch_resource();
        const auto& manifest = slab->manifest();
        const auto rows = resource == ScratchResource::cohort ? manifest.U : manifest.V;
        const auto bytes = resource == ScratchResource::cohort ? manifest.B_U : manifest.B_V;
        constexpr std::uint64_t header = sizeof(Links) + sizeof(Value);
        // Packed nodes have no inter-row alignment: the append cursor minus
        // the null byte and fixed headers is exactly aggregate stored key bytes.
        const auto key_bytes = end_ - 1 - header * size_;
        if (size_ > rows || extra_rows > rows - size_)
            throw ScratchCapacityError(ids_total_label(resource),
                                       scratch_count_limit_message(resource, rows));
        if (key_bytes > bytes || extra_bytes > bytes - key_bytes)
            throw ScratchCapacityError(ids_total_label(resource),
                                       scratch_byte_limit_message(resource, bytes));
        const auto capacity = slab->region(resource).capacity;
        if (end_ > capacity || extra_rows > (capacity - end_) / header)
            throw ScratchCapacityError(ids_total_label(resource),
                                       scratch_byte_limit_message(resource, bytes));
        const auto fixed_bytes = header * extra_rows;
        if (extra_bytes > capacity - end_ - fixed_bytes)
            throw ScratchCapacityError(ids_total_label(resource),
                                       scratch_byte_limit_message(resource, bytes));
        slab->require_extend(resource, end_ + fixed_bytes + extra_bytes);
    }
    [[noreturn]] void io_error(int error, const char* operation) const {
        throw AuxiliaryScratchIoError(error, operation);
    }
    Offset leftmost(Offset at) const {
        while (at != 0) {
            const auto row = links(at);
            if (row.left == 0) break;
            at = row.left;
        }
        return at;
    }
    void validate(Offset at) const {
        if (at == 0 || at >= end_ || end_ - at < sizeof(Links) + sizeof(Value))
            io_error(EIO, "scratch id invalid offset");
    }
    Links links(Offset at) const {
        validate(at);
        Links row{};
        read(at, &row, sizeof row);
        if (row.height == 0 || row.height > 128
            || row.key_size > end_ - at - sizeof(Links) - sizeof(Value))
            io_error(EIO, "scratch id invalid node");
        return row;
    }
    void put_links(Offset at, const Links& row) { write(at, &row, sizeof row); }
    std::string read_key(Offset at, const Links& row) const {
        if (row.key_size > std::string{}.max_size())
            io_error(EFBIG, "scratch id key too long");
        std::string key(static_cast<std::size_t>(row.key_size), '\0');
        read(at + sizeof(Links) + sizeof(Value), key.data(), key.size());
        return key;
    }
    std::uint64_t height(Offset at) const { return at == 0 ? 0 : links(at).height; }
    void refresh(Links& row) const { row.height = 1 + std::max(height(row.left), height(row.right)); }
    int balance(const Links& row) const {
        return static_cast<int>(height(row.left)) - static_cast<int>(height(row.right));
    }
    Offset rotate(Offset at, bool left) {
        auto old = links(at);
        const Offset top_at = left ? old.right : old.left;
        auto top = links(top_at);
        const Offset middle_at = left ? top.left : top.right;
        const Offset parent_at = old.parent;
        if (parent_at != 0) {
            auto parent = links(parent_at);
            (parent.left == at ? parent.left : parent.right) = top_at;
            put_links(parent_at, parent);
        } else root_ = top_at;
        top.parent = parent_at;
        old.parent = top_at;
        (left ? old.right : old.left) = middle_at;
        if (middle_at != 0) {
            auto middle = links(middle_at);
            middle.parent = at;
            put_links(middle_at, middle);
        }
        refresh(old);
        put_links(at, old);
        (left ? top.left : top.right) = at;
        refresh(top);
        put_links(top_at, top);
        return top_at;
    }
    void rebalance(Offset at) {
        while (at != 0) {
            auto row = links(at);
            refresh(row);
            put_links(at, row);
            const int lean = balance(row);
            if (lean > 1) {
                if (balance(links(row.left)) < 0) (void)rotate(row.left, true);
                at = rotate(at, false);
            } else if (lean < -1) {
                if (balance(links(row.right)) > 0) (void)rotate(row.right, false);
                at = rotate(at, true);
            }
            at = links(at).parent;
        }
    }

    ScratchFile file_{ScratchFile::IoRaiser::auxiliary};
    IdTableReadCache read_cache_;
    Offset root_ = 0, first_ = 0, last_ = 0, end_ = 1;
    std::uint64_t size_ = 0;
};
} // namespace pineforge::detail
