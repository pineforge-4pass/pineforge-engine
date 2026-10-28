#include <pineforge/detail/matrix_store.hpp>
#include <pineforge/detail/script_store.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace pineforge::detail::matrix_store {
static_assert(kPrefixBytes >= sizeof(Cells::Shape) && kPrefixBytes <= kTransferBytes,
              "a get prefix holds the descriptor and fits one transfer buffer");
namespace {
// The calling thread's matrix Runtime source (current_runtime): a
// ScopedRuntime's explicit choice while one is in force, else the run whose
// public call is in progress. Restored as each scope ends, so a thread holds
// no Runtime between calls.
thread_local Runtime* active_runtime = nullptr;
thread_local bool runtime_chosen = false;
thread_local RunRuntime* active_run = nullptr;

// Not representable as a Store object: the script-visible matrix element limit,
// raised with the code the matrix element cap uses. There is no engine constant
// for this bound, so the limit is named without a max (pine_limit_stop's "none").
[[noreturn]] void address_space_exhausted() {
    throw coded<std::overflow_error>(RunFailureCode::pine_runtime_limit,
                                     {{"limit", "matrix_elements"}},
                                     "matrix address space exhausted");
}
}

Runtime::Runtime(std::unique_ptr<ScratchBacking> backing, const o1::Shape& shape)
    : backing_(std::move(backing)) {
    if (!backing_) invariant("matrix runtime without a backing");
    owned_ = o1::Store::open(shape, *backing_);
    store_ = owned_.get();
}

Runtime* current_runtime() {
    if (runtime_chosen) return active_runtime;
    return active_run ? &active_run->runtime() : nullptr;
}
ScopedRuntime::ScopedRuntime(Runtime* runtime) noexcept
    : previous_(active_runtime), previous_chosen_(runtime_chosen) {
    active_runtime = runtime;
    runtime_chosen = true;
}
ScopedRuntime::~ScopedRuntime() {
    active_runtime = previous_;
    runtime_chosen = previous_chosen_;
}

Runtime& RunRuntime::runtime() {
    if (!runtime_) {
        runtime_ = std::make_shared<Runtime>(make_script_scratch_backing());
        if (calls_) begin_call();
    }
    return *runtime_;
}
void RunRuntime::begin_call() { call_ = runtime_->store().begin(); }
void RunRuntime::finish_call() noexcept {
    if (!call_) return;
    try {
        call_->commit();
    } catch (const o1::RollbackOnly&) {
        // The commit rolled the failed call's matrix changes back.
        runtime_->store().note_cleanup_skipped(o1::CleanupSkip::rollback_only);
    } catch (...) {
        runtime_->store().note_cleanup_skipped(o1::CleanupSkip::failed);
    }
    call_.reset();
}
void RunRuntime::close() noexcept {
    finish_call();
    runtime_.reset();
}
RunScope::RunScope(RunRuntime& run) noexcept : run_(run), previous_(active_run) {
    // A Runtime an earlier call of the run opened (a stream's) takes this
    // call's transaction now. A refused begin leaves every operation its own
    // one-call transaction; a Store that refuses it fails those visibly.
    if (run_.calls_++ == 0 && run_.runtime_) {
        try { run_.begin_call(); } catch (...) {}
    }
    active_run = &run_;
}
RunScope::~RunScope() {
    if (--run_.calls_ == 0) run_.finish_call();
    active_run = previous_;
}

std::uint64_t Cells::product(std::uint64_t a, std::uint64_t b) {
    if (b && a > std::numeric_limits<std::uint64_t>::max() / b)
        address_space_exhausted();
    return a * b;
}

std::uint64_t Cells::byte_size(const Header& h) {
    const auto bytes = product(product(h.rows, h.columns), h.width);
    if (bytes > std::numeric_limits<std::uint64_t>::max() - sizeof(Header))
        address_space_exhausted();
    return sizeof(Header) + bytes;
}

std::uint64_t Cells::layout_bytes(std::uint64_t rows, std::uint64_t columns,
                                  std::uint64_t width) {
    return byte_size(Header{rows, columns, width, 0});
}

std::uint64_t Cells::Shape::count() const { return product(rows, columns); }

Cells::Cells(Runtime& runtime, std::uint64_t rows, std::uint64_t columns,
             std::uint64_t width)
    : runtime_(runtime), owner_(runtime.weak_from_this().lock()) {
    if (!width) invariant("zero matrix record width");
    const Header h{rows, columns, width, 0};
    const auto bytes = byte_size(h); // a shape that cannot be stored is refused before any Store call
    Scope transaction(runtime_);
    id_ = runtime_.store().create(bytes, alignof(Header));
    set_header(h);
    transaction.commit();
    owning_ = true;
}

Cells::Cells(Runtime& runtime, std::uint64_t rows, std::uint64_t columns,
             std::uint64_t width, unsigned char* image)
    : runtime_(runtime), owner_(runtime.weak_from_this().lock()) {
    if (!width) invariant("zero matrix record width");
    const Header h{rows, columns, width, 0};
    const auto bytes = byte_size(h); // refused before any Store call, as above
    if (bytes > kTransferBytes) invariant("matrix image larger than its transfer buffer");
    std::memcpy(image, &h, sizeof h);
    Scope transaction(runtime_);
    id_ = runtime_.store().create(bytes, alignof(Header));
    exact_write(0, image, static_cast<std::size_t>(bytes));
    transaction.commit();
    owning_ = true;
}

Cells::Cells(Runtime& runtime, Id id)
    : runtime_(runtime), id_(id), owner_(runtime.weak_from_this().lock()) {
    const auto h = header();
    if (!h.width || h.reserved || runtime_.store().size(id_) != byte_size(h))
        invariant("invalid borrowed matrix descriptor");
}

Cells Cells::borrow(Runtime& runtime, Id id) {
    return Cells(runtime, id);
}

Cells::~Cells() noexcept {
    // A rolled-back speculative object is already dead. A backing failure
    // poisons the Store transaction; cleanup must not mask the original error.
    try { close(); } catch (...) {}
}

void Cells::close() {
    if (owning_ && runtime_.store().alive(id_)) runtime_.store().release(id_);
    owning_ = false;
}

void Cells::exact_read(std::uint64_t offset, void* bytes, std::size_t size) const {
    if (runtime_.store().read(id_, offset, bytes, size) != size)
        invariant("short matrix object read");
}
void Cells::exact_write(std::uint64_t offset, const void* bytes, std::size_t size) {
    if (runtime_.store().write(id_, offset, bytes, size) != size)
        invariant("short matrix object write");
}
Cells::Header Cells::header() const {
    Header h{};
    exact_read(0, &h, sizeof h);
    return h;
}
void Cells::set_header(const Header& h) { exact_write(0, &h, sizeof h); }
Cells::Shape Cells::shape() const { return header(); }
std::uint64_t Cells::rows() const { return header().rows; }
std::uint64_t Cells::columns() const { return header().columns; }
std::uint64_t Cells::width() const { return header().width; }
std::uint64_t Cells::count() const { return header().count(); }

std::uint64_t Cells::index(const Shape& h, std::uint64_t row, std::uint64_t column) {
    if (row >= h.rows || column >= h.columns)
        invariant("matrix coordinate outside its schema");
    return product(row, h.columns) + column;
}
std::uint64_t Cells::index(std::uint64_t row, std::uint64_t column) const {
    return index(header(), row, column);
}

void Cells::read(const Shape& h, std::uint64_t cell, void* record, std::size_t bytes) const {
    if (bytes != h.width || cell >= product(h.rows, h.columns))
        invariant("matrix record read outside its schema");
    exact_read(sizeof(Header) + cell * h.width, record, bytes);
}
void Cells::write(const Shape& h, std::uint64_t cell, const void* record, std::size_t bytes) {
    if (bytes != h.width || cell >= product(h.rows, h.columns))
        invariant("matrix record write outside its schema");
    exact_write(sizeof(Header) + cell * h.width, record, bytes);
}
void Cells::read(std::uint64_t cell, void* record, std::size_t bytes) const {
    read(header(), cell, record, bytes);
}
void Cells::write(std::uint64_t cell, const void* record, std::size_t bytes) {
    write(header(), cell, record, bytes);
}

// The Store returns the bytes up to the object's end: a short count here is
// an object smaller than the prefix, not an error.
Cells::Prefix::Prefix(const Cells& cells) : cells_(cells) {
    loaded_ = cells_.runtime_.store().read(cells_.id_, 0, bytes_.data(), bytes_.size());
    if (loaded_ < sizeof(Header)) invariant("short matrix object read");
    std::memcpy(&shape_, bytes_.data(), sizeof shape_);
}
void Cells::Prefix::read(std::uint64_t cell, void* record, std::size_t bytes) const {
    if (bytes != shape_.width || cell >= product(shape_.rows, shape_.columns))
        invariant("matrix record read outside its schema");
    const auto offset = sizeof(Header) + cell * shape_.width;
    if (offset <= loaded_ && bytes <= loaded_ - offset)
        std::memcpy(record, bytes_.data() + offset, bytes);
    else
        cells_.exact_read(offset, record, bytes);
}

// The prefix first, and the rest of the object only when all of it fits the
// buffer: a larger object costs one prefix read, as a descriptor read does.
Cells::Shape Cells::load(std::array<unsigned char, kTransferBytes>& buffer, bool& whole) const {
    auto loaded = runtime_.store().read(id_, 0, buffer.data(), kPrefixBytes);
    if (loaded < sizeof(Header)) invariant("short matrix object read");
    Header h{};
    std::memcpy(&h, buffer.data(), sizeof h);
    const auto bytes = byte_size(h);
    if (bytes > loaded && bytes <= buffer.size()) {
        exact_read(loaded, buffer.data() + loaded, static_cast<std::size_t>(bytes - loaded));
        loaded = static_cast<std::size_t>(bytes);
    }
    whole = bytes <= loaded;
    return h;
}

void Cells::read_span(const Shape& h, std::uint64_t first, std::uint64_t count,
                      void* records) const {
    if (first > product(h.rows, h.columns) || count > product(h.rows, h.columns) - first ||
        product(count, h.width) > kTransferBytes)
        invariant("matrix record read outside its schema");
    exact_read(sizeof(Header) + first * h.width, records,
               static_cast<std::size_t>(count * h.width));
}
void Cells::write_span(const Shape& h, std::uint64_t first, std::uint64_t count,
                       const void* records) {
    if (first > product(h.rows, h.columns) || count > product(h.rows, h.columns) - first ||
        product(count, h.width) > kTransferBytes)
        invariant("matrix record write outside its schema");
    exact_write(sizeof(Header) + first * h.width, records,
                static_cast<std::size_t>(count * h.width));
}

void Cells::move_bytes(std::uint64_t from, std::uint64_t to, std::uint64_t length) {
    // As memmove: toward the front piece by piece from the start, toward the
    // back from the end, so no piece is read after it was overwritten.
    std::array<unsigned char, kTransferBytes> buffer;
    if (to < from) {
        for (std::uint64_t done = 0; done < length;) {
            const auto n = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), length - done));
            exact_read(from + done, buffer.data(), n);
            exact_write(to + done, buffer.data(), n);
            done += n;
        }
    } else if (to > from) {
        for (std::uint64_t left = length; left > 0;) {
            const auto n = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), left));
            left -= n;
            exact_read(from + left, buffer.data(), n);
            exact_write(to + left, buffer.data(), n);
        }
    }
}

void Cells::swap_bytes(std::uint64_t first, std::uint64_t second, std::uint64_t length) {
    std::array<unsigned char, kTransferBytes> left, right;
    for (std::uint64_t offset = 0; offset < length;) {
        const auto n = static_cast<std::size_t>(
            std::min<std::uint64_t>(left.size(), length - offset));
        exact_read(first + offset, left.data(), n);
        exact_read(second + offset, right.data(), n);
        exact_write(first + offset, right.data(), n);
        exact_write(second + offset, left.data(), n);
        offset += n;
    }
}

void Cells::copy_bytes(std::uint64_t to, const Cells& source, std::uint64_t from,
                       std::uint64_t length) {
    std::array<unsigned char, kTransferBytes> buffer;
    for (std::uint64_t offset = 0; offset < length;) {
        const auto n = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), length - offset));
        source.exact_read(from + offset, buffer.data(), n);
        exact_write(to + offset, buffer.data(), n);
        offset += n;
    }
}

void Cells::swap_records(const Header& h, std::uint64_t first, std::uint64_t second) {
    if (first == second) return;
    if (first >= product(h.rows, h.columns) || second >= product(h.rows, h.columns))
        invariant("matrix record swap outside its schema");
    swap_bytes(sizeof(Header) + first * h.width, sizeof(Header) + second * h.width, h.width);
}

void Cells::replace(const Header& h, const unsigned char* image, std::size_t written) {
    if (!runtime_.store().resize(id_, byte_size(h))) invariant("matrix resize failed");
    exact_write(0, image, written);
}

void Cells::copy_cell(std::uint64_t target, const Cells& source, std::uint64_t from) {
    const auto h = header(), s = source.header();
    if (h.width != s.width || target >= product(h.rows, h.columns) ||
        from >= product(s.rows, s.columns))
        invariant("matrix record copy outside its schema");
    Scope transaction(runtime_);
    std::array<unsigned char, kTransferBytes> buffer{};
    for (std::uint64_t offset = 0; offset < h.width;) {
        const auto n = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), h.width - offset));
        source.exact_read(sizeof(Header) + from * s.width + offset, buffer.data(), n);
        exact_write(sizeof(Header) + target * h.width + offset, buffer.data(), n);
        offset += n;
    }
    transaction.commit();
}

void Cells::swap_cells(std::uint64_t first, std::uint64_t second) {
    if (first == second) return;
    const auto h = header();
    if (first >= product(h.rows, h.columns) || second >= product(h.rows, h.columns))
        invariant("matrix record swap outside its schema");
    Scope transaction(runtime_);
    swap_bytes(sizeof(Header) + first * h.width, sizeof(Header) + second * h.width, h.width);
    transaction.commit();
}

void Cells::resize(std::uint64_t rows, std::uint64_t columns) {
    auto h = header();
    h.rows = rows;
    h.columns = columns;
    Scope transaction(runtime_);
    if (!runtime_.store().resize(id_, byte_size(h))) invariant("matrix resize failed");
    set_header(h);
    transaction.commit();
}

// The shifts below move whole runs of records, as the per-cell copies did one
// record at a time and in the same order. An object that fits one transfer
// buffer is changed in the buffer and written back with its new descriptor in
// one write; a larger one moves its runs a buffer at a time.
void Cells::insert_row(std::uint64_t index, std::uint64_t columns) {
    std::array<unsigned char, kTransferBytes> image;
    bool whole = false;
    const auto h = load(image, whole);
    if (index > h.rows || (h.rows && columns != h.columns))
        invariant("matrix row insertion outside its schema");
    Scope transaction(runtime_);
    const auto old_end = product(h.rows, columns);
    auto grown = h;
    grown.rows = h.rows + 1;
    grown.columns = columns;
    const auto bytes = byte_size(grown);
    // Records [begin, old_end) move up one row. The row they leave keeps its
    // bytes, zero past the old end, as the per-cell shift left it.
    const auto begin = product(index, columns);
    const auto from = sizeof(Header) + begin * h.width;
    const auto to = from + columns * h.width;
    const auto length = (old_end - begin) * h.width;
    if (whole && bytes <= kTransferBytes) {
        const auto old_bytes = sizeof(Header) + old_end * h.width;
        std::memset(image.data() + old_bytes, 0, static_cast<std::size_t>(bytes - old_bytes));
        std::memmove(image.data() + to, image.data() + from, static_cast<std::size_t>(length));
        std::memcpy(image.data(), &grown, sizeof grown);
        replace(grown, image.data(),
                length ? static_cast<std::size_t>(bytes) : sizeof(Header));
    } else {
        if (!runtime_.store().resize(id_, bytes)) invariant("matrix resize failed");
        set_header(grown);
        move_bytes(from, to, length);
    }
    transaction.commit();
}

void Cells::insert_column(std::uint64_t index) {
    std::array<unsigned char, kTransferBytes> image;
    bool whole = false;
    const auto h = load(image, whole);
    if (index > h.columns) invariant("matrix column insertion outside its schema");
    Scope transaction(runtime_);
    auto grown = h;
    grown.columns = h.columns + 1;
    const auto bytes = byte_size(grown);
    const auto w = h.width;
    // Last row first: a row's records from `index` on move one record past
    // its new start, the records before `index` to its new start. The new
    // slot keeps the bytes it had.
    if (whole && bytes <= kTransferBytes) {
        const auto old_bytes = static_cast<std::size_t>(byte_size(h));
        std::memset(image.data() + old_bytes, 0, static_cast<std::size_t>(bytes) - old_bytes);
        for (auto r = h.rows; r > 0; --r) {
            const auto from = sizeof(Header) + (r - 1) * h.columns * w;
            const auto to = sizeof(Header) + (r - 1) * grown.columns * w;
            std::memmove(image.data() + to + (index + 1) * w, image.data() + from + index * w,
                         static_cast<std::size_t>((h.columns - index) * w));
            std::memmove(image.data() + to, image.data() + from,
                         static_cast<std::size_t>(index * w));
        }
        std::memcpy(image.data(), &grown, sizeof grown);
        replace(grown, image.data(), static_cast<std::size_t>(bytes));
    } else {
        if (!runtime_.store().resize(id_, bytes)) invariant("matrix resize failed");
        set_header(grown);
        for (auto r = h.rows; r > 0; --r) {
            const auto from = sizeof(Header) + (r - 1) * h.columns * w;
            const auto to = sizeof(Header) + (r - 1) * grown.columns * w;
            move_bytes(from + index * w, to + (index + 1) * w, (h.columns - index) * w);
            move_bytes(from, to, index * w);
        }
    }
    transaction.commit();
}

void Cells::remove_row(std::uint64_t index) {
    std::array<unsigned char, kTransferBytes> image;
    bool whole = false;
    const auto h = load(image, whole);
    if (index >= h.rows) invariant("matrix row removal outside its schema");
    Scope transaction(runtime_);
    const auto end = product(h.rows, h.columns);
    const auto first = product(index + 1, h.columns);
    auto shrunk = h;
    shrunk.rows = h.rows - 1;
    // Records after row `index` move down one row, then the object shrinks.
    const auto from = sizeof(Header) + first * h.width;
    const auto to = from - h.columns * h.width;
    const auto length = (end - first) * h.width;
    if (whole) {
        std::memmove(image.data() + to, image.data() + from, static_cast<std::size_t>(length));
        std::memcpy(image.data(), &shrunk, sizeof shrunk);
        replace(shrunk, image.data(),
                length ? static_cast<std::size_t>(byte_size(shrunk)) : sizeof(Header));
    } else {
        move_bytes(from, to, length);
        if (!runtime_.store().resize(id_, byte_size(shrunk))) invariant("matrix resize failed");
        set_header(shrunk);
    }
    transaction.commit();
}

void Cells::remove_column(std::uint64_t index) {
    std::array<unsigned char, kTransferBytes> image;
    bool whole = false;
    const auto h = load(image, whole);
    if (index >= h.columns) invariant("matrix column removal outside its schema");
    Scope transaction(runtime_);
    auto shrunk = h;
    shrunk.columns = h.columns - 1;
    const auto w = h.width;
    // First row first: a row's records before `index` move to its new start,
    // those after `index` to the record after them; then the object shrinks.
    if (whole) {
        for (std::uint64_t r = 0; r < h.rows; ++r) {
            const auto from = sizeof(Header) + r * h.columns * w;
            const auto to = sizeof(Header) + r * shrunk.columns * w;
            std::memmove(image.data() + to, image.data() + from,
                         static_cast<std::size_t>(index * w));
            std::memmove(image.data() + to + index * w, image.data() + from + (index + 1) * w,
                         static_cast<std::size_t>((shrunk.columns - index) * w));
        }
        std::memcpy(image.data(), &shrunk, sizeof shrunk);
        replace(shrunk, image.data(), static_cast<std::size_t>(byte_size(shrunk)));
    } else {
        for (std::uint64_t r = 0; r < h.rows; ++r) {
            const auto from = sizeof(Header) + r * h.columns * w;
            const auto to = sizeof(Header) + r * shrunk.columns * w;
            move_bytes(from, to, index * w);
            move_bytes(from + (index + 1) * w, to + index * w, (shrunk.columns - index) * w);
        }
        if (!runtime_.store().resize(id_, byte_size(shrunk))) invariant("matrix resize failed");
        set_header(shrunk);
    }
    transaction.commit();
}

void Cells::swap_rows(std::uint64_t first, std::uint64_t second) {
    std::array<unsigned char, kTransferBytes> image;
    bool whole = false;
    const auto h = load(image, whole);
    if (first >= h.rows || second >= h.rows) invariant("matrix row swap index");
    Scope transaction(runtime_);
    const auto run = h.columns * h.width;
    if (first != second && run) {
        const auto a = sizeof(Header) + std::min(first, second) * run;
        const auto b = sizeof(Header) + std::max(first, second) * run;
        if (whole) {
            std::swap_ranges(image.data() + a, image.data() + a + run, image.data() + b);
            exact_write(a, image.data() + a, static_cast<std::size_t>(b + run - a));
        } else {
            swap_bytes(a, b, run);
        }
    }
    transaction.commit();
}
void Cells::swap_columns(std::uint64_t first, std::uint64_t second) {
    std::array<unsigned char, kTransferBytes> image;
    bool whole = false;
    const auto h = load(image, whole);
    if (first >= h.columns || second >= h.columns) invariant("matrix column swap index");
    Scope transaction(runtime_);
    const auto w = h.width;
    if (first != second && h.rows) {
        if (whole) {
            for (std::uint64_t r = 0; r < h.rows; ++r) {
                unsigned char* row = image.data() + sizeof(Header) + r * h.columns * w;
                std::swap_ranges(row + first * w, row + (first + 1) * w, row + second * w);
            }
            const auto a = sizeof(Header) + std::min(first, second) * w;
            const auto b = sizeof(Header) +
                           ((h.rows - 1) * h.columns + std::max(first, second) + 1) * w;
            exact_write(a, image.data() + a, static_cast<std::size_t>(b - a));
        } else {
            for (std::uint64_t r = 0; r < h.rows; ++r)
                swap_records(h, r * h.columns + first, r * h.columns + second);
        }
    }
    transaction.commit();
}

void Cells::reverse(bool both_axes) {
    std::array<unsigned char, kTransferBytes> image;
    bool whole = false;
    const auto h = load(image, whole);
    Scope transaction(runtime_);
    const auto w = h.width;
    unsigned char* cells = image.data() + sizeof(Header);
    if (both_axes) {
        // Record i and record n - 1 - i trade places.
        const auto n = product(h.rows, h.columns);
        if (n >= 2 && whole) {
            for (std::uint64_t i = 0; i < n / 2; ++i)
                std::swap_ranges(cells + i * w, cells + (i + 1) * w, cells + (n - 1 - i) * w);
            exact_write(sizeof(Header), cells, static_cast<std::size_t>(n * w));
        } else if (n >= 2 && w <= kTransferBytes) {
            // Runs from both ends, each reversed and written to the other end.
            std::array<unsigned char, kTransferBytes> back;
            const auto reverse_run = [w](unsigned char* run, std::uint64_t k) {
                for (std::uint64_t i = 0; i < k / 2; ++i)
                    std::swap_ranges(run + i * w, run + (i + 1) * w, run + (k - 1 - i) * w);
            };
            for (std::uint64_t low = 0, high = n; high - low >= 2;) {
                const auto k = std::min<std::uint64_t>(kTransferBytes / w, (high - low) / 2);
                const auto bytes = static_cast<std::size_t>(k * w);
                exact_read(sizeof(Header) + low * w, image.data(), bytes);
                exact_read(sizeof(Header) + (high - k) * w, back.data(), bytes);
                reverse_run(image.data(), k);
                reverse_run(back.data(), k);
                exact_write(sizeof(Header) + low * w, back.data(), bytes);
                exact_write(sizeof(Header) + (high - k) * w, image.data(), bytes);
                low += k;
                high -= k;
            }
        } else {
            for (std::uint64_t i = 0; i < n / 2; ++i) swap_records(h, i, n - 1 - i);
        }
    } else {
        // Row r and row rows - 1 - r trade places.
        const auto run = h.columns * w;
        if (h.rows >= 2 && run) {
            if (whole) {
                for (std::uint64_t r = 0; r < h.rows / 2; ++r)
                    std::swap_ranges(cells + r * run, cells + (r + 1) * run,
                                     cells + (h.rows - 1 - r) * run);
                exact_write(sizeof(Header), cells, static_cast<std::size_t>(h.rows * run));
            } else {
                for (std::uint64_t r = 0; r < h.rows / 2; ++r)
                    swap_bytes(sizeof(Header) + r * run,
                               sizeof(Header) + (h.rows - 1 - r) * run, run);
            }
        }
    }
    transaction.commit();
}

void Cells::fill(const void* record, std::size_t bytes, std::uint64_t first) {
    const auto h = header();
    const auto count = product(h.rows, h.columns);
    if (first >= count) return;
    if (bytes != h.width) invariant("matrix record write outside its schema");
    const auto w = h.width;
    if (w > kTransferBytes) {
        for (auto cell = first; cell < count; ++cell)
            exact_write(sizeof(Header) + cell * w, record, bytes);
        return;
    }
    std::array<unsigned char, kTransferBytes> buffer;
    const auto span = std::min<std::uint64_t>(buffer.size() / w, count - first);
    for (std::uint64_t i = 0; i < span; ++i)
        std::memcpy(buffer.data() + i * w, record, bytes);
    for (auto cell = first; cell < count;) {
        const auto n = std::min<std::uint64_t>(span, count - cell);
        exact_write(sizeof(Header) + cell * w, buffer.data(), static_cast<std::size_t>(n * w));
        cell += n;
    }
}

std::shared_ptr<Cells> Cells::transpose(bool collapse_empty) const {
    std::array<unsigned char, kTransferBytes> source, target;
    bool whole = false;
    const auto h = load(source, whole);
    Scope transaction(runtime_);
    // The rows x columns cells move; with no rows there are none.
    const auto rows = h.rows, columns = h.rows ? h.columns : 0;
    const auto out_rows = collapse_empty ? columns : h.columns;
    const auto out_columns = collapse_empty ? (out_rows ? h.rows : 0) : h.rows;
    const auto w = h.width;
    std::shared_ptr<Cells> out;
    if (whole) {
        unsigned char* to = target.data() + sizeof(Header);
        const unsigned char* from = source.data() + sizeof(Header);
        for (std::uint64_t r = 0; r < rows; ++r)
            for (std::uint64_t c = 0; c < columns; ++c)
                std::memcpy(to + (c * rows + r) * w, from + (r * columns + c) * w,
                            static_cast<std::size_t>(w));
        out = std::make_shared<Cells>(runtime_, out_rows, out_columns, w, target.data());
    } else {
        out = std::make_shared<Cells>(runtime_, out_rows, out_columns, w);
        if (rows && columns && w <= kTransferBytes) {
            // Tiles of at most one buffer: a rows of b records read (one read
            // when they are whole source rows), written as b runs of a
            // records (one write when they are whole target rows).
            const auto cap = kTransferBytes / w;
            std::uint64_t side = 1;
            while ((side + 1) * (side + 1) <= cap) ++side;
            const auto b = std::min(columns, cap / std::min(rows, side));
            const auto a = std::min(rows, cap / b);
            for (std::uint64_t r0 = 0; r0 < rows; r0 += a)
                for (std::uint64_t c0 = 0; c0 < columns; c0 += b) {
                    const auto ra = std::min(a, rows - r0), cb = std::min(b, columns - c0);
                    if (cb == columns)
                        exact_read(sizeof(Header) + r0 * columns * w, source.data(),
                                   static_cast<std::size_t>(ra * cb * w));
                    else
                        for (std::uint64_t i = 0; i < ra; ++i)
                            exact_read(sizeof(Header) + ((r0 + i) * columns + c0) * w,
                                       source.data() + i * cb * w,
                                       static_cast<std::size_t>(cb * w));
                    for (std::uint64_t j = 0; j < cb; ++j)
                        for (std::uint64_t i = 0; i < ra; ++i)
                            std::memcpy(target.data() + (j * ra + i) * w,
                                        source.data() + (i * cb + j) * w,
                                        static_cast<std::size_t>(w));
                    if (ra == rows)
                        out->exact_write(sizeof(Header) + c0 * rows * w, target.data(),
                                         static_cast<std::size_t>(cb * ra * w));
                    else
                        for (std::uint64_t j = 0; j < cb; ++j)
                            out->exact_write(sizeof(Header) + ((c0 + j) * rows + r0) * w,
                                             target.data() + j * ra * w,
                                             static_cast<std::size_t>(ra * w));
                }
        } else {
            for (std::uint64_t r = 0; r < rows; ++r)
                for (std::uint64_t c = 0; c < columns; ++c)
                    out->copy_bytes(sizeof(Header) + (c * rows + r) * w, *this,
                                    sizeof(Header) + (r * columns + c) * w, w);
        }
    }
    transaction.commit();
    return out;
}

std::shared_ptr<Cells> Cells::block(std::uint64_t row, std::uint64_t column,
                                    std::uint64_t rows, std::uint64_t columns) const {
    std::array<unsigned char, kTransferBytes> source, target;
    bool whole = false;
    const auto h = load(source, whole);
    const bool any = rows && columns;
    if (any && (row > h.rows || rows > h.rows - row || column > h.columns ||
                columns > h.columns - column))
        return nullptr;
    Scope transaction(runtime_);
    const auto w = h.width;
    std::shared_ptr<Cells> out;
    if (layout_bytes(rows, columns, w) <= kTransferBytes) {
        // Each block row is one run of the source row: copied from the
        // loaded object, or read straight into the new object's image.
        if (any)
            for (std::uint64_t r = 0; r < rows; ++r) {
                unsigned char* to = target.data() + sizeof(Header) + r * columns * w;
                const auto from = sizeof(Header) + ((row + r) * h.columns + column) * w;
                if (whole)
                    std::memcpy(to, source.data() + from, static_cast<std::size_t>(columns * w));
                else
                    exact_read(from, to, static_cast<std::size_t>(columns * w));
            }
        out = std::make_shared<Cells>(runtime_, rows, columns, w, target.data());
    } else {
        out = std::make_shared<Cells>(runtime_, rows, columns, w);
        if (column == 0 && columns == h.columns)
            out->copy_bytes(sizeof(Header), *this, sizeof(Header) + row * columns * w,
                            rows * columns * w);
        else
            for (std::uint64_t r = 0; r < rows; ++r)
                out->copy_bytes(sizeof(Header) + r * columns * w, *this,
                                sizeof(Header) + ((row + r) * h.columns + column) * w,
                                columns * w);
    }
    transaction.commit();
    return out;
}

bool Cells::copy_block(const Cells& source, std::uint64_t row, std::uint64_t column,
                       std::uint64_t rows, std::uint64_t columns, std::uint64_t width) {
    std::array<unsigned char, kTransferBytes> loaded, image;
    bool whole = false;
    const auto h = source.load(loaded, whole);
    if (h.width != width || row > h.rows || rows > h.rows - row || column > h.columns ||
        columns > h.columns - column)
        return false;
    Scope transaction(runtime_);
    const auto run = columns * width;
    const auto from = [&](std::uint64_t r) {
        return sizeof(Header) + ((row + r) * h.columns + column) * width;
    };
    if (rows * run <= kTransferBytes) {
        for (std::uint64_t r = 0; r < rows; ++r) {
            if (whole)
                std::memcpy(image.data() + r * run, loaded.data() + from(r),
                            static_cast<std::size_t>(run));
            else
                source.exact_read(from(r), image.data() + r * run, static_cast<std::size_t>(run));
        }
        exact_write(sizeof(Header), image.data(), static_cast<std::size_t>(rows * run));
    } else if (column == 0 && columns == h.columns) {
        copy_bytes(sizeof(Header), source, from(0), rows * run);
    } else {
        for (std::uint64_t r = 0; r < rows; ++r)
            copy_bytes(sizeof(Header) + r * run, source, from(r), run);
    }
    transaction.commit();
    return true;
}

std::shared_ptr<Cells> Cells::concat(const Cells& other, bool horizontal,
                                     bool collapse_empty) const {
    std::array<unsigned char, kTransferBytes> left, right;
    bool left_whole = false, right_whole = false;
    const auto l = load(left, left_whole);
    const auto r = other.load(right, right_whole);
    const auto lc = collapse_empty && !l.rows ? 0 : l.columns;
    const auto rc = collapse_empty && !r.rows ? 0 : r.columns;
    if (l.width != r.width || (horizontal ? l.rows != r.rows : lc != rc))
        invariant("matrix record copy outside its schema");
    Scope transaction(runtime_);
    const auto rows = horizontal ? l.rows : l.rows + r.rows;
    const auto columns = collapse_empty && !rows ? 0 : (horizontal ? lc + rc : lc);
    const auto w = l.width;
    // Horizontal: result row i is left row i, then right row i. Vertical: the
    // left object's cells, then the right's.
    const auto left_run = (horizontal ? lc : l.rows * lc) * w;
    const auto right_run = (horizontal ? rc : r.rows * rc) * w;
    const auto runs = horizontal ? rows : (rows ? 1 : 0);
    std::shared_ptr<Cells> out;
    // Neither operand is larger than the result, so a result that fits one
    // buffer means both were loaded whole.
    if (layout_bytes(rows, columns, w) <= kTransferBytes && left_whole && right_whole) {
        std::array<unsigned char, kTransferBytes> target;
        unsigned char* to = target.data() + sizeof(Header);
        for (std::uint64_t i = 0; i < runs; ++i) {
            std::memcpy(to, left.data() + sizeof(Header) + i * left_run,
                        static_cast<std::size_t>(left_run));
            std::memcpy(to + left_run, right.data() + sizeof(Header) + i * right_run,
                        static_cast<std::size_t>(right_run));
            to += left_run + right_run;
        }
        out = std::make_shared<Cells>(runtime_, rows, columns, w, target.data());
    } else {
        out = std::make_shared<Cells>(runtime_, rows, columns, w);
        for (std::uint64_t i = 0; i < runs; ++i) {
            const auto to = sizeof(Header) + i * (left_run + right_run);
            out->copy_bytes(to, *this, sizeof(Header) + i * left_run, left_run);
            out->copy_bytes(to + left_run, other, sizeof(Header) + i * right_run, right_run);
        }
    }
    transaction.commit();
    return out;
}

std::shared_ptr<Cells> Cells::clone() const {
    const auto h = header();
    Scope transaction(runtime_);
    auto frozen = std::make_shared<Cells>(runtime_, h.rows, h.columns, h.width);
    frozen->restore(*this);
    transaction.commit();
    return frozen;
}

void Cells::restore(const Cells& frozen) {
    if (this == &frozen) return;
    const auto h = frozen.header();
    if (h.width != width()) invariant("matrix restore record width mismatch");
    Scope transaction(runtime_);
    resize(h.rows, h.columns);
    std::array<unsigned char, kTransferBytes> buffer{};
    const auto end = byte_size(h);
    for (std::uint64_t offset = 0; offset < end;) {
        const auto n = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), end - offset));
        frozen.exact_read(offset, buffer.data(), n);
        exact_write(offset, buffer.data(), n);
        offset += n;
    }
    transaction.commit();
}

std::uint64_t Cells::hash_state() const {
    std::uint64_t hash = UINT64_C(1469598103934665603);
    const auto fold = [&hash](unsigned char byte) {
        hash = (hash ^ byte) * UINT64_C(1099511628211);
    };
    for (unsigned int i = 0; i < 8; ++i) fold(static_cast<unsigned char>(id_ >> (8 * i)));
    std::array<unsigned char, kTransferBytes> buffer{};
    const auto end = byte_size(header());
    for (std::uint64_t offset = 0; offset < end;) {
        const auto n = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), end - offset));
        exact_read(offset, buffer.data(), n);
        for (std::size_t i = 0; i < n; ++i) fold(buffer[i]);
        offset += n;
    }
    return hash;
}

} // namespace pineforge::detail::matrix_store
