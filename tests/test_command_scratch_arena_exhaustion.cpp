// Set A: no new production API. On c7b06a6a19 the bounded puts never encounter
// arena exhaustion because the production factory silently uses a disk file.
#include "command_scratch_arena_fixture.hpp"

namespace {
using namespace command_arena_test;

std::uint64_t arena_for_physical_exhaustion_control() {
    // Only frozen APIs: size the fixture for three chunks of real backing
    // traffic, without guessing the new share or importing its constant.
    const auto control_extent = 3 * ScratchArena::kChunkBytes;
    auto arena_bytes = 4 * ScratchArena::kChunkBytes;
    for (unsigned attempt = 0; attempt < 30; ++attempt) {
        ArenaAttachment attachment(arena_bytes);
        auto backing = make_command_scratch_backing(kPageBytes);
        std::array<std::byte, kPageBytes> image{};
        const auto status = backing->write_page(control_extent / kPageBytes - 1,
                                                {image.data(), image.size()});
        if (status == ScratchStatus::ok) {
            require(ScratchArena::attached()->stats().bytes_used == control_extent,
                    "command backing must claim the attached arena, not bypass it with private disk scratch");
            return arena_bytes;
        }
        require(status == ScratchStatus::io_error,
                "control extent must either fit or encounter backing exhaustion");
        arena_bytes *= 2;
    }
    throw std::runtime_error("no fixture arena accommodates the physical-exhaustion control extent");
}

void exhausted_store_and_released_filler_control() {
    // Set B separately derives/tests the exact share boundary. This setup
    // isolates physical exhaustion and gives the released-filler control room.
    ArenaAttachment attachment(arena_for_physical_exhaustion_control());
    auto* arena = ScratchArena::attached();
    const auto empty = arena->stats();
    require(empty.bytes_used == 0 && empty.chunks_free == empty.chunks_total,
            "empty arena accounting");
    const auto chunk = empty.bytes_total / empty.chunks_total;
    ScratchFile filler;
    filler.open("command-arena-filler");
    filler.ensure_size(empty.bytes_total - chunk);
    const auto filled = arena->stats();
    require(filled.bytes_used == empty.bytes_total - chunk && filled.chunks_free == 1,
            "filler really leaves exactly one allocatable chunk");

    FailedWrites failures;
    ObserverRegistration observation(failures);
    std::array<std::byte, 4 * kPageBytes> image{};
    image.fill(std::byte{0x6d});
    // Even a payload-only directory must exceed the single free chunk within
    // this many puts. The store's journals/indexes can only reach it sooner.
    const auto max_puts = chunk / image.size() + 1;
    std::uint64_t accepted = 0;
    {
        auto backing = make_command_scratch_backing(kPageBytes);
        CommandScratchStore store(config(), backing.get());
        require(store.status() == ScratchStatus::ok, "open production command store");
        require(store.begin() == ScratchStatus::ok, "begin exhausted-store transaction");
        ScratchStatus status = ScratchStatus::ok;
        for (; accepted < max_puts; ++accepted) {
            ScratchHandle handle = scratch_no_handle;
            status = put(store, accepted, image, handle);
            if (status != ScratchStatus::ok) break;
        }
        require(status == ScratchStatus::io_error,
                "command puts must exhaust the attached arena with io_error before the bound");
        require(accepted > 0 && accepted < max_puts, "exhaustion witness is nonempty and bounded");
        require(failures.io_errors == 1, "the backing itself returned io_error without retry");
        require(failures.snapshot.bytes_used == filled.bytes_total
                && failures.snapshot.chunks_free == 0,
                "failed command write used the last chunk, not private disk scratch");
    }
    require(arena->stats().bytes_used == filled.bytes_used,
            "destroying the failed store releases its command region");
    filler.clear();
    require(arena->stats().bytes_used == 0, "filler release restores all chunks");

    // Same workload plus the refused put, now with the filler released.
    {
        auto backing = make_command_scratch_backing(kPageBytes);
        CommandScratchStore store(config(), backing.get());
        require(store.status() == ScratchStatus::ok, "open control command store");
        require(store.begin() == ScratchStatus::ok, "begin control transaction");
        ScratchHandle last = scratch_no_handle;
        for (std::uint64_t key = 0; key <= accepted; ++key)
            require(put(store, key, image, last) == ScratchStatus::ok,
                    "released-filler control accepts every put including the refused one");
        require(store.commit(ScratchDurability::buffered) == ScratchStatus::ok,
                "commit released-filler control");
        require(store.record_count() == accepted + 1, "control retained every row");
        for (std::uint64_t key = 0; key <= accepted; ++key) {
            ScratchSpanSource probe({reinterpret_cast<const std::byte*>(&key), sizeof key});
            ScratchHandle handle = scratch_no_handle;
            require(store.lower_bound(probe, handle) == ScratchStatus::ok,
                    "control locates each written key");
            std::uint64_t actual_key = max_puts;
            require(store.read(handle, ScratchField::key, 0,
                    {reinterpret_cast<std::byte*>(&actual_key), sizeof actual_key})
                        == ScratchStatus::ok && actual_key == key,
                    "control reads the exact key");
            std::array<std::byte, 4 * kPageBytes> actual{};
            require(store.read(handle, ScratchField::value, 0, {actual.data(), actual.size()})
                        == ScratchStatus::ok && actual == image,
                    "control reads the exact payload");
        }
        require(arena->stats().bytes_used > chunk, "control really crosses the refused extent");
    }
    require(arena->stats().bytes_used == 0, "control releases every region");
}
} // namespace

int main() {
    try {
        exhausted_store_and_released_filler_control();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL command scratch arena exhaustion: %s\n", error.what());
        return 1;
    }
}
