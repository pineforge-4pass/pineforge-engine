#pragma once

#include <pineforge/detail/disk_id_table.hpp>
#include <pineforge/native_order_identity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace pineforge::detail {

struct OrderBookEntry {
    native_order::RequestHandle handle;
    bool pending_until_armed = false;
};

// Ordered keys without a specified scratch codec retain the original map
// requirements. In particular, no hash or trivial-serialization requirement
// is imposed on user-defined keys.
template<class Key>
class OrderBookStorage {
public:
    std::optional<OrderBookEntry> find(const Key& key) const {
        const auto at = entries_.find(key);
        if (at == entries_.end()) return std::nullopt;
        return at->second;
    }
    void put(const Key& key, const OrderBookEntry& value) {
        entries_.insert_or_assign(key, value);
    }
    void erase(const Key& key) { entries_.erase(key); }
    std::size_t size() const noexcept { return entries_.size(); }
    std::size_t resident_size() const noexcept { return entries_.size(); }

private:
    std::map<Key, OrderBookEntry> entries_;
};

template<>
class OrderBookStorage<std::string> {
    // Anonymous same-process scratch only: SessionKey's interned token remains
    // valid for the lifetime of these records. Never hash or checkpoint these
    // bytes. Explicit reserved bytes make the LP64 record width unambiguous.
    struct Record {
        native_order::RequestHandle handle;
        std::uint8_t pending_until_armed = 0;
        std::uint8_t bound = 0;
        std::uint8_t reserved[6]{};
    };
    static_assert(sizeof(Record) == 32, "OrderBook scratch record requires LP64");
    static_assert(std::is_trivially_copyable_v<Record>, "OrderBook scratch record");
    using Table = DiskIdTable<Record>;
    using Offset = Table::Offset;
    struct Slot {
        Offset offset = 0;
        Record record{};
    };
    static_assert(sizeof(Slot) == 40, "OrderBook window slot requires LP64");

public:
    static constexpr std::size_t resident_capacity = 64;

    // Scratch opens lazily on the first accepted binding, never at book
    // construction. Other DiskIdTable users retain their own backing policy.
    OrderBookStorage() noexcept = default;
    OrderBookStorage(const OrderBookStorage& other) : OrderBookStorage() {
        // Parent-linked walk holds one key/record at a time. Tombstones have
        // no public identity and need not be copied into the independent file.
        other.table_.for_each_sorted([&](const std::string& key, const Record& record) {
            if (record.bound) put(key, {record.handle, record.pending_until_armed != 0});
        });
    }
    OrderBookStorage& operator=(const OrderBookStorage& other) {
        if (this != &other) {
            OrderBookStorage copy(other);
            *this = std::move(copy);
        }
        return *this;
    }
    OrderBookStorage(OrderBookStorage&& other) noexcept { *this = std::move(other); }
    OrderBookStorage& operator=(OrderBookStorage&& other) noexcept {
        if (this != &other) {
            table_ = std::move(other.table_);
            window_ = other.window_;
            cursor_ = std::exchange(other.cursor_, 0);
            resident_size_ = std::exchange(other.resident_size_, 0);
            bound_size_ = std::exchange(other.bound_size_, 0);
            other.window_ = {};
        }
        return *this;
    }

    std::optional<OrderBookEntry> find(const std::string& key) const {
        const auto at = table_.find(key);
        if (!at) return std::nullopt; // no negative cache or historical RAM index
        const auto record = read(*at);
        if (!record.bound) return std::nullopt;
        return OrderBookEntry{record.handle, record.pending_until_armed != 0};
    }
    void put(const std::string& key, const OrderBookEntry& entry) {
        const Record record{entry.handle,
                            static_cast<std::uint8_t>(entry.pending_until_armed), 1, {}};
        const auto inserted = table_.insert(key, record);
        if (!inserted.second) {
            const bool was_bound = read(inserted.first).bound != 0;
            table_.set(inserted.first, record); // publish only after write-through
            if (!was_bound) ++bound_size_;
        } else {
            ++bound_size_;
        }
        cache(inserted.first, record);
    }
    void erase(const std::string& key) {
        const auto at = table_.find(key);
        if (!at) return;
        auto record = read(*at);
        if (!record.bound) return;
        record.bound = 0;
        table_.set(*at, record);
        cache(*at, record);
        --bound_size_;
    }
    std::size_t size() const noexcept { return bound_size_; }
    std::size_t resident_size() const noexcept { return resident_size_; }

private:
    Record read(Offset at) const {
        for (const auto& slot : window_)
            if (slot.offset == at) return slot.record;
        const auto record = table_.get(at);
        cache(at, record);
        return record;
    }
    void cache(Offset at, const Record& record) const noexcept {
        for (auto& slot : window_) {
            if (slot.offset == at) {
                slot.record = record;
                return;
            }
        }
        auto& slot = window_[cursor_];
        if (slot.offset == 0) ++resident_size_;
        slot = Slot{at, record};
        cursor_ = (cursor_ + 1) % resident_capacity;
    }

    Table table_{Table::BackingPolicy::bounded};
    mutable std::array<Slot, resident_capacity> window_{};
    mutable std::size_t cursor_ = 0;
    mutable std::size_t resident_size_ = 0;
    // DiskIdTable size includes tombstones; logical size deliberately does not.
    std::size_t bound_size_ = 0;
};

} // namespace pineforge::detail
