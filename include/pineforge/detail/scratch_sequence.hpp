#pragma once

#include <pineforge/detail/scratch_file.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <type_traits>

#ifdef PINEFORGE_SCRATCH_SEQUENCE_COUNTERS
#include <cstdlib>
#include <cstring>

#endif
namespace pineforge::detail {

#ifdef PINEFORGE_SCRATCH_SEQUENCE_COUNTERS
// Single-threaded diagnostics only. The serial keeps the last eight switches
// in event order when live and already-destroyed instances share a tag.
struct ScratchSequenceCounters {
    std::uint64_t flush_writes = 0, flush_bytes = 0;
    std::uint64_t loads = 0, load_bytes = 0, page_switches = 0;
    struct Switch {
        std::size_t from = 0, to = 0;
        std::uint64_t serial = 0;
    };
    std::array<Switch, 8> last_switches{};
};

struct ScratchSequenceCounterTotal {
    const char* tag = nullptr;
    ScratchSequenceCounters counters{};
};

inline void add_scratch_sequence_counters(ScratchSequenceCounters& total,
                                          const ScratchSequenceCounters& counters) noexcept {
    total.flush_writes += counters.flush_writes;
    total.flush_bytes += counters.flush_bytes;
    total.loads += counters.loads;
    total.load_bytes += counters.load_bytes;
    total.page_switches += counters.page_switches;
    for (const auto& change : counters.last_switches) {
        if (change.serial == 0) continue;
        auto oldest = std::min_element(total.last_switches.begin(), total.last_switches.end(),
            [](const auto& a, const auto& b) { return a.serial < b.serial; });
        if (change.serial > oldest->serial) *oldest = change;
    }
}

using ScratchSequenceCounterTotals = std::array<ScratchSequenceCounterTotal, 64>;

inline ScratchSequenceCounters& scratch_sequence_tag_total(
        ScratchSequenceCounterTotals& totals, const char* tag) noexcept {
    for (auto& total : totals) {
        if (total.tag && std::strcmp(total.tag, tag) == 0) return total.counters;
    }
    for (auto& total : totals) {
        if (total.tag) continue;
        total.tag = tag;
        return total.counters;
    }
    // Never silently drop an instance or a tag if this diagnostic bound is hit.
    std::abort();
}

struct ScratchSequenceDiagnostic;
struct ScratchSequenceCounterRegistry {
    std::array<ScratchSequenceDiagnostic*, 64> live{};
    ScratchSequenceCounterTotals retired{};
    std::uint64_t switch_serial = 0;
};

inline ScratchSequenceCounterRegistry& scratch_sequence_counter_registry() {
    static ScratchSequenceCounterRegistry registry;
    return registry;
}

struct ScratchSequenceDiagnostic {
    const char* tag = "other";
    ScratchSequenceCounters counters{};

    ScratchSequenceDiagnostic() noexcept {
        for (auto& slot : scratch_sequence_counter_registry().live) {
            if (slot) continue;
            slot = this;
            return;
        }
        std::abort();
    }
    ~ScratchSequenceDiagnostic() {
        auto& registry = scratch_sequence_counter_registry();
        add_scratch_sequence_counters(scratch_sequence_tag_total(registry.retired, tag),
                                      counters);
        for (auto& slot : registry.live)
            if (slot == this) slot = nullptr;
    }
    // Identity and work belong to the object, not its swapped backing file.
    // Keep ScratchSequence's implicit moves usable by stable_sort/std::swap.
    ScratchSequenceDiagnostic(ScratchSequenceDiagnostic&&) noexcept
        : ScratchSequenceDiagnostic() {}
    ScratchSequenceDiagnostic& operator=(ScratchSequenceDiagnostic&&) noexcept {
        return *this;
    }
};

inline ScratchSequenceCounterTotals scratch_sequence_counter_totals() {
    auto& registry = scratch_sequence_counter_registry();
    auto totals = registry.retired;
    for (const auto* instance : registry.live) {
        if (instance)
            add_scratch_sequence_counters(scratch_sequence_tag_total(totals, instance->tag),
                                          instance->counters);
    }
    return totals;
}

#endif
// A numeric sequence over ScratchFile, with one fixed 64 KiB write-back
// window. Prefix retirement changes an offset, never moves rows or builds
// an in-memory directory. These are exact bytes, not a hash summary.
template<class T>
class ScratchSequence {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(sizeof(T) <= 64 * 1024);
    static constexpr std::size_t kValues = 64 * 1024 / sizeof(T);
public:
    ScratchSequence() {
        file_.bound_cache();
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
        file_.diagnostic_tag("other");
#endif
    }
#ifdef PINEFORGE_SCRATCH_SEQUENCE_COUNTERS
    // Tags must outlive the diagnostic registry (the call sites use literals).
    void diagnostic_tag(const char* tag) noexcept {
        diagnostic_.tag = tag ? tag : "other";
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
        file_.diagnostic_tag(diagnostic_.tag);
#endif
    }
#endif
    class const_iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using reference = T;
        using pointer = void;
        const_iterator() = default;
        T operator*() const { return owner_->at(index_); }
        const_iterator& operator++() { ++index_; return *this; }
        const_iterator operator++(int) { auto old = *this; ++*this; return old; }
        const_iterator operator+(std::size_t n) const { return {owner_, index_ + n}; }
        bool operator==(const const_iterator& o) const {
            return owner_ == o.owner_ && index_ == o.index_;
        }
        bool operator!=(const const_iterator& o) const { return !(*this == o); }
    private:
        friend class ScratchSequence;
        const_iterator(const ScratchSequence* owner, std::size_t index)
            : owner_(owner), index_(index) {}
        const ScratchSequence* owner_ = nullptr;
        std::size_t index_ = 0;
    };

    void set_directory(const std::string& directory) {
        file_.set_directory(directory);
        directory_ = directory;
    }
    std::size_t size() const noexcept { return end_ - base_; }
    bool empty() const noexcept { return size() == 0; }
    static constexpr std::size_t resident_capacity() noexcept { return kValues; }
    void clear() noexcept {
        file_.clear();
        base_ = end_ = page_ = loaded_ = 0;
        dirty_ = false;
    }
    T at(std::size_t index) const {
        if (index >= size()) throw std::out_of_range("scratch sequence index");
        load(base_ + index);
        return window_[(base_ + index) % kValues];
    }
    T operator[](std::size_t index) const { return at(index); }
    void set(std::size_t index, T value) {
        if (index >= size()) throw std::out_of_range("scratch sequence index");
        load(base_ + index);
        window_[(base_ + index) % kValues] = value;
        dirty_ = true;
    }
    void push_back(T value) {
        if (end_ == std::numeric_limits<std::size_t>::max())
            throw ScratchIoError(EFBIG, "scratch sequence extent");
        ScratchFile::check_extent(byte_offset(end_), sizeof(T));
        load(end_);
        window_[end_ % kValues] = value;
        loaded_ = std::max(loaded_, end_ % kValues + 1);
        dirty_ = true;
        ++end_;
    }
    void resize(std::size_t count, T value = T{}) {
        while (size() < count) push_back(value);
        if (count == size()) return;
        flush();
        end_ = base_ + count;
        loaded_ = 0;
    }
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, size()}; }
    void erase(const_iterator first, const_iterator last) {
        if (first.owner_ != this || last.owner_ != this
            || first.index_ != 0 || last.index_ > size())
            throw std::logic_error("scratch sequence erase is not a prefix");
        base_ += last.index_;
    }
    template<class Compare>
    void stable_sort(Compare before) {
        ScratchSequence scratch;
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
        scratch.file_.diagnostic_tag("ScratchSequence::stable_sort");
#endif
        scratch.set_directory(directory_);
        for (std::size_t width = 1; width < size();) {
            scratch.clear();
            PageReader left_page(*this), right_page(*this);
            for (std::size_t start = 0; start < size();) {
                const auto middle = start + std::min(width, size() - start);
                const auto stop = middle + std::min(width, size() - middle);
                auto left = start, right = middle;
                while (left < middle || right < stop) {
                    if (right == stop || (left < middle
                        && !before(right_page.at(right), left_page.at(left))))
                        scratch.push_back(left_page.at(left++));
                    else
                        scratch.push_back(right_page.at(right++));
                }
                start = stop;
            }
            using std::swap;
            swap(*this, scratch);
            if (width > size() / 2) break;
            width *= 2;
        }
    }
private:
    class PageReader {
    public:
        explicit PageReader(const ScratchSequence& source) : source_(source) {}
        T at(std::size_t index) {
            if (index < first_ || index - first_ >= count_) {
                source_.flush();
                const auto first = index - index % kValues;
                const auto count = std::min(kValues, source_.size() - first);
                count_ = 0;
                source_.file_.read(byte_offset(source_.base_ + first),
                                   values_.data(), count * sizeof(T));
#ifdef PINEFORGE_SCRATCH_SEQUENCE_COUNTERS
                if (count != 0) {
                    ++source_.diagnostic_.counters.loads;
                    source_.diagnostic_.counters.load_bytes += count * sizeof(T);
                }
#endif
                first_ = first;
                count_ = count;
            }
            return values_[index - first_];
        }
    private:
        const ScratchSequence& source_;
        std::array<T, kValues> values_{};
        std::size_t first_ = 0, count_ = 0;
    };
    static std::uint64_t byte_offset(std::size_t index) {
        if (index > std::numeric_limits<std::uint64_t>::max() / sizeof(T))
            throw ScratchIoError(EFBIG, "scratch sequence offset");
        return static_cast<std::uint64_t>(index) * sizeof(T);
    }
    void flush() const {
        if (!dirty_) return;
        file_.open("pineforge-sequence");
        file_.write(byte_offset(page_), window_.data(), loaded_ * sizeof(T));
#ifdef PINEFORGE_SCRATCH_SEQUENCE_COUNTERS
        if (loaded_ != 0) {
            ++diagnostic_.counters.flush_writes;
            diagnostic_.counters.flush_bytes += loaded_ * sizeof(T);
        }
#endif
        dirty_ = false;
    }
    void load(std::size_t global) const {
        const std::size_t next = global - global % kValues;
        if (loaded_ != 0 && page_ == next) return;
        flush();
        const auto count = next < end_ ? std::min(kValues, end_ - next) : 0;
        loaded_ = 0;
        if (count != 0)
            file_.read(byte_offset(next), window_.data(), count * sizeof(T));
#ifdef PINEFORGE_SCRATCH_SEQUENCE_COUNTERS
        auto& counters = diagnostic_.counters;
        if (count != 0) {
            ++counters.loads;
            counters.load_bytes += count * sizeof(T);
        }
        if (page_ != next) {
            counters.last_switches[counters.page_switches % counters.last_switches.size()] =
                {page_, next, ++scratch_sequence_counter_registry().switch_serial};
            ++counters.page_switches;
        }
#endif
        page_ = next;
        loaded_ = count;
    }
    mutable ScratchFile file_;
    std::string directory_;
    mutable std::array<T, kValues> window_{};
    std::size_t base_ = 0, end_ = 0;
    mutable std::size_t page_ = 0, loaded_ = 0;
    mutable bool dirty_ = false;
#ifdef PINEFORGE_SCRATCH_SEQUENCE_COUNTERS
    mutable ScratchSequenceDiagnostic diagnostic_;
#endif
};
} // namespace pineforge::detail
