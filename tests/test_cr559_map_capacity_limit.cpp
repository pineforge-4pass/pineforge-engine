// Expected red on a3e7822e: the checked map-policy export is absent.
// Once plumbing exists, the old map still reports pine_runtime_limit and
// legacy English instead of the RE10070 context asserted here.
#include "cr559_map_capacity_fixture.hpp"

#include <pineforge/ta.hpp>

namespace {
struct Words {
    std::vector<std::uint64_t> values;
    void u(std::uint64_t value) { values.push_back(value); }
};

std::vector<std::uint64_t> state_words(const cr559::Map& map) {
    Words sink;
    map.hash_into(sink, [](Words& out, cr559::Scalar key) { out.u(key); },
                   [](Words& out, cr559::Scalar value) { out.u(value); });
    return sink.values;
}

template <class Key, class MakeKey>
void parity_limit(MakeKey make_key, cr559::Scalar bar) {
    using Map = pineforge::PineMap<Key, cr559::Scalar>;
    cr559::Session session;
    cr559::LimitScope policy(50000);
    auto map = Map::new_();
    policy.unlimited(); // An existing map must retain its own parameter.
    pineforge::ta::BarContextScope context(bar, 0);
    for (cr559::Scalar index = 0; index < 50000; ++index) {
        const auto old = map.put(make_key(index), cr559::value_for(index));
        cr559::require(pineforge::is_na(old), "distinct key was treated as replacement");
    }
    cr559::require(map.size() == 50000, "the 50000th distinct key did not succeed");
    cr559::require(map.put(make_key(19), -23) == cr559::value_for(19) &&
                       map.get(make_key(19)) == -23 && map.size() == 50000,
                   "replace-put at the configured maximum was counted as growth");
    cr559::expect_capacity_error([&] { (void)map.put(make_key(50000), 91); },
                                50000, 50001, bar);
    cr559::require(map.size() == 50000 && !map.contains(make_key(50000)) &&
                       map.get(make_key(19)) == -23,
                   "refused put changed the map");

    // Removing one distinct key opens exactly one admission slot.
    cr559::require(map.remove(make_key(7)) == cr559::value_for(7), "remove lost its value");
    cr559::require(pineforge::is_na(map.put(make_key(50000), 91)) &&
                       map.size() == 50000 && map.get(make_key(50000)) == 91,
                   "remove did not free one place under the parameter");
    cr559::expect_capacity_error([&] { (void)map.put(make_key(50001), 93); },
                                50000, 50001, bar);

    auto source = Map::new_(); // Created after reset: unlimited.
    (void)source.put(make_key(19), 777);
    (void)source.put(make_key(50001), 888);
    cr559::expect_capacity_error([&] { map.put_all(source); }, 50000, 50001, bar);
    cr559::require(map.get(make_key(19)) == -23 && !map.contains(make_key(50001)),
                   "put_all capacity preflight partially overwrote the destination");
    auto copy = map.copy();
    cr559::expect_capacity_error([&] { (void)copy.put(make_key(50002), 95); },
                                50000, 50001, bar);
    map.clear();
    (void)map.put(make_key(3), 31);
    cr559::require(map.size() == 1 && map.get(make_key(3)) == 31,
                   "clear lost the map's reusable geometry");
}

void non_tv_parameter_and_unlimited_reset() {
    cr559::Session session;
    cr559::LimitScope policy(3);
    policy.refuse_negative();
    auto three = cr559::Map::new_();
    policy.set(0);
    auto unlimited = cr559::Map::new_();
    pineforge::ta::BarContextScope context(29, 0);
    for (cr559::Scalar index = 0; index < 3; ++index)
        (void)three.put(cr559::key_for(index), cr559::value_for(index));
    const auto limited_words = state_words(three);
    const auto checkpoint = three.snapshot();
    cr559::expect_capacity_error([&] { (void)three.put(cr559::key_for(3), 41); },
                                3, 4, 29);
    three.clear();
    for (cr559::Scalar index = 7; index < 10; ++index)
        (void)three.put(cr559::key_for(index), cr559::value_for(index));
    cr559::expect_capacity_error([&] { (void)three.put(cr559::key_for(10), 43); },
                                3, 4, 29);
    three.restore(checkpoint);
    cr559::require(three.size() == 3 && three.get(cr559::key_for(0)) == cr559::value_for(0) &&
                       !three.contains(cr559::key_for(7)) && state_words(three) == limited_words,
                   "limited snapshot did not restore its entries");
    cr559::expect_capacity_error([&] { (void)three.put(cr559::key_for(3), 47); },
                                3, 4, 29);
    // A small parameter must use the same logic as TV parity, while zero
    // must remove the admission bound rather than select the historical cap.
    for (cr559::Scalar index = 0; index < 50003; ++index)
        (void)unlimited.put(cr559::key_for(index), cr559::value_for(index));
    cr559::require(unlimited.size() == 50003 &&
                       unlimited.get(cr559::key_for(50002)) == cr559::value_for(50002),
                   "zero map-pair parameter retained a hidden historical cap");
    unlimited.clear(); // Retain the large physical geometry, with new logical contents.
    policy.set(4);
    auto four = cr559::Map::new_();
    policy.unlimited();
    auto fresh = cr559::Map::new_();
    std::vector<std::uint64_t> original_fold{3};
    for (cr559::Scalar index = 0; index < 3; ++index) {
        (void)unlimited.put(cr559::key_for(index), cr559::value_for(index));
        (void)fresh.put(cr559::key_for(index), cr559::value_for(index));
        (void)four.put(cr559::key_for(index), cr559::value_for(index));
        original_fold.push_back(static_cast<std::uint64_t>(cr559::key_for(index)));
        original_fold.push_back(static_cast<std::uint64_t>(cr559::value_for(index)));
    }
    cr559::require(state_words(unlimited) == original_fold && state_words(fresh) == original_fold,
                   "unlimited map changed the original count/key/value state fold");
    cr559::require(limited_words != original_fold &&
                       state_words(four) != original_fold && state_words(four) != limited_words,
                   "per-map policy or its actual parameter value is missing from state");
}
} // namespace

int main() {
    try {
        // Pull the runtime C ABI object into a static-library test as well as
        // exporting its additive setter for dlsym.
        cr559::require(strategy_last_run_status(nullptr) == -1,
                       "null strategy status contract changed");
        parity_limit<cr559::Scalar>([](cr559::Scalar index) { return cr559::key_for(index); }, 0);
        parity_limit<std::string>([](cr559::Scalar index) {
            return "key:" + std::to_string(cr559::key_for(index));
        }, 0);
        non_tv_parameter_and_unlimited_reset();
        std::puts("CR559 limit: int/string boundaries, exact TV context, per-map policy checked");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CR559 limit FAIL: %s\n", error.what());
        return 1;
    }
}
