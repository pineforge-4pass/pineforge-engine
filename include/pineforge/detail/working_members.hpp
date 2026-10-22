#pragma once
#include <pineforge/detail/working_store.hpp>
#include <exception>
#include <iterator>
#include <utility>

namespace pineforge::detail {

// A sequence has two fixed read/write frames. Its positions and size are store
// records once bound; the vector is solely the unbound compatibility backend.
// A returned bound reference lasts until another position is read.
template<class T>
class WorkingSequence {
    struct Frame {
        std::optional<T> row;
        std::size_t index = 0;
        bool dirty = false;
    };
public:
    using value_type = T;
    using size_type = std::size_t;
    template<bool Const> class Iterator {
        using Owner = std::conditional_t<Const, const WorkingSequence, WorkingSequence>;
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<Const, const T&, T&>;
        using pointer = std::conditional_t<Const, const T*, T*>;
        Iterator() = default;
        Iterator(Owner* owner, std::size_t index) : owner_(owner), index_(index) {}
        reference operator*() const { return (*owner_)[index_]; }
        pointer operator->() const { return &**this; }
        reference operator[](difference_type n) const { return *(*this + n); }
        Iterator& operator++() { ++index_; return *this; }
        Iterator operator++(int) { auto old = *this; ++*this; return old; }
        Iterator& operator--() { --index_; return *this; }
        Iterator operator--(int) { auto old = *this; --*this; return old; }
        Iterator& operator+=(difference_type n) { index_ += n; return *this; }
        Iterator& operator-=(difference_type n) { index_ -= n; return *this; }
        friend Iterator operator+(Iterator a, difference_type n) { return a += n; }
        friend Iterator operator+(difference_type n, Iterator a) { return a += n; }
        friend Iterator operator-(Iterator a, difference_type n) { return a -= n; }
        friend difference_type operator-(const Iterator& a, const Iterator& b) {
            return static_cast<difference_type>(a.index_) - static_cast<difference_type>(b.index_);
        }
        friend bool operator==(const Iterator& a, const Iterator& b) {
            return a.owner_ == b.owner_ && a.index_ == b.index_;
        }
        friend bool operator!=(const Iterator& a, const Iterator& b) { return !(a == b); }
        friend bool operator<(const Iterator& a, const Iterator& b) { return a.index_ < b.index_; }
        friend bool operator>(const Iterator& a, const Iterator& b) { return b < a; }
        friend bool operator<=(const Iterator& a, const Iterator& b) { return !(b < a); }
        friend bool operator>=(const Iterator& a, const Iterator& b) { return !(a < b); }
        std::size_t index() const { return index_; }
    private:
        Owner* owner_ = nullptr;
        std::size_t index_ = 0;
    };
    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    WorkingSequence() = default;
    WorkingSequence(const WorkingSequence& other) {
        if (other.store_) {
            store_ = other.store_; domain_ = other.domain_;
            binding_ = other.binding_;
            for (const auto& row : other) push_back(row);
        } else memory_ = other.memory_;
    }
    WorkingSequence& operator=(const WorkingSequence& other) {
        if (this == &other) return *this;
        clear();
        for (const auto& row : other) push_back(row);
        return *this;
    }
    WorkingSequence(WorkingSequence&& other) noexcept
        : store_(other.store_), domain_(other.domain_), owner_(std::exchange(other.owner_, 0)),
          count_(std::exchange(other.count_, 0)), memory_(std::move(other.memory_)),
          frames_(std::move(other.frames_)), next_frame_(other.next_frame_),
          binding_(other.binding_) {
        other.drop_frames();
    }
    WorkingSequence& operator=(WorkingSequence&& other) noexcept {
        if (this == &other) return *this;
        drop_frames();
        store_ = other.store_; domain_ = other.domain_;
        owner_ = std::exchange(other.owner_, 0); count_ = std::exchange(other.count_, 0);
        memory_ = std::move(other.memory_); frames_ = std::move(other.frames_);
        next_frame_ = other.next_frame_; binding_ = other.binding_; other.drop_frames();
        return *this;
    }
    WorkingSequence(std::initializer_list<T> values) : memory_(values) {}
    WorkingSequence(const std::vector<T>& values) : memory_(values) {}
    WorkingSequence(std::vector<T>&& values) noexcept : memory_(std::move(values)) {}
    WorkingSequence& operator=(const std::vector<T>& values) {
        if (!store_) { memory_ = values; return *this; }
        const auto before = count_;
        try { working_store::transaction(*store_, [&] {
            clear();
            for (const auto& row : values) push_back(row);
        }); } catch (...) { restore_size(before); throw; }
        return *this;
    }
    WorkingSequence& operator=(std::vector<T>&& values) {
        if (!store_) { memory_ = std::move(values); return *this; }
        return *this = static_cast<const std::vector<T>&>(values);
    }
    friend bool operator==(const WorkingSequence& a, const WorkingSequence& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) if (!(a[i] == b[i])) return false;
        return true;
    }
    friend bool operator!=(const WorkingSequence& a, const WorkingSequence& b) { return !(a == b); }
    void bind(ScratchStore& store, std::uint64_t domain, std::uint64_t owner) {
        const bool first = !store_;
        store_ = &store; domain_ = domain;
        if (!binding_) binding_ = std::make_shared<working_store::Binding>();
        binding_->store = &store;
        if (first) owner_ = owner;
        drop_frames();
        if (const auto count = working_store::get_word(store, key(0))) {
            if (*count > max_size()) working_store::check(ScratchStatus::corrupt);
            count_ = static_cast<std::size_t>(*count);
            std::vector<T>{}.swap(memory_);
        } else {
            if (!first && owner_) working_store::check(ScratchStatus::corrupt);
            count_ = 0;
            auto memory = std::move(memory_);
            working_store::transaction(store, [&] {
                ensure_owner();
                working_store::put_word(store, key(0), 0);
                for (const auto& row : memory) append_pending(row);
            });
        }
    }
    void bind_existing(ScratchStore& store, std::uint64_t domain, std::uint64_t owner) {
        if (!working_store::get_word(store, {domain, owner, 0}))
            working_store::check(ScratchStatus::corrupt);
        bind(store, domain, owner);
    }
    bool bound() const noexcept { return store_ != nullptr; }
    void validate() const {
        if (!store_) return;
        ScratchRecordInfo ignored{};
        const auto status = store_->point(0, ignored);
        if (status != ScratchStatus::not_found) working_store::check(status);
    }
    void detach() noexcept { store_ = nullptr; owner_ = 0; count_ = 0; drop_frames(); memory_.clear(); }
    void restore_size(std::size_t count) noexcept { count_ = count; drop_frames(); }
    std::size_t size() const noexcept { return store_ ? count_ : memory_.size(); }
    bool empty() const noexcept { return size() == 0; }
    std::size_t max_size() const noexcept {
        return store_ ? std::numeric_limits<std::size_t>::max() : memory_.max_size();
    }
    std::size_t capacity() const noexcept { return store_ ? frames_.size() : memory_.capacity(); }
    void reserve(std::size_t n) { if (!store_) memory_.reserve(n); }
    // Observe current values, including a dirty frame, without publishing or
    // evicting frames. Lazy payload readers retain their usual version pins.
    T observe_at(std::size_t index) const {
        if (index >= size())
            throw coded<std::out_of_range>(RunFailureCode::engine_invariant, {},
                                           "working sequence position");
        if (!store_) return memory_[index];
        validate();
        for (const auto& frame : frames_)
            if (frame.row && frame.index == index) return *frame.row;
        const auto handle = working_store::find(*store_, key(index + 1));
        if (!handle) working_store::check(ScratchStatus::corrupt);
        return working_store::read<T>(*store_, handle, binding_);
    }
    T value_at(std::size_t index) const {
        if (index >= size()) throw std::out_of_range("working sequence position");
        if (!store_) return memory_[index];
        flush();
        return read_row(index);
    }
    const T& operator[](std::size_t index) const {
        if (!store_) return memory_[index];
        return *load(index).row;
    }
    T& operator[](std::size_t index) {
        if (!store_) return memory_[index];
        auto& frame = load(index); frame.dirty = true; return *frame.row;
    }
    const T& at(std::size_t index) const {
        if (index >= size()) throw std::out_of_range("working sequence position");
        return (*this)[index];
    }
    T& at(std::size_t index) {
        if (index >= size()) throw std::out_of_range("working sequence position");
        return (*this)[index];
    }
    const T& front() const { return at(0); }
    T& front() { return at(0); }
    const T& back() const { return at(size() - 1); }
    T& back() { return at(size() - 1); }
    iterator begin() { flush(); return {this, 0}; }
    iterator end() { return {this, size()}; }
    const_iterator begin() const { flush(); return {this, 0}; }
    const_iterator end() const { return {this, size()}; }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }
    reverse_iterator rbegin() { return reverse_iterator(end()); }
    reverse_iterator rend() { return reverse_iterator(begin()); }
    const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
    const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }
    void push_back(const T& row) {
        if (!store_) { memory_.push_back(row); return; }
        flush();
        const auto old = count_;
        try { working_store::transaction(*store_, [&] { ensure_owner(); append_pending(row); }); }
        catch (...) { count_ = old; drop_frames(); throw; }
    }
    void push_back(T&& row) { push_back(static_cast<const T&>(row)); }
    template<class... Args> void emplace_back(Args&&... args) {
        T row(std::forward<Args>(args)...); push_back(std::move(row));
    }
    iterator erase(iterator first, iterator last) {
        const auto start = first.index(), stop = last.index();
        if (!store_) {
            memory_.erase(memory_.begin() + start, memory_.begin() + stop);
            return {this, start};
        }
        flush();
        const auto old = count_;
        working_store::transaction(*store_, [&] {
            for (std::size_t i = stop; i < old; ++i) {
                const T row = read_row(i);
                write_row(i - (stop - start), row);
            }
            for (std::size_t i = old - (stop - start); i < old; ++i)
                working_store::erase_value(*store_, key(i + 1), binding_);
            working_store::put_word(*store_, key(0), old - (stop - start));
        });
        count_ = old - (stop - start); drop_frames();
        working_store::reclaim_versions(*store_, binding_);
        return {this, start};
    }
    iterator erase(iterator at) { return erase(at, at + 1); }
    iterator erase(const_iterator at) { return erase(iterator(this, at.index())); }
    void pop_back() { erase(end() - 1, end()); }
    iterator insert(iterator at, const T& value) {
        const auto position = at.index();
        if (!store_) { memory_.insert(memory_.begin() + position, value); return {this, position}; }
        const T row = value;
        flush();
        const auto old = count_;
        working_store::transaction(*store_, [&] {
            ensure_owner();
            for (std::size_t i = old; i > position; --i) {
                const T previous = read_row(i - 1);
                write_row(i, previous);
            }
            write_row(position, row);
            working_store::put_word(*store_, key(0), old + 1);
        });
        count_ = old + 1; drop_frames();
        working_store::reclaim_versions(*store_, binding_);
        return {this, position};
    }
    void set(std::size_t index, const T& value) {
        if (!store_) { memory_[index] = value; return; }
        const T row = value;
        flush();
        working_store::transaction(*store_, [&] { write_row(index, row); });
        drop_frames();
        working_store::reclaim_versions(*store_, binding_);
    }
    void clear() {
        if (!store_) { memory_.clear(); return; }
        drop_frames();
        if (!owner_) {
            count_ = 0;
            working_store::reclaim_versions(*store_, binding_);
            return;
        }
        working_store::transaction(*store_, [&] {
            for (std::size_t i = 0; i < count_; ++i)
                working_store::erase_value(*store_, key(i + 1), binding_);
            working_store::put_word(*store_, key(0), 0);
            working_store::collect_versions(*store_, binding_);
        });
        count_ = 0;
    }
    // A drained temporary either returns its empty owner to the member or
    // removes the obsolete count record. The binding still carries live readers.
    void recycle_into(WorkingSequence& member) {
        clear();
        if (member.empty() && (member.bound() || member.capacity() == 0)) member.swap(*this);
        if (store_ && owner_) {
            working_store::transaction(*store_, [&] {
                working_store::erase(*store_, key(0));
                working_store::collect_versions(*store_, binding_);
            });
            owner_ = 0;
        }
    }
    class DrainStorage {
    public:
        DrainStorage(WorkingSequence& queued, WorkingSequence& member)
            : queued_(queued), member_(member) {}
        DrainStorage(const DrainStorage&) = delete;
        DrainStorage& operator=(const DrainStorage&) = delete;
        ~DrainStorage() noexcept(false) {
            if (std::uncaught_exceptions() == 0) queued_.recycle_into(member_);
        }
    private:
        WorkingSequence& queued_;
        WorkingSequence& member_;
    };
    DrainStorage recycle_on_exit(WorkingSequence& member) { return {*this, member}; }
    void swap(WorkingSequence& other) {
        if (store_ && !other.store_) {
            auto rows = std::move(other.memory_);
            flush();
            other = std::move(*this);
            for (const auto& row : rows) push_back(row);
            return;
        }
        if (other.store_ && !store_) { other.swap(*this); return; }
        using std::swap;
        flush(); other.flush();
        swap(store_, other.store_); swap(domain_, other.domain_); swap(owner_, other.owner_);
        swap(count_, other.count_); memory_.swap(other.memory_); drop_frames(); other.drop_frames();
        binding_.swap(other.binding_);
    }
    template<class Compare>
    void stable_sort(Compare&& compare) {
        if (!store_) {
            std::stable_sort(memory_.begin(), memory_.end(), std::forward<Compare>(compare));
            return;
        }
        flush();
        working_store::transaction(*store_, [&] {
            for (std::size_t i = 1; i < count_; ++i) {
                const T value = read_row(i);
                auto at = i;
                while (at != 0) {
                    const T previous = read_row(at - 1);
                    if (!compare(value, previous)) break;
                    write_row(at, previous);
                    --at;
                }
                if (at != i) write_row(at, value);
            }
        });
        drop_frames();
        working_store::reclaim_versions(*store_, binding_);
    }
    void flush() const {
        if (!store_) return;
        validate();
        try {
            for (auto& frame : frames_) {
                if (!frame.dirty || !frame.row) continue;
                const auto handle = working_store::find(*store_, key(frame.index + 1));
                if (!handle) working_store::check(ScratchStatus::corrupt);
                if (working_store::equal_bytes(*frame.row, working_store::bytes(*store_, handle))) {
                    frame.dirty = false;
                    continue;
                }
                working_store::transaction(*store_, [&] { write_row(frame.index, *frame.row); });
                frame.dirty = false;
                frame.row = read_row(frame.index);
            }
            working_store::reclaim_versions(*store_, binding_);
        } catch (...) { drop_frames(); throw; }
    }
    operator std::vector<T>() const {
        std::vector<T> out; out.reserve(size());
        for (const auto& row : *this) out.push_back(row);
        return out;
    }
private:
    working_store::Key key(std::uint64_t item) const { return {domain_, owner_, item}; }
    void ensure_owner() {
        if (!owner_) owner_ = working_store::allocate(*store_) | (std::uint64_t{1} << 63);
    }
    T read_row(std::size_t i) const {
        const auto handle = working_store::find(*store_, key(i + 1));
        if (!handle) working_store::check(ScratchStatus::corrupt);
        return working_store::read<T>(*store_, handle, binding_);
    }
    void write_row(std::size_t i, const T& row) const {
        // Copied lazy readers pin their version; only those versions survive
        // replacement, and the next write boundary collects released pins.
        working_store::write(*store_, key(i + 1), row, false, binding_);
    }
    void append_pending(const T& row) {
        if (count_ == max_size()) throw std::overflow_error("working sequence position domain exhausted");
        write_row(count_, row);
        working_store::put_word(*store_, key(0), count_ + 1);
        ++count_;
    }
    void drop_frames() const {
        for (auto& frame : frames_) { frame.row.reset(); frame.dirty = false; }
    }
    Frame& load(std::size_t index) const {
        validate();
        for (auto& frame : frames_) if (frame.row && frame.index == index) return frame;
        auto& frame = frames_[next_frame_];
        next_frame_ = (next_frame_ + 1) % frames_.size();
        if (frame.dirty && frame.row) {
            try {
                flush();
            } catch (...) { drop_frames(); throw; }
        }
        frame.row = read_row(index); frame.index = index;
        return frame;
    }
    ScratchStore* store_ = nullptr;
    std::uint64_t domain_ = 0, owner_ = 0;
    std::size_t count_ = 0;
    std::vector<T> memory_;
    mutable std::array<Frame, 2> frames_{};
    mutable std::size_t next_frame_ = 0;
    std::shared_ptr<working_store::Binding> binding_;
};

template<class T>
class WorkingMap {
    using Rows = WorkingSequence<std::pair<std::uint64_t, T>>;
public:
    using value_type = typename Rows::value_type;
    using iterator = typename Rows::const_iterator;
    using const_iterator = typename Rows::const_iterator;
    void bind(ScratchStore& store, std::uint64_t domain, std::uint64_t owner) {
        rows_.bind(store, domain, owner);
    }
    bool bound() const noexcept { return rows_.bound(); }
    std::size_t size() const noexcept { return rows_.size(); }
    bool empty() const noexcept { return rows_.empty(); }
    iterator begin() { return rows_.cbegin(); }
    iterator end() { return rows_.cend(); }
    const_iterator begin() const { return rows_.begin(); }
    const_iterator end() const { return rows_.end(); }
    std::size_t count(std::uint64_t key) const { return find(key) == end() ? 0 : 1; }
    bool contains(std::uint64_t key) const { return count(key) != 0; }
    iterator find(std::uint64_t key) {
        rows_.validate();
        const auto at = lower(key);
        return at < size() && static_cast<const Rows&>(rows_)[at].first == key
            ? iterator(&rows_, at) : end();
    }
    const_iterator find(std::uint64_t key) const {
        rows_.validate();
        const auto at = lower(key);
        return at < size() && rows_[at].first == key ? const_iterator(&rows_, at) : end();
    }
    class Reference {
    public:
        Reference(WorkingMap& owner, std::uint64_t key) : owner_(owner), key_(key) {}
        Reference& operator=(const T& value) { owner_.set(key_, value); return *this; }
        Reference& operator+=(const T& value) { return *this = get() + value; }
        Reference& operator-=(const T& value) { return *this = get() - value; }
        operator T() const { return get(); }
    private:
        T get() const {
            const auto at = owner_.find(key_);
            if (at != owner_.end()) return at->second;
            owner_.set(key_, T{}); return {};
        }
        WorkingMap& owner_;
        std::uint64_t key_;
    };
    Reference operator[](std::uint64_t key) { return {*this, key}; }
    void set(std::uint64_t key, const T& value) {
        const auto at = lower(key);
        if (at == size() || static_cast<const Rows&>(rows_)[at].first != key)
            rows_.insert(typename Rows::iterator(&rows_, at), {key, value});
        else rows_.set(at, {key, value});
    }
    const T& at(std::uint64_t key) const {
        auto found = find(key);
        if (found == end()) throw std::out_of_range("working map key");
        return found->second;
    }
    template<class... Args>
    std::pair<iterator, bool> emplace(std::uint64_t key, Args&&... args) {
        const auto at = lower(key);
        if (at < size() && static_cast<const Rows&>(rows_)[at].first == key)
            return {iterator(&rows_, at), false};
        rows_.insert(typename Rows::iterator(&rows_, at), {key, T(std::forward<Args>(args)...)});
        return {iterator(&rows_, at), true};
    }
    iterator erase(iterator at) { const auto rank = at.index(); rows_.erase(at); return iterator(&rows_, rank); }
    std::size_t erase(std::uint64_t key) {
        const auto at = find(key); if (at == end()) return 0;
        erase(at); return 1;
    }
    void clear() { rows_.clear(); }
    void swap(WorkingMap& other) { rows_.swap(other.rows_); }
    std::size_t bucket_count() const noexcept { return 0; }
    void rehash(std::size_t) {}
    void reserve(std::size_t n) { rows_.reserve(n); }
    void flush() const { rows_.flush(); }
private:
    std::size_t lower(std::uint64_t key) const {
        std::size_t first = 0, last = size();
        while (first < last) {
            const auto mid = first + (last - first) / 2;
            if (rows_[mid].first < key) first = mid + 1; else last = mid;
        }
        return first;
    }
    Rows rows_;
};

} // namespace pineforge::detail
