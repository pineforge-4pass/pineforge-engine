// Matrix cells over the single run-owned o1::Store. No native tile directory.
//
// Layout: one stable id addresses a 32-byte (rows, columns, record width,
// reserved) descriptor followed by row-major fixed-width records. Both shape
// and cells are read from Store, so an outer calc-on-fill transaction restores
// them together; no native dimension cache or version chain needs undo.
// hash_state() folds the id and every descriptor/cell byte.
//
// Owning Cells release their id. detach() transfers that responsibility to a
// durable generated root/edge; borrowed Cells never release it. Retained script
// graphs must store those ids, rather than a growing collection of C++ handles.
// Each active public matrix handle still has fixed shared_ptr control blocks.
// The run owner keeps Runtime, Store and backing alive through all borrows and
// snapshots, and retains every borrowed id through the last reachable edge.
//
// Copies/snapshots copy cell records, preserving shallow reference elements.
// The graph owner supplies reachability and child rollback for opaque ids;
// copying native owning strings/vectors as raw object bytes is forbidden.
// Unknown element types keep the existing native GenericMatrix semantics.
// Store rollback does not undo native handle-variable rebinding or detach().
// Generated roots/edges must version that ownership state in Store too.
#pragma once

#include <pineforge/detail/o1_store.hpp>
#include <pineforge/run_failure.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace pineforge::detail::matrix_store {

using Id = o1::Id;
inline constexpr std::size_t kTransferBytes = 4096;

[[noreturn]] inline void invariant(const char* why) {
    throw coded<std::logic_error>(RunFailureCode::engine_invariant, {}, why);
}

// Install before constructing script state; the borrowed Store/backing and
// Runtime must outlive all handles, snapshots and temporary borrows.
class Runtime {
public:
    explicit Runtime(o1::Store& store) noexcept : store_(store) {}
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    o1::Store& store() const noexcept { return store_; }
private:
    o1::Store& store_;
};

// A matrix operation that may change more than one Store record runs in one
// Store::scope(). Inside an open transaction (the caller's, an enclosing
// matrix operation's, or another container's) it joins without a handle; with
// none open it owns one handle until it finishes. Live handles are therefore
// the caller's explicit begin() handles, or one while a lone operation runs,
// however deep matrices nest (a Cells call made by a matrix operation, or a
// child matrix operation made by an element codec). An unfinished joined scope
// leaves the enclosing transaction rollback-only; an unfinished owned one
// rolls back. Scopes flatten; joining is not a savepoint.
class Scope {
public:
    explicit Scope(Runtime& runtime) : transaction_(runtime.store().scope()) {}
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void commit() { transaction_.commit(); }
private:
    o1::TxScope transaction_;
};

Runtime* current_runtime() noexcept;

class ScopedRuntime {
public:
    explicit ScopedRuntime(Runtime* runtime) noexcept;
    explicit ScopedRuntime(Runtime& runtime) noexcept : ScopedRuntime(&runtime) {}
    ~ScopedRuntime();
    ScopedRuntime(const ScopedRuntime&) = delete;
    ScopedRuntime& operator=(const ScopedRuntime&) = delete;
private:
    Runtime* previous_;
};

// A fixed descriptor precedes row-major cells in one stable Store object.
// The object layer supplies chunking, paging, zero growth and transaction undo.
// There is no matrix-sized resident buffer and no independent store/cache.
class Cells {
public:
    Cells(Runtime&, std::uint64_t rows, std::uint64_t columns, std::uint64_t width);
    ~Cells() noexcept;
    Cells(const Cells&) = delete;
    Cells& operator=(const Cells&) = delete;

    // A borrow neither allocates a native control block nor owns the id. The
    // generated graph owner must keep its matrix edge alive throughout it.
    Cells(Runtime&, Id);
    static Cells borrow(Runtime& runtime, Id id);
    Runtime& runtime() const noexcept { return runtime_; }
    Id identity() const noexcept { return id_; }
    // Transfer the Store object's lifetime to a generated graph edge/root.
    // That owner must eventually call Store::release; borrows stay non-owning.
    Id detach() noexcept { owning_ = false; return id_; }
    std::uint64_t rows() const;
    std::uint64_t columns() const;
    std::uint64_t count() const;
    std::uint64_t width() const;
    std::uint64_t hash_state() const;
    std::uint64_t index(std::uint64_t row, std::uint64_t column) const;
    // Object bytes (descriptor plus cells) of a shape, or the element-limit
    // refusal when that byte count is not representable. Touches no Store.
    static std::uint64_t layout_bytes(std::uint64_t rows, std::uint64_t columns,
                                      std::uint64_t width);

    void read(std::uint64_t cell, void* record, std::size_t bytes) const;
    void write(std::uint64_t cell, const void* record, std::size_t bytes);
    void copy_cell(std::uint64_t target, const Cells& source, std::uint64_t from);
    void swap_cells(std::uint64_t first, std::uint64_t second);
    void resize(std::uint64_t rows, std::uint64_t columns);
    void insert_row(std::uint64_t index, std::uint64_t columns);
    void insert_column(std::uint64_t index);
    void remove_row(std::uint64_t index);
    void remove_column(std::uint64_t index);
    void swap_rows(std::uint64_t first, std::uint64_t second);
    void swap_columns(std::uint64_t first, std::uint64_t second);
    void reverse(bool both_axes);
    std::shared_ptr<Cells> clone() const;
    void restore(const Cells& frozen);
    void close();

private:
    struct Header {
        std::uint64_t rows, columns, width, reserved;
    };
    static_assert(sizeof(Header) == 32, "matrix descriptor has no padding");
    Header header() const;
    void set_header(const Header&);
    void exact_read(std::uint64_t offset, void*, std::size_t) const;
    void exact_write(std::uint64_t offset, const void*, std::size_t);
    static std::uint64_t product(std::uint64_t, std::uint64_t);
    static std::uint64_t byte_size(const Header&);
    Runtime& runtime_;
    Id id_ = o1::kNullId;
    bool owning_ = false;
};

// Never infer ownership from a native struct's bytes. A generated UDT mapping
// must explicitly encode a stable handle, with graph ownership supplied by the UDT graph module.
// Unknown native values (including owning string/vector fields) remain native.
template<class T, class Enable = void>
struct codec { static constexpr bool mapped = false; };

template<class T>
struct codec<T, std::enable_if_t<std::is_arithmetic_v<T> || std::is_enum_v<T>>> {
    static constexpr bool mapped = true;
    static constexpr std::size_t record_bytes = sizeof(T);
    static void encode(const T& value, unsigned char* record) {
        std::memcpy(record, &value, sizeof value);
    }
    static T decode(const unsigned char* record) {
        T value{};
        std::memcpy(&value, record, sizeof value);
        return value;
    }
};

namespace codec_detail {
// A view codec may need the run's Store to reconstruct a borrowed handle.
// Prefer the Runtime-aware overloads; retain the plain fixed-record codec
// interface for arithmetic and existing specializations. No TLS lookup is
// needed to read a matrix constructed under an earlier ScopedRuntime.
template<class T>
inline auto decode(Runtime& runtime, const unsigned char* record, int)
    -> decltype(codec<T>::decode(runtime, record)) {
    return codec<T>::decode(runtime, record);
}
template<class T>
inline auto decode(Runtime&, const unsigned char* record, long)
    -> decltype(codec<T>::decode(record)) {
    return codec<T>::decode(record);
}
template<class T>
inline auto encode(Runtime& runtime, const T& value, unsigned char* record, int)
    -> decltype(codec<T>::encode(runtime, value, record), void()) {
    codec<T>::encode(runtime, value, record);
}
template<class T>
inline auto encode(Runtime&, const T& value, unsigned char* record, long)
    -> decltype(codec<T>::encode(value, record), void()) {
    codec<T>::encode(value, record);
}
} // namespace codec_detail

template<class T>
inline T read_value(const Cells& cells, std::uint64_t index) {
    if constexpr (codec<T>::mapped) {
        std::array<unsigned char, codec<T>::record_bytes> record{};
        cells.read(index, record.data(), record.size());
        return codec_detail::decode<T>(cells.runtime(), record.data(), 0);
    } else {
        invariant("unmapped matrix element read");
    }
}

template<class T>
inline void write_value(Cells& cells, std::uint64_t index, const T& value) {
    if constexpr (codec<T>::mapped) {
        std::array<unsigned char, codec<T>::record_bytes> record{};
        codec_detail::encode<T>(cells.runtime(), value, record.data(), 0);
        cells.write(index, record.data(), record.size());
    } else {
        invariant("unmapped matrix element write");
    }
}

template<class T>
inline std::shared_ptr<Cells> make_cells(Runtime& runtime, std::uint64_t rows,
                                         std::uint64_t columns, const T& initial) {
    if constexpr (codec<T>::mapped) {
        Scope transaction(runtime);
        auto cells = std::make_shared<Cells>(runtime, rows, columns, codec<T>::record_bytes);
        for (std::uint64_t i = 0, n = cells->count(); i < n; ++i)
            write_value(*cells, i, initial);
        transaction.commit();
        return cells;
    } else {
        invariant("unmapped matrix element construction");
    }
}

} // namespace pineforge::detail::matrix_store
