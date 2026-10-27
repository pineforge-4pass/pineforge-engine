#pragma once

// CR-150 L19l-C. Exact string / string-pair tables with no per-key RAM
// directory. Like L19a's successor-root file, scratch files are private,
// unlinked at creation, close-on-exec, and accessed with pread/pwrite.
#include <pineforge/detail/id_pair_ordered_fold.hpp>
#include <pineforge/detail/id_pair_slab_fold.hpp>
#include <pineforge/detail/scratch_slab.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace pineforge::detail {

// IdPairScratchFile and its errno-preserving auxiliary failure live in
// id_pair_scratch_file.hpp, shared with the ordered-fold implementation above.
// One 40-byte record plus both strings per distinct key ever seen, and a
// 16-byte on-disk hash slot at <= 1/2 load (32..64 bytes/key after warmup).
// Replacing a value, erasing and recurring reuse the original record.
// Single keys and pairs are separate domains; ("x", "") is not single "x".
// Hash collisions are always resolved by comparing the complete byte strings.
template<class Value, bool MaintainSortedFold = false>
class IdPairStore {
public:
    // Observe the actual comparison and byte-fold sites, including the old
    // slab selection loop, so a lab can measure fail-before with these
    // observation-only hunks applied to the previous implementation.
    struct FixtureFoldWork {
        std::uint64_t row_comparisons = 0;
        std::uint64_t byte_steps = 0; // All 257 seeds, not just input bytes.
        std::uint64_t compositions = 0;
    };

private:
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

    // Opt-in only for the consumed-cycle single-key table. Ordinary minima
    // tables keep their existing disk layout, lookup and iteration paths.
    struct NoFold {};
    struct FoldState {
        IdPairOrderedFold ordered;
        IdPairSlabFold bounded;
        Header pending_header;
        Slot pending_slot;
        std::uint64_t pending_offset = 0, pending_at = 0;
        bool pending = false, slot_pending = false;
        FixtureFoldWork* fixture_work = nullptr;
    };

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
                    owner_->read_data(offset_ + sizeof(Header), row.first.data(), row.first.size());
                    row.second = header.value;
                    if ((header.flags & kPair) != 0) {
                        row.from_entry.emplace(static_cast<std::size_t>(header.second_size), '\0');
                        owner_->read_data(offset_ + sizeof(Header) + header.first_size,
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
        slots_ = slab_ ? fixed_slots_ : 0;
        used_ = 0;
        size_ = 0;
        if constexpr (MaintainSortedFold) {
            fold_.ordered.clear();
            fold_.bounded.reset();
            fold_.pending = fold_.slot_pending = false;
        }
        if (memory_) memory_->clear();
        if (slab_) {
            slab_->reset(data_resource_);
            slab_->reset(index_resource_);
            slab_->extend(index_resource_, slots_ * sizeof(Slot));
        }
    }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    std::uint64_t disk_bytes() const noexcept {
        if (slab_) return 0;
        const auto table = data_bytes_ + slots_ * sizeof(Slot);
        if constexpr (MaintainSortedFold) {
            const auto ordered = fold_.ordered.disk_bytes();
            const auto maximum = std::numeric_limits<std::uint64_t>::max();
            return ordered > maximum - table ? maximum : table + ordered;
        }
        return table;
    }
    const_iterator begin() const { return const_iterator(this, false); }
    const_iterator end() const { return const_iterator(this, true); }

    // Bind once after reset, before the first key. Exactly one fixed index
    // and one data slice; clear() reuses them without dropping their owner.
    // An empty owner restores disk mode (an explicit fixture oracle survives).
    void bind_scratch_tables(const std::shared_ptr<ScratchSlab>& slab,
                             ScratchResource data, ScratchResource index) {
        if (used_ != 0 || size_ != 0 || data_.opened() || index_.opened())
            throw std::logic_error("id table scratch backend changed after use");
        if (slab && memory_)
            throw std::logic_error("id table fixture oracle cannot bind production scratch");
        std::uint64_t limit = 0;
        if (data == ScratchResource::erased_data && index == ScratchResource::erased_index)
            limit = slab ? slab->manifest().K_e : 0;
        else if (data == ScratchResource::consumed_data && index == ScratchResource::consumed_index)
            limit = slab ? slab->manifest().K_c : 0;
        else
            throw std::logic_error("id table scratch resource pair is invalid");
        const auto count = scratch_layout_detail::slots(limit);
        if (slab) slab->require_extend(index, count * sizeof(Slot));
        if constexpr (MaintainSortedFold) {
            if (slab) {
                fold_.bounded.configure(slab->region(data).capacity, limit, count);
                // Fits the OLD 16*Q budget for every admitted manifest.
                slab->require_extend(index, fold_.bounded.storage_bytes());
            }
        }
        slab_ = slab;
        data_resource_ = data;
        index_resource_ = index;
        key_limit_ = limit;
        fixed_slots_ = count;
        clear();
    }

    struct KeyView {
        std::string_view first;
        std::optional<std::string_view> second;
    };

    // enumerate(emit) is a repeatable, borrowed workset view. Revisit its
    // prefix to deduplicate genuinely new ever-keys, including both domains.
    // O(n^2) key comparisons, O(1) workspace; no growing pending-key directory.
    // Neither a successful ticket nor a refusal changes extents or bytes.
    template<class Enumerate>
    void preflight_keys(const Enumerate& enumerate) const {
        if (!slab_) return;
        std::uint64_t keys = used_;
        std::uint64_t extent = data_bytes_;
        std::size_t ordinal = 0;
        enumerate([&](KeyView key) {
            auto first = key.first;
            auto second = key.second;
            if (data_resource_ == ScratchResource::consumed_data && second
                && (first.find('\x1f') != std::string_view::npos
                    || second->find('\x1f') != std::string_view::npos)) {
                throw ScratchCapacityError(data_resource_,
                    "order ids cannot contain the control character 0x1f");
            }
            if (data_resource_ == ScratchResource::consumed_data && !second) {
                // Consumed singles join two ids with '\x1f'. A boundary
                // after at most 64 bytes is safe because the source id
                // components are rejected before this representation is made.
                const auto separator = first.rfind('\x1f', 64);
                if (separator != std::string_view::npos) {
                    second = first.substr(separator + 1);
                    first = first.substr(0, separator);
                }
            }
            require_id_components(first.size(), second ? second->size() : 0);
            const auto own_ordinal = ordinal++;
            if (slots_ != 0 && locate(key.first, key.second,
                                     hash_for_key(key.first, key.second)).second.offset_plus_one != 0)
                return; // Updates and resurrection consume no new bytes/slots.
            bool duplicate = false;
            std::size_t prior = 0;
            enumerate([&](KeyView earlier) {
                if (prior++ < own_ordinal && earlier.first == key.first
                    && earlier.second == key.second) duplicate = true;
            });
            if (duplicate) return;
            // Q=0 refuses here, before any Q-1 mask or probing.
            preflight_new_key(keys, extent, key.first.size(),
                              key.second ? key.second->size() : 0);
        });
    }
    void preflight_assign(std::string_view first,
                          std::optional<std::string_view> second = std::nullopt) const {
        preflight_keys([&](const auto& emit) { emit(KeyView{first, second}); });
    }

    // A single key formed by first + separator + last, not the pair domain.
    // PRECOMMIT can inspect the existing consumed-cycle string without making
    // a temporary string allocation (including for an eventual refusal).
    void preflight_joined_single(std::string_view first, char separator,
                                 std::string_view last) const {
        if (!slab_) return;
        if (data_resource_ == ScratchResource::consumed_data
            && (first.find('\x1f') != std::string_view::npos
                || last.find('\x1f') != std::string_view::npos)) {
            throw ScratchCapacityError(data_resource_,
                "order ids cannot contain the control character 0x1f");
        }
        require_id_components(first.size(), last.size());
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (first.size() >= maximum || last.size() > maximum - first.size() - 1)
            throw ScratchCapacityError(data_resource_, scratch_byte_limit_message(data_resource_,
                data_resource_ == ScratchResource::erased_data
                    ? slab_->manifest().B_e : slab_->manifest().B_c));
        const auto length = static_cast<std::uint64_t>(first.size()) + 1 + last.size();
        std::uint64_t hash = 1469598103934665603ULL;
        const auto byte = [&](unsigned char value) { hash ^= value; hash *= 1099511628211ULL; };
        byte(0); // The single-string domain used by key_hash.
        for (unsigned shift = 0; shift < 64; shift += 8)
            byte(static_cast<unsigned char>(length >> shift));
        for (const unsigned char value : first) byte(value);
        byte(static_cast<unsigned char>(separator));
        for (const unsigned char value : last) byte(value);
        if (fixture_hash_) hash = *fixture_hash_;
        if (slots_ != 0) {
            const auto found = locate_hash(hash, [&](std::uint64_t offset, const Header& header) {
                return (header.flags & kPair) == 0 && header.first_size == length
                    && equal_string(offset + sizeof(Header), first.size(), first)
                    && equal_string(offset + sizeof(Header) + first.size(), 1,
                                    std::string_view(&separator, 1))
                    && equal_string(offset + sizeof(Header) + first.size() + 1,
                                    last.size(), last);
            });
            if (found.second.offset_plus_one != 0) return;
        }
        auto keys = used_;
        auto extent = data_bytes_;
        preflight_new_key(keys, extent, length, 0);
    }

    // The count and byte spelling are exactly the former hash_state loop.
    // The explicit memory fixture remains an independent sorted reference.
    void fold_sorted(BrokerStateHashSink& sink) const {
        static_assert(MaintainSortedFold, "ordered fold must be selected for this table");
        sink.u(size_);
        if (memory_) {
            for (const auto& row : *memory_) {
                sink.s(row.first.first);
                sink.i(row.second);
            }
        } else if (slab_) {
            sink.h = fold_.bounded.apply(sink.h);
        } else {
            sink.h = fold_.ordered.apply(sink.h);
        }
    }
    void fixture_count_io(IdPairScratchFile::FixtureIO* io) noexcept {
        data_.fixture_count_io(io); index_.fixture_count_io(io);
        if constexpr (MaintainSortedFold) fold_.ordered.fixture_count_io(io);
    }
    void fixture_count_fold_work(FixtureFoldWork* work) noexcept {
        static_assert(MaintainSortedFold, "fold work requires the ordered-fold table");
        fold_.fixture_work = work;
    }

    // Explicit per-instance fixture seams, never chosen by a production binder.
    // No global switch, environment knob, cached key roster or fallback on IO
    // failure. clear() preserves this choice for a second run on the host.
    void fixture_use_memory_reference() {
        if (slab_ || used_ != 0 || size_ != 0 || data_.opened() || index_.opened())
            throw std::logic_error("id table fixture backend changed after use");
        memory_ = std::make_unique<MemoryMap>();
    }
    void fixture_force_hash_collisions(std::uint64_t hash) {
        if (used_ != 0 || size_ != 0 || data_.opened() || index_.opened())
            throw std::logic_error("id table fixture hash changed after use");
        fixture_hash_ = hash;
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
        static_assert(!MaintainSortedFold, "consumed-cycle sorted fold accepts single keys");
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
    void require_id_components(std::size_t first, std::size_t second) const {
        if (first > 64 || second > 64)
            throw ScratchCapacityError(data_resource_, "id longer than 64 bytes");
    }
    void preflight_new_key(std::uint64_t& keys, std::uint64_t& extent,
                           std::uint64_t first, std::uint64_t second) const {
        if (keys >= key_limit_ || keys >= slots_ / 2
            || keys >= std::numeric_limits<std::size_t>::max())
            throw ScratchCapacityError(index_resource_,
                scratch_count_limit_message(index_resource_, std::min(key_limit_, slots_ / 2)));
        ++keys;
        const auto append = [&](std::uint64_t bytes) {
            const auto capacity = slab_->region(data_resource_).capacity;
            if (extent > capacity || bytes > capacity - extent)
                throw ScratchCapacityError(data_resource_,
                    scratch_byte_limit_message(data_resource_,
                        data_resource_ == ScratchResource::erased_data
                            ? slab_->manifest().B_e : slab_->manifest().B_c));
            extent += bytes;
        };
        append(sizeof(Header));
        append(first);
        append(second);
    }
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
        swap(slab_, other.slab_);
        swap(data_resource_, other.data_resource_);
        swap(index_resource_, other.index_resource_);
        swap(key_limit_, other.key_limit_);
        swap(fixed_slots_, other.fixed_slots_);
        swap(fixture_hash_, other.fixture_hash_);
        swap(fold_, other.fold_);
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
    std::uint64_t hash_for_key(std::string_view first,
                              std::optional<std::string_view> second) const noexcept {
        return fixture_hash_ ? *fixture_hash_ : key_hash(first, second);
    }
    void read_data(std::uint64_t offset, void* destination, std::size_t length) const {
        if (slab_) slab_->read_at(data_resource_, offset, destination, length);
        else data_.read(offset, destination, length);
    }
    void write_data(std::uint64_t offset, const void* source, std::size_t length) {
        if (slab_) slab_->write_at(data_resource_, offset, source, length);
        else data_.write(offset, source, length);
    }
    void read_index(std::uint64_t offset, void* destination, std::size_t length) const {
        if constexpr (MaintainSortedFold) {
            if (slab_) {
                Slot slot;
                slot.offset_plus_one = fold_.bounded.read_slot(*slab_, index_resource_, offset / sizeof(Slot));
                if (slot.offset_plus_one != 0)
                    read_data(slot.offset_plus_one - 1, &slot.hash, sizeof(slot.hash));
                std::memcpy(destination, &slot, length);
                return;
            }
        }
        if (slab_) slab_->read_at(index_resource_, offset, destination, length);
        else index_.read(offset, destination, length);
    }
    void write_index(std::uint64_t offset, const void* source, std::size_t length) {
        if constexpr (MaintainSortedFold) {
            if (slab_) {
                Slot slot;
                std::memcpy(&slot, source, length);
                fold_.bounded.write_slot(*slab_, index_resource_, offset / sizeof(Slot), slot.offset_plus_one);
                return;
            }
        }
        if (slab_) slab_->write_at(index_resource_, offset, source, length);
        else index_.write(offset, source, length);
    }
    Header read_header(std::uint64_t offset) const {
        if (offset > data_bytes_ || data_bytes_ - offset < sizeof(Header))
            throw IdPairStoreError(EIO, "id table truncated record");
        Header header;
        if constexpr (MaintainSortedFold) {
            if (fold_.pending && offset == fold_.pending_offset) header = fold_.pending_header;
            else read_data(offset, &header, sizeof(header));
        } else {
            read_data(offset, &header, sizeof(header));
        }
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
            read_data(offset + at, buffer.data(), count);
            if (key.compare(at, count, buffer.data(), count) != 0) return false;
            at += count;
        }
        return true;
    }
    // The first empty slot or the exact key's slot. Dead records retain
    // their slot, so erasing cannot break another key's probe chain.
    std::pair<std::uint64_t, Slot> locate(std::string_view first,
            std::optional<std::string_view> second, std::uint64_t hash) const {
        return locate_hash(hash, [&](std::uint64_t offset, const Header& header) {
            return ((header.flags & kPair) != 0) == second.has_value()
                && equal_string(offset + sizeof(Header), header.first_size, first)
                && (!second || equal_string(offset + sizeof(Header) + header.first_size,
                                             header.second_size, *second));
        });
    }
    template<class Equal>
    std::pair<std::uint64_t, Slot> locate_hash(std::uint64_t hash, const Equal& equal) const {
        std::uint64_t at = hash & (slots_ - 1);
        for (std::uint64_t probes = 0; probes < slots_; ++probes) {
            Slot slot;
            if constexpr (MaintainSortedFold) {
                if (fold_.slot_pending && at == fold_.pending_at) slot = fold_.pending_slot;
                else read_index(at * sizeof(Slot), &slot, sizeof(slot));
            } else {
                read_index(at * sizeof(Slot), &slot, sizeof(slot));
            }
            if (slot.offset_plus_one == 0) return {at, slot};
            if (slot.hash == hash) {
                const auto offset = slot.offset_plus_one - 1;
                const Header header = read_header(offset);
                if (header.hash != slot.hash)
                    throw IdPairStoreError(EIO, "id table mismatched index");
                if (equal(offset, header)) return {at, slot};
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
        IdPairScratchFile next(MaintainSortedFold
            ? ScratchStore::partial_cycles_idx : ScratchStore::erased_minima_idx);
        next.fixture_count_io(index_.fixture_io());
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
        const auto found = locate(first, second, hash_for_key(first, second)).second;
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
        // Bounded admission precedes disk growth, record writes and counters.
        preflight_assign(first, second);
        if constexpr (MaintainSortedFold) flush_pending();
        const auto hash = hash_for_key(first, second);
        if (!slab_ && slots_ == 0) grow_index();
        auto found = locate(first, second, hash);
        if (found.second.offset_plus_one != 0) {
            const auto offset = found.second.offset_plus_one - 1;
            Header header = read_header(offset);
            const bool inserted = (header.flags & kLive) == 0;
            header.value = value;
            header.flags |= kLive;
            if constexpr (MaintainSortedFold) publish_fold(first, offset, header);
            else write_data(offset, &header, sizeof(header));
            if (inserted) ++size_;
            return inserted;
        }
        if (used_ >= slots_ / 2) {
            if (slab_) throw ScratchCapacityError(index_resource_,
                scratch_count_limit_message(index_resource_, std::min(key_limit_, slots_ / 2)));
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
        if (!slab_) data_.open(directory_);
        write_data(data_bytes_, &header, sizeof(header));
        write_data(data_bytes_ + sizeof(Header), first.data(), first.size());
        if (second)
            write_data(data_bytes_ + sizeof(Header) + first.size(), second->data(), second->size());
        const Slot slot{hash, data_bytes_ + 1};
        if constexpr (MaintainSortedFold) publish_fold(first, data_bytes_, header, found.first, slot);
        else write_index(found.first * sizeof(Slot), &slot, sizeof(slot));
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
        if constexpr (MaintainSortedFold) flush_pending();
        if (slots_ == 0) return false;
        const auto found = locate(first, second, hash_for_key(first, second)).second;
        if (found.offset_plus_one == 0) return false;
        const auto offset = found.offset_plus_one - 1;
        Header header = read_header(offset);
        if ((header.flags & kLive) == 0) return false;
        header.flags &= ~kLive;
        if constexpr (MaintainSortedFold) publish_fold(first, offset, header);
        else write_data(offset, &header, sizeof(header));
        --size_;
        return true;
    }

    // Exactly one committed header and (on append) index slot may be pending
    // in fixed RAM. Every read consults them. Flush BOTH before discarding
    // either overlay: a partial header/slot write cannot expose mixed state.
    // This also avoids relying on pwrite atomicity or a fallible rollback
    // after the ordered root has been published.
    void flush_pending() {
        if (!fold_.pending) return;
        write_data(fold_.pending_offset, &fold_.pending_header, sizeof(Header));
        if (fold_.slot_pending)
            write_index(fold_.pending_at * sizeof(Slot), &fold_.pending_slot, sizeof(Slot));
        fold_.pending = fold_.slot_pending = false;
    }

    struct FoldReader {
        const IdPairStore& owner;
        std::uint64_t changed;
        const Header& prospective;
        std::array<unsigned char, 512> a{}, b{};

        Header header(std::uint64_t row) const {
            return row == changed ? prospective : owner.read_header(row);
        }
        unsigned symbol(std::uint64_t row, std::uint64_t at) const {
            const auto h = header(row);
            if (at >= h.first_size) return 0;
            unsigned char byte = 0;
            owner.read_data(row + sizeof(Header) + at, &byte, 1);
            return static_cast<unsigned>(byte) + 1;
        }
        int compare(std::uint64_t row, std::string_view key) {
            if (owner.fold_.fixture_work) ++owner.fold_.fixture_work->row_comparisons;
            const auto h = header(row);
            const auto common = std::min<std::uint64_t>(h.first_size, key.size());
            for (std::uint64_t at = 0; at < common;) {
                const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(a.size(), common - at));
                owner.read_data(row + sizeof(Header) + at, a.data(), n);
                const int order = std::char_traits<char>::compare(
                    reinterpret_cast<const char*>(a.data()), key.data() + at, n);
                if (order != 0) return order;
                at += n;
            }
            return h.first_size < key.size() ? -1 : (h.first_size > key.size() ? 1 : 0);
        }
        int compare_rows(std::uint64_t first, std::uint64_t second) {
            if (owner.fold_.fixture_work) ++owner.fold_.fixture_work->row_comparisons;
            const auto x = header(first), y = header(second);
            const auto common = std::min(x.first_size, y.first_size);
            for (std::uint64_t at = 0; at < common;) {
                const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(a.size(), common - at));
                owner.read_data(first + sizeof(Header) + at, a.data(), n);
                owner.read_data(second + sizeof(Header) + at, b.data(), n);
                const int order = std::char_traits<char>::compare(
                    reinterpret_cast<const char*>(a.data()),
                    reinterpret_cast<const char*>(b.data()), n);
                if (order != 0) return order;
                at += n;
            }
            return x.first_size < y.first_size ? -1 : (x.first_size > y.first_size ? 1 : 0);
        }
        IdPairOrderedFold::Position difference(std::uint64_t row, std::string_view key) {
            const auto h = header(row);
            const auto common = std::min<std::uint64_t>(h.first_size, key.size());
            for (std::uint64_t at = 0; at < common;) {
                const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(a.size(), common - at));
                owner.read_data(row + sizeof(Header) + at, a.data(), n);
                for (std::size_t i = 0; i < n; ++i) {
                    const auto byte = static_cast<unsigned char>(key[at + i]);
                    if (a[i] != byte)
                        return IdPairOrderedFold::differing_symbol(at + i, a[i] + 1U, byte + 1U);
                }
                at += n;
            }
            if (h.first_size == key.size()) throw std::logic_error("id fold duplicate row");
            return IdPairOrderedFold::differing_symbol(common, symbol(row, common),
                                                      IdPairOrderedFold::symbol(key, common));
        }
        IdPairOrderedFold::Position difference(std::uint64_t first, std::uint64_t second) {
            const auto x = header(first), y = header(second);
            const auto common = std::min(x.first_size, y.first_size);
            for (std::uint64_t at = 0; at < common;) {
                const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(a.size(), common - at));
                owner.read_data(first + sizeof(Header) + at, a.data(), n);
                owner.read_data(second + sizeof(Header) + at, b.data(), n);
                for (std::size_t i = 0; i < n; ++i)
                    if (a[i] != b[i])
                        return IdPairOrderedFold::differing_symbol(at + i, a[i] + 1U, b[i] + 1U);
                at += n;
            }
            if (x.first_size == y.first_size) throw std::logic_error("id fold duplicate row");
            return IdPairOrderedFold::differing_symbol(common, symbol(first, common), symbol(second, common));
        }
        void bytes(BrokerStateByteFold& fold, const void* data, std::size_t size) {
            if (owner.fold_.fixture_work)
                owner.fold_.fixture_work->byte_steps += 257 * static_cast<std::uint64_t>(size);
            fold.bytes(data, size);
        }
        void compose(BrokerStateByteFold& before, const BrokerStateByteFold& after) {
            if (owner.fold_.fixture_work) ++owner.fold_.fixture_work->compositions;
            before.then(after);
        }
        void append(std::uint64_t row, BrokerStateByteFold& fold) {
            const auto h = header(row);
            if ((h.flags & kLive) == 0) return;
            // BrokerStateHashSink::s uses u(size), followed by the raw key;
            // ::i uses the native int64 representation. No endian rewrite.
            bytes(fold, &h.first_size, sizeof(h.first_size));
            for (std::uint64_t at = 0; at < h.first_size;) {
                const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(a.size(), h.first_size - at));
                owner.read_data(row + sizeof(Header) + at, a.data(), n);
                bytes(fold, a.data(), n);
                at += n;
            }
            const auto value = static_cast<std::int64_t>(h.value);
            bytes(fold, &value, sizeof(value));
        }
    };
    void publish_fold(std::string_view key, std::uint64_t offset, const Header& header) {
        publish_fold(key, offset, header, 0, Slot{});
    }
    void publish_fold(std::string_view key, std::uint64_t offset, const Header& header,
                      std::uint64_t at, Slot slot) {
        FoldReader reader{*this, offset, header};
        if (slab_) {
            fold_.bounded.update(*slab_, index_resource_, used_,
                                 slot.offset_plus_one != 0, key, offset, reader);
            // Admission already checked these exact spans; slab writes copy
            // without file IO. No pending overlay is needed for this backend.
            write_data(offset, &header, sizeof(header));
            if (slot.offset_plus_one != 0) write_index(at * sizeof(Slot), &slot, sizeof(slot));
            return;
        }
        fold_.ordered.update(key, offset, directory_, reader);
        // The root and table commit together with no more IO or allocation.
        fold_.pending_header = header; fold_.pending_offset = offset;
        fold_.pending_slot = slot; fold_.pending_at = at;
        fold_.pending = true; fold_.slot_pending = slot.offset_plus_one != 0;
    }

    IdPairScratchFile data_{MaintainSortedFold
        ? ScratchStore::partial_cycles : ScratchStore::erased_minima};
    IdPairScratchFile index_{MaintainSortedFold
        ? ScratchStore::partial_cycles_idx : ScratchStore::erased_minima_idx};
    std::string directory_;
    std::uint64_t data_bytes_ = 0;
    std::uint64_t slots_ = 0;
    std::uint64_t used_ = 0; // distinct keys including erased keys
    std::size_t size_ = 0;  // live keys
    std::unique_ptr<MemoryMap> memory_; // allocated only by the explicit fixture seam
    std::shared_ptr<ScratchSlab> slab_;
    ScratchResource data_resource_ = ScratchResource::erased_data;
    ScratchResource index_resource_ = ScratchResource::erased_index;
    std::uint64_t key_limit_ = 0;
    std::uint64_t fixed_slots_ = 0;
    std::optional<std::uint64_t> fixture_hash_; // collision oracle seam, physical only
    std::conditional_t<MaintainSortedFold, FoldState, NoFold> fold_;
};

static_assert(sizeof(IdPairStore<std::int64_t, true>) + IdPairOrderedFold::workspace_bytes() + 8192 <= 32768,
              "consumed-cycle store plus all fixed fold buffers <=32 KiB");

} // namespace pineforge::detail
