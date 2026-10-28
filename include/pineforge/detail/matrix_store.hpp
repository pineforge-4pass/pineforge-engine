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
// A run's own Runtime (RunRuntime, made current by the engine for every public
// call that runs script code) owns its Store and backing, and every Cells made
// from it shares that ownership. A borrowed Runtime's owner keeps Runtime,
// Store and backing alive through all borrows and snapshots. Either way the
// generated graph retains every borrowed id through the last reachable edge.
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
// A get reads its matrix's descriptor and leading records together, in one
// Store read of at most this many bytes: one data chunk of the default
// o1::Shape, the chunk a descriptor read alone touches.
inline constexpr std::size_t kPrefixBytes = o1::Shape{}.chunk_bytes;

[[noreturn]] inline void invariant(const char* why) {
    throw coded<std::logic_error>(RunFailureCode::engine_invariant, {}, why);
}

// The Store matrices live in. Over a borrowed Store: install it before
// constructing script state; the Store/backing and this Runtime must outlive
// all handles, snapshots and temporary borrows. Over a backing of its own (a
// run's, see RunRuntime): it opens its Store on that backing and, held by a
// shared_ptr, is shared by every Cells made from it, so the Store closes and
// the backing's bytes are freed with the last of them and its owner's share.
class Runtime : public std::enable_shared_from_this<Runtime> {
public:
    explicit Runtime(o1::Store& store) noexcept : store_(&store) {}
    explicit Runtime(std::unique_ptr<ScratchBacking> backing, const o1::Shape& shape = {});
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    o1::Store& store() const noexcept { return *store_; }
private:
    std::unique_ptr<ScratchBacking> backing_; // declared first: destroyed last
    std::unique_ptr<o1::Store> owned_;
    o1::Store* store_;
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

// The Runtime a matrix created now on this thread lives in, or null for a
// native (heap) matrix: the choice of the innermost ScopedRuntime in force (an
// explicit null keeps matrices native), else the Runtime of the run whose
// public call is in progress (RunScope), opened on the run's first matrix,
// else null: a library caller outside any run keeps native matrices. Opening
// a run's Runtime can fail; the failure propagates, so a run never falls back
// to native matrices.
Runtime* current_runtime();

// An explicit choice for the calling thread while the scope lives, over any
// run's own Runtime: a caller that supplies its own Store (or null, for native
// matrices) keeps that choice across an engine run it makes inside the scope.
class ScopedRuntime {
public:
    explicit ScopedRuntime(Runtime* runtime) noexcept;
    explicit ScopedRuntime(Runtime& runtime) noexcept : ScopedRuntime(&runtime) {}
    ~ScopedRuntime();
    ScopedRuntime(const ScopedRuntime&) = delete;
    ScopedRuntime& operator=(const ScopedRuntime&) = delete;
private:
    Runtime* previous_;
    bool previous_chosen_;
};

// One run owner's matrix Runtime. The engine keeps one (in its execution
// consumer) and makes it current for each public call that runs script code
// (src/engine_consumer.cpp). runtime() opens it on the run's first matrix,
// over a script scratch backing of its own (script_store.hpp), so a run that
// creates no matrix opens nothing. close() ends the owner's share when the run
// ends; the Runtime closes, and its scratch bytes are freed, with the last
// Cells, snapshot or borrow still reading it. The next run's runtime() opens a
// fresh one: no two runs share a Runtime and no thread keeps one between runs.
//
// The matrix work of one outermost call runs in one transaction on the run's
// Store, begun when the call starts (or when the call opens the Runtime) and
// committed when it returns, so a lone matrix operation joins it instead of
// committing on its own. Values are what they would be without it. A
// failure inside an operation leaves the call's transaction rollback-only,
// as any joined scope does; the commit then rolls the call's matrix changes
// back and the refusal is recorded in the Store's cleanup counters, never
// thrown. A host's own begin() on the run's Store joins the call; its
// rollback() makes the call rollback-only (there is no savepoint).
class RunRuntime {
public:
    RunRuntime() noexcept = default;
    ~RunRuntime() { close(); }
    RunRuntime(const RunRuntime&) = delete;
    RunRuntime& operator=(const RunRuntime&) = delete;
    Runtime& runtime();
    // Commits the open call's transaction and ends the owner's share.
    void close() noexcept;
    // The Runtime this run has opened, or null.
    Runtime* opened() const noexcept { return runtime_.get(); }
private:
    friend class RunScope;
    void begin_call();
    void finish_call() noexcept;
    std::shared_ptr<Runtime> runtime_;
    std::unique_ptr<o1::Tx> call_; // declared after runtime_: destroyed first
    std::uint64_t calls_ = 0;
};

// Makes `run` the calling thread's run for one public call (current_runtime).
// Calls nest; another engine's run called from a callback covers this one
// until it returns. outermost(): no other call of this run is in progress on
// the thread, so the owner may end the run when this call returns.
class RunScope {
public:
    explicit RunScope(RunRuntime& run) noexcept;
    ~RunScope();
    RunScope(const RunScope&) = delete;
    RunScope& operator=(const RunScope&) = delete;
    bool outermost() const noexcept { return run_.calls_ == 1; }
private:
    RunRuntime& run_;
    RunRuntime* previous_;
};

// A fixed descriptor precedes row-major cells in one stable Store object.
// The object layer supplies chunking, paging, zero growth and transaction undo.
// There is no matrix-sized resident buffer and no independent store/cache.
// An operation reads the descriptor once and moves records in spans of at
// most kTransferBytes through fixed stack buffers: an object that fits one
// buffer is read, or written, in one Store call.
class Cells {
public:
    // The descriptor, as stored.
    struct Shape {
        std::uint64_t rows, columns, width, reserved;
        std::uint64_t count() const; // rows x columns, or the element-limit refusal
    };
    static_assert(sizeof(Shape) == 32, "matrix descriptor has no padding");

    Cells(Runtime&, std::uint64_t rows, std::uint64_t columns, std::uint64_t width);
    // A new object whose bytes are `image`: its first sizeof(Shape) bytes are
    // set here to the descriptor and the rest are the row-major cells. One
    // create and one write; the object must fit kTransferBytes.
    Cells(Runtime&, std::uint64_t rows, std::uint64_t columns, std::uint64_t width,
          unsigned char* image);
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
    // The descriptor in one Store read. rows(), columns(), width() and count()
    // are each such a read; an operation that needs several reads shape().
    Shape shape() const;
    std::uint64_t rows() const;
    std::uint64_t columns() const;
    std::uint64_t count() const;
    std::uint64_t width() const;
    std::uint64_t hash_state() const;
    std::uint64_t index(std::uint64_t row, std::uint64_t column) const;
    // index() against a descriptor the calling operation has read.
    static std::uint64_t index(const Shape&, std::uint64_t row, std::uint64_t column);
    // Object bytes (descriptor plus cells) of a shape, or the element-limit
    // refusal when that byte count is not representable. Touches no Store.
    static std::uint64_t layout_bytes(std::uint64_t rows, std::uint64_t columns,
                                      std::uint64_t width);

    void read(std::uint64_t cell, void* record, std::size_t bytes) const;
    void write(std::uint64_t cell, const void* record, std::size_t bytes);
    // read()/write() against a descriptor the calling operation has read, and
    // that nothing has changed since: the same refusals, no descriptor read.
    void read(const Shape&, std::uint64_t cell, void* record, std::size_t bytes) const;
    void write(const Shape&, std::uint64_t cell, const void* record, std::size_t bytes);

    // The descriptor and the object's first bytes (at most kPrefixBytes) from
    // one Store read. read() copies a record lying in those bytes and reads
    // any other record alone, with Cells::read's refusals.
    class Prefix {
    public:
        explicit Prefix(const Cells&);
        Prefix(const Prefix&) = delete;
        Prefix& operator=(const Prefix&) = delete;
        const Cells& cells() const noexcept { return cells_; }
        const Shape& shape() const noexcept { return shape_; }
        void read(std::uint64_t cell, void* record, std::size_t bytes) const;
    private:
        const Cells& cells_;
        Shape shape_{};
        std::size_t loaded_ = 0;
        std::array<unsigned char, kPrefixBytes> bytes_; // only [0, loaded_) is read
    };

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
    // Records from `first` on set to `record` (`bytes` must be the width),
    // one write per kTransferBytes of records. No Scope of its own: the
    // calling matrix operation's makes the writes one change.
    void fill(const void* record, std::size_t bytes, std::uint64_t first = 0);
    // A new object, the transpose of this one. A matrix with no cells becomes
    // 0 x 0 when `collapse_empty` (the generic matrix's rule), else columns x
    // rows (the numeric matrix's).
    std::shared_ptr<Cells> transpose(bool collapse_empty) const;
    // A new rows x columns object: the block of this one at (row, column).
    // Null, with nothing created, when a block that has cells does not lie
    // inside this object; the caller keeps its own per-cell path for that.
    std::shared_ptr<Cells> block(std::uint64_t row, std::uint64_t column,
                                 std::uint64_t rows, std::uint64_t columns) const;
    // The same block copied into this object, just created rows x columns of
    // records `width` wide, after it exists, as per-cell copies would fill it.
    // False, with nothing written, when the block does not lie inside
    // `source` or the widths differ; the caller copies per cell then.
    bool copy_block(const Cells& source, std::uint64_t row, std::uint64_t column,
                    std::uint64_t rows, std::uint64_t columns, std::uint64_t width);
    // A new object: this one and `other` side by side (`horizontal`; equal
    // rows) or this one above `other` (equal columns), with transpose()'s
    // `collapse_empty` rule for rows and columns. Both of one record width.
    std::shared_ptr<Cells> concat(const Cells& other, bool horizontal,
                                  bool collapse_empty) const;
    // The descriptor and, when the whole object fits kTransferBytes, every
    // cell: one Store read up to kPrefixBytes of object, two up to
    // kTransferBytes. `whole` says whether `buffer` holds it all.
    Shape load(std::array<unsigned char, kTransferBytes>& buffer, bool& whole) const;
    // Records [first, first + count) of a descriptor the calling operation
    // has read, in one Store call; count x width at most kTransferBytes.
    void read_span(const Shape&, std::uint64_t first, std::uint64_t count, void* records) const;
    void write_span(const Shape&, std::uint64_t first, std::uint64_t count,
                    const void* records);
    std::shared_ptr<Cells> clone() const;
    void restore(const Cells& frozen);
    void close();

private:
    using Header = Shape;
    Header header() const;
    void set_header(const Header&);
    void exact_read(std::uint64_t offset, void*, std::size_t) const;
    void exact_write(std::uint64_t offset, const void*, std::size_t);
    // Byte-range transfers, one kTransferBytes piece per Store read/write.
    void move_bytes(std::uint64_t from, std::uint64_t to, std::uint64_t length);
    void swap_bytes(std::uint64_t first, std::uint64_t second, std::uint64_t length);
    void copy_bytes(std::uint64_t to, const Cells& source, std::uint64_t from,
                    std::uint64_t length);
    void swap_records(const Header&, std::uint64_t first, std::uint64_t second);
    // Resizes to `h` and writes image[0, written), `h` first: the whole-object
    // path's one resize and one write.
    void replace(const Header& h, const unsigned char* image, std::size_t written);
    static std::uint64_t product(std::uint64_t, std::uint64_t);
    static std::uint64_t byte_size(const Header&);
    Runtime& runtime_;
    Id id_ = o1::kNullId;
    bool owning_ = false;
    // The share of a Runtime held by a shared_ptr (a run's own); null for a
    // borrowed Runtime, whose owner keeps it alive.
    std::shared_ptr<Runtime> owner_;
};

// Never infer ownership from a native struct's bytes. A generated UDT mapping
// must explicitly encode a stable handle, with graph ownership supplied by the UDT graph module.
// Unknown native values (including owning string/vector fields) remain native.
template<class T, class Enable = void>
struct codec { static constexpr bool mapped = false; };

template<class T>
struct codec<T, std::enable_if_t<std::is_arithmetic_v<T> || std::is_enum_v<T>>> {
    static constexpr bool mapped = true;
    // A record is the value's bytes: encoding calls no Store and keeps no
    // state, so bulk paths encode once and move many records per Store call.
    static constexpr bool bytewise = true;
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

// Whether T has the bytewise codec. Any other codec may call the Store while
// it encodes (a handle element refreshing the matrix it names), so its
// records are encoded and written one at a time, in record order.
template<class T, class = void>
struct bytewise_codec : std::false_type {};
template<class T>
struct bytewise_codec<T, std::void_t<decltype(codec<T>::bytewise)>>
    : std::bool_constant<codec<T>::bytewise> {};

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

// get(): the record from the Prefix that served the bounds checks.
template<class T>
inline T read_value(const Cells::Prefix& prefix, std::uint64_t index) {
    if constexpr (codec<T>::mapped) {
        std::array<unsigned char, codec<T>::record_bytes> record{};
        prefix.read(index, record.data(), record.size());
        return codec_detail::decode<T>(prefix.cells().runtime(), record.data(), 0);
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

// set(): a bytewise record is written against the descriptor that served the
// bounds checks. Any other codec's encode may change the matrix, so its write
// checks a descriptor read after the encode, as write_value() above does.
template<class T>
inline void write_value(Cells& cells, const Cells::Shape& shape, std::uint64_t index,
                        const T& value) {
    if constexpr (bytewise_codec<T>::value) {
        std::array<unsigned char, codec<T>::record_bytes> record{};
        codec<T>::encode(value, record.data());
        cells.write(shape, index, record.data(), record.size());
    } else {
        write_value(cells, index, value);
    }
}

// fill(): records from `first` on set to `value`. A bytewise value is encoded
// once and written a span at a time; any other is encoded once per record.
template<class T>
inline void fill_value(Cells& cells, const T& value, std::uint64_t first = 0) {
    if constexpr (bytewise_codec<T>::value) {
        std::array<unsigned char, codec<T>::record_bytes> record{};
        codec<T>::encode(value, record.data());
        cells.fill(record.data(), record.size(), first);
    } else {
        for (std::uint64_t i = first, n = cells.count(); i < n; ++i)
            write_value(cells, i, value);
    }
}

template<class T>
inline std::shared_ptr<Cells> make_cells(Runtime& runtime, std::uint64_t rows,
                                         std::uint64_t columns, const T& initial) {
    if constexpr (codec<T>::mapped) {
        Scope transaction(runtime);
        std::shared_ptr<Cells> cells;
        constexpr std::size_t width = codec<T>::record_bytes;
        if constexpr (bytewise_codec<T>::value) {
            // An object that fits one transfer buffer: one create, one write.
            if (Cells::layout_bytes(rows, columns, width) <= kTransferBytes) {
                std::array<unsigned char, kTransferBytes> image;
                std::array<unsigned char, width> record{};
                codec<T>::encode(initial, record.data());
                for (std::uint64_t i = 0, n = rows * columns; i < n; ++i)
                    std::memcpy(image.data() + sizeof(Cells::Shape) + i * width,
                                record.data(), width);
                cells = std::make_shared<Cells>(runtime, rows, columns, width, image.data());
                transaction.commit();
                return cells;
            }
        }
        cells = std::make_shared<Cells>(runtime, rows, columns, width);
        fill_value(*cells, initial);
        transaction.commit();
        return cells;
    } else {
        invariant("unmapped matrix element construction");
    }
}

} // namespace pineforge::detail::matrix_store
