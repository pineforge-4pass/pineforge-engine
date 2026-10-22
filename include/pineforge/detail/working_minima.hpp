#pragma once

#include <pineforge/detail/id_pair_store.hpp>
#include <pineforge/detail/working_store.hpp>
#include <string_view>

namespace pineforge::detail {

// The erased-placement minima are an index of the existing placement history,
// not a second semantic fold. Single and pair keys remain separate domains.
class WorkingMinima {
    using Legacy = IdPairStore<std::uint64_t>;
    class Key final : public ScratchByteSource {
    public:
        Key(std::string_view first, std::optional<std::string_view> second)
            : prefix_(65, 1, second ? 1 : 0), first_(first), second_(second) {
            const auto extra = second ? std::uint64_t{48} : std::uint64_t{40};
            const auto maximum = std::numeric_limits<std::uint64_t>::max();
            if (first.size() > maximum - extra
                || (second && second->size() > maximum - extra - first.size()))
                working_store::check(ScratchStatus::programming_error);
            size_ = extra + first.size() + (second ? second->size() : 0);
        }
        std::uint64_t size() const noexcept override { return size_; }
        ScratchStatus read(std::uint64_t at, ScratchBuffer out) const noexcept override {
            if (at > size_ || out.size > size_ - at || (out.size && !out.data))
                return ScratchStatus::programming_error;
            try {
                std::array<std::byte, 32> prefix{};
                working_store::check(prefix_.read(0, {prefix.data(), prefix.size()}));
                working_store::Save ar(at, out);
                ar.bytes(prefix.data(), prefix.size());
                ar(static_cast<std::uint64_t>(first_.size()));
                ar.bytes(first_.data(), first_.size());
                if (second_) {
                    ar(static_cast<std::uint64_t>(second_->size()));
                    ar.bytes(second_->data(), second_->size());
                }
                return ScratchStatus::ok;
            } catch (...) { return ScratchStatus::programming_error; }
        }
    private:
        working_store::Key prefix_;
        std::string_view first_;
        std::optional<std::string_view> second_;
        std::uint64_t size_ = 0;
    };
public:
    using KeyView = Legacy::KeyView;
    void bind_store(ScratchStore& store) {
        const bool first = !store_;
        store_ = &store;
        if (!first) return;
        working_store::transaction(store, [&] {
            for (const auto& row : legacy_) {
                const auto from = row.from_entry
                    ? std::optional<std::string_view>(*row.from_entry) : std::nullopt;
                assign(row.first, from, row.second);
            }
        });
        legacy_.clear();
    }
    template<class Enumerate> void preflight_keys(const Enumerate& enumerate) const {
        if (!store_) { legacy_.preflight_keys(enumerate); return; }
        enumerate([](const KeyView& key) {
            (void)Key(key.first, key.second).size();
        });
    }
    std::optional<std::uint64_t> lookup(std::string_view id) const {
        if (!store_) return legacy_.lookup(id);
        return lookup_key(Key(id, {}));
    }
    std::optional<std::uint64_t> lookup(std::string_view id, std::string_view from) const {
        if (!store_) return legacy_.lookup(id, from);
        return lookup_key(Key(id, from));
    }
    void min_assign(std::string_view id, std::uint64_t value) {
        if (!store_) { legacy_.min_assign(id, value); return; }
        assign(id, {}, value);
    }
    void min_assign(std::string_view id, std::string_view from, std::uint64_t value) {
        if (!store_) { legacy_.min_assign(id, from, value); return; }
        assign(id, from, value);
    }
    void clear() {
        if (!store_) { legacy_.clear(); return; }
        working_store::transaction(*store_, [&] {
            ScratchHandle at = 0;
            auto status = store_->lower_bound(working_store::Key(65, 1, 0), at);
            while (status == ScratchStatus::ok && in_domain(at)) {
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
    void set_directory(const std::string& directory) { legacy_.set_directory(directory); }
    void bind_scratch_tables(const std::shared_ptr<ScratchSlab>& slab,
                             ScratchResource data, ScratchResource index) {
        legacy_.bind_scratch_tables(slab, data, index);
    }
    void fixture_use_memory_reference() { legacy_.fixture_use_memory_reference(); }
    // A bound index owns no separate file; the borrowed store reports its extent.
    std::uint64_t disk_bytes() const noexcept { return store_ ? 0 : legacy_.disk_bytes(); }
private:
    ScratchHandle find(const Key& key) const {
        ScratchHandle at = 0;
        const auto status = store_->lower_bound(key, at);
        if (status == ScratchStatus::end) return 0;
        working_store::check(status);
        ScratchRecordInfo info{}; working_store::check(store_->point(at, info));
        if (info.key_bytes != key.size()) return 0;
        std::array<std::byte, 64> left{}, right{};
        for (std::uint64_t offset = 0; offset < key.size();) {
            const auto n = static_cast<std::size_t>(
                std::min<std::uint64_t>(left.size(), key.size() - offset));
            working_store::check(key.read(offset, {left.data(), n}));
            working_store::check(store_->read(at, ScratchField::key, offset, {right.data(), n}));
            if (std::memcmp(left.data(), right.data(), n)) return 0;
            offset += n;
        }
        return at;
    }
    std::optional<std::uint64_t> lookup_key(const Key& key) const {
        const auto at = find(key);
        if (!at) return {};
        ScratchRecordInfo info{}; working_store::check(store_->point(at, info));
        if (info.value_bytes != 8) working_store::check(ScratchStatus::corrupt);
        std::array<std::byte, 8> value{};
        working_store::check(store_->read(at, ScratchField::value, 0, {value.data(), value.size()}));
        return working_store::word(value.data());
    }
    void assign(std::string_view id, std::optional<std::string_view> from, std::uint64_t value) {
        const Key key(id, from);
        const auto previous = lookup_key(key);
        if (previous && *previous <= value) return;
        working_store::transaction(*store_, [&] {
            std::array<std::byte, 8> bytes{};
            working_store::le(value, bytes.data());
            ScratchSpanSource source({bytes.data(), bytes.size()});
            ScratchHandle ignored = 0;
            working_store::check(store_->put(key, source, ignored));
        });
    }
    bool in_domain(ScratchHandle at) const {
        ScratchRecordInfo info{}; working_store::check(store_->point(at, info));
        if (info.key_bytes < 24) return false;
        std::array<std::byte, 24> actual{}, expected{};
        working_store::check(store_->read(at, ScratchField::key, 0, {actual.data(), actual.size()}));
        working_store::check(working_store::Key(65, 1, 0).read(0, {expected.data(), expected.size()}));
        return actual == expected;
    }
    Legacy legacy_;
    ScratchStore* store_ = nullptr;
};

} // namespace pineforge::detail
