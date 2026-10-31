#pragma once

#include <pineforge/detail/scratch_store.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace command_cache_test {
using namespace pineforge::detail;

inline constexpr std::size_t page_bytes = 512;

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

inline std::vector<std::byte> pattern(std::size_t size, unsigned seed) {
    std::vector<std::byte> result(size);
    for (std::size_t i = 0; i < size; ++i)
        result[i] = static_cast<std::byte>((i * 13u + i / 251u + seed * 29u) & 0xffu);
    return result;
}

inline ScratchBytes bytes(const std::vector<std::byte>& value) noexcept {
    return {value.data(), value.size()};
}

inline ScratchBuffer buffer(std::vector<std::byte>& value) noexcept {
    return {value.data(), value.size()};
}

inline ScratchStoreConfig config() noexcept {
    const ScratchKeyOrder order{
        UINT64_C(0x5046434143484501), nullptr,
        +[](void*, const ScratchByteSource& a, const ScratchByteSource& b,
            int& result) noexcept {
            std::uint64_t left = 0, right = 0;
            if (a.size() != sizeof left || b.size() != sizeof right)
                return ScratchStatus::programming_error;
            auto status = a.read(0, {reinterpret_cast<std::byte*>(&left), sizeof left});
            if (status != ScratchStatus::ok) return status;
            status = b.read(0, {reinterpret_cast<std::byte*>(&right), sizeof right});
            if (status != ScratchStatus::ok) return status;
            result = left < right ? -1 : right < left ? 1 : 0;
            return ScratchStatus::ok;
        }};
    return {2, page_bytes, 1, 1, order, ScratchOpenMode::create_empty};
}

inline void append_word(std::vector<std::byte>& output, std::uint64_t word) {
    for (unsigned i = 0; i < 8; ++i) {
        output.push_back(static_cast<std::byte>(word & 0xffu));
        word >>= 8;
    }
}

struct Row {
    std::uint64_t key = 0;
    ScratchHandle handle = scratch_no_handle;
    std::vector<std::byte> value;
    bool live = false;
};

using Rows = std::array<Row, 11>;

inline ScratchStatus put(CommandScratchStore& store, const Row& row,
                         ScratchHandle& handle) noexcept {
    ScratchSpanSource key({reinterpret_cast<const std::byte*>(&row.key), sizeof row.key});
    ScratchSpanSource value(bytes(row.value));
    return store.put(key, value, handle);
}

// Serialize every live key, handle and value in cursor order, plus both
// continuation folds. The separate input rows are an oracle for every byte:
// equality between two equally broken cache implementations cannot pass.
inline void snapshot(CommandScratchStore& store, const Rows& rows,
                     std::vector<std::byte>& output) {
    std::uint64_t count = 0;
    for (const auto& row : rows) count += row.live;
    require(store.record_count() == count, "cache workload retains every live record");
    append_word(output, count);
    ScratchHandle cursor = scratch_no_handle;
    auto status = store.first(cursor);
    for (std::uint64_t key = 0; key < rows.size(); ++key) {
        const Row* expected = nullptr;
        for (const auto& row : rows)
            if (row.live && row.key == key) expected = &row;
        if (!expected) continue;
        require(status == ScratchStatus::ok && cursor == expected->handle,
                "cache workload preserves complete ordered handle traversal");
        ScratchRecordInfo info;
        require(store.point(cursor, info) == ScratchStatus::ok
                && info.handle == cursor && info.key_bytes == sizeof key
                && info.value_bytes == expected->value.size(),
                "cache workload preserves complete record descriptors");
        std::uint64_t actual_key = 0;
        std::vector<std::byte> actual(expected->value.size());
        require(store.read(cursor, ScratchField::key, 0,
                    {reinterpret_cast<std::byte*>(&actual_key), sizeof actual_key})
                    == ScratchStatus::ok
                && actual_key == key
                && store.read(cursor, ScratchField::value, 0, buffer(actual)) == ScratchStatus::ok
                && actual == expected->value,
                "cache workload reads every key and value byte from the input oracle");
        append_word(output, info.handle);
        append_word(output, info.key_bytes);
        append_word(output, actual_key);
        append_word(output, info.value_bytes);
        output.insert(output.end(), actual.begin(), actual.end());
        status = store.next(cursor, cursor);
    }
    require(status == ScratchStatus::end && cursor == scratch_no_handle,
            "cache workload cursor has no omitted or extra rows");
    std::array<std::byte, page_bytes> workspace{};
    std::uint64_t ordered = 0, state = 0;
    require(store.ordered_digest({workspace.data(), workspace.size()}, ordered) == ScratchStatus::ok
            && store.state_digest({workspace.data(), workspace.size()}, state) == ScratchStatus::ok,
            "cache workload computes both complete continuation folds");
    append_word(output, ordered);
    append_word(output, state);
}

// A full store run with a value larger than the default decorator, scrambled
// key order, upsert, erase, rollback, handle reuse after rollback and reopen.
// Only this test-owned output grows; production transfer workspaces stay fixed.
inline std::vector<std::byte> run(ScratchBacking& backing) {
    Rows rows;
    constexpr std::array<std::uint64_t, 11> keys{{8, 2, 6, 0, 7, 3, 5, 1, 4, 9, 10}};
    for (std::size_t i = 0; i < rows.size(); ++i) {
        rows[i].key = keys[i];
        rows[i].value = pattern(i == 0 ? 2 * 64 * 1024 + 37 : page_bytes + 19 + i * 17,
                                static_cast<unsigned>(i + 1));
    }
    std::vector<std::byte> output;
    auto geometry = config();
    {
        CommandScratchStore store(geometry, &backing);
        require(store.status() == ScratchStatus::ok, "cache workload opens its real command store");
        for (std::size_t i = 0; i < 9; ++i) {
            require(store.begin() == ScratchStatus::ok
                    && put(store, rows[i], rows[i].handle) == ScratchStatus::ok
                    && rows[i].handle == i + 1
                    && store.commit(ScratchDurability::buffered) == ScratchStatus::ok,
                    "cache workload commits each record with the expected handle");
            rows[i].live = true;
            append_word(output, rows[i].handle);
        }
        snapshot(store, rows, output);
        std::vector<std::byte> before_rollback;
        snapshot(store, rows, before_rollback);
        Row replacement = rows[2];
        replacement.value = pattern(3 * page_bytes + 31, 23);
        ScratchHandle replaced = scratch_no_handle, provisional = scratch_no_handle;
        require(store.begin() == ScratchStatus::ok
                && store.erase(rows[0].handle) == ScratchStatus::ok
                && put(store, replacement, replaced) == ScratchStatus::ok
                && replaced == rows[2].handle
                && put(store, rows[9], provisional) == ScratchStatus::ok
                && provisional == 10 && store.rollback() == ScratchStatus::ok,
                "cache workload rolls back an erase, replacement and allocation");
        std::vector<std::byte> after_rollback;
        snapshot(store, rows, after_rollback);
        require(after_rollback == before_rollback, "rollback restores all bytes and handle state");
        output.insert(output.end(), after_rollback.begin(), after_rollback.end());
        require(store.begin() == ScratchStatus::ok
                && put(store, rows[9], rows[9].handle) == ScratchStatus::ok
                && rows[9].handle == provisional
                && store.erase(rows[3].handle) == ScratchStatus::ok
                && put(store, replacement, replaced) == ScratchStatus::ok
                && replaced == rows[2].handle
                && put(store, rows[10], rows[10].handle) == ScratchStatus::ok
                && rows[10].handle == 11
                && store.commit(ScratchDurability::buffered) == ScratchStatus::ok,
                "cache workload commits edits with unchanged handle allocation");
        rows[9].live = rows[10].live = true;
        rows[3].live = false;
        rows[2].value = std::move(replacement.value);
        ScratchRecordInfo missing;
        require(store.point(rows[3].handle, missing) == ScratchStatus::not_found,
                "cache workload retains authoritative erased-handle absence");
        snapshot(store, rows, output);
    }
    geometry.mode = ScratchOpenMode::reopen;
    CommandScratchStore reopened(geometry, &backing);
    require(reopened.status() == ScratchStatus::ok, "cache workload reopens its committed backing");
    snapshot(reopened, rows, output);
    return output;
}
} // namespace command_cache_test
