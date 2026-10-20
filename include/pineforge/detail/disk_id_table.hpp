#pragma once

#include <pineforge/detail/scratch_file.hpp>

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

// An AVL tree of byte-string keys and fixed-size values in an anonymous file.
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
    DiskIdTable(const DiskIdTable&) = delete;
    DiskIdTable& operator=(const DiskIdTable&) = delete;
    DiskIdTable(DiskIdTable&& other) noexcept { *this = std::move(other); }
    DiskIdTable& operator=(DiskIdTable&& other) noexcept {
        if (this != &other) {
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
    void set_directory(const std::string& directory) { file_.set_directory(directory); }
    void clear() noexcept {
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
    Value get(Offset at) const {
        validate(at);
        Value value{};
        file_.read(at + sizeof(Links), &value, sizeof value);
        return value;
    }
    void set(Offset at, const Value& value) {
        validate(at);
        file_.write(at + sizeof(Links), &value, sizeof value);
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
        constexpr std::size_t header = sizeof(Links) + sizeof(Value);
        const bool arena = file_.opened() ? file_.arena_backed() : ScratchArena::attached() != nullptr;
        ScratchFile::check_extent(end_, header, arena);
        ScratchFile::check_extent(end_ + header, key.size(), arena);
        if (size_ == std::numeric_limits<std::uint64_t>::max())
            throw ScratchIoError(EFBIG, "scratch id count exhausted",
                                 {{"reason", arena ? "arena_io" : "scratch_io"}});
        file_.open("pineforge-ids");
        at = end_;
        Links row;
        row.parent = parent;
        row.key_size = key.size();
        file_.write(at, &row, sizeof row);
        file_.write(at + sizeof(Links), &value, sizeof value);
        file_.write(at + header, key.data(), key.size());
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
    [[noreturn]] void io_error(int error, const char* operation) const {
        throw ScratchIoError(error, operation,
                             {{"reason", file_.arena_backed() ? "arena_io" : "scratch_io"}});
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
        file_.read(at, &row, sizeof row);
        if (row.height == 0 || row.height > 128
            || row.key_size > end_ - at - sizeof(Links) - sizeof(Value))
            io_error(EIO, "scratch id invalid node");
        return row;
    }
    void put_links(Offset at, const Links& row) { file_.write(at, &row, sizeof row); }
    std::string read_key(Offset at, const Links& row) const {
        if (row.key_size > std::string{}.max_size())
            io_error(EFBIG, "scratch id key too long");
        std::string key(static_cast<std::size_t>(row.key_size), '\0');
        file_.read(at + sizeof(Links) + sizeof(Value), key.data(), key.size());
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

    ScratchFile file_;
    Offset root_ = 0, first_ = 0, last_ = 0, end_ = 1;
    std::uint64_t size_ = 0;
};
} // namespace pineforge::detail
