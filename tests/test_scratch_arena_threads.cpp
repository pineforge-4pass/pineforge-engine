// Shared allocator/LRU concurrency; separate from the single-threaded
// global-allocation replacement used by test_scratch_arena.
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/detail/scratch_file.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <random>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using pineforge::detail::ScratchArena;
using pineforge::detail::ScratchFile;
constexpr auto kChunk = ScratchArena::kChunkBytes;
constexpr std::size_t kWorkers = 4;
constexpr std::uint64_t kCap = 128 * 1024;

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

struct Descriptor {
    int fd;
    explicit Descriptor(int value) : fd(value) { require(fd >= 0, "create thread-test backing"); }
    ~Descriptor() { ::close(fd); }
};

struct Attachment {
    Attachment(int fd, std::uint64_t bytes) { ScratchArena::attach(fd, bytes); }
    ~Attachment() { ScratchArena::detach(); }
};

void shared_allocator() {
    require(::setenv("PF_SCRATCH_WINDOW_BYTES", "131072", 1) == 0, "set shared window cap");
    std::array<char, 48> name{};
    constexpr char pattern[] = "/tmp/pineforge-arena-threads-XXXXXX";
    std::copy(std::begin(pattern), std::end(pattern), name.begin());
    Descriptor backing(::mkstemp(name.data()));
    require(::unlink(name.data()) == 0, "unlink thread-test backing");
    constexpr auto arena_bytes = kWorkers * 2 * kChunk;
    require(::ftruncate(backing.fd, static_cast<off_t>(arena_bytes)) == 0,
            "size the shared backing");
    Attachment attachment(backing.fd, arena_bytes);
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};
    std::array<std::exception_ptr, kWorkers> failures{};
    std::array<std::thread, kWorkers> workers;
    const auto check = [&] {
        const auto stats = ScratchArena::attached()->stats();
        require(stats.windows_mapped_bytes <= kCap, "all threads share one mapping cap");
        require(stats.bytes_used <= arena_bytes
                    && stats.chunks_free + stats.bytes_used / kChunk == kWorkers * 2,
                "concurrent ownership partitions the arena");
    };
    try {
        for (std::size_t worker = 0; worker < kWorkers; ++worker) {
            workers[worker] = std::thread([&, worker] {
                bool announced = false;
                try {
                    ScratchFile file;
                    file.open("concurrent-region");
                    file.ensure_size(2 * kChunk);
                    ready.fetch_add(1);
                    announced = true;
                    while (!start.load()) std::this_thread::yield();
                    std::mt19937_64 random(0x95c31a27ULL + worker);
                    for (std::size_t round = 0; round < 24; ++round) {
                        if (round != 0) {
                            file.open("reused-concurrent-region");
                            file.ensure_size(2 * kChunk);
                        }
                        std::vector<std::uint8_t> reference(static_cast<std::size_t>(2 * kChunk));
                        std::array<std::uint8_t, 8193> payload{};
                        for (std::size_t step = 0; step < 12; ++step) {
                            for (auto& byte : payload) byte = static_cast<std::uint8_t>(random());
                            const auto offset = step == 0 ? kChunk - 31
                                : random() % (2 * kChunk - payload.size() + 1);
                            file.write(offset, payload.data(), payload.size());
                            std::copy(payload.begin(), payload.end(),
                                      reference.begin() + static_cast<std::ptrdiff_t>(offset));
                            check();
                            std::array<std::uint8_t, 8193> observed{};
                            file.read(offset, observed.data(), observed.size());
                            require(observed == payload, "concurrent overwrite matches private payload");
                            check();
                            std::this_thread::yield();
                        }
                        std::vector<std::uint8_t> observed(reference.size(), 0xa1);
                        file.read(0, observed.data(), observed.size());
                        require(observed == reference,
                                "concurrent chunk reuse matches independent per-thread vector");
                        check();
                        file.clear();
                    }
                } catch (...) {
                    failures[worker] = std::current_exception();
                    if (!announced) ready.fetch_add(1);
                }
            });
        }
    } catch (...) {
        start.store(true);
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        throw;
    }
    while (ready.load() != kWorkers) std::this_thread::yield();
    start.store(true);
    for (auto& worker : workers) worker.join();
    for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
    const auto stats = ScratchArena::attached()->stats();
    require(stats.bytes_used == 0 && stats.chunks_free == kWorkers * 2
                && stats.windows_mapped_bytes == 0,
            "all threads released their chunks and mappings");
    require(stats.high_water == arena_bytes,
            "startup barrier independently forces simultaneous ownership of every chunk");
}
} // namespace

int main() {
    try {
        shared_allocator();
        std::puts("test_scratch_arena_threads: concurrent reference checks passed");
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "test_scratch_arena_threads: %s\n", failure.what());
        return 1;
    }
}
