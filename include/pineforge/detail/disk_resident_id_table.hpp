#pragma once

#include <pineforge/detail/disk_id_table.hpp>
#include <pineforge/detail/scratch_defaults.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace pineforge::detail {

// An id index plus the rows that still own live data. Value supplies a fixed
// Record, retired_record(), restore_record(Record), and spillable(). Retired
// scalar history lives on disk; live rosters stay resident. Iterators pin a
// row across nested lookups. No destructor does I/O: eviction writes before
// erasing, at the next lookup or explicit spill(), where failures can throw.
//
// begin()/end() visit RESIDENT rows, newest first. A spilled row
// has no live members, so source decisions over live members can omit it.
// resident_rows() visits them in ascending original offset order, including
// rows restored from disk. Empty rows can await an implicit spill: readers
// must use live members, never physical residency, as a decision predicate.
// all_sorted() visits EVERY logical row, including retired ones, for hashing.
// size() is the logical count, resident_size() the physical count.
// Values with an opening/live-unit view additionally supply held(). Their
// writers call refresh_held() after changing that predicate; admission and
// restore register it automatically. held_rows() walks exactly that subset.
template<class Value>
class DiskResidentIdTable {
    using Index = DiskIdTable<typename Value::Record>;
    using Offset = typename Index::Offset;
    using Row = std::pair<const std::string, Value>;
    struct ResidentProbe {
        std::string_view key;
        std::uint64_t* comparisons;
    };
    struct ResidentKeyLess {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const noexcept { return a < b; }
        bool operator()(const ResidentProbe& a, std::string_view b) const noexcept {
            ++*a.comparisons;
            return a.key < b;
        }
        bool operator()(std::string_view a, const ResidentProbe& b) const noexcept {
            ++*b.comparisons;
            return a < b.key;
        }
    };
    // Borrowed stable keys, worst-case O(log R), no arena reads. The probe
    // overloads count actual comparisons, not a hard-coded cost per call.
    using ResidentKeys = std::map<std::string_view, Offset, ResidentKeyLess>;
    struct ResidentRow {
        std::shared_ptr<Row> owner;
        typename ResidentKeys::iterator key;
    };
    using Residents = std::map<Offset, ResidentRow>;
    // Derived ordered set of offsets, with a row pin for O(1) dereference.
    // Only rows whose Value::held() predicate is true belong here. Pending
    // origins alone do not qualify. One map node per held cohort, O(H) space.
    using Held = std::map<Offset, std::shared_ptr<Row>>;

public:
    // Physical work only; not logical broker state and never hashed.
    // A6 counters are single-threaded per run; mutable members are written
    // from const methods because reads may restore, spill or instrument rows.
    struct WorkCounters {
        std::uint64_t rows_visited_by_spill = 0;
        std::uint64_t find_resident_probes = 0;
        std::uint64_t resident_rows_visited = 0;
        std::uint64_t held_rows_visited = 0;
        std::uint64_t iterated_rows = 0;
        std::uint64_t rows_visited_by_capacity = 0;
        std::uint64_t rows_spilled = 0, rows_restored = 0;
    };
    const WorkCounters& work_counters() const noexcept { return work_; }
    static constexpr std::size_t resident_row_bytes() noexcept { return sizeof(Row); }
    std::size_t resident_directory_heap_estimate() const noexcept {
        // Portable estimate, not allocator accounting: two map nodes'
        // links/color and the shared-row control block.
        return resident_.size() * (sizeof(typename Residents::value_type) + 4 * sizeof(void*)
            + sizeof(typename ResidentKeys::value_type) + 4 * sizeof(void*) + 2 * sizeof(void*))
            + held_.size() * (sizeof(typename Held::value_type) + 4 * sizeof(void*));
    }

    template<bool Const, bool Sorted = false, bool Insertion = false>
    class Iterator {
        friend class DiskResidentIdTable;
        using Owner = std::conditional_t<Const, const DiskResidentIdTable, DiskResidentIdTable>;
        Owner* owner_ = nullptr;
        Offset at_ = 0;
        std::shared_ptr<Row> row_;

        Iterator(Owner* owner, Offset at, std::shared_ptr<Row> row) noexcept
            : owner_(owner), at_(at), row_(std::move(row)) {}
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Row;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<Const, const Row&, Row&>;
        using pointer = std::conditional_t<Const, const Row*, Row*>;
        Iterator() = default;
        reference operator*() const noexcept {
            ++owner_->work_.iterated_rows;
            return *row_;
        }
        pointer operator->() const noexcept {
            ++owner_->work_.iterated_rows;
            return row_.get();
        }
        Iterator& operator++() {
            ++owner_->work_.iterated_rows;
            if constexpr (Sorted) {
                const auto next = owner_->index_.next_sorted(at_);
                row_.reset();
                owner_->spill_implicitly();
                at_ = next;
                if (at_ != 0) row_ = owner_->load(at_);
            } else if constexpr (Insertion) {
                // Nested lookups can erase other empty rows. Re-seek by
                // offset rather than keeping an invalidatable map iterator.
                const auto next = owner_->resident_.upper_bound(at_);
                at_ = next == owner_->resident_.end() ? 0 : next->first;
                row_ = at_ == 0 ? nullptr : next->second.owner;
            } else {
                auto next = owner_->resident_.lower_bound(at_);
                if (next == owner_->resident_.begin()) {
                    at_ = 0;
                    row_.reset();
                } else {
                    --next;
                    at_ = next->first;
                    row_ = next->second.owner;
                }
            }
            return *this;
        }
        Iterator operator++(int) { auto old = *this; ++*this; return old; }
        bool operator==(const Iterator& other) const noexcept {
            return owner_ == other.owner_ && at_ == other.at_;
        }
        bool operator!=(const Iterator& other) const noexcept { return !(*this == other); }
    };
    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;
    using sorted_iterator = Iterator<false, true>;
    using const_sorted_iterator = Iterator<true, true>;
    using insertion_iterator = Iterator<true, false, true>;

    // Pin the CURRENT row while walking map nodes directly. A nested lookup
    // may spill other rows, but cannot erase this node before ++ advances.
    // This keeps an ascending walk O(R), rather than re-seeking R times.
    // As with ordinary container ranges, clear/move during a walk is invalid.
    template<bool Const> class ResidentIterator {
        friend class DiskResidentIdTable;
        using Owner = std::conditional_t<Const, const DiskResidentIdTable, DiskResidentIdTable>;
        Owner* owner_ = nullptr;
        typename Residents::const_iterator at_{};
        std::shared_ptr<Row> row_;
        ResidentIterator(Owner* owner, typename Residents::const_iterator at) noexcept
            : owner_(owner), at_(at),
              row_(at == owner->resident_.end() ? nullptr : at->second.owner) {}
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Row;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<Const, const Row&, Row&>;
        using pointer = std::conditional_t<Const, const Row*, Row*>;
        ResidentIterator() = default;
        reference operator*() const noexcept {
            ++owner_->work_.resident_rows_visited;
            return *row_;
        }
        pointer operator->() const noexcept {
            ++owner_->work_.resident_rows_visited;
            return row_.get();
        }
        ResidentIterator& operator++() noexcept {
            ++at_; // The current node is still pinned.
            row_ = at_ == owner_->resident_.end() ? nullptr : at_->second.owner;
            return *this;
        }
        ResidentIterator operator++(int) noexcept { auto old = *this; ++*this; return old; }
        bool operator==(const ResidentIterator& other) const noexcept {
            return owner_ == other.owner_ && (!owner_ || at_ == other.at_);
        }
        bool operator!=(const ResidentIterator& other) const noexcept { return !(*this == other); }
    };

    // Ascending offsets, like resident_rows(), but only openings/live units.
    // Membership changes must use refresh_held(). When removing the current
    // member during a walk, advance the iterator before changing that row.
    template<bool Const> class HeldIterator {
        friend class DiskResidentIdTable;
        using Owner = std::conditional_t<Const, const DiskResidentIdTable, DiskResidentIdTable>;
        Owner* owner_ = nullptr;
        typename Held::const_iterator at_{};
        Offset offset_ = 0;
        std::shared_ptr<Row> row_;
        HeldIterator(Owner* owner, typename Held::const_iterator at) noexcept
            : owner_(owner), at_(at),
              offset_(at == owner->held_.end() ? 0 : at->first),
              row_(at == owner->held_.end() ? nullptr : at->second) {}
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Row;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<Const, const Row&, Row&>;
        using pointer = std::conditional_t<Const, const Row*, Row*>;
        HeldIterator() = default;
        reference operator*() const noexcept {
            ++owner_->work_.held_rows_visited;
            return *row_;
        }
        pointer operator->() const noexcept {
            ++owner_->work_.held_rows_visited;
            return row_.get();
        }
        HeldIterator& operator++() noexcept {
            ++at_;
            offset_ = at_ == owner_->held_.end() ? 0 : at_->first;
            row_ = at_ == owner_->held_.end() ? nullptr : at_->second;
            return *this;
        }
        HeldIterator operator++(int) noexcept { auto old = *this; ++*this; return old; }
        bool operator==(const HeldIterator& other) const noexcept {
            return owner_ == other.owner_ && (!owner_ || at_ == other.at_);
        }
        bool operator!=(const HeldIterator& other) const noexcept { return !(*this == other); }
    };

    template<class Iter> struct Range {
        Iter first, last;
        Iter begin() const { return first; }
        Iter end() const { return last; }
    };
    template<class Iter> class KeyIterator {
        Iter at_;
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::string;
        using difference_type = std::ptrdiff_t;
        using reference = const std::string&;
        using pointer = const std::string*;
        KeyIterator() = default;
        explicit KeyIterator(Iter at) : at_(std::move(at)) {}
        reference operator*() const noexcept { return at_->first; }
        pointer operator->() const noexcept { return &at_->first; }
        KeyIterator& operator++() { ++at_; return *this; }
        KeyIterator operator++(int) { auto old = *this; ++*this; return old; }
        bool operator==(const KeyIterator& other) const noexcept { return at_ == other.at_; }
        bool operator!=(const KeyIterator& other) const noexcept { return !(*this == other); }
    };

    DiskResidentIdTable() = default;
    DiskResidentIdTable(const DiskResidentIdTable&) = delete;
    DiskResidentIdTable& operator=(const DiskResidentIdTable&) = delete;
    DiskResidentIdTable(DiskResidentIdTable&& other) noexcept { *this = std::move(other); }
    DiskResidentIdTable& operator=(DiskResidentIdTable&& other) noexcept {
        if (this != &other) {
            clear(); // Drop borrowed keys before dropping their row strings.
            index_ = std::move(other.index_);
            resident_ = std::move(other.resident_);
            resident_by_key_ = std::move(other.resident_by_key_);
            held_ = std::move(other.held_);
            work_ = std::exchange(other.work_, {});
            spill_threshold_ = std::exchange(other.spill_threshold_, kSpillFloor);
            spill_interval_ = std::exchange(other.spill_interval_, kSpillFloor);
            calls_since_spill_ = std::exchange(other.calls_since_spill_, 0);
            fixture_spill_interval_ = std::exchange(other.fixture_spill_interval_, 0);
            fixture_spill_predicate_ = std::exchange(other.fixture_spill_predicate_, nullptr);
            other.resident_by_key_.clear();
            other.resident_.clear();
            other.held_.clear();
            assert(held_rows_exact() && other.held_rows_exact());
        }
        return *this;
    }

    void set_store(ScratchStore store) noexcept { index_.set_store(store); }
    void set_directory(const std::string& directory) { index_.set_directory(directory); }
    void bind_scratch(const std::shared_ptr<ScratchSlab>& slab, ScratchResource resource) {
        index_.bind_scratch(slab, resource);
    }
    // A ticket anticipates making this id live, including a retired id that
    // is about to reopen. It is read-only even when a dirty empty row awaits
    // writeback: refusal must precede table AND kernel mutation.
    void require_insert(const std::string& key) const {
        index_.require_insert(key);
        if (!index_.bounded_cohorts()) return;
        const auto found = find_resident(key);
        if (found == end() || found->second.spillable()) require_live_growth(1);
    }
    template<class Enumerate> void require_inserts(Enumerate&& enumerate) const {
        index_.require_inserts(std::forward<Enumerate>(enumerate));
        if (!index_.bounded_cohorts()) return;
        std::size_t ordinal = 0, extra = 0;
        enumerate([&](const std::string& key) {
            const auto current = ordinal++;
            const auto found = find_resident(key);
            if (found != end() && !found->second.spillable()) return;
            bool repeated = false;
            std::size_t prior = 0;
            enumerate([&](const std::string& other) {
                if (prior++ < current && other == key) repeated = true;
            });
            if (!repeated) {
                if (extra == kMaxResidentCohorts) require_live_growth(extra + 1);
                ++extra;
            }
        });
        require_live_growth(extra); // At most one dry resident pass for the whole ticket.
    }
    void clear() noexcept {
        held_.clear();
        resident_by_key_.clear();
        resident_.clear();
        index_.clear();
        work_ = {};
        spill_threshold_ = spill_interval_ = kSpillFloor;
        calls_since_spill_ = 0;
        assert(held_rows_exact());
    }
    std::uint64_t size() const noexcept { return index_.size(); }
    std::size_t resident_size() const noexcept { return resident_.size(); }
    std::size_t held_size() const noexcept { return held_.size(); }
    iterator begin() noexcept { return resident_begin<false>(this); }
    iterator end() noexcept { return {this, 0, {}}; }
    const_iterator begin() const noexcept { return resident_begin<true>(this); }
    const_iterator end() const noexcept { return {this, 0, {}}; }
    auto resident_rows() noexcept {
        return Range<ResidentIterator<false>>{
            {this, resident_.begin()}, {this, resident_.end()}};
    }
    auto resident_rows() const noexcept {
        return Range<ResidentIterator<true>>{
            {this, resident_.begin()}, {this, resident_.end()}};
    }
    auto held_rows() noexcept {
        assert(held_rows_exact());
        return Range<HeldIterator<false>>{{this, held_.begin()}, {this, held_.end()}};
    }
    auto held_rows() const noexcept {
        assert(held_rows_exact());
        return Range<HeldIterator<true>>{{this, held_.begin()}, {this, held_.end()}};
    }
    // O(log H) update using the row's already-known offset and pin. No id
    // search or full resident walk on the Release mutation path.
    void refresh_held(const iterator& row) const {
        assert(row.owner_ == this && row.row_);
        refresh_held(row.at_, row.row_);
    }
    template<bool Const> void refresh_held(const HeldIterator<Const>& row) const {
        assert(row.owner_ == this && row.row_);
        refresh_held(row.offset_, row.row_);
    }
    // Independent, allocation-free oracles. They deliberately do not charge
    // production work counters; assert-enabled builds still pay O(R).
    bool held_rows_exact() const noexcept {
        auto next = held_.begin();
        for (const auto& row : resident_) {
            if (!is_held(row.second.owner->second, 0)) continue;
            if (next == held_.end() || next->first != row.first
                || next->second != row.second.owner) return false;
            ++next;
        }
        return next == held_.end();
    }
    std::size_t resident_opened_slot_count() const noexcept {
        std::size_t total = 0;
        for (const auto& row : resident_) total += row.second.owner->second.opened.size();
        return total;
    }
    auto resident_ids() const noexcept {
        const auto at = resident_.begin();
        const insertion_iterator first{this, at == resident_.end() ? 0 : at->first,
                                       at == resident_.end() ? nullptr : at->second.owner};
        return Range<KeyIterator<insertion_iterator>>{
            KeyIterator<insertion_iterator>(first),
            KeyIterator<insertion_iterator>(insertion_iterator{this, 0, {}})};
    }
    auto all_sorted() {
        const auto at = index_.first_sorted();
        return Range<sorted_iterator>{{this, at, at ? load(at) : nullptr}, {this, 0, {}}};
    }
    auto all_sorted() const {
        const auto at = index_.first_sorted();
        return Range<const_sorted_iterator>{{this, at, at ? load(at) : nullptr}, {this, 0, {}}};
    }
    // Full insertion-order replay without a growing vector of keys.
    template<class F> void for_each_id(F&& visit) const {
        for (auto at = index_.first_inserted(); at != 0; at = index_.next_inserted(at))
            visit(index_.key(at));
    }
    // Decisions over historical ids must not depend on row residency.
    // Walk keys alone, without restoring facts or retaining a key vector.
    template<class F> void for_each_sorted_id(F&& visit) const {
        for (auto at = index_.first_sorted(); at != 0; at = index_.next_sorted(at))
            visit(index_.key(at));
    }
    iterator find(const std::string& key) {
        spill_implicitly();
        const auto at = index_.find(key);
        return at ? iterator{this, *at, load(*at)} : end();
    }
    const_iterator find(const std::string& key) const {
        spill_implicitly();
        const auto at = index_.find(key);
        return at ? const_iterator{this, *at, load(*at)} : end();
    }
    // A retired row cannot carry exposure/opened handles. These reads stay
    // allocation-free and noexcept even when the disk is unavailable.
    iterator find_resident(const std::string& key) noexcept {
        const auto at = resident_by_key_.find(ResidentProbe{key, &work_.find_resident_probes});
        if (at == resident_by_key_.end()) return end();
        const auto row = resident_.find(at->second);
        return {this, row->first, row->second.owner};
    }
    const_iterator find_resident(const std::string& key) const noexcept {
        const auto at = resident_by_key_.find(ResidentProbe{key, &work_.find_resident_probes});
        if (at == resident_by_key_.end()) return end();
        const auto row = resident_.find(at->second);
        return {this, row->first, row->second.owner};
    }
    std::pair<iterator, bool> emplace(const std::string& key, Value value) {
        // A refused new logical row must not first overwrite/evict an eligible
        // resident row. This search reads index bytes only, without restoring.
        index_.require_insert(key);
        std::size_t extra = 0;
        if (index_.bounded_cohorts() && !value.spillable() && !index_.find(key)) {
            extra = 1; // An existing logical row ignores the incoming value.
            require_live_growth(extra);
        }
        spill_before_admission(extra);
        spill_implicitly();
        const auto placed = index_.insert(key, value.retired_record());
        if (!placed.second) return {{this, placed.first, load(placed.first)}, false};
        auto row = std::make_shared<Row>(key, std::move(value));
        insert_resident(placed.first, row);
        return {{this, placed.first, std::move(row)}, true};
    }
    void spill() const {
        for (auto at = resident_.begin(); at != resident_.end();) {
            ++work_.rows_visited_by_spill;
            if (at->second.owner.use_count() == 1 && at->second.owner->second.spillable()
                && (fixture_spill_predicate_ == nullptr
                    || fixture_spill_predicate_(at->second.owner->first))
            ) {
                index_.set(at->first, at->second.owner->second.retired_record());
                ++work_.rows_spilled;
                assert(held_.find(at->first) == held_.end());
                resident_by_key_.erase(at->second.key); // Amortised O(1), no key search.
                at = resident_.erase(at);
            } else ++at;
        }
        const auto count = resident_.size();
        const auto maximum = std::numeric_limits<std::size_t>::max();
        spill_threshold_ = std::max(kSpillFloor, count > maximum / 2 ? maximum : count * 2);
        spill_interval_ = std::max(kSpillFloor, count);
        calls_since_spill_ = 0;
    }
    // Growth and a call budget both trigger a pass. The floor handles a pass
    // that left zero/one rows; the budget also drains rows closed in place
    // without further growth. A full pass costs O(rows at the previous pass
    // + new rows), amortised over that many implicit calls or insertions.
    void spill_implicitly() const {
        const auto interval = fixture_spill_interval_ ? fixture_spill_interval_ : spill_interval_;
        if (++calls_since_spill_ >= interval || resident_.size() >= spill_threshold_) spill();
    }
    // Test-only schedule control: 0 selects the production policy, 1 the old
    // eager policy. The growth guard remains in force for every schedule.
    void fixture_implicit_spill_interval(std::size_t calls) noexcept {
        fixture_spill_interval_ = calls;
    }
    // Test-only observation hook. Production leaves this null, so the hot
    // spill path has one predictable null check per candidate row.
    void fixture_spill_predicate(bool (*predicate)(const std::string&)) noexcept {
        fixture_spill_predicate_ = predicate;
    }

private:
    void require_live_growth(std::size_t extra) const {
        if (extra == 0 || !index_.bounded_cohorts()) return;
        if (resident_.size() <= kMaxResidentCohorts
            && extra <= kMaxResidentCohorts - resident_.size()) return;
        // Admission's O(R) dry pass must finish before spill can write any
        // bytes. It is at most one such pass per spill, never a per-ticket
        // scan: callers batch the ticket's extra rows before reaching here.
        // Counting physical rows here would falsely refuse deferred empties
        // (including empty rows pinned by an enclosing iterator).
        std::size_t live = 0;
        for (const auto& row : resident_) {
            ++work_.rows_visited_by_capacity;
            if (!row.second.owner->second.spillable()) ++live;
        }
        if (live > kMaxResidentCohorts || extra > kMaxResidentCohorts - live)
            throw ScratchCapacityError(ScratchResource::cohort,
                scratch_resident_cohort_limit_message(kMaxResidentCohorts));
    }
    void spill_before_admission(std::size_t extra) const {
        if (extra != 0 && index_.bounded_cohorts() && resident_.size() >= kMaxResidentCohorts)
            spill(); // The read-only ticket has already proved this can fit.
    }
    void insert_resident(Offset at, const std::shared_ptr<Row>& row) const {
        // Views borrow the immutable key of a heap-pinned row, never the
        // caller's string. Erase a view before its last row owner is released.
        const auto key = std::string_view(row->first);
        const auto indexed = resident_by_key_.emplace(key, at);
        try {
            resident_.emplace(at, ResidentRow{row, indexed.first});
            if (is_held(row->second, 0)) held_.emplace(at, row);
        } catch (...) {
            resident_.erase(at);
            resident_by_key_.erase(indexed.first);
            throw;
        }
        assert(held_rows_exact());
    }
    template<class T>
    static auto is_held(const T& value, int) noexcept -> decltype(value.held()) {
        return value.held();
    }
    // Other users of the generic resident table have no opening/live-unit
    // view and need no held index.
    static bool is_held(const Value&, long) noexcept { return false; }
    void refresh_held(Offset at, const std::shared_ptr<Row>& row) const {
        const auto found = held_.lower_bound(at);
        if (is_held(row->second, 0)) {
            if (found == held_.end() || found->first != at) held_.emplace_hint(found, at, row);
        } else if (found != held_.end() && found->first == at) {
            held_.erase(found);
        }
        assert(held_rows_exact());
    }
    // Codecs that validate stored bytes may accept their physical origin.
    // Keep the original one-argument codec contract for other value types.
    template<class T = Value>
    auto restore_record(const typename T::Record& record, int) const
        -> decltype(T::restore_record(record, bool{})) {
        return T::restore_record(record, index_.arena_backed());
    }
    Value restore_record(const typename Value::Record& record, long) const {
        return Value::restore_record(record);
    }
    template<bool Const, class Owner>
    static Iterator<Const> resident_begin(Owner* owner) noexcept {
        const auto at = owner->resident_.rbegin();
        return {owner, at == owner->resident_.rend() ? 0 : at->first,
                at == owner->resident_.rend() ? nullptr : at->second.owner};
    }
    std::shared_ptr<Row> load(Offset at) const {
        const auto found = resident_.find(at);
        if (found != resident_.end()) return found->second.owner;
        auto value = restore_record(index_.get(at), 0);
        const std::size_t extra = value.spillable() ? 0 : 1;
        require_live_growth(extra);
        spill_before_admission(extra);
        auto row = std::make_shared<Row>(index_.key(at), std::move(value));
        insert_resident(at, row);
        ++work_.rows_restored;
        return row;
    }
    static constexpr std::size_t kSpillFloor = 64;
    mutable Index index_;
    mutable Residents resident_;
    mutable ResidentKeys resident_by_key_; // One borrowed key per resident row.
    mutable Held held_;
    mutable WorkCounters work_{};
    mutable std::size_t spill_threshold_ = kSpillFloor, spill_interval_ = kSpillFloor;
    mutable std::size_t calls_since_spill_ = 0;
    std::size_t fixture_spill_interval_ = 0;
    bool (*fixture_spill_predicate_)(const std::string&) = nullptr;
};
} // namespace pineforge::detail
