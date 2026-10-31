#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/map.hpp>

#include <pineforge/na.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using pineforge::PineMap;

// Count the actual Store::read boundary, with real backing and transactions.
// No lifecycle call, epoch query, or map operation increments this counter.
class ReadCountingStore final : public pineforge::detail::o1::Store {
    using Store = pineforge::detail::o1::Store;
    using Id = pineforge::detail::o1::Id;
    using Tx = pineforge::detail::o1::Tx;
    Store& inner_;

public:
    explicit ReadCountingStore(Store& inner) noexcept : inner_(inner) {}
    mutable std::size_t reads = 0;

    Id create(std::uint64_t bytes, std::uint64_t align) override {
        return inner_.create(bytes, align);
    }
    void release(Id id) override { inner_.release(id); }
    bool alive(Id id) const override { return inner_.alive(id); }
    std::uint64_t size(Id id) const override { return inner_.size(id); }
    bool resize(Id id, std::uint64_t bytes) override { return inner_.resize(id, bytes); }
    std::size_t read(Id id, std::uint64_t off, void* dst, std::size_t n) const override {
        ++reads;
        return inner_.read(id, off, dst, n);
    }
    std::size_t write(Id id, std::uint64_t off, const void* src, std::size_t n) override {
        return inner_.write(id, off, src, n);
    }
    std::unique_ptr<Tx> begin() override { return inner_.begin(); }
    bool join_active_tx() override { return inner_.join_active_tx(); }
    void abandon_joined_tx() noexcept override { inner_.abandon_joined_tx(); }
    std::unique_ptr<Tx> begin_cleanup() override { return inner_.begin_cleanup(); }
    void note_cleanup_skipped(pineforge::detail::o1::CleanupSkip reason) noexcept override {
        inner_.note_cleanup_skipped(reason);
    }
    pineforge::detail::o1::Stats stats() const override { return inner_.stats(); }
    std::uint64_t rollback_epoch() const noexcept override { return inner_.rollback_epoch(); }
};

template <typename Fn>
static void expect_runtime_error(Fn&& fn, const char* message) {
    bool threw = false;
    try {
        fn();
    } catch (const std::runtime_error& error) {
        threw = true;
        assert(std::string(error.what()) == message);
    }
    assert(threw);
}

template <typename Map, typename = void>
struct has_public_snapshot : std::false_type {};

template <typename Map>
struct has_public_snapshot<
    Map, std::void_t<decltype(std::declval<const Map&>().snapshot())>>
    : std::true_type {};

struct SnapshotUnsafeUdt {
    int value = 0;
};

struct HeapValueProbe {
    inline static std::size_t default_constructed = 0;

    HeapValueProbe() { ++default_constructed; }
    explicit HeapValueProbe(int input) : value(input) {}

    std::string payload;
    int value = 0;
};

static void test_na_id_and_stable_errors() {
    using Map = PineMap<std::string, int>;
    constexpr const char* na_error = "map operation on na ID";

    Map null_map;
    auto typed_na = pineforge::na<Map>();
    assert(pineforge::is_na(null_map));
    assert(pineforge::is_na(typed_na));

    auto allocated = Map::new_();
    assert(!pineforge::is_na(allocated));
    auto snapshot = allocated.snapshot();

    expect_runtime_error([&] { (void)null_map.put("x", 1); }, na_error);
    expect_runtime_error([&] { (void)null_map.get("x"); }, na_error);
    expect_runtime_error([&] { (void)null_map.remove("x"); }, na_error);
    expect_runtime_error([&] { (void)null_map.contains("x"); }, na_error);
    expect_runtime_error([&] { (void)null_map.size(); }, na_error);
    expect_runtime_error([&] { null_map.clear(); }, na_error);
    expect_runtime_error([&] { (void)null_map.keys(); }, na_error);
    expect_runtime_error([&] { (void)null_map.values(); }, na_error);
    expect_runtime_error([&] { (void)null_map.copy(); }, na_error);
    expect_runtime_error([&] { (void)null_map.snapshot(); }, na_error);
    expect_runtime_error([&] { null_map.put_all(allocated); }, na_error);
    expect_runtime_error([&] { allocated.put_all(null_map); }, na_error);
    auto active_snapshot = std::move(snapshot);
    expect_runtime_error(
        [&] { null_map.restore(snapshot); },
        "map restore from invalid snapshot");
    null_map.restore(active_snapshot);
    assert(!pineforge::is_na(null_map));
}

static void test_copy_aliases_and_explicit_copy_is_independent() {
    auto map = PineMap<std::string, double>::new_();
    const auto initial_previous = map.put("alpha", 1.0);
    assert(pineforge::is_na(initial_previous));

    auto alias = map;
    assert(alias.get("alpha") == 1.0);
    const auto alias_previous = alias.put("alpha", 2.0);
    assert(alias_previous == 1.0);
    assert(map.get("alpha") == 2.0);

    auto independent = map.copy();
    const auto independent_previous = independent.put("alpha", 3.0);
    const auto independent_missing = independent.put("beta", 4.0);
    assert(independent_previous == 2.0);
    assert(pineforge::is_na(independent_missing));
    assert(map.get("alpha") == 2.0);
    assert(!map.contains("beta"));

    auto assigned = PineMap<std::string, double>::new_();
    assigned = map;
    assigned.put("gamma", 5.0);
    assert(map.get("gamma") == 5.0);
}

static void test_move_preserves_source_handle_alias() {
    auto source = PineMap<std::string, int>::new_();
    source.put("a", 1);

    PineMap<std::string, int> constructed(std::move(source));
    constructed.put("b", 2);
    assert(source.get("b") == 2);

    auto assigned = PineMap<std::string, int>::new_();
    assigned.put("discarded", 99);
    assigned = std::move(source);
    assigned.put("c", 3);

    assert(source.get("c") == 3);
    assert(constructed.get("c") == 3);
    assert(!assigned.contains("discarded"));

}

static void test_put_get_and_stable_insertion_order() {
    auto map = PineMap<std::string, int>::new_();
    const auto second_previous = map.put("second", 2);
    const auto first_previous = map.put("first", 1);
    const auto third_previous = map.put("third", 3);
    assert(pineforge::is_na(second_previous));
    assert(pineforge::is_na(first_previous));
    assert(pineforge::is_na(third_previous));

    const auto overwrite_previous = map.put("first", 10);
    assert(overwrite_previous == 1);
    assert((map.keys() == std::vector<std::string>{"second", "first", "third"}));
    assert((map.values() == std::vector<int>{2, 10, 3}));
    assert(map.size() == 3);
    assert(map.contains("first"));
    assert(!map.contains("missing"));
    assert(map.get("first") == 10);
    assert(pineforge::is_na(map.get("missing")));
}

static void test_float_keys_use_pine_na_and_zero_equality() {
    auto map = PineMap<double, int>::new_();
    const double na_one = std::numeric_limits<double>::quiet_NaN();
    const double na_two = std::nan("42");

    const auto na_previous = map.put(na_one, 1);
    assert(pineforge::is_na(na_previous));
    assert(map.contains(na_two));
    assert(map.get(na_two) == 1);
    const auto na_overwrite_previous = map.put(na_two, 2);
    assert(na_overwrite_previous == 1);
    assert(map.size() == 1);
    assert(map.keys().size() == 1);
    assert(std::isnan(map.keys().front()));

    const auto zero_previous = map.put(-0.0, 3);
    assert(pineforge::is_na(zero_previous));
    assert(map.contains(+0.0));
    assert(map.get(+0.0) == 3);
    const auto zero_overwrite_previous = map.put(+0.0, 4);
    assert(zero_overwrite_previous == 3);
    assert(map.size() == 2);

    const auto removed_na = map.remove(na_two);
    assert(removed_na == 2);
    assert(!map.contains(na_one));
    assert((map.values() == std::vector<int>{4}));
}

static void test_remove_updates_order_and_reinsert_appends() {
    auto map = PineMap<int, std::string>::new_();
    map.put(1, "one");
    map.put(2, "two");
    map.put(3, "three");
    map.put(4, "four");

    const auto removed_two = map.remove(2);
    assert(removed_two == "two");
    assert((map.keys() == std::vector<int>{1, 3, 4}));
    assert((map.values() == std::vector<std::string>{"one", "three", "four"}));
    const auto removed_missing = map.remove(99);
    assert(removed_missing.empty());
    assert((map.keys() == std::vector<int>{1, 3, 4}));

    const auto reinsert_previous = map.put(2, "two-again");
    assert(reinsert_previous.empty());
    assert((map.keys() == std::vector<int>{1, 3, 4, 2}));
}

static void test_put_all_overwrites_in_source_order() {
    auto target = PineMap<std::string, int>::new_();
    target.put("a", 1);
    target.put("b", 2);
    target.put("c", 3);

    auto source = PineMap<std::string, int>::new_();
    source.put("b", 20);
    source.put("d", 40);
    source.put("a", 10);
    source.put("e", 50);

    target.put_all(source);
    assert((target.keys() == std::vector<std::string>{"a", "b", "c", "d", "e"}));
    assert((target.values() == std::vector<int>{10, 20, 3, 40, 50}));

    // Aliases identify the same map ID; put_all on itself must remain safe and
    // must not duplicate or reorder entries.
    auto alias = target;
    target.put_all(alias);
    assert((target.keys() == std::vector<std::string>{"a", "b", "c", "d", "e"}));
    assert((target.values() == std::vector<int>{10, 20, 3, 40, 50}));
}

static void test_50000_pair_capacity_and_put_all_preflight() {
    using Map = PineMap<int, int>;
    constexpr const char* capacity_error =
        "map cannot contain more than 50000 key-value pairs";

    auto map = Map::new_();
    for (int key = 0; key < Map::max_pairs; ++key) {
        const auto previous = map.put(key, key);
        assert(pineforge::is_na(previous));
    }
    assert(map.size() == Map::max_pairs);

    // Overwriting at the limit is permitted and does not move or grow a key.
    const auto limit_overwrite_previous = map.put(0, -1);
    assert(limit_overwrite_previous == 0);
    assert(map.size() == Map::max_pairs);
    expect_runtime_error(
        [&] { (void)map.put(Map::max_pairs, 1); }, capacity_error);
    assert(map.size() == Map::max_pairs);

    const auto removed_for_capacity = map.remove(2);
    const auto replacement_previous = map.put(Map::max_pairs, 50000);
    assert(removed_for_capacity == 2);
    assert(pineforge::is_na(replacement_previous));
    assert(map.size() == Map::max_pairs);

    auto overwrite_only = Map::new_();
    overwrite_only.put(0, 100);
    overwrite_only.put(1, 101);
    map.put_all(overwrite_only);
    assert(map.get(0) == 100);
    assert(map.get(1) == 101);

    auto mixed = Map::new_();
    mixed.put(0, 200);
    mixed.put(Map::max_pairs + 1, 50001);
    expect_runtime_error([&] { map.put_all(mixed); }, capacity_error);

    // Capacity is preflighted: the overwrite before the new source key was
    // not partially applied.
    assert(map.get(0) == 100);
    assert(!map.contains(Map::max_pairs + 1));
    assert(map.size() == Map::max_pairs);
}

static void test_clear_mutates_alias_only() {
    auto map = PineMap<int, int>::new_();
    map.put(1, 10);
    map.put(2, 20);
    auto alias = map;
    auto independent = map.copy();

    alias.clear();
    assert(map.size() == 0);
    assert(alias.size() == 0);
    assert(independent.size() == 2);
    assert(independent.get(1) == 10);
}

static void test_typed_missing_values_for_primitive_templates() {
    auto doubles = PineMap<int, double>::new_();
    assert(pineforge::is_na(doubles.get(1)));
    const auto removed_double = doubles.remove(1);
    assert(pineforge::is_na(removed_double));

    auto integers = PineMap<std::string, int64_t>::new_();
    assert(pineforge::is_na(integers.get("missing")));
    const auto integer_previous = integers.put("answer", int64_t{42});
    assert(pineforge::is_na(integer_previous));
    assert(integers.get("answer") == 42);

    auto strings = PineMap<bool, std::string>::new_();
    assert(strings.get(true).empty());
    const auto removed_string = strings.remove(false);
    assert(removed_string.empty());

    auto booleans = PineMap<double, bool>::new_();
    assert(!booleans.get(1.5));
    const auto removed_bool = booleans.remove(1.5);
    const auto bool_previous = booleans.put(1.5, true);
    assert(!removed_bool);
    assert(!bool_previous);
    assert(booleans.get(1.5));
}

static void test_copy_of_nested_handle_is_outer_deep_inner_shallow() {
    using Inner = PineMap<int, int>;
    using Outer = PineMap<int, Inner>;

    auto inner = Inner::new_();
    inner.put(1, 10);
    auto outer = Outer::new_();
    const auto outer_previous = outer.put(7, inner);
    assert(pineforge::is_na(outer_previous));

    auto copied = outer.copy();
    copied.get(7).put(2, 20);
    assert(inner.get(2) == 20);
    assert(outer.get(7).get(2) == 20);

    copied.remove(7);
    assert(outer.contains(7));
    assert(!copied.contains(7));
}

static void test_snapshot_surface_is_primitive_only() {
    using Primitive = PineMap<int, double>;
    using UdtValue = PineMap<int, SnapshotUnsafeUdt>;
    using CollectionValue = PineMap<int, std::vector<int>>;
    using HandleValue = PineMap<int, PineMap<int, int>>;

    static_assert(Primitive::snapshot_supported);
    static_assert(has_public_snapshot<Primitive>::value);
    static_assert(!UdtValue::snapshot_supported);
    static_assert(!CollectionValue::snapshot_supported);
    static_assert(!HandleValue::snapshot_supported);
    static_assert(!has_public_snapshot<UdtValue>::value);
    static_assert(!has_public_snapshot<CollectionValue>::value);
    static_assert(!has_public_snapshot<HandleValue>::value);
    static_assert(!pineforge::is_pine_map_snapshot_value_v<int&>);

    // Non-primitive values remain usable for ordinary Pine map operations;
    // only the unsafe public rollback surface is unavailable.
    auto udts = UdtValue::new_();
    const auto udt_previous = udts.put(1, SnapshotUnsafeUdt{7});
    assert(udt_previous.value == 0);
    assert(udts.get(1).value == 7);
}

static void test_snapshot_restores_contents_identity_and_rebinding() {
    auto live = PineMap<std::string, int>::new_();
    live.put("a", 1);
    live.put("b", 2);
    auto alias = live;
    auto snapshot = live.snapshot();

    alias.put("a", 10);
    alias.remove("b");
    alias.put("c", 3);

    auto replacement = PineMap<std::string, int>::new_();
    replacement.put("replacement", 99);
    auto replacement_alias = replacement;
    live = replacement;
    assert(live.contains("replacement"));

    live.restore(snapshot);
    assert((live.keys() == std::vector<std::string>{"a", "b"}));
    assert((live.values() == std::vector<int>{1, 2}));

    // The pre-snapshot alias sees the in-place rollback, and the restored
    // variable has reattached to that same original map ID.
    assert((alias.keys() == std::vector<std::string>{"a", "b"}));
    live.put("d", 4);
    assert(alias.get("d") == 4);

    // Reattaching live must not mutate the temporary replacement identity.
    assert(replacement_alias.size() == 1);
    assert(replacement_alias.get("replacement") == 99);
    assert(!replacement_alias.contains("d"));

    // Snapshots are reusable and remain detached from later mutations.
    alias.put("a", 100);
    alias.remove("b");
    alias.remove("d");
    live.restore(snapshot);
    assert((live.keys() == std::vector<std::string>{"a", "b"}));
    assert((live.values() == std::vector<int>{1, 2}));
}

static void test_snapshot_restores_after_rebind_to_na() {
    using Map = PineMap<std::string, int>;
    auto live = Map::new_();
    live.put("a", 1);
    live.put("b", 2);
    auto alias = live;
    auto snapshot = live.snapshot();

    live = Map{};
    assert(pineforge::is_na(live));
    alias.put("a", 10);
    alias.remove("b");
    alias.put("c", 3);

    live.restore(snapshot);
    assert(!pineforge::is_na(live));
    assert((live.keys() == std::vector<std::string>{"a", "b"}));
    assert((alias.values() == std::vector<int>{1, 2}));
    live.put("d", 4);
    assert(alias.get("d") == 4);

    // A snapshot is reusable even after another live -> na transition.
    live = Map{};
    assert(pineforge::is_na(live));
    alias.put("a", 100);
    alias.remove("b");
    alias.remove("d");
    live.restore(snapshot);
    assert((live.keys() == std::vector<std::string>{"a", "b"}));
    assert((live.values() == std::vector<int>{1, 2}));
    live.put("e", 5);
    assert(alias.get("e") == 5);
}

static void test_two_alias_members_restore_to_one_identity() {
    auto first = PineMap<int, int>::new_();
    first.put(1, 10);
    first.put(2, 20);
    auto second = first;

    auto first_snapshot = first.snapshot();
    auto second_snapshot = second.snapshot();

    first.put(1, 100);
    second.put(3, 30);
    first = PineMap<int, int>::new_();
    second = PineMap<int, int>::new_();

    first.restore(first_snapshot);
    second.restore(second_snapshot);
    assert((first.keys() == std::vector<int>{1, 2}));
    assert((second.keys() == std::vector<int>{1, 2}));

    second.put(4, 40);
    assert(first.get(4) == 40);
    first.remove(1);
    assert(!second.contains(1));
}

static void test_snapshot_keeps_original_identity_alive() {
    using Map = PineMap<int, int>;
    auto snapshot = [] {
        auto ephemeral = Map::new_();
        ephemeral.put(7, 70);
        return ephemeral.snapshot();
    }();

    auto restored = Map::new_();
    restored.put(8, 80);
    restored.restore(snapshot);
    assert(restored.size() == 1);
    assert(restored.get(7) == 70);
    assert(!restored.contains(8));
}

static void test_hot_cache_alias_invalidation() {
    using Map = PineMap<std::string, double>;
    auto map = Map::new_();
    map.put("alpha", 1.0);
    assert(map.get("alpha") == 1.0); // Populate the shared cache.

    auto alias = map;
    alias.put("alpha", 2.0);
    assert(map.get("alpha") == 2.0);

    alias.put("beta", 3.0);
    assert(map.contains("beta"));
    assert(map.get("beta") == 3.0);

    assert(map.remove("alpha") == 2.0);
    assert(!alias.contains("alpha"));
    assert(pineforge::is_na(map.get("alpha")));

    alias.put("alpha", pineforge::na<double>());
    assert(map.contains("alpha"));
    assert(pineforge::is_na(map.get("alpha")));
}

static void test_hot_cache_copy_independence_and_exact_string_keys() {
    using Map = PineMap<std::string, double>;
    auto original = Map::new_();
    original.put("answer", 1.0);
    assert(original.get("answer") == 1.0); // Populate before copying.

    auto independent = original.copy();
    independent.put("answer", 2.0);
    independent.put("only-copy", 3.0);
    assert(original.get("answer") == 1.0);
    assert(!original.contains("only-copy"));

    original.put("only-original", 4.0);
    assert(!independent.contains("only-original"));

    // Distinct external keys with the same first 24 bytes remain distinct.
    // This is a regression row; their hashes need not collide, so it does not
    // independently force the cache's full-key comparison after its hash filter.
    const std::string prefix(24, 'p');
    const std::string first = prefix + "aaaaaa";
    const std::string second = prefix + "bbbbbb";
    auto keys = Map::new_();
    keys.put(first, 11.0);
    keys.put(second, 22.0);
    assert(keys.get(first) == 11.0);
    assert(keys.get(second) == 22.0);
    assert(keys.get(first) == 11.0);
    assert(keys.remove(first) == 11.0);
    assert(keys.contains(second));
    assert(keys.get(second) == 22.0);
}

static void test_hot_cache_invalidation_for_each_map_mutation() {
    using Map = PineMap<std::string, double>;
    auto map = Map::new_();

    map.put("put", 1.0);
    assert(map.get("put") == 1.0);
    map.put("put", 2.0);
    assert(map.get("put") == 2.0);

    map.put("remove", 3.0);
    assert(map.get("remove") == 3.0);
    assert(map.remove("remove") == 3.0);
    assert(!map.contains("remove"));
    assert(pineforge::is_na(map.get("remove")));

    map.put("clear", 4.0);
    assert(map.get("clear") == 4.0);
    map.clear();
    assert(!map.contains("clear"));
    assert(pineforge::is_na(map.get("clear")));

    auto target = Map::new_();
    target.put("existing", 5.0);
    assert(target.get("existing") == 5.0);
    auto source = Map::new_();
    source.put("existing", 6.0);
    source.put("added", 7.0);
    target.put_all(source);
    assert(target.get("existing") == 6.0);
    assert(target.get("added") == 7.0);

    auto snapshot = target.snapshot();
    target.put("existing", 8.0);
    assert(target.get("existing") == 8.0);
    target.restore(snapshot);
    assert(target.get("existing") == 6.0);
    assert(target.get("added") == 7.0);
}

static void test_hot_cache_na_absent_empty_and_numeric_keys() {
    using Map = PineMap<int, double>;
    auto map = Map::new_();
    assert(map.size() == 0);
    assert(!map.contains(17));
    assert(pineforge::is_na(map.get(17)));

    map.put(17, pineforge::na<double>());
    assert(map.contains(17));
    assert(pineforge::is_na(map.get(17)));
    assert(pineforge::is_na(map.remove(17)));
    assert(!map.contains(17));
    assert(pineforge::is_na(map.get(17)));

    map.clear();
    assert(map.size() == 0);
    assert(!map.contains(17));
}

struct HotReadObservation {
    bool present;
    double value;
};

static std::vector<HotReadObservation> run_hot_read_sequence(bool warm) {
    using Map = PineMap<std::string, double>;
    auto map = Map::new_();
    map.put("alpha", 1.0);
    map.put("beta", pineforge::na<double>());
    map.put("gamma", 3.0);

    if (warm) {
        for (const char* key : {"alpha", "beta", "gamma", "missing"}) {
            (void)map.contains(key);
            (void)map.get(key);
        }
    }

    std::vector<HotReadObservation> observations;
    const auto observe = [&observations, &map](const char* key) {
        observations.push_back({map.contains(key), map.get(key)});
    };
    observe("alpha");
    observe("beta");
    observe("missing");
    map.put("alpha", 4.0);
    observe("alpha");
    map.remove("gamma");
    observe("gamma");
    map.put("delta", 5.0);
    observe("delta");
    return observations;
}

static void test_hot_cache_cold_and_warm_sequences_match() {
    const auto cold = run_hot_read_sequence(false);
    const auto warm = run_hot_read_sequence(true);
    const std::array<HotReadObservation, 6> expected{{
        {true, 1.0},
        {true, pineforge::na<double>()},
        {false, pineforge::na<double>()},
        {true, 4.0},
        {false, pineforge::na<double>()},
        {true, 5.0},
    }};
    assert(cold.size() == expected.size());
    assert(warm.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        assert(cold[index].present == expected[index].present);
        assert(warm[index].present == expected[index].present);
        const bool expected_na = pineforge::is_na(expected[index].value);
        const bool cold_na = pineforge::is_na(cold[index].value);
        const bool warm_na = pineforge::is_na(warm[index].value);
        assert(cold_na == expected_na);
        assert(warm_na == expected_na);
        if (!expected_na) {
            assert(cold[index].value == expected[index].value);
            assert(warm[index].value == expected[index].value);
        }
    }
}

static void test_hot_cache_lifetime_and_history() {
    using Map = PineMap<int, int>;
    auto var = Map::new_();
    auto varip = var;
    var.put(1, 10);
    const auto read_var = [&var] { return var.get(1); };
    assert(read_var() == 10);

    varip.put(1, 11);
    assert(read_var() == 11);
    auto previous = var.snapshot();
    var.put(1, 12);
    assert(read_var() == 12);

    auto history = Map::new_();
    history.restore(previous);
    assert(history.get(1) == 11);
    var.restore(previous);
    assert(read_var() == 11);
}

static void test_hot_cache_fixed_memory_and_replacement() {
    using Cache = pineforge::detail::MapReadCache<std::uint64_t, std::uint64_t>;
    static_assert(std::is_trivially_copyable_v<Cache>);
    static_assert(sizeof(Cache) == Cache::capacity * sizeof(Cache::Entry) +
                                       sizeof(std::size_t) + sizeof(std::uint64_t));
    Cache one;
    one.record(1, 1, 1, 2);
    std::size_t one_valid_slot = 0;
    for (const auto& entry : one.entries)
        if (entry.valid) ++one_valid_slot;
    assert(one_valid_slot == 1);
    assert(one.entries[0].key == 1 && one.entries[0].value == 2);

    Cache many;
    for (std::uint64_t index = 0; index != 100'000; ++index) {
        many.record(index, static_cast<std::uint32_t>(index), index, index + 1);
    }
    std::size_t valid_slots = 0;
    std::uint64_t expected_key = 99'992; // Final eight records, in ring order.
    for (const auto& entry : many.entries) {
        if (entry.valid) ++valid_slots;
        assert(entry.key == expected_key);
        assert(entry.value == expected_key + 1);
        ++expected_key;
    }
    assert(valid_slots == Cache::capacity);

    using Map = PineMap<std::string, double>;
    static_assert(Map::hot_read_cache_slots == 8);
    static_assert(Map::hot_read_cache_bytes == 528);
}

static void test_hot_cache_store_read_counts() {
    using Map = PineMap<std::string, double>;
    auto owner = std::make_shared<pineforge::detail::MapStoreHolder>();
    ReadCountingStore counting(*owner->store);
    auto holder = std::make_shared<pineforge::detail::MapStoreHolder>(counting);
    pineforge::detail::MapStoreScope scope(holder);
    auto map = Map::new_();
    map.put("inline", 1.0);

    auto before = counting.reads;
    assert(map.get("inline") == 1.0);
    // Header, bucket, links, key rep, value rep: the uncached read budget.
    assert(counting.reads - before == 5);
    before = counting.reads;
    assert(map.get("inline") == 1.0);
    assert(counting.reads == before);
    assert(map.contains("inline"));
    assert(counting.reads == before);

    map.put("inline", 2.0);
    before = counting.reads;
    assert(map.contains("inline"));
    // contains fetches both reps in the existing key-rep read.
    assert(counting.reads - before == 4);
    before = counting.reads;
    assert(map.contains("inline"));
    assert(counting.reads == before);
    assert(map.get("inline") == 2.0);
    assert(counting.reads == before);

    map.put("inline", 3.0);
    before = counting.reads;
    assert(map.get("inline") == 3.0);
    assert(counting.reads - before == 5);

    // A Store rollback invalidates even an untouched map. Refill must stamp
    // the new epoch, or the subsequent identical get can never become a hit.
    {
        auto tx = counting.begin();
        tx->rollback();
    }
    before = counting.reads;
    assert(map.get("inline") == 3.0);
    assert(counting.reads - before == 5);
    before = counting.reads;
    assert(map.get("inline") == 3.0);
    assert(counting.reads == before);

    before = counting.reads;
    assert(!map.contains("absent"));
    const auto absent_reads = counting.reads - before;
    assert(absent_reads > 0);
    before = counting.reads;
    assert(!map.contains("absent"));
    assert(counting.reads - before == absent_reads);
    before = counting.reads;
    assert(pineforge::is_na(map.get("absent")));
    assert(counting.reads - before == absent_reads);

    map.clear();
    for (int index = 0; index < 9; ++index)
        map.put(std::to_string(index), static_cast<double>(index));
    for (int index = 0; index < 9; ++index)
        assert(map.get(std::to_string(index)) == static_cast<double>(index));
    before = counting.reads;
    assert(map.get("0") == 0.0);
    assert(counting.reads > before); // The ninth distinct key evicted the first.
    before = counting.reads;
    assert(map.get("0") == 0.0);
    assert(counting.reads == before);
}

static void test_hot_cache_outer_transaction_rollback() {
    using Map = PineMap<std::string, double>;
    auto holder = std::make_shared<pineforge::detail::MapStoreHolder>();
    pineforge::detail::MapStoreScope scope(holder);
    auto map = Map::new_();
    map.put("k", 1.0);
    {
        auto tx = holder->store->begin();
        map.put("k", 2.0);
        map.put("j", 3.0);
        assert(map.get("k") == 2.0);
        assert(map.contains("j"));
        tx->rollback();
    }
    assert(map.get("k") == 1.0);
    assert(!map.contains("j"));

    // Exercise contains as the first lookup after a second rollback too.
    {
        auto tx = holder->store->begin();
        map.put("k", 4.0);
        map.put("j", 5.0);
        assert(map.get("k") == 4.0);
        assert(map.contains("j"));
        tx->rollback();
    }
    assert(!map.contains("j"));
    assert(map.get("k") == 1.0);

    using StringMap = PineMap<std::string, std::string>;
    auto strings = StringMap::new_();
    const std::string persistent(25, 'p');
    const std::string added(25, 'j');
    const std::string initial(65, 'v');
    const std::string changed(66, 'w');
    strings.put(persistent, initial);
    {
        auto tx = holder->store->begin();
        strings.put(persistent, changed);
        strings.put(added, changed);
        assert(strings.get(persistent) == changed);
        assert(strings.contains(added));
        tx->rollback();
    }
    // The added key's cached payload id and the changed value's payload id
    // are dead now. Check the epoch before comparing or decoding either rep.
    assert(!strings.contains(added));
    assert(strings.get(persistent) == initial);
}

static void test_hot_cache_rollback_only_commit_and_abandonment() {
    using Map = PineMap<std::string, double>;
    auto holder = std::make_shared<pineforge::detail::MapStoreHolder>();
    pineforge::detail::MapStoreScope scope(holder);
    auto map = Map::new_();
    map.put("k", 1.0);
    {
        auto tx = holder->store->begin();
        map.put("k", 2.0);
        map.put("j", 3.0);
        assert(map.get("k") == 2.0);
        assert(map.contains("j"));
        {
            auto joined = holder->store->begin();
            joined->rollback(); // Poison the outer transaction.
        }
        bool rollback_only = false;
        try {
            tx->commit(); // Restores bytes before throwing RollbackOnly.
        } catch (const pineforge::detail::o1::RollbackOnly&) {
            rollback_only = true;
        }
        assert(rollback_only);
    }
    assert(map.get("k") == 1.0);
    assert(!map.contains("j"));

    {
        auto tx = holder->store->begin();
        map.put("k", 4.0);
        map.put("j", 5.0);
        assert(map.get("k") == 4.0);
        assert(map.contains("j"));
        // Unfinished outer handle rolls back in its destructor.
    }
    assert(!map.contains("j"));
    assert(map.get("k") == 1.0);
}

template <typename Value>
static void check_hot_cache_read_views(const std::string& key, const Value& initial,
                                      const Value& changed, std::size_t key_reads,
                                      std::size_t value_reads) {
    using Map = PineMap<std::string, Value>;
    auto owner = std::make_shared<pineforge::detail::MapStoreHolder>();
    ReadCountingStore counting(*owner->store);
    auto holder = std::make_shared<pineforge::detail::MapStoreHolder>(counting);
    pineforge::detail::MapStoreScope scope(holder);
    auto map = Map::new_();
    map.put(key, initial);
    assert(map.get(key) == initial);
    const auto check_cached = [&] {
        auto before = counting.reads;
        assert(map.get(key) == initial);
        assert(counting.reads - before == key_reads + value_reads);
        before = counting.reads;
        assert(map.contains(key));
        assert(counting.reads - before == key_reads);
    };
    check_cached();
    {
        const auto keys = map.keys();
        assert(keys.size() == 1 && keys[0] == key);
        check_cached();
        const auto values = map.values();
        assert(values.size() == 1 && values[0] == initial);
        check_cached();
        const auto independent = map.copy();
        assert(independent.get(key) == initial);
        check_cached();
        const auto snapshot = map.snapshot();
        check_cached();

        map.put(key, changed);
        assert(map.get(key) == changed);
        map.restore(snapshot);
        assert(map.get(key) == initial);
        check_cached();
    } // Retained images/copy release payload refcounts without changing this map.
    check_cached();
}

static void test_hot_cache_read_views_preserve_hits() {
    check_hot_cache_read_views(std::string(24, 'i'), 1.0, 2.0, 0, 0);
    check_hot_cache_read_views("inline-value", std::string(65, 'v'),
                              std::string(66, 'w'), 0, 1);
    // The external key needs two 512-byte comparisons; the external value
    // needs one decode read, even when the map-slot representations are cached.
    check_hot_cache_read_views(std::string(513, 'k'), std::string(65, 'v'),
                              std::string(66, 'w'), 2, 1);
}

static void test_empty_maps_materialize_fixed_headers_only() {
    using Map = PineMap<std::string, double>;
    using Holder = pineforge::detail::MapStoreHolder;
    using Stats = pineforge::detail::o1::Stats;

    auto holder = std::make_shared<Holder>();
    pineforge::detail::MapStoreScope scope(holder);
    const Stats baseline = holder->store->stats();
    constexpr std::size_t map_count = 2000;
    constexpr std::uint64_t logical_bytes_per_map = UINT64_C(3862176);
    constexpr std::uint64_t max_backing_bytes_per_map = UINT64_C(512) * 1024;
    constexpr std::array<std::size_t, 3> marks{{1, 100, map_count}};
    std::vector<Map> maps;
    maps.reserve(map_count);

    std::size_t next_mark = 0;
    for (std::size_t index = 0; index < map_count; ++index) {
        maps.push_back(Map::new_());
        if (index + 1 == marks[next_mark]) {
            const Stats checkpoint = holder->store->stats();
            const std::uint64_t count = marks[next_mark];
            assert(checkpoint.objects_live == 2 * count);
            assert(checkpoint.bytes_live == count * logical_bytes_per_map);
            assert(checkpoint.bytes_backing >= baseline.bytes_backing);
            assert(checkpoint.bytes_backing - baseline.bytes_backing <=
                   count * max_backing_bytes_per_map);
            ++next_mark;
        }
    }

    assert(next_mark == marks.size());
    assert(maps.back().size() == 0);

    // The same handles can execute a real put/get workload after the empty
    // construction checkpoints. These values are inline, so no payload object
    // changes the two-map-object accounting above.
    for (std::size_t index = 0; index < maps.size(); index += 500) {
        maps[index].put("bar", static_cast<double>(index));
        assert(maps[index].get("bar") == static_cast<double>(index));
    }
}

static void test_map_growth_rolls_back_and_abandons_cleanly() {
    using Map = PineMap<std::string, int>;
    auto holder = std::make_shared<pineforge::detail::MapStoreHolder>();
    pineforge::detail::MapStoreScope scope(holder);
    auto map = Map::new_();
    const auto fill = [](Map& target) {
        for (int index = 0; index < 40; ++index)
            target.put("k" + std::to_string(index), index);
    };

    {
        auto transaction = holder->store->begin();
        fill(map);
        assert(map.size() == 40);
        assert(map.get("k39") == 39);
        transaction->rollback();
    }
    assert(map.size() == 0);
    assert(!map.contains("k39"));

    {
        auto transaction = holder->store->begin();
        fill(map);
        assert(map.size() == 40);
    }
    assert(map.size() == 0);
    assert(!map.contains("k0"));

    {
        auto transaction = holder->store->begin();
        fill(map);
        auto joined = holder->store->begin();
        joined->rollback();
        bool rollback_only = false;
        try {
            transaction->commit();
        } catch (const pineforge::detail::o1::RollbackOnly&) {
            rollback_only = true;
        }
        assert(rollback_only);
    }
    assert(map.size() == 0);
    assert(!map.contains("k20"));
}

static void test_map_clear_copy_and_snapshot_cross_growth_step() {
    using Map = PineMap<std::string, int>;
    auto map = Map::new_();
    const std::vector<std::string> expected_keys{
        "k0",  "k1",  "k2",  "k3",  "k4",  "k5",  "k6",  "k7",
        "k8",  "k9",  "k10", "k11", "k12", "k13", "k14", "k15"};
    const std::vector<int> expected_values{
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int index = 0; index < 16; ++index)
        map.put("k" + std::to_string(index), index);

    auto independent = map.copy();
    auto snapshot = map.snapshot();
    map.clear();
    assert(map.size() == 0);
    assert(independent.keys() == expected_keys);
    assert(independent.values() == expected_values);

    map.restore(snapshot);
    assert(map.keys() == expected_keys);
    assert(map.values() == expected_values);
    map.put("k16", 16);
    assert(map.size() == 17);
    map.restore(snapshot);
    assert(map.keys() == expected_keys);
    assert(map.values() == expected_values);
}

static void test_heap_value_maps_grow_the_value_table_on_use() {
    using Map = PineMap<int, HeapValueProbe>;
    // Open the shared Store before counting this map's own allocations.
    auto holder = std::make_shared<pineforge::detail::MapStoreHolder>();
    pineforge::detail::MapStoreScope scope(holder);
    HeapValueProbe::default_constructed = 0;
    const std::size_t bytes_before = global_allocation::live_bytes;
    global_allocation::recording = true;
    {
        auto map = Map::new_();
        const std::size_t creation_bytes = global_allocation::live_bytes - bytes_before;
        // reserve() constructs no probes, but its full-capacity allocation
        // alone reaches this limit on the eager implementation.
        assert(creation_bytes < std::size_t{50000} * sizeof(HeapValueProbe));
        assert(HeapValueProbe::default_constructed == 0);

        map.put(7, HeapValueProbe{42});
        assert(map.size() == 1);
        assert(map.get(7).value == 42);
    }
    // Destroy every recorded block while byte accounting is still enabled.
    global_allocation::recording = false;
}

int main() {
    static_assert(std::is_copy_constructible_v<PineMap<int, double>>);
    static_assert(std::is_copy_assignable_v<PineMap<int, double>>);
    static_assert(std::is_nothrow_default_constructible_v<PineMap<int, double>>);
    static_assert(std::is_copy_constructible_v<PineMap<int, double>::Snapshot>);
    static_assert(std::is_copy_assignable_v<PineMap<int, double>::Snapshot>);
    static_assert(std::is_same_v<
                  decltype(std::declval<PineMap<int, double>&>().put(1, 2.0)),
                  double>);

    test_na_id_and_stable_errors();
    test_copy_aliases_and_explicit_copy_is_independent();
    test_move_preserves_source_handle_alias();
    test_put_get_and_stable_insertion_order();
    test_float_keys_use_pine_na_and_zero_equality();
    test_remove_updates_order_and_reinsert_appends();
    test_put_all_overwrites_in_source_order();
    test_50000_pair_capacity_and_put_all_preflight();
    test_clear_mutates_alias_only();
    test_typed_missing_values_for_primitive_templates();
    test_copy_of_nested_handle_is_outer_deep_inner_shallow();
    test_snapshot_surface_is_primitive_only();
    test_snapshot_restores_contents_identity_and_rebinding();
    test_snapshot_restores_after_rebind_to_na();
    test_two_alias_members_restore_to_one_identity();
    test_snapshot_keeps_original_identity_alive();
    test_hot_cache_alias_invalidation();
    test_hot_cache_copy_independence_and_exact_string_keys();
    test_hot_cache_invalidation_for_each_map_mutation();
    test_hot_cache_na_absent_empty_and_numeric_keys();
    test_hot_cache_cold_and_warm_sequences_match();
    test_hot_cache_lifetime_and_history();
    test_hot_cache_fixed_memory_and_replacement();
    test_hot_cache_store_read_counts();
    test_hot_cache_outer_transaction_rollback();
    test_hot_cache_rollback_only_commit_and_abandonment();
    test_hot_cache_read_views_preserve_hits();
    test_empty_maps_materialize_fixed_headers_only();
    test_map_growth_rolls_back_and_abandons_cleanly();
    test_map_clear_copy_and_snapshot_cross_growth_step();
    test_heap_value_maps_grow_the_value_table_on_use();

    std::printf("All test_map tests passed.\n");
    return 0;
}
