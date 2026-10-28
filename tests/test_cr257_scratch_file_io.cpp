#include "cr257_io.hpp"

#include <algorithm>
#include <array>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace cr257 {

void scratch_file_io() {
    using pineforge::detail::ScratchFile;
    require(platform_page > 0 && kCacheBytes % platform_page == 0
            && kCacheBytes / platform_page > 1,
            "ScratchFile fixture needs multiple platform pages per window");
    const Count chunks_per_window = kCacheBytes / platform_page;
    constexpr Count windows = 2; // The second window also proves the advice wrapper is live.
    const Count chunks = windows * chunks_per_window;
    std::array<unsigned char, windows * ScratchFile::kCacheBytes> expected{};
    for (std::size_t i = 0; i < expected.size(); ++i)
        expected[i] = static_cast<unsigned char>((i * 17 + i / 251) & 0xff);
    ScratchFile file;
    file.set_directory(".");
    file.bound_cache();
    file.open("cr257-clean-window");
    require(file.descriptor() >= 0 && !file.arena_backed(),
            "ScratchFile must exercise disk mode");
    struct stat status{};
    require(::fstat(file.descriptor(), &status) == 0
            && S_ISREG(status.st_mode) && status.st_nlink == 0,
            "ScratchFile must be an unlinked regular file");
    file.write(0, expected.data(), expected.size()); // Seed outside measurement.
    std::vector<unsigned char> actual(platform_page);
    Counters first_window;
    Measure run;
    for (Count chunk = 0; chunk < chunks; ++chunk) {
        const auto offset = static_cast<std::size_t>(chunk * platform_page);
        file.read(offset, actual.data(), actual.size());
        require(std::equal(actual.begin(), actual.end(), expected.begin() + offset),
            "Chunk read lost or ignored its input");
        if (chunk + 1 == chunks_per_window) first_window = counters;
    }
    const auto io = run.finish();
    validate(io);
    require(io.total.pread.calls == chunks && io.total.pread.bytes == expected.size()
            && io.total.pwrite.calls == 0 && io.total.fdatasync.calls == 0,
            "ScratchFile wrapper did not observe exactly the real chunk reads");
    const Context context{chunks, chunks, 0};
    // Each group of k actual reads consumes ONE fixed byte window. RANDOM was
    // issued at open, outside RUN, so every measured advice must be DONTNEED.
    require(io.total.fadvise.calls == io.total.dontneed && io.total.random == 0,
            "ScratchFile advice was not clean-cache eviction");
    require(io.total.fadvise.calls > 0,
            "Two full read windows never reached the advice wrapper");
    bound("scratch.one_window_dontneed_calls", first_window.total.dontneed,
          ceil_div(first_window.total.pread.bytes, kCacheBytes),
          {chunks_per_window, chunks, 0});
    bound("scratch.two_window_dontneed_calls", io.total.dontneed,
          ceil_div(io.total.pread.bytes, kCacheBytes), context);
}

} // namespace cr257
