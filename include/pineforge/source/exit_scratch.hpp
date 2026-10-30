#pragma once

#include <pineforge/detail/scratch_store.hpp>
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace pineforge::source {
class PineExecutionAdapter;
struct PlacementSnapshot;

namespace detail {
using pineforge::detail::ScratchBuffer;
using pineforge::detail::ScratchByteSource;
using pineforge::detail::ScratchDurability;
using pineforge::detail::ScratchField;
using pineforge::detail::ScratchHandle;
using pineforge::detail::ScratchKeyOrder;
using pineforge::detail::ScratchReadback;
using pineforge::detail::ScratchRecordInfo;
using pineforge::detail::ScratchSpanSource;
using pineforge::detail::ScratchStatus;
using pineforge::detail::CommandScratchStore;

// At most this many scope rows per teardown transaction. The final
// partial batch commits once; scope population never sizes resident storage.
inline constexpr std::size_t kExitScopeEraseBatch = 64;

// Terminal diagnostic only, like CommandScratchStore::carried_failure().
// Retain the first teardown failure across rollback and further unwinding;
// it is neither logical scratch state nor a population-sized allocation.
class ExitScratchTeardownFailure {
public:
    bool pending() const noexcept { return status_ != ScratchStatus::ok; }
    void remember(ScratchStatus status, const ScratchReadback& store) noexcept {
        if (pending() || status == ScratchStatus::ok) return;
        status_ = status;
        if (status == ScratchStatus::io_error)
            if (const auto* failure = store.carried_failure()) failure_ = *failure;
    }
    void check() const;
private:
    ScratchStatus status_ = ScratchStatus::ok;
    std::optional<pineforge::detail::ReportScratchIoError> failure_;
};
static_assert(std::is_nothrow_copy_constructible_v<pineforge::detail::ReportScratchIoError>
              && std::is_nothrow_copy_assignable_v<pineforge::detail::ReportScratchIoError>,
              "teardown must retain the typed backing failure without throwing");

// The source run owner provisions the CX-O1C store. The version owner must retain
// every payload field externally until release_scope, including across nested
// Applied callbacks; it may not keep an owning row/token table in RAM. Reads
// reproduce the frozen value, not the current source row. These integration
// declarations do not implement a second store.
class ExitScratchVersions {
public:
    virtual ~ExitScratchVersions() = default;
    virtual std::uint64_t placement(std::uint64_t scope, const PlacementSnapshot&) = 0;
    virtual PlacementSnapshot placement(std::uint64_t version) const = 0;
    virtual std::uint64_t request(std::uint64_t scope, const native_order::Request&) = 0;
    virtual native_order::Request request(std::uint64_t version) const = 0;
    virtual std::uint64_t pending(std::uint64_t scope, const native_order::Request&,
                                  const PlacementSnapshot&, const std::string& key) = 0;
    virtual void pending(std::uint64_t version, native_order::Request&,
                         PlacementSnapshot&, std::string& key) const = 0;
    // OPEN: native owners must retain this fixed membership beyond scratch
    // scope destruction. A live BindCohort cannot implement this operation.
    using OpeningReader = ScratchStatus (*)(void*, std::uint64_t,
                                            native_order::RequestHandle&);
    virtual native_order::Owner freeze_openings(const native_order::RunIdentity&,
        std::int64_t cycle, std::uint64_t count, void*, OpeningReader) = 0;
    virtual void release_scope(std::uint64_t scope) noexcept = 0;
    void check_teardown_failure() const { teardown_failure_.check(); }
    bool teardown_failed() const noexcept { return teardown_failure_.pending(); }
protected:
    ExitScratchTeardownFailure teardown_failure_;
};

struct ExitScratchContext {
    CommandScratchStore& store;
    native_order::RunIdentity run;
    ExitScratchVersions* versions = nullptr;
    std::uint64_t next_scope = 0;
    void* observation = nullptr;
    void (*observe_rows)(void*, std::uint64_t scope, std::size_t rows) noexcept = nullptr;
    void (*observe_lookup)(void*, std::size_t candidates) noexcept = nullptr;
    ScratchStatus (*lots)(const NativeStrategyHost&, double, void*,
                         bool (*)(void*, const NativeOpenLot&)) = nullptr;
    ScratchStatus (*working)(const NativeStrategyHost&, void*,
                            bool (*)(void*, const NativeWorkingRequest&)) = nullptr;
    ExitScratchTeardownFailure teardown_failure;
};

class ExitScratchError final : public coded<std::logic_error> {
public:
    explicit ExitScratchError(ScratchStatus status);
    ScratchStatus status() const noexcept { return status_; }
private:
    ScratchStatus status_;
};

// Defined in pine_adapter.cpp, including the unbound/error paths. project()
// installs the run owner; bind_exit_scratch supplies an explicit borrowed override.
ExitScratchContext& exit_scratch(const PineExecutionAdapter&);
[[noreturn]] void exit_scratch_failure(ScratchStatus);

inline void exit_scratch_check(ScratchStatus status) {
    if (status != ScratchStatus::ok) exit_scratch_failure(status);
}
[[noreturn]] inline void exit_scratch_failure(ScratchStatus status, const ScratchReadback& store) {
    if (status == ScratchStatus::io_error)
        pineforge::detail::rethrow_carried_scratch_failure(store.carried_failure());
    exit_scratch_failure(status);
}
inline void exit_scratch_check(ScratchStatus status, const ScratchReadback& store) {
    if (status != ScratchStatus::ok) exit_scratch_failure(status, store);
}
inline void ExitScratchTeardownFailure::check() const {
    if (!pending()) return;
    pineforge::detail::rethrow_carried_scratch_failure(failure_ ? &*failure_ : nullptr);
    exit_scratch_failure(status_);
}
inline void exit_scratch_check_teardown(const ExitScratchContext& context) {
    if (context.versions) context.versions->check_teardown_failure();
    context.teardown_failure.check();
}
inline ExitScratchVersions& exit_scratch_versions(ExitScratchContext& context) {
    exit_scratch_check_teardown(context);
    if (!context.versions) exit_scratch_failure(ScratchStatus::programming_error);
    return *context.versions;
}

template<class Visit> void exit_scratch_lots(ExitScratchContext& scratch,
        const NativeStrategyHost& host, double mark, Visit visit) {
    exit_scratch_check_teardown(scratch);
    if (!scratch.lots) exit_scratch_failure(ScratchStatus::programming_error);
    const auto status = scratch.lots(host, mark, &visit,
        [](void* context, const NativeOpenLot& lot) {
            return (*static_cast<Visit*>(context))(lot);
        });
    if (status != ScratchStatus::cancelled) exit_scratch_check(status, scratch.store);
}
template<class Visit> void exit_scratch_working(ExitScratchContext& scratch,
        const NativeStrategyHost& host, Visit visit) {
    exit_scratch_check_teardown(scratch);
    if (!scratch.working) exit_scratch_failure(ScratchStatus::programming_error);
    const auto status = scratch.working(host, &visit,
        [](void* context, const NativeWorkingRequest& request) {
            return (*static_cast<Visit*>(context))(request);
        });
    if (status != ScratchStatus::cancelled) exit_scratch_check(status, scratch.store);
}

// A handle plus a borrowed readback, never a cache pointer or owning string.
// It remains readable across eviction and nested scopes.
class ExitScratchText final : public ScratchByteSource {
public:
    ExitScratchText(const ScratchReadback& reader, ScratchHandle handle,
                    ScratchField field = ScratchField::value, std::uint64_t offset = 0)
        : reader_(&reader), handle_(handle), field_(field), offset_(offset) {
        ScratchRecordInfo info{};
        exit_scratch_check(reader.point(handle, info), reader);
        const auto bytes = field == ScratchField::key ? info.key_bytes : info.value_bytes;
        if (offset > bytes) exit_scratch_failure(ScratchStatus::corrupt);
        size_ = bytes - offset;
    }
    std::uint64_t size() const noexcept override { return size_; }
    const pineforge::detail::ReportScratchIoError* carried_failure() const noexcept override {
        return reader_->carried_failure();
    }
    bool empty() const noexcept { return size_ == 0; }
    ScratchStatus read(std::uint64_t offset, ScratchBuffer destination) const noexcept override {
        if (offset > size_ || destination.size > size_ - offset)
            return ScratchStatus::programming_error;
        return reader_->read(handle_, field_, offset_ + offset, destination);
    }
private:
    const ScratchReadback* reader_;
    ScratchHandle handle_;
    ScratchField field_;
    std::uint64_t offset_;
    std::uint64_t size_ = 0;
};

inline ScratchSpanSource exit_scratch_text(const std::string& text) noexcept {
    return ScratchSpanSource({reinterpret_cast<const std::byte*>(text.data()), text.size()});
}

// The exact source_id + "\x1f" + from_entry bytes without a joined string.
class ExitScratchPairText final : public ScratchByteSource {
public:
    ExitScratchPairText(const std::string& left, const std::string& right) noexcept
        : parts_{{{reinterpret_cast<const std::byte*>(left.data()), left.size()},
                  {reinterpret_cast<const std::byte*>("\x1f"), 1},
                  {reinterpret_cast<const std::byte*>(right.data()), right.size()}}} {}
    std::uint64_t size() const noexcept override {
        return parts_[0].size + 1 + parts_[2].size;
    }
    ScratchStatus read(std::uint64_t offset, ScratchBuffer destination) const noexcept override {
        if (offset > size() || destination.size > size() - offset
            || (destination.size && !destination.data))
            return ScratchStatus::programming_error;
        for (const auto& part : parts_) {
            if (offset >= part.size) { offset -= part.size; continue; }
            const auto count = std::min<std::size_t>(destination.size, part.size - offset);
            if (count) std::memcpy(destination.data, part.data + offset, count);
            if (count) destination.data += count;
            destination.size -= count;
            offset = 0;
            if (!destination.size) break;
        }
        return ScratchStatus::ok;
    }
private:
    std::array<pineforge::detail::ScratchBytes, 3> parts_;
};

inline ScratchStatus exit_scratch_compare_text(const ScratchByteSource& left,
        std::uint64_t left_offset, const ScratchByteSource& right,
        std::uint64_t right_offset, int& result) noexcept {
    if (left_offset > left.size() || right_offset > right.size())
        return ScratchStatus::programming_error;
    const auto left_size = left.size() - left_offset;
    const auto right_size = right.size() - right_offset;
    const auto common = std::min(left_size, right_size);
    std::array<std::byte, 128> a{}, b{};
    for (std::uint64_t offset = 0; offset < common;) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
            a.size(), common - offset));
        auto status = left.read(left_offset + offset, {a.data(), count});
        if (status != ScratchStatus::ok) return status;
        status = right.read(right_offset + offset, {b.data(), count});
        if (status != ScratchStatus::ok) return status;
        const int order = std::memcmp(a.data(), b.data(), count);
        if (order != 0) { result = order; return ScratchStatus::ok; }
        offset += count;
    }
    result = left_size < right_size ? -1 : (right_size < left_size ? 1 : 0);
    return ScratchStatus::ok;
}
inline bool exit_scratch_equal(const ScratchByteSource& left,
                               const ScratchByteSource& right) {
    if (left.size() != right.size()) return false;
    int order = 0;
    const auto status = exit_scratch_compare_text(left, 0, right, 0, order);
    if (status == ScratchStatus::io_error) {
        pineforge::detail::rethrow_carried_scratch_failure(left.carried_failure());
        pineforge::detail::rethrow_carried_scratch_failure(right.carried_failure());
    }
    exit_scratch_check(status);
    return order == 0;
}
inline bool operator==(const ExitScratchText& left, const std::string& right) {
    return exit_scratch_equal(left, exit_scratch_text(right));
}
inline bool operator==(const std::string& left, const ExitScratchText& right) {
    return right == left;
}
inline bool operator!=(const ExitScratchText& left, const std::string& right) {
    return !(left == right);
}
inline bool operator!=(const std::string& left, const ExitScratchText& right) {
    return !(right == left);
}

// All rows and secondary indexes use this ONE store. Order is scope, kind,
// major, text bytes, ordinal. Ordinal breaks exact ties in original queue
// order. Sources borrow input bytes; no put source reads this store.
using ExitScratchKeyWords = std::array<std::uint64_t, 4>;
inline constexpr std::size_t exit_scratch_key_bytes = sizeof(ExitScratchKeyWords);
class ExitScratchKey final : public ScratchByteSource {
public:
    ExitScratchKey(std::uint64_t scope, std::uint64_t kind = 0,
                   std::uint64_t major = 0, std::uint64_t ordinal = 0,
                   const ScratchByteSource* text = nullptr) noexcept
        : words_{scope, kind, major, ordinal}, text_(text) {}
    std::uint64_t size() const noexcept override {
        return exit_scratch_key_bytes + (text_ ? text_->size() : 0);
    }
    ScratchStatus read(std::uint64_t offset, ScratchBuffer destination) const noexcept override {
        if (offset > size() || destination.size > size() - offset
            || (destination.size && !destination.data))
            return ScratchStatus::programming_error;
        if (destination.size && offset < exit_scratch_key_bytes) {
            const auto count = std::min<std::size_t>(destination.size,
                exit_scratch_key_bytes - static_cast<std::size_t>(offset));
            std::memcpy(destination.data,
                reinterpret_cast<const std::byte*>(words_.data()) + offset, count);
            destination.data += count;
            destination.size -= count;
            offset += count;
        }
        return destination.size
            ? text_->read(offset - exit_scratch_key_bytes, destination) : ScratchStatus::ok;
    }
private:
    ExitScratchKeyWords words_;
    const ScratchByteSource* text_;
};

struct ExitScratchOrderStats { std::uint64_t comparisons = 0; };
inline ScratchKeyOrder exit_scratch_key_order(ExitScratchOrderStats* stats = nullptr) noexcept {
    return {0x5832355832370002ULL, stats,
        [](void* context, const ScratchByteSource& left, const ScratchByteSource& right,
           int& result) noexcept {
            if (context) ++static_cast<ExitScratchOrderStats*>(context)->comparisons;
            ExitScratchKeyWords a{}, b{};
            if (left.size() < sizeof(a) || right.size() < sizeof(b))
                return ScratchStatus::programming_error;
            auto status = left.read(0, {reinterpret_cast<std::byte*>(a.data()), sizeof(a)});
            if (status != ScratchStatus::ok) return status;
            status = right.read(0, {reinterpret_cast<std::byte*>(b.data()), sizeof(b)});
            if (status != ScratchStatus::ok) return status;
            for (std::size_t i = 0; i < 3; ++i) {
                if (a[i] == b[i]) continue;
                result = a[i] < b[i] ? -1 : 1;
                return ScratchStatus::ok;
            }
            status = exit_scratch_compare_text(left, sizeof(a), right, sizeof(b), result);
            if (status != ScratchStatus::ok || result != 0) return status;
            result = a[3] < b[3] ? -1 : (b[3] < a[3] ? 1 : 0);
            return ScratchStatus::ok;
        }};
}

inline ExitScratchKeyWords exit_scratch_words(ExitScratchContext& context,
                                             ScratchHandle handle) {
    exit_scratch_check_teardown(context);
    ExitScratchKeyWords words{};
    exit_scratch_check(context.store.read(handle, ScratchField::key, 0,
        {reinterpret_cast<std::byte*>(words.data()), sizeof(words)}), context.store);
    return words;
}
inline ScratchHandle exit_scratch_seek(ExitScratchContext& context,
                                      const ScratchByteSource& key) {
    exit_scratch_check_teardown(context);
    ScratchHandle handle = 0;
    const auto status = context.store.lower_bound(key, handle);
    if (status == ScratchStatus::end) return 0;
    exit_scratch_check(status, context.store);
    return handle;
}
inline void exit_scratch_edit_failure(ExitScratchContext& context,
                                      ScratchStatus status) {
    // Rollback makes further backing calls. Preserve the first failure before
    // cleanup can replace the backing's borrowed diagnostic.
    std::optional<pineforge::detail::ReportScratchIoError> failure;
    if (status == ScratchStatus::io_error)
        if (const auto* carried = context.store.carried_failure()) failure = *carried;
    const auto rollback_status = context.store.rollback();
    pineforge::detail::rethrow_carried_scratch_failure(failure ? &*failure : nullptr);
    exit_scratch_check(rollback_status, context.store);
    exit_scratch_failure(status);
}
template<class Edit> void exit_scratch_edit(ExitScratchContext& context, Edit operation) {
    exit_scratch_check_teardown(context);
    exit_scratch_check(context.store.begin(), context.store);
    auto status = operation();
    if (status == ScratchStatus::ok)
        status = context.store.commit(ScratchDurability::buffered);
    if (status != ScratchStatus::ok) exit_scratch_edit_failure(context, status);
}

// Teardown uses only status-returning store operations. Snapshot the original
// typed failure before rollback can replace the backing's borrowed diagnostic.
// Reads include pending erases, so each lookup finds the next live scope row
// without a resident handle list or a cursor invalidated by erase.
inline void exit_scratch_erase_scope_noexcept(CommandScratchStore& store,
        std::uint64_t scope, std::uint64_t first_kind, std::uint64_t last_kind,
        ExitScratchTeardownFailure& failure) noexcept {
    if (failure.pending()) return;
    const ExitScratchKey key(scope, first_kind);
    const auto next = [&](ScratchHandle& handle) noexcept {
        auto status = store.lower_bound(key, handle);
        if (status != ScratchStatus::ok) return status;
        ExitScratchKeyWords words{};
        status = store.read(handle, ScratchField::key, 0,
            {reinterpret_cast<std::byte*>(words.data()), sizeof(words)});
        if (status != ScratchStatus::ok) return status;
        return words[0] != scope || words[1] > last_kind
            ? ScratchStatus::end : ScratchStatus::ok;
    };
    for (;;) {
        ScratchHandle handle = 0;
        auto status = next(handle);
        if (status == ScratchStatus::end) return;
        if (status != ScratchStatus::ok) {
            failure.remember(status, store);
            return;
        }
        status = store.begin();
        if (status != ScratchStatus::ok) {
            failure.remember(status, store);
            return;
        }
        for (std::size_t erased = 0; erased < kExitScopeEraseBatch; ++erased) {
            status = store.erase(handle);
            if (status != ScratchStatus::ok || erased + 1 == kExitScopeEraseBatch) break;
            status = next(handle);
            if (status == ScratchStatus::end) {
                status = ScratchStatus::ok;
                break;
            }
            if (status != ScratchStatus::ok) break;
        }
        if (status == ScratchStatus::ok)
            status = store.commit(ScratchDurability::buffered);
        if (status != ScratchStatus::ok) {
            failure.remember(status, store);
            const auto rollback_status = store.rollback();
            failure.remember(rollback_status, store);
            return;
        }
    }
}

class ExitScratchScope {
public:
    explicit ExitScratchScope(ExitScratchContext& context) : context_(&context) {
        exit_scratch_check_teardown(context);
        if (context.next_scope == std::numeric_limits<std::uint64_t>::max())
            exit_scratch_failure(ScratchStatus::programming_error);
        scope_ = ++context.next_scope;
    }
    ExitScratchScope(const ExitScratchScope&) = delete;
    ExitScratchScope& operator=(const ExitScratchScope&) = delete;
    ExitScratchScope(ExitScratchScope&& other) noexcept
        : context_(std::exchange(other.context_, nullptr)), scope_(other.scope_) {}
    ~ExitScratchScope() noexcept(false) {
        if (!context_) return;
        if (!context_->teardown_failure.pending()
                && !(context_->versions && context_->versions->teardown_failed())) {
            if (context_->versions) context_->versions->release_scope(scope_);
            if (!(context_->versions && context_->versions->teardown_failed()))
                exit_scratch_erase_scope_noexcept(context_->store, scope_, 0,
                    std::numeric_limits<std::uint64_t>::max(), context_->teardown_failure);
        }
        // Normal scope exit is a throwing boundary, even for the final scope
        // in a callback. During unwinding preserve the active exception and
        // retain the teardown fault for the next scratch/provider boundary.
        if (std::uncaught_exceptions() == 0) exit_scratch_check_teardown(*context_);
    }
    std::uint64_t scope() const noexcept { return scope_; }
    ExitScratchContext& context() const noexcept { return *context_; }
    void clear() {
        exit_scratch_check_teardown(*context_);
        exit_scratch_erase_scope_noexcept(context_->store, scope_, 0,
            std::numeric_limits<std::uint64_t>::max(), context_->teardown_failure);
        exit_scratch_check_teardown(*context_);
    }
private:
    ExitScratchContext* context_;
    std::uint64_t scope_ = 0;
};

// An ordered index is records in the same store, not a resident side table.
// Each lookup examines at most one candidate after lower_bound.
class ExitScratchIndex {
public:
    explicit ExitScratchIndex(ExitScratchContext& context) : scope_(context) {}
    explicit ExitScratchIndex(const PineExecutionAdapter& owner) : scope_(exit_scratch(owner)) {}
    void put(std::uint64_t major, std::uint64_t value, std::uint64_t ordinal = 0) {
        const ExitScratchKey key(scope_.scope(), 0, major, ordinal);
        put_key(key, value);
    }
    void put(const std::string& text, std::uint64_t value) {
        const auto bytes = exit_scratch_text(text);
        put(bytes, value);
    }
    void put(const ScratchByteSource& bytes, std::uint64_t value) {
        const ExitScratchKey key(scope_.scope(), 0, 0, 0, &bytes);
        put_key(key, value);
    }
    static constexpr std::uint64_t absent = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t find(std::uint64_t major) const {
        const ExitScratchKey key(scope_.scope(), 0, major);
        return find_key(key);
    }
    std::uint64_t find(const std::string& text) const {
        const auto bytes = exit_scratch_text(text);
        return find(bytes);
    }
    std::uint64_t find(const ScratchByteSource& bytes) const {
        const ExitScratchKey key(scope_.scope(), 0, 0, 0, &bytes);
        return find_key(key);
    }
    template<class Visit> void each(Visit visit) const { walk(0, false, visit); }
    template<class Visit> void each(std::uint64_t major, Visit visit) const {
        walk(major, true, visit);
    }
private:
    void put_key(const ScratchByteSource& key, std::uint64_t value) {
        const ScratchSpanSource payload({reinterpret_cast<const std::byte*>(&value), sizeof(value)});
        auto& context = scope_.context();
        exit_scratch_edit(context, [&] {
            ScratchHandle handle = 0;
            return context.store.put(key, payload, handle);
        });
    }
    std::uint64_t find_key(const ScratchByteSource& key) const {
        auto& context = scope_.context();
        const auto handle = exit_scratch_seek(context, key);
        if (context.observe_lookup)
            context.observe_lookup(context.observation, handle ? 1 : 0);
        if (!handle) return absent;
        const ExitScratchText found(context.store, handle, ScratchField::key);
        if (!exit_scratch_equal(key, found)) return absent;
        std::uint64_t value = 0;
        exit_scratch_check(context.store.read(handle, ScratchField::value, 0,
            {reinterpret_cast<std::byte*>(&value), sizeof(value)}), context.store);
        return value;
    }
    template<class Visit> void walk(std::uint64_t major, bool restricted, Visit visit) const {
        auto& context = scope_.context();
        const ExitScratchKey key(scope_.scope(), 0, major);
        auto handle = exit_scratch_seek(context, key);
        while (handle) {
            const auto words = exit_scratch_words(context, handle);
            if (words[0] != scope_.scope() || (restricted && words[2] != major)) break;
            std::uint64_t value = 0;
            exit_scratch_check(context.store.read(handle, ScratchField::value, 0,
                {reinterpret_cast<std::byte*>(&value), sizeof(value)}), context.store);
            if (!visit(value)) break;
            const auto status = context.store.next(handle, handle);
            if (status == ScratchStatus::end) break;
            exit_scratch_check(status, context.store);
        }
    }
    ExitScratchScope scope_;
};

template<class T> struct ExitScratchValue {
    static_assert(std::is_trivially_copyable_v<T>,
                  "external payloads require a version, never an object/pointer spill");
    static ScratchSpanSource source(const T& value) noexcept {
        return ScratchSpanSource({reinterpret_cast<const std::byte*>(&value), sizeof(value)});
    }
    static T read(const ExitScratchContext& context, ScratchHandle handle) {
        T value{};
        exit_scratch_check(context.store.read(handle, ScratchField::value, 0,
            {reinterpret_cast<std::byte*>(&value), sizeof(value)}), context.store);
        return value;
    }
};
template<> struct ExitScratchValue<std::string> {
    static ScratchSpanSource source(const std::string& value) noexcept {
        return exit_scratch_text(value);
    }
    static ExitScratchText read(const ExitScratchContext& context, ScratchHandle handle) {
        return {context.store, handle};
    }
};

// Every selected live handle belongs to this run. Keep its lifetime-managed
// identity once, in the context, rather than spilling a SessionKey pointer.
template<> struct ExitScratchValue<native_order::RequestHandle> {
    static ScratchSpanSource source(const native_order::RequestHandle& value) noexcept {
        return ScratchSpanSource({reinterpret_cast<const std::byte*>(&value.incarnation),
                                  sizeof(value.incarnation)});
    }
    static native_order::RequestHandle read(const ExitScratchContext& context,
                                            ScratchHandle handle) {
        std::uint64_t incarnation = 0;
        exit_scratch_check(context.store.read(handle, ScratchField::value, 0,
            {reinterpret_cast<std::byte*>(&incarnation), sizeof(incarnation)}), context.store);
        return {context.run, incarnation};
    }
};

// No store visitor is active while a caller cancels, replaces, executes or
// creates another scope. Each edit commits separately, so nested Applied
// callbacks require neither nested transactions nor owning snapshots.
enum class ExitScratchRowMode : std::uint8_t {
    per_row,
    bounded_batch,
};

template<class T, ExitScratchRowMode Mode = ExitScratchRowMode::per_row>
class ExitScratchRows {
public:
    explicit ExitScratchRows(const PineExecutionAdapter& owner)
        : ExitScratchRows(exit_scratch(owner)) {}
    explicit ExitScratchRows(ExitScratchContext& context) : scope_(context) {}
    ExitScratchRows(const ExitScratchRows&) = delete;
    ExitScratchRows& operator=(const ExitScratchRows&) = delete;
    ExitScratchRows(ExitScratchRows&& other) noexcept
        : scope_(std::move(other.scope_)), count_(other.count_) {}
    std::uint64_t scope() const noexcept { return scope_.scope(); }
    ExitScratchContext& context() const noexcept { return scope_.context(); }
    std::size_t size() const noexcept { return static_cast<std::size_t>(count_); }
    bool empty() const noexcept { return count_ == 0; }

    void push_back(const T& value) {
        if constexpr (std::is_same_v<T, native_order::RequestHandle>) {
            if (value.run != context().run)
                exit_scratch_failure(ScratchStatus::programming_error);
        }
        if constexpr (Mode == ExitScratchRowMode::bounded_batch) {
            if (batch_enabled()) {
                if (count_ % kExitScopeEraseBatch == 0
                        || !context().store.transaction_active())
                    begin_batch();
                put_batch(count_, value);
                ++count_;
                if (count_ % kExitScopeEraseBatch == 0) finish();
            } else {
                finish();
                put(count_, value);
                ++count_;
            }
        } else {
            put(count_, value);
            ++count_;
        }
        observe();
    }
    void finish() {
        if constexpr (Mode == ExitScratchRowMode::bounded_batch) {
            if (count_ == 0
                    || !context().store.transaction_active())
                return;
            exit_scratch_check_teardown(context());
            const auto status = context().store.commit(ScratchDurability::buffered);
            if (status != ScratchStatus::ok)
                exit_scratch_edit_failure(context(), status);
        }
    }
    void set(std::size_t index, const T& value) {
        static_assert(!std::is_same_v<T, std::string>, "text selections are immutable");
        finish();
        put(index, value);
    }
    auto operator[](std::size_t index) const {
        return ExitScratchValue<T>::read(context(), find(index));
    }
    auto front() const { return (*this)[0]; }
    void clear() {
        finish();
        scope_.clear();
        count_ = 0;
        observe();
    }
    template<class Key, class Visit> void ordered(Key key, Visit visit) const {
        ExitScratchIndex index(context());
        for (std::size_t i = 0; i < size(); ++i) index.put(key((*this)[i]), i, i);
        index.each([&](std::uint64_t ordinal) {
            return visit((*this)[ordinal], static_cast<std::size_t>(ordinal));
        });
    }
    bool contains(const std::string& text) const {
        static_assert(std::is_same_v<T, std::string>, "text index only");
        const auto bytes = exit_scratch_text(text);
        const ExitScratchKey key(scope(), 1, 0, 0, &bytes);
        const auto handle = exit_scratch_seek(context(), key);
        if (context().observe_lookup)
            context().observe_lookup(context().observation, handle ? 1 : 0);
        if (!handle) return false;
        const auto words = exit_scratch_words(context(), handle);
        return words[0] == scope() && words[1] == 1
            && exit_scratch_equal(bytes, ExitScratchText(context().store, handle,
                ScratchField::key, exit_scratch_key_bytes));
    }
    template<class Visit> void ordered_text(Visit visit, bool unique = false) const {
        static_assert(std::is_same_v<T, std::string>, "text index only");
        const ExitScratchKey key(scope(), 1);
        auto handle = exit_scratch_seek(context(), key);
        ScratchHandle previous = 0;
        while (handle) {
            const auto words = exit_scratch_words(context(), handle);
            if (words[0] != scope() || words[1] != 1) break;
            const auto row = find(words[3]);
            const ExitScratchText text(context().store, row);
            if (!unique || !previous || !exit_scratch_equal(
                    text, ExitScratchText(context().store, previous))) {
                if (!visit(text)) break;
            }
            previous = row;
            const auto status = context().store.next(handle, handle);
            if (status == ScratchStatus::end) break;
            exit_scratch_check(status, context().store);
        }
    }

    struct const_iterator {
        using iterator_category = std::input_iterator_tag;
        using value_type = decltype(ExitScratchValue<T>::read(
            std::declval<const ExitScratchContext&>(), ScratchHandle{}));
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = value_type;
        const ExitScratchRows* rows = nullptr;
        std::size_t index = 0;
        value_type operator*() const { return (*rows)[index]; }
        const_iterator& operator++() { ++index; return *this; }
        const_iterator operator++(int) { auto prior = *this; ++*this; return prior; }
        bool operator==(const const_iterator& other) const {
            return rows == other.rows && index == other.index;
        }
        bool operator!=(const const_iterator& other) const { return !(*this == other); }
    };
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, size()}; }

private:
    bool batch_enabled() const noexcept {
        if constexpr (Mode != ExitScratchRowMode::bounded_batch) {
            return false;
        } else {
            return !context().store.has_io_observer()
                && context().observe_rows == nullptr
                && context().observe_lookup == nullptr;
        }
    }
    void begin_batch() {
        exit_scratch_check_teardown(context());
        exit_scratch_check(context().store.begin(), context().store);
    }
    ScratchStatus put_value(std::uint64_t index, const T& value) {
        const ExitScratchKey key(scope(), 0, 0, index);
        const auto payload = ExitScratchValue<T>::source(value);
        ScratchHandle handle = 0;
        auto status = context().store.put(key, payload, handle);
        if constexpr (std::is_same_v<T, std::string>) {
            if (status != ScratchStatus::ok) return status;
            const ExitScratchKey ordered(scope(), 1, 0, index, &payload);
            const ScratchSpanSource empty({nullptr, 0});
            status = context().store.put(ordered, empty, handle);
        }
        return status;
    }
    void put_batch(std::uint64_t index, const T& value) {
        const auto status = put_value(index, value);
        if (status != ScratchStatus::ok)
            exit_scratch_edit_failure(context(), status);
    }
    void observe() const noexcept {
        if (context().observe_rows)
            context().observe_rows(context().observation, scope(), size());
    }
    ScratchHandle find(std::uint64_t index) const {
        const ExitScratchKey key(scope(), 0, 0, index);
        const auto handle = exit_scratch_seek(context(), key);
        if (!handle || exit_scratch_words(context(), handle)
                != ExitScratchKeyWords{scope(), 0, 0, index})
            exit_scratch_failure(ScratchStatus::programming_error);
        return handle;
    }
    void put(std::uint64_t index, const T& value) {
        exit_scratch_edit(context(), [&] {
            return put_value(index, value);
        });
    }
    ExitScratchScope scope_;
    std::uint64_t count_ = 0;
};

template<class T>
using ExitScratchBatchRows = ExitScratchRows<T, ExitScratchRowMode::bounded_batch>;

} // namespace detail
} // namespace pineforge::source
