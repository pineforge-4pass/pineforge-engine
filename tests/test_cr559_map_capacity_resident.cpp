// Expected red on a3e7822e: the empty map declares a 50,000-slot arena,
// exceeding the fixed-step initial geometry bound below. If that assertion
// is bypassed, put 50,001 still refuses and neither root ever resizes.
#include "p2_a5strmap_fixture.hpp"
#include "cr559_map_capacity_fixture.hpp"

int main() {
    try {
        cr559::Session session;
        const auto baseline = session.store.stats();
        const auto resident_ceiling = a5_fixture::store_ceiling(session.disk.shape);
        cr559::require(baseline.bytes_resident <= resident_ceiling,
                       "Store workspace exceeds the neighbouring geometry helper");
        {
            auto map = cr559::Map::new_();
            constexpr std::uint64_t step = 4096;
            constexpr std::uint64_t header = sizeof(pineforge::detail::MapHeader);
            constexpr std::uint64_t slot =
                (sizeof(pineforge::detail::MapLinks) + 2 * sizeof(cr559::Scalar) + 7) / 8 * 8;
            constexpr std::uint64_t bucket = sizeof(pineforge::detail::MapLinks{}.chain);
            // Up to one arena step at open; a power-of-two bucket table with
            // <= 3/4 load needs fewer than four buckets per reserved slot.
            cr559::require(session.store.stats().bytes_live - baseline.bytes_live <=
                               header + step * (slot + 4 * bucket),
                           "empty map still declares the whole historical capped arena");

            // The Store stats prove its fixed resident window. The adjacent
            // A5 heap census also catches a map-sized C++ rehash directory or
            // retained node table hidden outside that window.
            const auto heap_before = a5_heap::live.load();
            a5_heap::reset_peak();
            a5_heap::Window measured;
            cr559::Scalar inserted = 0;
            for (const cr559::Scalar wanted :
                 {1, 4096, 4097, 50000, 50001, 120000, 1000000}) {
                while (inserted < wanted) {
                    (void)map.put(cr559::key_for(inserted), cr559::value_for(inserted));
                    ++inserted;
                }
                const auto stats = session.store.stats();
                const auto capacity =
                    (static_cast<std::uint64_t>(wanted) + step - 1) / step * step;
                cr559::require(stats.bytes_resident == baseline.bytes_resident &&
                                   stats.bytes_resident <= resident_ceiling,
                               "map population grew the Store resident workspace");
                cr559::require(stats.bytes_live - baseline.bytes_live <=
                                   header + capacity * (slot + 4 * bucket),
                               "map logical footprint grew faster than pairs plus one step");
                cr559::require(a5_heap::growth(a5_heap::live.load(), heap_before) <=
                                   a5_fixture::operation_ceiling &&
                                   a5_heap::growth(a5_heap::peak.load(), heap_before) <=
                                   a5_fixture::operation_ceiling,
                               "map growth retained or materialized population-sized C++ heap");
                cr559::require(3 * session.store.bucket_count() >=
                                   4 * static_cast<std::uint64_t>(wanted),
                               "bucket population no longer bounds average hash-chain load");
                cr559::require(map.get(cr559::key_for(wanted - 1)) ==
                                   cr559::value_for(wanted - 1),
                               "resident sample lost its last written value");
                std::printf("CR559 resident pairs=%lld resident=%llu live=%llu buckets=%llu\n",
                            static_cast<long long>(wanted),
                            static_cast<unsigned long long>(stats.bytes_resident),
                            static_cast<unsigned long long>(stats.bytes_live),
                            static_cast<unsigned long long>(session.store.bucket_count()));
            }
            cr559::require(session.store.arena_growths > 1 && session.store.index_growths > 1,
                           "map arena/index did not grow through Store::resize");
            cr559::require(session.store.fixed_arena_steps &&
                               session.store.arena_step_bytes == step * slot,
                           "unlimited arena growth did not use the documented fixed step");
            const auto stats = session.store.stats();
            cr559::require(stats.bytes_backing > stats.bytes_resident,
                           "resident row did not spill beyond its fixed working set");
            const auto reads_before = session.store.read_calls;
            for (cr559::Scalar index = 0; index < inserted; ++index)
                cr559::require(map.get(cr559::key_for(index)) == cr559::value_for(index),
                               "million-pair rehash changed a stored value");
            // For this distributed integer population, uncached get reads a
            // header, a bucket, a short chain, a key and a value. Count Store
            // reads instead of timing: allocating unused extra buckets while
            // still hashing into the old 16-bit domain must fail too.
            cr559::require(session.store.read_calls - reads_before <=
                               8 * static_cast<std::uint64_t>(inserted),
                           "million-pair lookup chains still grow with population");
            cr559::require(a5_heap::growth(a5_heap::live.load(), heap_before) <=
                               a5_fixture::operation_ceiling &&
                               a5_heap::growth(a5_heap::peak.load(), heap_before) <=
                               a5_fixture::operation_ceiling,
                           "map lookups retained or materialized population-sized C++ heap");
        }
        cr559::require(session.store.stats().bytes_live == baseline.bytes_live,
                       "resident row retained dead map roots");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CR559 resident FAIL: %s\n", error.what());
        return 1;
    }
}
