#pragma once

// The pure derivation of a calculation schedule: from the driving events of a
// trace, the cadence a binding declared and the kernel's own bar timeline, which
// bar lifecycle every calculation belongs to, where on the event-time axis it
// sits and whether it confirms its bar. Internal to the kernel; the plain data
// it speaks in is include/pineforge/native_calc_point.hpp.
//
// One function, plan_schedule, serves the count query and the apply that
// commits: it reads a State and writes none, and a caller that commits keeps the
// State it returns. Nothing here reads a clock, a calendar or a consumer: the
// timeline arrives as a BarClock, which the consumer builds over the calendar
// and the script timeframe its own sealing path reads, so the end of a bar is
// derived by the kernel and never stated by a caller.

#include <pineforge/native_calc_point.hpp>
#include <pineforge/detail/id_pair_store.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace pineforge::detail {

// Internal run workspaces shared by the schedule and execution consumer.
// The index is external too: neither row count nor replacement history grows
// a RAM directory. Reads return values; each iterator owns its current row,
// so nested reads/evictions do not invalidate a row a callback is reading.
// Strings belong to ONE current row, never to a cached population of rows.
class RunRowWriter;
class RunRowReader;
template<class T> struct RunRowCodec {
    static void write(RunRowWriter& out, const T& row);
    static void read(RunRowReader& in, T& row);
};

class RunRowWriter {
public:
    // `counting` measures a record without writing it: the same codec walk
    // advances the offset and no byte reaches the file. A caller that must
    // decide between rewriting a record in place and appending it (set())
    // learns its size first, so a grown record can never overwrite the row
    // that follows it.
    RunRowWriter(IdPairScratchFile& file, std::uint64_t offset, bool counting = false)
        : file_(file), offset_(offset), counting_(counting) {}
    void bytes(const void* data, std::size_t size) {
        if (!counting_) file_.write(offset_, data, size);
        offset_ += size;
    }
    template<class... T> void fields(const T&... fields) {
        (RunRowCodec<T>::write(*this, fields), ...);
    }
    std::uint64_t offset() const noexcept { return offset_; }
private:
    IdPairScratchFile& file_;
    std::uint64_t offset_;
    bool counting_;
};
class RunRowReader {
public:
    RunRowReader(const IdPairScratchFile& file, std::uint64_t offset) : file_(file), offset_(offset) {}
    void bytes(void* data, std::size_t size) {
        file_.read(offset_, data, size);
        offset_ += size;
    }
    template<class... T> void fields(T&... fields) { (RunRowCodec<T>::read(*this, fields), ...); }
private:
    const IdPairScratchFile& file_;
    std::uint64_t offset_;
};
template<class T> void RunRowCodec<T>::write(RunRowWriter& out, const T& row) {
    static_assert(std::is_trivially_copyable_v<T>, "nontrivial external row needs a codec");
    out.bytes(&row, sizeof(row));
}
template<class T> void RunRowCodec<T>::read(RunRowReader& in, T& row) {
    static_assert(std::is_trivially_copyable_v<T>, "nontrivial external row needs a codec");
    in.bytes(&row, sizeof(row));
}
template<> struct RunRowCodec<std::string> {
    static void write(RunRowWriter& out, const std::string& row) {
        const auto size = static_cast<std::uint64_t>(row.size());
        out.fields(size); out.bytes(row.data(), row.size());
    }
    static void read(RunRowReader& in, std::string& row) {
        std::uint64_t size = 0;
        in.fields(size); row.resize(static_cast<std::size_t>(size));
        in.bytes(row.data(), row.size());
    }
};
template<class A, class B> struct RunRowCodec<std::pair<A, B>> {
    static void write(RunRowWriter& out, const std::pair<A, B>& row) {
        out.fields(row.first, row.second);
    }
    static void read(RunRowReader& in, std::pair<A, B>& row) { in.fields(row.first, row.second); }
};

template<class T, class Codec = RunRowCodec<T>> class PagedRunRows {
    // One directory entry: where the row starts and how many bytes it
    // occupies. The length lets `set` rewrite a same-sized row in place, so a
    // replace-heavy reader (an ordering selection swap) cannot grow the file.
    struct Slot {
        std::uint64_t offset = 0;
        std::uint64_t bytes = 0;
    };
public:
    using value_type = T;
    template<bool Reverse> class iterator {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;
        iterator() = default;
        reference operator*() const {
            if (!row_) row_.emplace(owner_->at(Reverse ? limit_ - index_ - 1 : index_));
            return *row_;
        }
        pointer operator->() const { return &**this; }
        iterator& operator++() { ++index_; row_.reset(); return *this; }
        iterator operator++(int) { auto old = *this; ++*this; return old; }
        iterator& operator--() { --index_; row_.reset(); return *this; }
        iterator operator--(int) { auto old = *this; --*this; return old; }
        bool operator==(const iterator& other) const {
            return owner_ == other.owner_ && index_ == other.index_;
        }
        bool operator!=(const iterator& other) const { return !(*this == other); }
    private:
        friend class PagedRunRows;
        iterator(const PagedRunRows* owner, std::size_t index)
            : owner_(owner), index_(index), limit_(owner->size()) {}
        const PagedRunRows* owner_ = nullptr;
        std::size_t index_ = 0;
        std::size_t limit_ = 0;
        mutable std::optional<T> row_;
    };
    using const_iterator = iterator<false>;
    // A reverse adapter that dereferences a temporary would return a reference
    // into a destroyed iterator. Reverse reads need their own current row too.
    using const_reverse_iterator = iterator<true>;

    PagedRunRows() = default;
    PagedRunRows(const PagedRunRows& other) {
        if (other.memory_) memory_ = std::make_unique<std::vector<T>>();
        for (const auto& row : other) push_back(row);
    }
    PagedRunRows& operator=(const PagedRunRows& other) {
        if (this != &other) { PagedRunRows copy(other); *this = std::move(copy); }
        return *this;
    }
    PagedRunRows(PagedRunRows&& other) noexcept { swap(other); }
    PagedRunRows& operator=(PagedRunRows&& other) noexcept {
        if (this != &other) { clear(); swap(other); }
        return *this;
    }
    void swap(PagedRunRows& other) noexcept {
        using std::swap;
        swap(data_, other.data_); swap(index_, other.index_);
        swap(size_, other.size_); swap(bytes_, other.bytes_); swap(memory_, other.memory_);
    }
    void prepare() { if (!memory_) { data_.open({}); index_.open({}); } }
    void clear() noexcept {
        size_ = 0; bytes_ = 0; // Reuse both files, overwriting from offset zero.
        if (memory_) memory_->clear();
    }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    // No population-dependent reservation. Kept for reader/source migration.
    void reserve(std::size_t) { prepare(); }
    std::size_t capacity() const noexcept { return memory_ ? memory_->capacity() : 0; }
    std::size_t max_size() const noexcept { return std::numeric_limits<std::size_t>::max(); }
    std::size_t resident_bytes() const noexcept {
        return sizeof(*this) + (memory_ ? memory_->capacity() * sizeof(T) : 0);
    }
    std::uint64_t external_bytes() const noexcept { return bytes_ + size_ * sizeof(Slot); }
    T at(std::size_t index) const {
        if (index >= size_) throw std::out_of_range("run row index");
        if (memory_) return memory_->at(index);
        Slot slot;
        index_.read(index * sizeof(Slot), &slot, sizeof(slot));
        RunRowReader reader(data_, slot.offset);
        T row{}; Codec::read(reader, row); return row;
    }
    T operator[](std::size_t index) const { return at(index); }
    T front() const { return at(0); }
    T back() const { return at(size_ - 1); }
    void push_back(const T& row) {
        if (memory_) { memory_->push_back(row); ++size_; return; }
        prepare();
        Slot slot;
        slot.offset = bytes_;
        RunRowWriter writer(data_, bytes_);
        Codec::write(writer, row);
        slot.bytes = writer.offset() - slot.offset;
        index_.write(size_ * sizeof(Slot), &slot, sizeof(slot));
        bytes_ = writer.offset(); ++size_;
    }
    void set(std::size_t index, const T& row) {
        if (index >= size_) throw std::out_of_range("run row index");
        if (memory_) { (*memory_)[index] = row; return; }
        Slot slot;
        index_.read(index * sizeof(Slot), &slot, sizeof(slot));
        // Measure first: a record that outgrows its slot must go to the file's
        // end, because writing it in place would run over the next row. A
        // record that fits keeps its address (fixed-width rows always do), so
        // a replace-heavy reader such as an ordering selection swap cannot
        // grow the file.
        RunRowWriter measure(data_, 0, /*counting=*/true);
        Codec::write(measure, row);
        if (measure.offset() <= slot.bytes) {
            RunRowWriter in_place(data_, slot.offset);
            Codec::write(in_place, row);
            return;
        }
        Slot appended;
        appended.offset = bytes_;
        RunRowWriter at_end(data_, bytes_);
        Codec::write(at_end, row);
        appended.bytes = at_end.offset() - appended.offset;
        index_.write(index * sizeof(Slot), &appended, sizeof(appended));
        bytes_ = at_end.offset();
    }
    template<class Predicate> void erase_if(Predicate&& remove) {
        std::size_t kept = 0;
        for (std::size_t i = 0; i < size_; ++i) {
            const auto row = at(i);
            if (remove(row)) continue;
            if (memory_) { if (kept != i) (*memory_)[kept] = row; }
            else if (kept != i) {
                Slot slot; index_.read(i * sizeof(Slot), &slot, sizeof(slot));
                index_.write(kept * sizeof(Slot), &slot, sizeof(slot));
            }
            ++kept;
        }
        size_ = kept;
        if (memory_) memory_->resize(kept);
    }
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, size_}; }
    const_reverse_iterator rbegin() const { return {this, 0}; }
    const_reverse_iterator rend() const { return {this, size_}; }
    // Explicit test oracle only. Production has no flag/environment fallback.
    void fixture_use_memory_reference() {
        if (!empty()) throw std::logic_error("run rows oracle changed after use");
        data_.reset(); index_.reset(); memory_ = std::make_unique<std::vector<T>>();
    }
private:
    IdPairScratchFile data_, index_;
    std::size_t size_ = 0;
    std::uint64_t bytes_ = 0;
    std::unique_ptr<std::vector<T>> memory_;
};

} // namespace pineforge::detail

namespace pineforge::native_calc {

/// One bar interval of the timebase.
struct Slot {
    /// Exclusive end: the nominal open of the next interval, the value the
    /// consumer's sealing path compares time against.
    std::int64_t end_ms = 0;
    /// Where the lookup of the interval after this one starts, skipping closed
    /// time. Must lie after the instant that was looked up for the walk to move.
    std::int64_t next_open_ms = 0;
    /// Whether a quiet interval here yields a carry-forward bar.
    bool tradable = false;
    /// False for a timebase without time boundaries (a raw-label partition,
    /// whose bars close on data alone): the other three fields then mean nothing.
    bool bounded = true;
};

/// The slot that holds an instant, or false when the timebase has none there.
using BarClock = std::function<bool(std::int64_t at_ms, Slot& slot)>;

/// Everything a schedule carries from one schedule to the next. The fields are
/// listed once, in tie(); the digest and every comparison read that list.
struct State {
    /// Trace ordinal of the next driving event (one-based).
    std::uint64_t trace_first = 1;
    /// Whether an event has been planned, and the time of the last one: the
    /// lower bound of a Pre position's window.
    bool has_prev_event = false;
    std::int64_t prev_event_ms = 0;
    /// Whether a bar lifecycle was ever opened, and the last ordinal assigned.
    bool has_last_key = false;
    std::uint64_t last_bar_key = 0;
    /// Whether the lifecycle of last_bar_key is still open, the exclusive end the
    /// kernel derived for it when it opened (zero while none is open, and zero
    /// for a bar of a timebase without time boundaries, which has none) and how
    /// many calculations it has had.
    bool bar_open = false;
    std::int64_t open_end_ms = 0;
    std::uint64_t calc_in_bar = 0;
    /// Where the next quiet interval is looked up: the interval after the last
    /// bar lifecycle's. Meaningful once has_last_key is set.
    std::int64_t cursor_ms = 0;

    auto tie() const noexcept {
        return std::tie(trace_first, has_prev_event, prev_event_ms, has_last_key,
                        last_bar_key, bar_open, open_end_ms, calc_in_bar, cursor_ms);
    }
};

/// The outcome of one derivation. `after` and `points` mean something only when
/// `status` is Ok; a refusal returns the State it was given and no points.
struct Plan {
    Status status = Status::Ok;
    /// Calculations planned; counted even when the points are not collected.
    std::uint64_t count = 0;
    State after;
    detail::PagedRunRows<Point> points;
};

/// Derives the calculations `events` make an instance with `cadence` perform,
/// starting from `before`, over the timeline `clock` answers for. Collects the
/// points only when asked; the count is exact either way.
Plan plan_schedule(const State& before, std::uint32_t cadence, const Event* events,
                   std::size_t count, const BarClock& clock, bool want_points);

/// A word that changes whenever any field of the State does.
std::uint64_t digest(const State& state) noexcept;

/// The continuation identity of a consumer whose schedule group is engaged: the
/// consumer's own identity folded with the group's digest.
std::uint64_t combine(std::uint64_t legacy, std::uint64_t group) noexcept;

}  // namespace pineforge::native_calc
