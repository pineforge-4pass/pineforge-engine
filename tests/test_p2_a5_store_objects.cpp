// CR-417: no fixed Store object count for string/map/split operations.
// Fail-before on ff813d6dd: the string's second object is number 1,048,577,
// and the old default refuses it. Counts and bytes below come from the fixture
// inputs and the two-object string/map layouts, never from observed outputs.
#include "p2_a5strmap_fixture.hpp"
#include "p2_a5strmap_store_fixture.hpp"

#include <pineforge/map.hpp>
#include <pineforge/str_utils.hpp>

namespace {
using namespace pineforge;
using a5_fixture::require;

void check_objects_beyond_old_boundary() {
    a5_fixture::Session session;
    auto& store = *session.store;
    constexpr std::uint64_t seed_objects = UINT64_C(1048575);
    const auto resident = store.stats().bytes_resident;
    // Seed only backing records, with no heap-side directory or retained
    // handles. Subsequent family operations must cross the former boundary.
    {
        auto tx = store.begin();
        for (std::uint64_t number = 1; number <= seed_objects; ++number)
            require(store.create(0, 1) == number, "seed object id differs");
        tx->commit();
    }
    require(store.stats().objects_live == seed_objects, "seed live count differs");
    {
        const ScriptString text(store, std::string_view("A\0B,,C,", 7));
        require(store.stats().objects_live == UINT64_C(1048577),
                "string did not retain its payload and root beyond the old boundary");
        require(store.alive(UINT64_C(1048576)) && store.alive(UINT64_C(1048577)),
                "string object number 1048577 does not exist");
        require(text == std::string_view("A\0B,,C,", 7), "boundary string bytes differ");
        const ScriptString separator(store, ",");
        require(store.stats().objects_live == UINT64_C(1048579),
                "separator did not retain both objects");
        {
            const auto parts = str_split(text, separator);
            require(parts.size() == 4, "boundary split lost empty or trailing parts");
            require(parts[0] == std::string_view("A\0B", 3) &&
                        parts[1].empty() && parts[2] == "C" && parts[3].empty(),
                    "boundary split changed exact bytes");
            require(store.stats().objects_live == UINT64_C(1048579),
                    "split retained extra Store objects");
        }

        auto holder = std::make_shared<detail::MapStoreHolder>(store);
        detail::MapStoreScope map_scope(holder);
        auto map = PineMap<int, std::string>::new_();
        require(store.stats().objects_live == UINT64_C(1048581),
                "string map did not retain its two arena objects");
        // Exceed the inline codec size so insertion must create a payload
        // object while the live count already exceeds the former boundary.
        std::string payload(96, 'q');
        payload[17] = '\0';
        (void)map.put(7, payload);
        require(store.stats().objects_live == UINT64_C(1048582),
                "map did not retain its external string payload");
        require(store.alive(UINT64_C(1048582)), "map payload object does not exist");
        require(map.size() == 1 && map.get(7) == payload,
                "boundary map changed exact payload bytes");

        auto prepared = PineMap<int, double>::new_();
        require(store.stats().objects_live == UINT64_C(1048584),
                "prepared map did not retain its two arena objects");
        (void)prepared.put(3, 12.5);
        (void)prepared.put(3, -7.25);
        require(prepared.size() == 1 && prepared.get(3) == -7.25,
                "boundary prepared map changed its scalar value");
        require(store.stats().objects_live == UINT64_C(1048584),
                "prepared scalar update created extra objects");
    }
    require(store.stats().objects_live == seed_objects,
            "boundary string/map cleanup leaked objects");
    require(store.stats().bytes_live == 0, "boundary cleanup leaked payload bytes");
    require(store.stats().cleanup_failed == 0 &&
                store.stats().cleanup_skipped_rollback_only == 0,
            "boundary cleanup swallowed a failure");
    require(store.stats().bytes_resident == resident,
            "Store workspace grew with its object population");
}
} // namespace

int main() {
    try {
        check_objects_beyond_old_boundary();
        std::puts("A5 store objects: strings/maps/split succeed beyond 1048576");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A5 store objects FAIL: %s\n", error.what());
        return 1;
    }
}
