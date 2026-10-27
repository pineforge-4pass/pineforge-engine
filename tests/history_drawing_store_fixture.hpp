#pragma once

#include <pineforge/detail/drawing_records.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace history_drawing_test {

struct HandleCounts {
    std::size_t live = 0;
    std::size_t peak = 0;
    std::size_t begins = 0;
    // Store::scope() made on the wrapper: scopes that joined an open
    // transaction, and joined scopes abandoned unfinished.
    std::size_t joined = 0;
    std::size_t abandoned = 0;
    // The id the wrapped Store returned from its latest admitted create().
    pineforge::detail::o1::Id last_id = pineforge::detail::o1::kNullId;
};

class CountedTx final : public pineforge::detail::o1::Tx {
    std::unique_ptr<pineforge::detail::o1::Tx> transaction_;
    HandleCounts& counts_;
public:
    CountedTx(std::unique_ptr<pineforge::detail::o1::Tx> transaction, HandleCounts& counts)
        : transaction_(std::move(transaction)), counts_(counts) {
        ++counts_.live;
        ++counts_.begins;
        counts_.peak = std::max(counts_.peak, counts_.live);
    }
    ~CountedTx() override {
        transaction_.reset();
        --counts_.live;
    }
    void commit() override { transaction_->commit(); }
    void rollback() override { transaction_->rollback(); }
};

class CountedStore final : public pineforge::detail::o1::Store {
    std::unique_ptr<pineforge::detail::o1::Store> store_;
    HandleCounts& counts_;
public:
    CountedStore(std::unique_ptr<pineforge::detail::o1::Store> store, HandleCounts& counts)
        : store_(std::move(store)), counts_(counts) {}
    pineforge::detail::o1::Id create(std::uint64_t bytes, std::uint64_t align) override {
        const auto id = store_->create(bytes, align);
        counts_.last_id = id;
        return id;
    }
    void release(pineforge::detail::o1::Id id) override { store_->release(id); }
    bool alive(pineforge::detail::o1::Id id) const override { return store_->alive(id); }
    std::uint64_t size(pineforge::detail::o1::Id id) const override { return store_->size(id); }
    bool resize(pineforge::detail::o1::Id id, std::uint64_t bytes) override {
        return store_->resize(id, bytes);
    }
    std::size_t read(pineforge::detail::o1::Id id, std::uint64_t offset,
                     void* destination, std::size_t bytes) const override {
        return store_->read(id, offset, destination, bytes);
    }
    std::size_t write(pineforge::detail::o1::Id id, std::uint64_t offset,
                      const void* source, std::size_t bytes) override {
        return store_->write(id, offset, source, bytes);
    }
    std::unique_ptr<pineforge::detail::o1::Tx> begin() override {
        return std::make_unique<CountedTx>(store_->begin(), counts_);
    }
    // Store::scope() calls these on the wrapper: each forwards to the real
    // Store and is only counted, so an operation that joined is visible.
    bool join_active_tx() override {
        const bool joined = store_->join_active_tx();
        if (joined) ++counts_.joined;
        return joined;
    }
    void abandon_joined_tx() noexcept override {
        ++counts_.abandoned;
        store_->abandon_joined_tx();
    }
    // A cleanup handle takes the real Store's reserved slot, not a caller
    // slot, so it is forwarded without a CountedTx.
    std::unique_ptr<pineforge::detail::o1::Tx> begin_cleanup() override {
        return store_->begin_cleanup();
    }
    void note_cleanup_skipped(pineforge::detail::o1::CleanupSkip reason) noexcept override {
        store_->note_cleanup_skipped(reason);
    }
    pineforge::detail::o1::Stats stats() const override { return store_->stats(); }
};

struct StoreFixture {
    HandleCounts handles;
    std::shared_ptr<pineforge::detail::DrawingStorage> storage =
        pineforge::detail::open_drawing_storage();
    // The real Store under the wrapper. Its Stats (the pool's handle
    // high-water, the live objects) are read here, through no wrapper.
    pineforge::detail::o1::Store* real = nullptr;
    StoreFixture() { wrap(); }
    // A Store of another shape (a small live-object limit, say) over the same
    // backing. A Store that made no object writes no page, so the first one
    // leaves the backing empty, as opening requires.
    explicit StoreFixture(const pineforge::detail::o1::Shape& shape) {
        storage->store.reset();
        if (storage->backing->page_count() != 0)
            throw std::runtime_error("the drawing backing is not empty before the reopen");
        storage->store = pineforge::detail::o1::Store::open(shape, *storage->backing);
        wrap();
    }

private:
    void wrap() {
        real = storage->store.get();
        storage->store = std::make_unique<CountedStore>(std::move(storage->store), handles);
    }
};

} // namespace history_drawing_test
