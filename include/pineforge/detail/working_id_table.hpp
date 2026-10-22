#pragma once

#include <pineforge/detail/disk_resident_id_table.hpp>
#include <pineforge/detail/working_members.hpp>

namespace pineforge::detail {

template<class T>
class WorkingScalar {
public:
    WorkingScalar(T value = {}) noexcept : value_(value) {}
    operator T() const noexcept { return value_; }
    WorkingScalar& operator=(T value) {
        if (store_) working_store::transaction(*store_, [&] {
            working_store::put_word(*store_, {43, owner_, field_}, static_cast<std::uint64_t>(value));
        });
        value_ = value; return *this;
    }
    WorkingScalar& operator|=(unsigned value) { return *this = static_cast<T>(value_ | value); }
    void bind(ScratchStore& store, std::uint64_t owner, std::uint64_t field) {
        store_ = &store; owner_ = owner; field_ = field;
        const auto value = working_store::get_word(store, {43, owner, field});
        if (value) value_ = static_cast<T>(*value);
        else *this = value_;
    }
private:
    T value_{};
    ScratchStore* store_ = nullptr;
    std::uint64_t owner_ = 0, field_ = 0;
};

// Cohort rows contain store-backed members and write-through scalar facts.
// The two cached row frames are derived and can be dropped at any time.
template<class Value>
class WorkingIdTable {
    using Legacy = DiskResidentIdTable<Value>;
    using Row = std::pair<const std::string, Value>;
    class IdKey final : public ScratchByteSource {
    public:
        IdKey(std::uint64_t owner, std::string_view text) : prefix_(47, owner, 0), text_(text) {}
        std::uint64_t size() const noexcept override { return 32 + text_.size(); }
        ScratchStatus read(std::uint64_t at, ScratchBuffer out) const noexcept override {
            if (at > size() || out.size > size() - at || (out.size && !out.data))
                return ScratchStatus::programming_error;
            if (at < 32) {
                const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size, 32 - at));
                const auto status = prefix_.read(at, {out.data, n});
                if (status != ScratchStatus::ok) return status;
                out.data += n; out.size -= n; at += n;
            }
            if (out.size) std::memcpy(out.data, text_.data() + at - 32, out.size);
            return ScratchStatus::ok;
        }
    private:
        working_store::Key prefix_;
        std::string_view text_;
    };
public:
    template<bool Const, bool Sorted = false>
    class Iterator {
        friend class WorkingIdTable;
        using Owner = std::conditional_t<Const, const WorkingIdTable, WorkingIdTable>;
        using Old = std::conditional_t<Sorted,
            std::conditional_t<Const, typename Legacy::const_sorted_iterator, typename Legacy::sorted_iterator>,
            std::conditional_t<Const, typename Legacy::const_iterator, typename Legacy::iterator>>;
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Row;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<Const, const Row&, Row&>;
        using pointer = std::conditional_t<Const, const Row*, Row*>;
        Iterator() = default;
        reference operator*() const { return owner_->store_ ? *row_ : *old_; }
        pointer operator->() const { return &**this; }
        Iterator& operator++() {
            if (!owner_->store_) { ++old_; return *this; }
            if constexpr (Sorted) {
                ScratchHandle next = 0;
                const auto status = owner_->store_->next(at_, next);
                if (status != ScratchStatus::end) working_store::check(status);
                at_ = status == ScratchStatus::end || !owner_->is_id(next) ? 0 : next;
            } else {
                if (!rank_) at_ = 0;
                else { --rank_; at_ = owner_->order_[rank_]; }
            }
            row_ = at_ ? owner_->load(at_) : nullptr;
            return *this;
        }
        Iterator operator++(int) { auto previous = *this; ++*this; return previous; }
        bool operator==(const Iterator& other) const {
            return owner_ == other.owner_ && (owner_ && owner_->store_ ? at_ == other.at_ : old_ == other.old_);
        }
        bool operator!=(const Iterator& other) const { return !(*this == other); }
    private:
        Iterator(Owner* owner, Old old) : owner_(owner), old_(std::move(old)) {}
        Iterator(Owner* owner, ScratchHandle at, std::size_t rank = 0)
            : owner_(owner), at_(at), rank_(rank), row_(at ? owner->load(at) : nullptr) {}
        Owner* owner_ = nullptr;
        Old old_{};
        ScratchHandle at_ = 0;
        std::size_t rank_ = 0;
        std::shared_ptr<Row> row_;
    };
    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;
    using sorted_iterator = Iterator<false, true>;
    using const_sorted_iterator = Iterator<true, true>;
    template<class I> struct Range {
        I first, last;
        I begin() const { return first; }
        I end() const { return last; }
    };
    void bind_store(ScratchStore& store, std::uint64_t owner = 1) {
        const bool first = !store_;
        store_ = &store; owner_ = owner;
        for (auto& row : cache_) row.reset();
        order_.bind(store, 48, owner);
        if (first) {
            for (const auto& row : legacy_.all_sorted()) {
                Value value = row.second;
                value.bind_members(store);
                emplace(row.first, std::move(value));
            }
            legacy_.clear();
        }
    }
    void set_directory(const std::string& value) { legacy_.set_directory(value); }
    void bind_scratch(const std::shared_ptr<ScratchSlab>& slab, ScratchResource resource) {
        legacy_.bind_scratch(slab, resource);
    }
    void require_insert(const std::string& key) const {
        if (!store_) legacy_.require_insert(key);
        else if (key.size() > std::numeric_limits<std::uint64_t>::max() - 32)
            throw std::length_error("working cohort identity length");
    }
    template<class Enumerate> void require_inserts(Enumerate&& enumerate) const {
        if (!store_) legacy_.require_inserts(std::forward<Enumerate>(enumerate));
        else enumerate([&](const std::string& key) { require_insert(key); });
    }
    std::uint64_t size() const noexcept { return store_ ? order_.size() : legacy_.size(); }
    std::size_t resident_size() const noexcept {
        if (!store_) return legacy_.resident_size();
        std::size_t count = 0; for (const auto& row : cache_) if (row) ++count; return count;
    }
    iterator begin() {
        if (!store_) return iterator(this, legacy_.begin());
        return order_.empty() ? end() : iterator(this, order_.back(), order_.size() - 1);
    }
    iterator end() { return store_ ? iterator(this, ScratchHandle{0}) : iterator(this, legacy_.end()); }
    const_iterator begin() const {
        if (!store_) return const_iterator(this, legacy_.begin());
        return order_.empty() ? end() : const_iterator(this, order_.back(), order_.size() - 1);
    }
    const_iterator end() const { return store_ ? const_iterator(this, ScratchHandle{0}) : const_iterator(this, legacy_.end()); }
    auto all_sorted() {
        if (!store_) {
            const auto old = legacy_.all_sorted();
            return Range<sorted_iterator>{{this, old.begin()}, {this, old.end()}};
        }
        return Range<sorted_iterator>{{this, first_id()}, {this, ScratchHandle{0}}};
    }
    auto all_sorted() const {
        if (!store_) {
            const auto old = legacy_.all_sorted();
            return Range<const_sorted_iterator>{{this, old.begin()}, {this, old.end()}};
        }
        return Range<const_sorted_iterator>{{this, first_id()}, {this, ScratchHandle{0}}};
    }
    iterator find(const std::string& key) {
        return store_ ? iterator(this, find_id(key)) : iterator(this, legacy_.find(key));
    }
    const_iterator find(const std::string& key) const {
        return store_ ? const_iterator(this, find_id(key)) : const_iterator(this, legacy_.find(key));
    }
    iterator find_resident(const std::string& key) { return store_ ? find(key) : iterator(this, legacy_.find_resident(key)); }
    const_iterator find_resident(const std::string& key) const { return store_ ? find(key) : const_iterator(this, legacy_.find_resident(key)); }
    std::pair<iterator, bool> emplace(const std::string& key, Value value) {
        if (!store_) {
            auto placed = legacy_.emplace(key, std::move(value));
            return {iterator(this, std::move(placed.first)), placed.second};
        }
        if (const auto old = find_id(key)) return {iterator(this, old), false};
        ScratchHandle at = 0;
        const auto count = order_.size();
        try { working_store::transaction(*store_, [&] {
            value.bind_members(*store_);
            std::array<std::byte, 8> bytes{}; working_store::le(value.handle.value, bytes.data());
            ScratchSpanSource source({bytes.data(), bytes.size()});
            working_store::check(store_->put(IdKey(owner_, key), source, at));
            order_.push_back(at);
        }); } catch (...) { order_.restore_size(count); throw; }
        auto row = std::make_shared<Row>(key, std::move(value));
        cache_[next_cache_] = row; next_cache_ = (next_cache_ + 1) % cache_.size();
        return {iterator(this, at), true};
    }
    void clear() {
        if (!store_) { legacy_.clear(); return; }
        const auto count = order_.size();
        try { working_store::transaction(*store_, [&] {
        for (const auto at : static_cast<const WorkingSequence<ScratchHandle>&>(order_)) {
            auto row = load(at);
            row->second.clear_members();
            working_store::check(store_->erase(at));
        }
        order_.clear();
        }); } catch (...) {
            order_.restore_size(count);
            for (auto& row : cache_) row.reset();
            throw;
        }
        for (auto& row : cache_) row.reset();
    }
    void spill() const { if (!store_) legacy_.spill(); }
    template<class Visit> void for_each_id(Visit&& visit) const {
        if (!store_) { legacy_.for_each_id(std::forward<Visit>(visit)); return; }
        for (const auto at : order_) visit(load(at)->first);
    }
    template<class Visit> void for_each_sorted_id(Visit&& visit) const {
        if (!store_) { legacy_.for_each_sorted_id(std::forward<Visit>(visit)); return; }
        for (const auto& row : all_sorted()) visit(row.first);
    }
    // The legacy caller uses this only as a key walk; the bound backend has
    // no distinction between a resident id and an addressable current id.
    auto resident_ids() const {
        using OldRange = decltype(std::declval<const Legacy&>().resident_ids());
        using OldIterator = decltype(std::declval<OldRange>().begin());
        struct Keys {
            const WorkingIdTable* table;
            struct It {
                const WorkingIdTable* table;
                OldIterator old;
                std::size_t rank;
                std::shared_ptr<Row> row;
                const std::string& operator*() const { return table->store_ ? row->first : *old; }
                It& operator++() {
                    if (!table->store_) ++old;
                    else {
                        ++rank;
                        row = rank < table->order_.size() ? table->load(table->order_[rank]) : nullptr;
                    }
                    return *this;
                }
                bool operator!=(const It& other) const {
                    return table->store_ ? rank != other.rank : old != other.old;
                }
            };
            It begin() const {
                if (!table->store_) return {table, table->legacy_.resident_ids().begin(), 0, {}};
                return {table, {}, 0, table->order_.empty() ? nullptr : table->load(table->order_[0])};
            }
            It end() const {
                if (!table->store_) return {table, table->legacy_.resident_ids().end(), 0, {}};
                return {table, {}, table->order_.size(), {}};
            }
        };
        return Keys{this};
    }
private:
    bool is_id(ScratchHandle at) const {
        if (!at) return false;
        ScratchRecordInfo info; working_store::check(store_->point(at, info));
        if (info.key_bytes < 32) return false;
        std::array<std::byte, 32> prefix{}, actual{};
        working_store::check(working_store::Key(47, owner_, 0).read(0, {prefix.data(), prefix.size()}));
        working_store::check(store_->read(at, ScratchField::key, 0, {actual.data(), actual.size()}));
        if (prefix != actual) return false;
        if (info.value_bytes != 8) working_store::check(ScratchStatus::corrupt);
        return true;
    }
    ScratchHandle first_id() const {
        ScratchHandle at = 0;
        const auto status = store_->lower_bound(IdKey(owner_, {}), at);
        if (status == ScratchStatus::end) return 0;
        working_store::check(status); return is_id(at) ? at : 0;
    }
    ScratchHandle find_id(std::string_view text) const {
        ScratchHandle at = 0;
        const auto status = store_->lower_bound(IdKey(owner_, text), at);
        if (status == ScratchStatus::end) return 0;
        working_store::check(status); if (!is_id(at)) return 0;
        ScratchRecordInfo info; working_store::check(store_->point(at, info));
        if (info.key_bytes != text.size() + 32) return 0;
        std::array<std::byte, 128> frame{};
        for (std::size_t offset = 0; offset < text.size();) {
            const auto n = std::min(frame.size(), text.size() - offset);
            working_store::check(store_->read(at, ScratchField::key, 32 + offset, {frame.data(), n}));
            if (std::memcmp(frame.data(), text.data() + offset, n)) return 0;
            offset += n;
        }
        return at;
    }
    std::shared_ptr<Row> load(ScratchHandle at) const {
        ScratchRecordInfo info; working_store::check(store_->point(at, info));
        std::array<std::byte, 8> bytes{};
        working_store::check(store_->read(at, ScratchField::value, 0, {bytes.data(), bytes.size()}));
        const auto handle = working_store::word(bytes.data());
        if (!handle) working_store::check(ScratchStatus::corrupt);
        for (const auto& row : cache_) if (row && row->second.handle.value == handle) return row;
        std::string text(static_cast<std::size_t>(info.key_bytes - 32), '\0');
        working_store::check(store_->read(at, ScratchField::key, 32,
            {reinterpret_cast<std::byte*>(text.data()), text.size()}));
        Value value; value.handle.value = handle; value.bind_members(*store_, true);
        auto row = std::make_shared<Row>(std::move(text), std::move(value));
        cache_[next_cache_] = row; next_cache_ = (next_cache_ + 1) % cache_.size();
        return row;
    }
    Legacy legacy_;
    ScratchStore* store_ = nullptr;
    std::uint64_t owner_ = 1;
    WorkingSequence<ScratchHandle> order_;
    mutable std::array<std::shared_ptr<Row>, 2> cache_{};
    mutable std::size_t next_cache_ = 0;
};

} // namespace pineforge::detail
