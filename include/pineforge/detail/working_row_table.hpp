#pragma once

#include <pineforge/detail/working_store.hpp>
#include <exception>
#include <utility>

namespace pineforge::detail {

// Only callers own decoded rows. The intrusive list coalesces simultaneous
// pins of the same key; it is not a directory of the stored population.
template<class T>
class WorkingRowTable {
    struct State {
        WorkingRowTable& owner;
        std::uint64_t key;
        T value;
        working_store::Bytes original;
        State* next;
        std::weak_ptr<const unsigned char> lifetime;
        bool writable = false;
        bool erased = false;
        State(WorkingRowTable& table, std::uint64_t id, T row,
              working_store::Bytes bytes)
            : owner(table), key(id), value(std::move(row)), original(std::move(bytes)),
              next(table.pins_), lifetime(table.lifetime_) { table.pins_ = this; }
        ~State() {
            if (lifetime.expired()) return;
            auto** at = &owner.pins_;
            while (*at && *at != this) at = &(*at)->next;
            if (*at) *at = next;
        }
        std::weak_ptr<State> self;
        std::shared_ptr<State> pending_next;
    };
public:
    class Pin {
    public:
        Pin() = default;
        Pin(const Pin&) = default;
        Pin(Pin&&) noexcept = default;
        Pin& operator=(const Pin& other) {
            if (state_ == other.state_) return *this;
            finish(); state_ = other.state_; return *this;
        }
        Pin& operator=(Pin&& other) {
            if (this == &other) return *this;
            finish(); state_ = std::move(other.state_); return *this;
        }
        ~Pin() noexcept { defer(); }
        explicit operator bool() const noexcept { return bool(state_); }
        T& get() const {
            if (state_->lifetime.expired()) working_store::check(ScratchStatus::programming_error);
            return state_->value;
        }
        void finish() {
            if (state_ && !state_->lifetime.expired()
                && state_.use_count() == 1 && std::uncaught_exceptions() == 0)
                state_->owner.flush_state(*state_);
            state_.reset();
        }
    private:
        friend class WorkingRowTable;
        explicit Pin(std::shared_ptr<State> state) : state_(std::move(state)) {}
        void defer() noexcept {
            if (state_ && !state_->lifetime.expired()
                && state_.use_count() == 1 && state_->writable && !state_->erased
                && std::uncaught_exceptions() == 0) {
                auto& owner = state_->owner;
                state_->pending_next = std::move(owner.pending_);
                owner.pending_ = std::move(state_);
            }
        }
        std::shared_ptr<State> state_;
    };

    bool bound() const noexcept { return store_ != nullptr; }
    void bind(ScratchStore& store, std::uint64_t domain, std::uint64_t owner) {
        store_ = &store; domain_ = domain; owner_ = owner;
        if (!binding_) binding_ = std::make_shared<working_store::Binding>();
        binding_->store = &store;
        // Reopen selects the committed relation. Never publish a decoded
        // provisional value left behind by a failed write/commit.
        for (auto* state = pins_; state; state = state->next) {
            const auto handle = working_store::find(store, {domain_, owner_, state->key});
            state->erased = handle == 0;
            if (!handle) continue;
            state->value = working_store::read<T>(store, handle, binding_);
            state->original = working_store::bytes(store, handle, binding_);
        }
        release_pending();
    }
    // A call-local copy for observers: pending writable values are visible,
    // without publishing them or creating/flushing a writable row pin.
    std::optional<T> observe(std::uint64_t key) const {
        const auto handle = working_store::find(*store_, {domain_, owner_, key});
        if (!handle) return {};
        for (const auto* state = pins_; state; state = state->next)
            if (state->key == key && !state->erased) return state->value;
        return working_store::read<T>(*store_, handle, binding_);
    }
    Pin find(std::uint64_t key, bool writable) const {
        flush();
        for (auto* state = pins_; state; state = state->next) {
            if (state->key == key && !state->erased) {
                state->writable |= writable;
                return Pin(state->self.lock());
            }
        }
        const auto handle = working_store::find(*store_, {domain_, owner_, key});
        if (!handle) return {};
        auto row = working_store::read<T>(*store_, handle, binding_);
        auto state = std::make_shared<State>(const_cast<WorkingRowTable&>(*this),
            key, std::move(row), working_store::bytes(*store_, handle, binding_));
        state->self = state; state->writable = writable;
        return Pin(std::move(state));
    }
    std::uint64_t next_after(std::uint64_t after) const {
        flush();
        if (after == std::numeric_limits<std::uint64_t>::max()) return 0;
        std::uint64_t key = 0;
        return working_store::seek(*store_, domain_, owner_, after + 1, key) ? key : 0;
    }
    bool contains(std::uint64_t key) const {
        flush();
        return working_store::find(*store_, {domain_, owner_, key}) != 0;
    }
    void put(std::uint64_t key, const T& value) {
        flush();
        working_store::write(*store_, {domain_, owner_, key}, value);
        for (auto* state = pins_; state; state = state->next) {
            if (state->key != key || state->erased) continue;
            state->value = value;
            const auto handle = working_store::find(*store_, {domain_, owner_, key});
            state->original = working_store::bytes(*store_, handle, binding_);
        }
    }
    void erase(std::uint64_t key) {
        flush();
        working_store::erase_value(*store_, {domain_, owner_, key});
        for (auto* state = pins_; state; state = state->next)
            if (state->key == key) state->erased = true;
    }
    void clear() {
        flush();
        while (const auto key = next_after(0)) erase(key);
    }
    void flush() const {
        if (!store_) return;
        for (auto* state = pins_; state; state = state->next) flush_state(*state);
        release_pending();
    }
private:
    void release_pending() const noexcept {
        auto pending = std::move(pending_);
        while (pending) {
            auto next = std::move(pending->pending_next);
            pending = std::move(next);
        }
    }
    void flush_state(State& state) const {
        if (state.erased || !state.writable
            || working_store::equal_bytes(state.value, state.original)) return;
        working_store::transaction(*store_, [&] {
            working_store::write(*store_, {domain_, owner_, state.key}, state.value);
        });
        const auto handle = working_store::find(*store_, {domain_, owner_, state.key});
        state.original = working_store::bytes(*store_, handle, binding_);
    }
    ScratchStore* store_ = nullptr;
    std::uint64_t domain_ = 0, owner_ = 0;
    std::shared_ptr<working_store::Binding> binding_;
    mutable State* pins_ = nullptr;
    mutable std::shared_ptr<State> pending_;
    std::shared_ptr<const unsigned char> lifetime_ = std::make_shared<const unsigned char>(0);
};

} // namespace pineforge::detail
