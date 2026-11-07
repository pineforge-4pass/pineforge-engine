// Expected red on a3e7822e: the speculative growth pass refuses pair 50,001.
// Store rollback must restore root extents as well as live bytes and entries;
// restoring only C++ counters or only the entry list cannot satisfy this row.
#include "cr559_map_capacity_fixture.hpp"

int main() {
    try {
        cr559::Session session;
        auto map = cr559::Map::new_();
        auto alias = map;
        cr559::Model baseline;
        cr559::append(map, baseline, 0, 4096);
        const auto checkpoint = map.snapshot();
        const auto bytes_before = session.store.stats().bytes_live;
        const auto arena_before = session.store.arena_bytes();
        const auto index_before = session.store.index_bytes();
        {
            auto tx = session.store.begin();
            auto speculative = baseline;
            cr559::append(map, speculative, 4096, 65539);
            cr559::require(session.store.arena_bytes() > arena_before &&
                               session.store.index_bytes() > index_before,
                           "speculative calculation did not grow both Store roots");
            cr559::require(alias.put(cr559::key_for(3), -61) == cr559::value_for(3),
                           "speculative replace lost its previous value");
            cr559::require(alias.remove(cr559::key_for(7)) == cr559::value_for(7),
                           "speculative remove lost its value");
            // Warm both a replaced representation and one beyond the old
            // extent, so a stale read cache cannot hide behind cold lookups.
            cr559::require(map.get(cr559::key_for(3)) == -61 &&
                               map.contains(cr559::key_for(65538)) &&
                               map.get(cr559::key_for(65538)) == cr559::value_for(65538),
                           "speculative state was not observable before rollback");
            tx->rollback();
        }
        cr559::require(session.store.stats().bytes_live == bytes_before &&
                           session.store.arena_bytes() == arena_before &&
                           session.store.index_bytes() == index_before,
                       "Store rollback did not restore growth geometry byte-exactly");
        cr559::exact(map, baseline);
        cr559::exact(alias, baseline);
        cr559::require(!map.contains(cr559::key_for(65538)) &&
                           pineforge::is_na(map.get(cr559::key_for(65538))),
                       "rollback retained a speculative key or hot cached value");

        // Grow the same identity again after rollback. Cached native geometry
        // from the discarded branch must not cause short Store transfers.
        auto grown = baseline;
        cr559::append(map, grown, 4096, 70007);
        cr559::exact(alias, grown);
        map.restore(checkpoint);
        cr559::exact(map, baseline);
        cr559::exact(alias, baseline);
        map = cr559::Map{};
        map.restore(checkpoint);
        cr559::exact(map, baseline);
        std::puts("CR559 rollback: grown roots, aliases, hot cache and reusable snapshot checked");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CR559 rollback FAIL: %s\n", error.what());
        return 1;
    }
}
