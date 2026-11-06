#pragma once

#include "p2_a5strmap_store_fixture.hpp"

#include <pineforge/map.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/run_failure.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cr559 {
namespace o1 = pineforge::detail::o1;
using Scalar = std::int64_t;
using Map = pineforge::PineMap<Scalar, Scalar>;
using Model = std::vector<std::pair<Scalar, Scalar>>;

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

inline Scalar key_for(Scalar index) { return index * 37 + 11; }
inline Scalar value_for(Scalar index) { return (index * index + 19) ^ (index * 101); }

// A forwarding wrapper over the real disk-backed Store, following
// test_o1_store_wrapper.cpp. Only the first map's two roots are observed;
// images and copies continue through the same provider without being retained.
class ObservedStore final : public o1::Store {
public:
    explicit ObservedStore(o1::Store& inner) : inner_(inner) {}

    o1::Id create(std::uint64_t bytes, std::uint64_t align) override {
        const auto id = inner_.create(bytes, align);
        if (roots_seen_ < roots_.size()) roots_[roots_seen_++] = id;
        return id;
    }
    void release(o1::Id id) override { inner_.release(id); }
    bool alive(o1::Id id) const override { return inner_.alive(id); }
    std::uint64_t size(o1::Id id) const override { return inner_.size(id); }
    bool resize(o1::Id id, std::uint64_t bytes) override {
        const auto before = inner_.size(id);
        const bool result = inner_.resize(id, bytes);
        if (result && bytes > before) {
            if (id == roots_[0]) {
                ++arena_growths;
                const auto step = bytes - before;
                if (arena_step_bytes == 0) arena_step_bytes = step;
                else fixed_arena_steps = fixed_arena_steps && step == arena_step_bytes;
            }
            if (id == roots_[1]) ++index_growths;
        }
        return result;
    }
    std::size_t read(o1::Id id, std::uint64_t offset, void* dst,
                     std::size_t bytes) const override {
        ++read_calls;
        return inner_.read(id, offset, dst, bytes);
    }
    std::size_t write(o1::Id id, std::uint64_t offset, const void* src,
                      std::size_t bytes) override {
        return inner_.write(id, offset, src, bytes);
    }
    std::unique_ptr<o1::Tx> begin() override { return inner_.begin(); }
    bool join_active_tx() override { return inner_.join_active_tx(); }
    void abandon_joined_tx() noexcept override { inner_.abandon_joined_tx(); }
    std::unique_ptr<o1::Tx> begin_cleanup() override { return inner_.begin_cleanup(); }
    void note_cleanup_skipped(o1::CleanupSkip reason) noexcept override {
        inner_.note_cleanup_skipped(reason);
    }
    o1::Stats stats() const override { return inner_.stats(); }
    std::uint64_t rollback_epoch() const noexcept override {
        return inner_.rollback_epoch();
    }

    std::uint64_t arena_bytes() const {
        require(roots_seen_ == roots_.size(), "map did not create its two Store roots");
        return inner_.size(roots_[0]);
    }
    std::uint64_t index_bytes() const {
        require(roots_seen_ == roots_.size(), "map did not create its two Store roots");
        return inner_.size(roots_[1]);
    }
    std::uint64_t bucket_count() const {
        return index_bytes() / sizeof(pineforge::detail::MapLinks{}.chain);
    }

    std::uint64_t arena_growths = 0;
    std::uint64_t index_growths = 0;
    std::uint64_t arena_step_bytes = 0;
    mutable std::uint64_t read_calls = 0;
    bool fixed_arena_steps = true;

private:
    o1::Store& inner_;
    std::array<o1::Id, 2> roots_{};
    std::size_t roots_seen_ = 0;
};

struct Session {
    a5_fixture::Session disk;
    ObservedStore store{*disk.store};
    std::shared_ptr<pineforge::detail::MapStoreHolder> holder =
        std::make_shared<pineforge::detail::MapStoreHolder>(store);
    pineforge::detail::MapStoreScope scope{holder};
};

inline void append(Map& map, Model& model, Scalar first, Scalar end) {
    for (Scalar index = first; index < end; ++index) {
        const auto previous = map.put(key_for(index), value_for(index));
        require(pineforge::is_na(previous), "distinct insertion returned an old value");
        model.emplace_back(key_for(index), value_for(index));
    }
}

inline void exact(const Map& map, const Model& model) {
    require(static_cast<std::size_t>(map.size()) == model.size(), "map size differs");
    const auto keys = map.keys();
    const auto values = map.values();
    require(keys.size() == model.size() && values.size() == model.size(),
            "enumeration size differs");
    for (std::size_t index = 0; index < model.size(); ++index) {
        const auto& row = model[index];
        require(keys[index] == row.first && values[index] == row.second,
                "insertion order or enumerated value differs");
        require(map.contains(row.first) && map.get(row.first) == row.second,
                "contains/get differs from the independent model");
    }
}

using SetLimit = int (*)(const std::int64_t*, char*, std::size_t);

// Resolve the additive export as the neighbouring output-series tests do.
// This compiles against the frozen base headers; an absent parameter fails
// explicitly instead of disabling the parameter rows or substituting a map.
class LimitScope {
public:
    explicit LimitScope(std::int64_t maximum)
        : setter_(reinterpret_cast<SetLimit>(
              ::dlsym(RTLD_DEFAULT, "pf_map_pairs_limit_set_checked"))) {
        require(setter_ != nullptr,
                "missing pf_map_pairs_limit_set_checked: map capacity is not configurable");
        set(maximum);
    }
    LimitScope(const LimitScope&) = delete;
    LimitScope& operator=(const LimitScope&) = delete;
    ~LimitScope() {
        char error[256]{};
        if (setter_(nullptr, error, sizeof error) != PF_SETTINGS_OK) {
            std::fprintf(stderr, "CR559 map limit reset failed: %s\n", error);
            std::terminate();
        }
    }
    void set(std::int64_t maximum) {
        char error[256]{};
        require(setter_(&maximum, error, sizeof error) == PF_SETTINGS_OK,
                "checked map limit setter refused a nonnegative limit");
        require(error[0] == '\0', "successful map limit setter left an error");
    }
    void unlimited() {
        char error[256]{};
        require(setter_(nullptr, error, sizeof error) == PF_SETTINGS_OK,
                "checked map limit setter refused unlimited");
        require(error[0] == '\0', "successful unlimited setter left an error");
    }
    void refuse_negative() {
        const std::int64_t maximum = -1;
        char error[256]{};
        require(setter_(&maximum, error, sizeof error) == PF_SETTINGS_INVALID_ARGUMENT,
                "negative map-pair limit was accepted");
        require(std::string(error) == "max map pairs must not be negative",
                "negative map-pair limit lost its checked error");
    }

private:
    SetLimit setter_;
};

inline std::string capacity_message(Scalar maximum, Scalar current, Scalar bar) {
    return "Error on bar " + std::to_string(bar) +
        ": A map cannot have more than " + std::to_string(maximum) +
        " key-value pairs. The current size is " + std::to_string(current) + ".";
}

inline std::string capacity_args(Scalar maximum, Scalar current, Scalar bar) {
    return "{\"bar_index\":" + std::to_string(bar) +
        ",\"code\":\"RE10070\",\"currSize\":" + std::to_string(current) +
        ",\"maxSize\":" + std::to_string(maximum) + "}";
}

template <class Operation>
void expect_capacity_error(Operation operation, Scalar maximum,
                            Scalar current, Scalar bar) {
    bool refused = false;
    try {
        operation();
    } catch (const std::runtime_error& error) {
        refused = true;
        require(dynamic_cast<const pineforge::RunFailureInfo*>(&error) != nullptr,
                "capacity refusal is missing typed run-failure information");
        const auto failure = pineforge::classify_run_failure(error);
        require(std::string(pineforge::run_failure_code_name(failure.code)) ==
                    "pine_map_capacity_exceeded",
                "capacity refusal did not reach its catalog code");
        require(failure.args && *failure.args == capacity_args(maximum, current, bar),
                "capacity refusal changed RE10070/maxSize/currSize/bar_index");
        require(std::string(error.what()) == capacity_message(maximum, current, bar),
                "capacity refusal differs from exact TradingView English");
    }
    require(refused, "distinct key beyond the configured maximum was accepted");
}
} // namespace cr559
