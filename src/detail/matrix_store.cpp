#include <pineforge/detail/matrix_store.hpp>
#include <pineforge/detail/script_store.hpp>
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace pineforge::detail::matrix_store {
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
std::uint64_t Cells::rows() const { return header().rows; }
std::uint64_t Cells::columns() const { return header().columns; }
std::uint64_t Cells::width() const { return header().width; }
std::uint64_t Cells::count() const {
    const auto h = header();
    return product(h.rows, h.columns);
}

std::uint64_t Cells::index(std::uint64_t row, std::uint64_t column) const {
    const auto h = header();
    if (row >= h.rows || column >= h.columns)
        invariant("matrix coordinate outside its schema");
    return product(row, h.columns) + column;
}

void Cells::read(std::uint64_t cell, void* record, std::size_t bytes) const {
    const auto h = header();
    if (bytes != h.width || cell >= product(h.rows, h.columns))
        invariant("matrix record read outside its schema");
    exact_read(sizeof(Header) + cell * h.width, record, bytes);
}
void Cells::write(std::uint64_t cell, const void* record, std::size_t bytes) {
    const auto h = header();
    if (bytes != h.width || cell >= product(h.rows, h.columns))
        invariant("matrix record write outside its schema");
    exact_write(sizeof(Header) + cell * h.width, record, bytes);
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
    std::array<unsigned char, kTransferBytes> left{}, right{};
    for (std::uint64_t offset = 0; offset < h.width;) {
        const auto n = static_cast<std::size_t>(
            std::min<std::uint64_t>(left.size(), h.width - offset));
        const auto a = sizeof(Header) + first * h.width + offset;
        const auto b = sizeof(Header) + second * h.width + offset;
        exact_read(a, left.data(), n);
        exact_read(b, right.data(), n);
        exact_write(a, right.data(), n);
        exact_write(b, left.data(), n);
        offset += n;
    }
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

void Cells::insert_row(std::uint64_t index, std::uint64_t columns) {
    const auto h = header();
    if (index > h.rows || (h.rows && columns != h.columns))
        invariant("matrix row insertion outside its schema");
    Scope transaction(runtime_);
    const auto old_end = product(h.rows, columns);
    resize(h.rows + 1, columns);
    const auto begin = product(index, columns);
    for (auto i = old_end; i > begin; --i)
        copy_cell(i + columns - 1, *this, i - 1);
    transaction.commit();
}

void Cells::insert_column(std::uint64_t index) {
    const auto h = header();
    if (index > h.columns) invariant("matrix column insertion outside its schema");
    Scope transaction(runtime_);
    resize(h.rows, h.columns + 1);
    for (auto r = h.rows; r > 0; --r)
        for (auto c = h.columns; c > 0; --c) {
            const auto column = c - 1;
            copy_cell((r - 1) * (h.columns + 1) + column + (column >= index),
                      *this, (r - 1) * h.columns + column);
        }
    transaction.commit();
}

void Cells::remove_row(std::uint64_t index) {
    const auto h = header();
    if (index >= h.rows) invariant("matrix row removal outside its schema");
    Scope transaction(runtime_);
    const auto end = product(h.rows, h.columns);
    for (auto i = product(index + 1, h.columns); i < end; ++i)
        copy_cell(i - h.columns, *this, i);
    resize(h.rows - 1, h.columns);
    transaction.commit();
}

void Cells::remove_column(std::uint64_t index) {
    const auto h = header();
    if (index >= h.columns) invariant("matrix column removal outside its schema");
    Scope transaction(runtime_);
    for (std::uint64_t r = 0; r < h.rows; ++r)
        for (std::uint64_t c = 0; c < h.columns; ++c)
            if (c != index)
                copy_cell(r * (h.columns - 1) + c - (c > index),
                          *this, r * h.columns + c);
    resize(h.rows, h.columns - 1);
    transaction.commit();
}

void Cells::swap_rows(std::uint64_t first, std::uint64_t second) {
    const auto h = header();
    if (first >= h.rows || second >= h.rows) invariant("matrix row swap index");
    Scope transaction(runtime_);
    for (std::uint64_t c = 0; c < h.columns; ++c)
        swap_cells(first * h.columns + c, second * h.columns + c);
    transaction.commit();
}
void Cells::swap_columns(std::uint64_t first, std::uint64_t second) {
    const auto h = header();
    if (first >= h.columns || second >= h.columns) invariant("matrix column swap index");
    Scope transaction(runtime_);
    for (std::uint64_t r = 0; r < h.rows; ++r)
        swap_cells(r * h.columns + first, r * h.columns + second);
    transaction.commit();
}

void Cells::reverse(bool both_axes) {
    const auto h = header();
    Scope transaction(runtime_);
    if (both_axes) {
        const auto n = product(h.rows, h.columns);
        for (std::uint64_t i = 0; i < n / 2; ++i) swap_cells(i, n - 1 - i);
    } else {
        for (std::uint64_t r = 0; r < h.rows / 2; ++r)
            for (std::uint64_t c = 0; c < h.columns; ++c)
                swap_cells(r * h.columns + c, (h.rows - 1 - r) * h.columns + c);
    }
    transaction.commit();
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
