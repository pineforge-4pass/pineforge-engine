#pragma once

#include <pineforge/detail/id_pair_store.hpp>
#include <pineforge/detail/working_store.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace pineforge::source {

// WorkingMinima's exact, streaming keyed-table pattern, with assign/erase
// instead of min-assign. Domain 68 is temporary lane-A derived bookkeeping.
// Bound owners borrow the adapter's store and retain no population directory.
class PendingProjectionCounts {
    using Store = ::pineforge::detail::ScratchStore;
    using Handle = native_order::RequestHandle;
    class Key final : public ::pineforge::detail::ScratchByteSource {
    public:
        Key(std::uint64_t kind, const std::string& id, const std::string& from,
            const Handle& origin) : prefix_(68, kind, 0), id_(id), from_(from),
                                    origin_(origin) {
            ::pineforge::detail::working_store::Save ar;
            fields(ar);
            size_ = ar.position();
        }
        std::uint64_t size() const noexcept override { return size_; }
        ::pineforge::detail::ScratchStatus read(std::uint64_t at,
                ::pineforge::detail::ScratchBuffer out) const noexcept override {
            using namespace ::pineforge::detail;
            if (at > size_ || out.size > size_ - at || (out.size && !out.data))
                return ScratchStatus::programming_error;
            try {
                working_store::Save ar(at, out);
                fields(ar);
                return ScratchStatus::ok;
            } catch (...) { return ScratchStatus::programming_error; }
        }
    private:
        void fields(::pineforge::detail::working_store::Save& ar) const {
            std::array<std::byte, 32> prefix{};
            ::pineforge::detail::working_store::check(
                prefix_.read(0, {prefix.data(), prefix.size()}));
            ar.bytes(prefix.data(), prefix.size());
            ar(id_, from_, origin_);
        }
        ::pineforge::detail::working_store::Key prefix_;
        const std::string& id_;
        const std::string& from_;
        const Handle& origin_;
        std::uint64_t size_ = 0;
    };
public:
    // Disjoint prefixes of the six raw owners, including slots with no placement.
    static std::array<std::size_t, 6> prefix_lengths(
            std::array<std::size_t, 6> lengths) noexcept {
        auto remaining = static_cast<std::size_t>(std::numeric_limits<int>::max());
        for (auto& length : lengths) {
            length = std::min(length, remaining);
            remaining -= length;
        }
        return lengths;
    }
    void set_directory(const std::string& directory) { legacy_.set_directory(directory); }
    void fixture_use_memory_reference() { legacy_.fixture_use_memory_reference(); }
    std::uint64_t disk_bytes() const noexcept { return store_ ? 0 : legacy_.disk_bytes(); }
    void bind(Store& store) {
        const bool first = !store_;
        store_ = &store;
        if (!first) return;
        // Like the canonical owners, an existing descriptor selects committed
        // state rather than overwriting it with an unbound compatibility row.
        if (!::pineforge::detail::working_store::get_word(store, {68, 0, 0})) {
            ::pineforge::detail::working_store::transaction(store, [&] {
                for (const auto& row : legacy_) {
                    ::pineforge::detail::ScratchSpanSource key({
                        reinterpret_cast<const std::byte*>(row.first.data()), row.first.size()});
                    put(key, row.second);
                }
            });
        }
        legacy_.clear();
    }
    std::uint64_t get(std::uint64_t kind, const std::string& id,
            const std::string& from, const Handle& origin) const {
        const Key key(kind, id, from, origin);
        if (!store_) return legacy_.lookup(text(key)).value_or(0);
        const auto at = find(key);
        if (!at) return 0;
        using namespace ::pineforge::detail;
        ScratchRecordInfo info{};
        working_store::check(store_->point(at, info));
        if (info.value_bytes != 8) working_store::check(ScratchStatus::corrupt);
        std::array<std::byte, 8> bytes{};
        working_store::check(store_->read(at, ScratchField::value, 0,
                                         {bytes.data(), bytes.size()}));
        return working_store::word(bytes.data());
    }
    void assign(std::uint64_t kind, const std::string& id,
            const std::string& from, const Handle& origin, std::uint64_t count) {
        const Key key(kind, id, from, origin);
        if (!store_) {
            const auto encoded = text(key);
            if (count) legacy_.insert_or_assign(encoded, count);
            else legacy_.erase(encoded);
            return;
        }
        ::pineforge::detail::working_store::transaction(*store_, [&] {
            if (count) put(key, count);
            else if (const auto at = find(key))
                ::pineforge::detail::working_store::check(store_->erase(at));
        });
    }
    void clear() {
        if (!store_) { legacy_.clear(); return; }
        using namespace ::pineforge::detail;
        working_store::transaction(*store_, [&] {
            ScratchHandle at = 0;
            auto status = store_->lower_bound(working_store::Key(68, 0, 0), at);
            while (status == ScratchStatus::ok) {
                ScratchRecordInfo info{};
                working_store::check(store_->point(at, info));
                if (info.key_bytes < 16) break;
                std::array<std::byte, 16> prefix{}, actual{};
                working_store::check(working_store::Key(68, 0, 0).read(
                    0, {prefix.data(), prefix.size()}));
                working_store::check(store_->read(at, ScratchField::key, 0,
                    {actual.data(), actual.size()}));
                if (prefix != actual) break;
                ScratchHandle next = 0;
                const auto step = store_->next(at, next);
                if (step != ScratchStatus::end) working_store::check(step);
                working_store::check(store_->erase(at));
                at = next; status = step;
            }
            if (status != ScratchStatus::ok && status != ScratchStatus::end)
                working_store::check(status);
        });
    }
private:
    static std::string text(const Key& key) {
        std::string result(static_cast<std::size_t>(key.size()), '\0');
        ::pineforge::detail::working_store::check(key.read(0, {
            reinterpret_cast<std::byte*>(result.data()), result.size()}));
        return result;
    }
    ::pineforge::detail::ScratchHandle find(const Key& key) const {
        using namespace ::pineforge::detail;
        ScratchHandle at = 0;
        const auto status = store_->lower_bound(key, at);
        if (status == ScratchStatus::end) return 0;
        working_store::check(status);
        ScratchRecordInfo info{};
        working_store::check(store_->point(at, info));
        if (info.key_bytes != key.size()) return 0;
        std::array<std::byte, 64> left{}, right{};
        for (std::uint64_t offset = 0; offset < key.size();) {
            const auto n = static_cast<std::size_t>(
                std::min<std::uint64_t>(left.size(), key.size() - offset));
            working_store::check(key.read(offset, {left.data(), n}));
            working_store::check(store_->read(at, ScratchField::key, offset,
                                              {right.data(), n}));
            if (std::memcmp(left.data(), right.data(), n)) return 0;
            offset += n;
        }
        return at;
    }
    void put(const ::pineforge::detail::ScratchByteSource& key, std::uint64_t count) {
        using namespace ::pineforge::detail;
        std::array<std::byte, 8> bytes{};
        working_store::le(count, bytes.data());
        ScratchSpanSource value({bytes.data(), bytes.size()});
        ScratchHandle ignored = 0;
        working_store::check(store_->put(key, value, ignored));
    }
    ::pineforge::detail::IdPairStore<std::uint64_t> legacy_;
    Store* store_ = nullptr;
};

} // namespace pineforge::source
