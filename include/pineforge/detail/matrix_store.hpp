// External matrix records share the UDT arena and its single object Store.
// Tiles are fixed in cells, independent of row width: wide rows cross tiles.
#pragma once

#include <pineforge/detail/udt_graph.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>

namespace pineforge::detail::matrix_store {

using Id = o1::Id;
inline constexpr std::uint64_t tile_bytes = 4096;

[[noreturn]] inline void invariant(const char* why) {
    throw coded<std::logic_error>(RunFailureCode::engine_invariant, {}, why);
}
inline std::uint64_t product(std::uint64_t a, std::uint64_t b) {
    if (b && a > std::numeric_limits<std::uint64_t>::max() / b)
        invariant("matrix address space exhausted");
    return a * b;
}

// The run owner provisions the Store/backing before bar zero and outlives all
// matrix handles. No process-global backing, second cache, or implicit medium.
class Runtime {
public:
    explicit Runtime(o1::Store& store) : graph_(store) {}
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    o1::UdtGraph& graph() noexcept { return graph_; }
    const o1::UdtGraph& graph() const noexcept { return graph_; }
    o1::Store& store() const noexcept { return graph_.store(); }
    std::uint64_t collect() { return graph_.collect(); }
    void close() { graph_.close(); }
private:
    o1::UdtGraph graph_;
};

inline thread_local Runtime* active_runtime = nullptr;
inline Runtime* current_runtime() noexcept { return active_runtime; }
class ScopedRuntime {
public:
    explicit ScopedRuntime(Runtime& runtime) noexcept
        : previous_(active_runtime) { active_runtime = &runtime; }
    ~ScopedRuntime() { active_runtime = previous_; }
    ScopedRuntime(const ScopedRuntime&) = delete;
    ScopedRuntime& operator=(const ScopedRuntime&) = delete;
private:
    Runtime* previous_;
};

// Payloads encoded here are immutable value leaves. Generated reference UDTs
// encode their graph id directly and list that id as an owning slot instead.
class Encoder {
public:
    explicit Encoder(o1::UdtGraph& graph) : graph_(graph) {}
    o1::UdtGraph& graph() const noexcept { return graph_; }
    Id put(const void* bytes, std::uint64_t size) const {
        const Id id = graph_.make(size);
        const auto* source = static_cast<const unsigned char*>(bytes);
        for (std::uint64_t off = 0; off < size;) {
            const auto count = static_cast<std::size_t>(std::min(tile_bytes, size - off));
            graph_.write(id, off, source + off, count);
            off += count;
        }
        return id;
    }
private:
    o1::UdtGraph& graph_;
};
class Decoder {
public:
    explicit Decoder(o1::UdtGraph& graph) : graph_(graph) {}
    o1::UdtGraph& graph() const noexcept { return graph_; }
    std::uint64_t size(Id id) const { return graph_.payload_size(id); }
    void get(Id id, void* bytes, std::uint64_t size) const {
        auto* target = static_cast<unsigned char*>(bytes);
        for (std::uint64_t off = 0; off < size;) {
            const auto count = static_cast<std::size_t>(std::min(tile_bytes, size - off));
            graph_.read(id, off, target + off, count);
            off += count;
        }
    }
private:
    o1::UdtGraph& graph_;
};

// There is no inference of a native UDT's ownership. A generated codec supplies
// a fixed record, a fixed number of owning slots, and owned(record, slot).
// Unknown native types retain their legacy representation and are NOT bounded.
template<class T, class Enable = void>
struct codec { static constexpr bool mapped = false; };

template<class T>
struct codec<T, std::enable_if_t<std::is_arithmetic_v<T> || std::is_enum_v<T>>> {
    static constexpr bool mapped = true;
    static constexpr std::uint32_t record_bytes = sizeof(T);
    static constexpr std::uint32_t owned_slots = 0;
    static void encode(Encoder&, const T& value, unsigned char* record) {
        std::memcpy(record, &value, sizeof value);
    }
    static T decode(const Decoder&, const unsigned char* record) {
        T value{};
        std::memcpy(&value, record, sizeof value);
        return value;
    }
    static Id owned(const unsigned char*, std::uint32_t) { return 0; }
};

template<>
struct codec<std::string, void> {
    static constexpr bool mapped = true;
    static constexpr std::uint32_t record_bytes = 16;
    static constexpr std::uint32_t owned_slots = 1;
    static void encode(Encoder& encoder, const std::string& value, unsigned char* record) {
        const std::uint64_t length = value.size();
        const Id id = length ? encoder.put(value.data(), length) : 0;
        std::memcpy(record, &length, 8);
        std::memcpy(record + 8, &id, 8);
    }
    static std::string decode(const Decoder& decoder, const unsigned char* record) {
        std::uint64_t length = 0;
        std::memcpy(&length, record, 8);
        std::string value(static_cast<std::size_t>(length), '\0');
        if (length) decoder.get(owned(record, 0), value.data(), length);
        return value;
    }
    static Id owned(const unsigned char* record, std::uint32_t) {
        Id id = 0;
        std::memcpy(&id, record + 8, 8);
        return id;
    }
};

class TileMatrix {
    struct Header {
        std::uint64_t rows, columns;
        std::uint32_t width, owned_slots;
        std::uint64_t cells_per_tile;
    };
    static_assert(sizeof(Header) == 32, "fixed matrix schema");
    struct Borrowed {};
    TileMatrix(Runtime& runtime, Id identity, Borrowed)
        : runtime_(runtime), header_(identity) {
        const auto h = header();
        if (!h.width || h.cells_per_tile != std::max<std::uint64_t>(1, tile_bytes / h.width))
            invariant("invalid borrowed matrix schema");
    }
public:
    TileMatrix(Runtime& runtime, std::uint64_t rows, std::uint64_t columns,
               std::uint32_t width, std::uint32_t owned_slots)
        : runtime_(runtime) {
        if (!width) invariant("zero matrix record width");
        header_ = graph().make(sizeof(Header));
        const auto per_tile = std::max<std::uint64_t>(1, tile_bytes / width);
        graph().set_scalar(header_, Header{0, columns, width, owned_slots, per_tile});
        root_ = graph().root(header_);
        resize(rows, columns);
    }
    ~TileMatrix() {
        // A speculative native handle may be destroyed after Store rollback.
        // Its id has died; it must neither be resurrected nor dropped twice.
        if (graph().alive(root_)) graph().drop_root(root_);
    }
    TileMatrix(const TileMatrix&) = delete;
    TileMatrix& operator=(const TileMatrix&) = delete;

    // Compiler-normalized collection fields store this identity as a graph
    // edge. A temporary borrow creates no native control block or graph root;
    // the compiler must keep its owner rooted across the entire borrow.
    static TileMatrix borrow(Runtime& runtime, Id identity) {
        return TileMatrix(runtime, identity, Borrowed{});
    }
    Runtime& runtime() const noexcept { return runtime_; }
    Id identity() const noexcept { return header_; }
    std::uint64_t rows() const { return header().rows; }
    std::uint64_t columns() const { return header().columns; }
    std::uint64_t count() const { const auto h = header(); return product(h.rows, h.columns); }
    std::uint32_t width() const { return header().width; }

    void read(std::uint64_t cell, void* record) const {
        const auto h = header();
        if (cell >= product(h.rows, h.columns)) invariant("matrix cell index");
        graph().read(tile(cell, h), (cell % h.cells_per_tile) * h.width, record, h.width);
    }
    void write(std::uint64_t cell, const void* record) {
        const auto h = header();
        if (cell >= product(h.rows, h.columns)) invariant("matrix cell index");
        graph().write(tile(cell, h), (cell % h.cells_per_tile) * h.width, record, h.width);
    }
    void own(std::uint64_t cell, std::uint32_t slot, Id child) {
        const auto h = header();
        if (cell >= product(h.rows, h.columns) || slot >= h.owned_slots)
            invariant("matrix owning slot index");
        graph().set_edge(tile(cell, h), (cell % h.cells_per_tile) * h.owned_slots + slot, child);
    }

    // Resize changes only the external directory and boundary tile. A record
    // wider than a page remains a fixed schema record, with no capacity cap.
    void resize(std::uint64_t rows, std::uint64_t columns) {
        Header h = header();
        const auto old_count = product(h.rows, h.columns), new_count = product(rows, columns);
        const auto old_tiles = graph().edge_count(header_);
        const auto new_tiles = new_count / h.cells_per_tile + (new_count % h.cells_per_tile != 0);
        if (new_count < old_count && new_tiles) {
            // Clear removed slots in the surviving boundary tile, including
            // ownership edges, so dead nested payloads can be reclaimed exactly.
            const auto end = std::min(old_count, product(new_tiles, h.cells_per_tile));
            const std::array<unsigned char, tile_bytes> zero{};
            for (auto cell = new_count; cell < end; ++cell) {
                const Id id = tile(cell, h);
                for (std::uint64_t off = 0; off < h.width;) {
                    const auto bytes = static_cast<std::size_t>(std::min<std::uint64_t>(
                        zero.size(), h.width - off));
                    graph().write(id, (cell % h.cells_per_tile) * h.width + off, zero.data(), bytes);
                    off += bytes;
                }
                for (std::uint32_t slot = 0; slot < h.owned_slots; ++slot)
                    graph().set_edge(id, (cell % h.cells_per_tile) * h.owned_slots + slot, 0);
            }
        }
        if (new_tiles != old_tiles) graph().resize_edges(header_, new_tiles);
        for (auto i = old_tiles; i < new_tiles; ++i) {
            const Id id = graph().make(product(h.cells_per_tile, h.width),
                                       product(h.cells_per_tile, h.owned_slots));
            graph().set_edge(header_, i, id);
        }
        h.rows = rows;
        h.columns = columns;
        graph().set_scalar(header_, h);
    }

    // Raw copies preserve immutable value payloads and reference ids. They also
    // copy owning slots; ordinary aliasing never creates a second child object.
    void copy_cell(std::uint64_t target, const TileMatrix& source, std::uint64_t from) {
        const auto h = header(), s = source.header();
        if (&runtime_ != &source.runtime_ || h.width != s.width || h.owned_slots != s.owned_slots)
            invariant("matrix schema or arena mismatch");
        if (target >= product(h.rows, h.columns) || from >= product(s.rows, s.columns))
            invariant("matrix copy cell index");
        const Id dst = tile(target, h), src = source.tile(from, s);
        const auto dst_off = (target % h.cells_per_tile) * h.width;
        const auto src_off = (from % s.cells_per_tile) * s.width;
        std::array<unsigned char, tile_bytes> buffer{};
        for (std::uint64_t off = 0; off < h.width;) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), h.width - off));
            graph().read(src, src_off + off, buffer.data(), count);
            graph().write(dst, dst_off + off, buffer.data(), count);
            off += count;
        }
        for (std::uint32_t slot = 0; slot < h.owned_slots; ++slot)
            graph().set_edge(dst, (target % h.cells_per_tile) * h.owned_slots + slot,
                graph().edge(src, (from % s.cells_per_tile) * s.owned_slots + slot));
    }
    void swap_cells(std::uint64_t first, std::uint64_t second) {
        if (first == second) return;
        const auto h = header();
        if (first >= product(h.rows, h.columns) || second >= product(h.rows, h.columns))
            invariant("matrix swap cell index");
        const Id a = tile(first, h), b = tile(second, h);
        const auto ao = first % h.cells_per_tile, bo = second % h.cells_per_tile;
        std::array<unsigned char, tile_bytes> left{}, right{};
        for (std::uint64_t off = 0; off < h.width;) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(left.size(), h.width - off));
            graph().read(a, ao * h.width + off, left.data(), count);
            graph().read(b, bo * h.width + off, right.data(), count);
            graph().write(a, ao * h.width + off, right.data(), count);
            graph().write(b, bo * h.width + off, left.data(), count);
            off += count;
        }
        for (std::uint32_t slot = 0; slot < h.owned_slots; ++slot) {
            const Id old = graph().edge(a, ao * h.owned_slots + slot);
            graph().set_edge(a, ao * h.owned_slots + slot, graph().edge(b, bo * h.owned_slots + slot));
            graph().set_edge(b, bo * h.owned_slots + slot, old);
        }
    }

    std::shared_ptr<TileMatrix> clone() const {
        const auto h = header();
        auto copy = std::make_shared<TileMatrix>(runtime_, h.rows, h.columns, h.width, h.owned_slots);
        for (std::uint64_t cell = 0, end = product(h.rows, h.columns); cell < end; ++cell)
            copy->copy_cell(cell, *this, cell);
        return copy;
    }
    void restore(const TileMatrix& snapshot) {
        if (this == &snapshot) return;
        const auto h = snapshot.header();
        resize(h.rows, h.columns);
        for (std::uint64_t cell = 0, end = product(h.rows, h.columns); cell < end; ++cell)
            copy_cell(cell, snapshot, cell);
    }

private:
    o1::UdtGraph& graph() const noexcept { return runtime_.graph(); }
    Header header() const { return graph().scalar<Header>(header_); }
    Id tile(std::uint64_t cell, const Header& h) const {
        return graph().edge(header_, cell / h.cells_per_tile);
    }
    Runtime& runtime_;
    Id header_ = 0, root_ = 0;
};

template<class T>
inline T read_value(const TileMatrix& matrix, std::uint64_t cell) {
    std::array<unsigned char, codec<T>::record_bytes> record{};
    matrix.read(cell, record.data());
    return codec<T>::decode(Decoder(matrix.runtime().graph()), record.data());
}
template<class T>
inline void write_value(TileMatrix& matrix, std::uint64_t cell, const T& value) {
    std::array<unsigned char, codec<T>::record_bytes> record{};
    Encoder encoder(matrix.runtime().graph());
    codec<T>::encode(encoder, value, record.data());
    matrix.write(cell, record.data());
    for (std::uint32_t slot = 0; slot < codec<T>::owned_slots; ++slot)
        matrix.own(cell, slot, codec<T>::owned(record.data(), slot));
}

} // namespace pineforge::detail::matrix_store
