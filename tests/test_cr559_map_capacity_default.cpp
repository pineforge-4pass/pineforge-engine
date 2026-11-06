// Expected red on a3e7822e: put_locked rejects the 50,001st distinct key
// with the old fixed-cap message, before reaching the requested 120,000.
#include "cr559_map_capacity_fixture.hpp"

int main() {
    try {
        cr559::Session session;
        {
            auto map = cr559::Map::new_();
            constexpr cr559::Scalar pairs = 120000;
            for (cr559::Scalar index = 0; index < pairs; ++index) {
                const auto previous = map.put(cr559::key_for(index), cr559::value_for(index));
                cr559::require(pineforge::is_na(previous), "default distinct put replaced a key");
            }
            cr559::require(map.size() == pairs, "default map omitted successful puts");
            for (cr559::Scalar index = 0; index < pairs; ++index) {
                cr559::require(map.contains(cr559::key_for(index)), "default map lost a key");
                cr559::require(map.get(cr559::key_for(index)) == cr559::value_for(index),
                               "default get differs from its put value");
            }
            cr559::require(!map.contains(cr559::key_for(pairs)), "absent key became present");
            const auto old = map.put(cr559::key_for(71), -19);
            cr559::require(old == cr559::value_for(71) &&
                               map.get(cr559::key_for(71)) == -19 && map.size() == pairs,
                           "default replace-put changed size or previous value");
        }
        cr559::require(session.store.stats().bytes_live == 0, "default map leaked Store objects");
        std::puts("CR559 default: 120000 distinct puts and every get checked");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CR559 default FAIL: %s\n", error.what());
        return 1;
    }
}
