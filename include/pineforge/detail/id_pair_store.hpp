#pragma once

// CR-150 L19l-C. Exact string / string-pair tables with no per-key RAM
// directory. Like L19a's successor-root file, scratch files are private,
// unlinked at creation, close-on-exec, and accessed with pread/pwrite.
// Those file-mode properties now belong to ScratchFile; an explicitly
// attached ScratchArena instead supplies a separate region for each file.
#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <system_error>
#include <type_traits>
#include <utility>

namespace pineforge::detail {

// Keep L19a's system_error surface, carrying the run's resource failure too.
class IdPairStoreError final : public std::system_error, public RunFailureInfo {
public:
    IdPairStoreError(int error, const char* operation)
        : std::system_error(error, std::generic_category(), operation),
          RunFailureInfo(RunFailureCode::report_sink_failed, {}) {}
};

class IdPairScratchFile {
public:
    IdPairScratchFile() = default;
    IdPairScratchFile(const IdPairScratchFile&) = delete;
    IdPairScratchFile& operator=(const IdPairScratchFile&) = delete;
    IdPairScratchFile(IdPairScratchFile&&) noexcept = default;
    IdPairScratchFile& operator=(IdPairScratchFile&&) noexcept = default;
    ~IdPairScratchFile() = default;

    bool opened() const noexcept { return file_.opened(); }
    void reset() noexcept { file_.clear(); }
    void open(const std::string& directory) {
        if (opened()) return;
        try {
            file_.set_directory(directory);
            file_.open("pineforge-id-pairs");
        } catch (const ScratchIoError& error) {
            throw IdPairStoreError(error.code().value(), "id table create");
        }
    }
    void resize(std::uint64_t bytes) {
        check_range(0, bytes);
        if (!opened()) throw IdPairStoreError(EBADF, "id table resize");
        try {
            file_.ensure_size(bytes);
        } catch (const ScratchIoError& error) {
            throw IdPairStoreError(error.code().value(), "id table resize");
        }
    }
    void read(std::uint64_t offset, void* destination, std::size_t length) const {
        check_range(offset, length);
        if (length != 0 && !opened()) throw IdPairStoreError(EBADF, "id table read");
        try {
            file_.read(offset, destination, length);
        } catch (const ScratchIoError& error) {
            throw IdPairStoreError(error.code().value(), "id table read");
        }
    }
    void write(std::uint64_t offset, const void* source, std::size_t length) {
        check_range(offset, length);
        if (length != 0 && !opened()) throw IdPairStoreError(EBADF, "id table write");
        try {
            file_.write(offset, source, length);
        } catch (const ScratchIoError& error) {
            throw IdPairStoreError(error.code().value(), "id table write");
        }
    }

private:
    static void check_range(std::uint64_t offset, std::uint64_t length) {
        constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        if (offset > maximum || length > maximum - offset)
            throw IdPairStoreError(EFBIG, "id table offset exhausted");
    }
    ScratchFile file_;
};

// One 40-byte record plus both strings per distinct key ever seen, and a
// 16-byte on-disk hash slot at <= 1/2 load (32..64 bytes/key after warmup).
// Replacing a value, erasing and recurring reuse the original record.
// Single keys and pairs are separate domains; ("x", "") is not single "x".
// Hash collisions are always resolved by comparing the complete byte strings.
// For N historical distinct singles and pairs, data D is the sum of
// (40 + |id|) per single and (40 + |id| + |entry|) per pair. Index bytes are
// 16*S, with S=0 for N=0, otherwise the least 64*2^k >= 2*N (k >= 0).
// For Q=1 MiB and R(x)=Q*ceil(x/Q), arena steady use is R(D)+R(16*S).
// Rebuilding S to 2*S peaks at R(D_before_append)+R(16*S)+R(32*S);
// initial construction has no old index. Admission takes the maximum of
// these rebuild peaks and the steady use after each distinct-key append.
template<class Value>
class IdPairStore {
    static_assert(std::is_integral_v<Value> && sizeof(Value) == 8,
                  "id auxiliary values are fixed-width 64-bit integers");
    using MemoryKey = std::pair<std::string, std::optional<std::string>>;
    using MemoryMap = std::map<MemoryKey, Value>;

    struct Slot {
        std::uint64_t hash = 0;
        std::uint64_t offset_plus_one = 0;
    };
    struct Header {
        std::uint64_t hash = 0;
        std::uint64_t first_size = 0;
        std::uint64_t second_size = 0;
        Value value = 0;
        std::uint64_t flags = 0;
    };
    static_assert(sizeof(Slot) == 16 && sizeof(Header) == 40,
                  "id scratch layout must have no padding");
    static constexpr std::uint64_t kLive = 1;
    static constexpr std::uint64_t kPair = 2;
    static constexpr std::uint64_t kInitialSlots = 64;

public:
    struct Entry {
        std::string first;
        Value second = 0;
        std::optional<std::string> from_entry;
    };

    // Streaming iteration owns only its current strings. It is invalidated
    // by mutation; no adapter caller keeps an iterator across a write.
    class const_iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = Entry;
        using difference_type = std::ptrdiff_t;
        using pointer = const Entry*;
        using reference = const Entry&;

        const_iterator() = default;
        reference operator*() const { return *entry_; }
        pointer operator->() const { return &*entry_; }
        const_iterator& operator++() {
            if (owner_->memory_) {
                ++memory_at_;
                load_memory();
            } else {
                offset_ = next_;
                load_disk();
            }
            return *this;
        }
        const_iterator operator++(int) {
            const_iterator before = *this;
            ++*this;
            return before;
        }
        bool operator==(const const_iterator& other) const {
            if (owner_ != other.owner_) return false;
            if (!entry_ || !other.entry_) return !entry_ && !other.entry_;
            return owner_->memory_ ? memory_at_ == other.memory_at_ : offset_ == other.offset_;
        }
        bool operator!=(const const_iterator& other) const { return !(*this == other); }

    private:
        friend class IdPairStore;
        const_iterator(const IdPairStore* owner, bool end) : owner_(owner) {
            if (end) return;
            if (owner_->memory_) {
                memory_at_ = owner_->memory_->begin();
                load_memory();
            } else {
                load_disk();
            }
        }
        void load_memory() {
            entry_.reset();
            if (memory_at_ != owner_->memory_->end())
                entry_.emplace(Entry{memory_at_->first.first, memory_at_->second,
                                      memory_at_->first.second});
        }
        void load_disk() {
            entry_.reset();
            while (offset_ < owner_->data_bytes_) {
                const Header header = owner_->read_header(offset_);
                next_ = offset_ + sizeof(Header) + header.first_size + header.second_size;
                if ((header.flags & kLive) != 0) {
                    Entry row;
                    row.first.resize(static_cast<std::size_t>(header.first_size));
                    owner_->data_.read(offset_ + sizeof(Header), row.first.data(), row.first.size());
                    row.second = header.value;
                    if ((header.flags & kPair) != 0) {
                        row.from_entry.emplace(static_cast<std::size_t>(header.second_size), '\0');
                        owner_->data_.read(offset_ + sizeof(Header) + header.first_size,
                                          row.from_entry->data(), row.from_entry->size());
                    }
                    entry_.emplace(std::move(row));
                    return;
                }
                offset_ = next_;
            }
        }
        const IdPairStore* owner_ = nullptr;
        std::uint64_t offset_ = 0;
        std::uint64_t next_ = 0;
        typename MemoryMap::const_iterator memory_at_{};
        std::optional<Entry> entry_;
    };

    IdPairStore() = default;
    IdPairStore(const IdPairStore&) = delete;
    IdPairStore& operator=(const IdPairStore&) = delete;
    IdPairStore(IdPairStore&& other) noexcept { swap(other); }
    IdPairStore& operator=(IdPairStore&& other) noexcept {
        if (this != &other) {
            IdPairStore moved(std::move(other));
            swap(moved);
        }
        return *this;
    }

    void set_directory(const std::string& directory) {
        if ((data_.opened() || index_.opened()) && directory != directory_)
            throw std::logic_error("id table directory changed during run");
        directory_ = directory;
    }
    void clear() noexcept {
        data_.reset();
        index_.reset();
        data_bytes_ = 0;
        slots_ = 0;
        used_ = 0;
        size_ = 0;
        if (memory_) memory_->clear();
    }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    std::uint64_t disk_bytes() const noexcept { return data_bytes_ + slots_ * sizeof(Slot); }
    const_iterator begin() const { return const_iterator(this, false); }
    const_iterator end() const { return const_iterator(this, true); }

    // An explicit, per-instance fixture seam; production always uses disk.
    // No global switch, environment knob, cached key roster or fallback on IO
    // failure. clear() preserves this choice for a second run on the host.
    void fixture_use_memory_reference() {
        if (used_ != 0 || size_ != 0 || data_.opened() || index_.opened())
            throw std::logic_error("id table fixture backend changed after use");
        memory_ = std::make_unique<MemoryMap>();
    }

    std::optional<Value> lookup(std::string_view id) const { return lookup_key(id, std::nullopt); }
    std::optional<Value> lookup(std::string_view id, std::string_view from_entry) const {
        return lookup_key(id, from_entry);
    }
    Value at(const std::string& id) const {
        const auto found = lookup(id);
        if (!found) throw std::out_of_range("id auxiliary key is absent");
        return *found;
    }
    bool insert_or_assign(std::string_view id, Value value) {
        return assign_key(id, std::nullopt, value);
    }
    bool insert_or_assign(std::string_view id, std::string_view from_entry, Value value) {
        return assign_key(id, from_entry, value);
    }
    void min_assign(std::string_view id, Value value) {
        const auto old = lookup(id);
        if (!old || value < *old) insert_or_assign(id, value);
    }
    void min_assign(std::string_view id, std::string_view from_entry, Value value) {
        const auto old = lookup(id, from_entry);
        if (!old || value < *old) insert_or_assign(id, from_entry, value);
    }
    bool erase(std::string_view id) { return erase_key(id, std::nullopt); }
    bool erase(std::string_view id, std::string_view from_entry) {
        return erase_key(id, from_entry);
    }

private:
    void swap(IdPairStore& other) noexcept {
        using std::swap;
        swap(data_, other.data_);
        swap(index_, other.index_);
        swap(directory_, other.directory_);
        swap(data_bytes_, other.data_bytes_);
        swap(slots_, other.slots_);
        swap(used_, other.used_);
        swap(size_, other.size_);
        swap(memory_, other.memory_);
    }
    static MemoryKey memory_key(std::string_view first, std::optional<std::string_view> second) {
        return {std::string(first), second ? std::optional<std::string>{std::string(*second)}
                                           : std::nullopt};
    }
    static std::uint64_t key_hash(std::string_view first,
                                  std::optional<std::string_view> second) noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
        const auto byte = [&hash](unsigned char value) {
            hash ^= value;
            hash *= 1099511628211ULL;
        };
        const auto string = [&byte](std::string_view value) {
            const auto length = static_cast<std::uint64_t>(value.size());
            for (unsigned shift = 0; shift < 64; shift += 8)
                byte(static_cast<unsigned char>(length >> shift));
            for (const unsigned char value_byte : value) byte(value_byte);
        };
        byte(second ? 1 : 0);
        string(first);
        if (second) string(*second);
        return hash;
    }
    Header read_header(std::uint64_t offset) const {
        if (offset > data_bytes_ || data_bytes_ - offset < sizeof(Header))
            throw IdPairStoreError(EIO, "id table truncated record");
        Header header;
        data_.read(offset, &header, sizeof(header));
        const auto available = data_bytes_ - offset - sizeof(Header);
        if (header.first_size > available || header.second_size > available - header.first_size
            || (header.flags & ~(kLive | kPair)) != 0
            || ((header.flags & kPair) == 0 && header.second_size != 0))
            throw IdPairStoreError(EIO, "id table invalid record");
        return header;
    }
    bool equal_string(std::uint64_t offset, std::uint64_t length, std::string_view key) const {
        if (length != key.size()) return false;
        std::array<char, 512> buffer{};
        for (std::size_t at = 0; at < key.size();) {
            const auto count = std::min(buffer.size(), key.size() - at);
            data_.read(offset + at, buffer.data(), count);
            if (key.compare(at, count, buffer.data(), count) != 0) return false;
            at += count;
        }
        return true;
    }
    // The first empty slot or the exact key's slot. Dead records retain
    // their slot, so erasing cannot break another key's probe chain.
    std::pair<std::uint64_t, Slot> locate(std::string_view first,
            std::optional<std::string_view> second, std::uint64_t hash) const {
        std::uint64_t at = hash & (slots_ - 1);
        for (std::uint64_t probes = 0; probes < slots_; ++probes) {
            Slot slot;
            index_.read(at * sizeof(Slot), &slot, sizeof(slot));
            if (slot.offset_plus_one == 0) return {at, slot};
            if (slot.hash == hash) {
                const auto offset = slot.offset_plus_one - 1;
                const Header header = read_header(offset);
                if (header.hash != slot.hash)
                    throw IdPairStoreError(EIO, "id table mismatched index");
                if (((header.flags & kPair) != 0) == second.has_value()
                    && equal_string(offset + sizeof(Header), header.first_size, first)
                    && (!second || equal_string(offset + sizeof(Header) + header.first_size,
                                                 header.second_size, *second)))
                    return {at, slot};
            }
            at = (at + 1) & (slots_ - 1);
        }
        throw IdPairStoreError(EIO, "id table full index");
    }
    static void place_slot(IdPairScratchFile& index, std::uint64_t slots, const Slot& slot) {
        std::uint64_t at = slot.hash & (slots - 1);
        for (std::uint64_t probes = 0; probes < slots; ++probes) {
            Slot existing;
            index.read(at * sizeof(Slot), &existing, sizeof(existing));
            if (existing.offset_plus_one == 0) {
                index.write(at * sizeof(Slot), &slot, sizeof(slot));
                return;
            }
            at = (at + 1) & (slots - 1);
        }
        throw IdPairStoreError(EIO, "id table rehash overflow");
    }
    void grow_index() {
        constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())
            / sizeof(Slot);
        if (slots_ > maximum / 2) throw IdPairStoreError(EFBIG, "id table index exhausted");
        const auto next_slots = slots_ == 0 ? kInitialSlots : slots_ * 2;
        IdPairScratchFile next;
        next.open(directory_);
        next.resize(next_slots * sizeof(Slot)); // fresh sparse bytes read as zero
        std::array<Slot, 128> buffer{};
        for (std::uint64_t at = 0; at < slots_;) {
            const auto count = std::min<std::uint64_t>(buffer.size(), slots_ - at);
            index_.read(at * sizeof(Slot), buffer.data(),
                        static_cast<std::size_t>(count) * sizeof(Slot));
            for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i)
                if (buffer[i].offset_plus_one != 0) place_slot(next, next_slots, buffer[i]);
            at += count;
        }
        // ScratchFile move assignment clears the old region immediately,
        // before taking the completed new region; no third index is held.
        index_ = std::move(next); // publish only a complete replacement
        slots_ = next_slots;
    }
    std::optional<Value> lookup_key(std::string_view first,
                                    std::optional<std::string_view> second) const {
        if (memory_) {
            const auto found = memory_->find(memory_key(first, second));
            return found == memory_->end() ? std::nullopt : std::optional<Value>{found->second};
        }
        if (slots_ == 0) return std::nullopt;
        const auto found = locate(first, second, key_hash(first, second)).second;
        if (found.offset_plus_one == 0) return std::nullopt;
        const Header header = read_header(found.offset_plus_one - 1);
        return (header.flags & kLive) != 0 ? std::optional<Value>{header.value} : std::nullopt;
    }
    bool assign_key(std::string_view first, std::optional<std::string_view> second, Value value) {
        if (memory_) {
            const auto inserted = memory_->insert_or_assign(memory_key(first, second), value);
            if (inserted.second) ++size_;
            return inserted.second;
        }
        const auto hash = key_hash(first, second);
        if (slots_ == 0) grow_index();
        auto found = locate(first, second, hash);
        if (found.second.offset_plus_one != 0) {
            const auto offset = found.second.offset_plus_one - 1;
            Header header = read_header(offset);
            const bool inserted = (header.flags & kLive) == 0;
            header.value = value;
            header.flags |= kLive;
            data_.write(offset, &header, sizeof(header));
            if (inserted) ++size_;
            return inserted;
        }
        if (used_ >= slots_ / 2) {
            grow_index();
            found = locate(first, second, hash);
        }
        if (size_ == std::numeric_limits<std::size_t>::max())
            throw IdPairStoreError(EFBIG, "id table size exhausted");
        const auto second_size = second ? second->size() : 0;
        constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        if (data_bytes_ > maximum - sizeof(Header)
            || first.size() > maximum - data_bytes_ - sizeof(Header)
            || second_size > maximum - data_bytes_ - sizeof(Header) - first.size())
            throw IdPairStoreError(EFBIG, "id table data exhausted");
        Header header{hash, static_cast<std::uint64_t>(first.size()),
                      static_cast<std::uint64_t>(second_size), value, kLive | (second ? kPair : 0)};
        data_.open(directory_);
        data_.write(data_bytes_, &header, sizeof(header));
        data_.write(data_bytes_ + sizeof(Header), first.data(), first.size());
        if (second)
            data_.write(data_bytes_ + sizeof(Header) + first.size(), second->data(), second->size());
        const Slot slot{hash, data_bytes_ + 1};
        index_.write(found.first * sizeof(Slot), &slot, sizeof(slot));
        data_bytes_ += sizeof(Header) + first.size() + second_size;
        ++used_;
        ++size_;
        return true;
    }
    bool erase_key(std::string_view first, std::optional<std::string_view> second) {
        if (memory_) {
            const auto count = memory_->erase(memory_key(first, second));
            size_ -= count;
            return count != 0;
        }
        if (slots_ == 0) return false;
        const auto found = locate(first, second, key_hash(first, second)).second;
        if (found.offset_plus_one == 0) return false;
        const auto offset = found.offset_plus_one - 1;
        Header header = read_header(offset);
        if ((header.flags & kLive) == 0) return false;
        header.flags &= ~kLive;
        data_.write(offset, &header, sizeof(header));
        --size_;
        return true;
    }

    IdPairScratchFile data_;
    IdPairScratchFile index_;
    std::string directory_;
    std::uint64_t data_bytes_ = 0;
    std::uint64_t slots_ = 0;
    std::uint64_t used_ = 0; // distinct keys including erased keys
    std::size_t size_ = 0;  // live keys
    std::unique_ptr<MemoryMap> memory_; // allocated only by the explicit fixture seam
};

} // namespace pineforge::detail
