#pragma once

// CR150 arena seam fixtures use only the pre-seam API. Execution and the
// fail-before diagnostic belong to the supervisor's remote build.
#include "../src/command_scratch_production.hpp"
#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/detail/scratch_store.hpp>

#include <array>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace command_arena_test {
using namespace pineforge::detail;

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

inline constexpr std::size_t kPageBytes = 512;
// Fixed small-arena witnesses must stay below the share's first full chunk.
inline constexpr std::uint64_t kSmallArenaBytes = 4 * ScratchArena::kChunkBytes;

inline ScratchKeyOrder key_order() noexcept {
    return {0x4352313530415245ULL, nullptr,
        [](void*, const ScratchByteSource& a, const ScratchByteSource& b,
           int& result) noexcept {
            std::uint64_t left = 0, right = 0;
            if (a.size() != sizeof left || b.size() != sizeof right)
                return ScratchStatus::programming_error;
            auto status = a.read(0, {reinterpret_cast<std::byte*>(&left), sizeof left});
            if (status != ScratchStatus::ok) return status;
            status = b.read(0, {reinterpret_cast<std::byte*>(&right), sizeof right});
            if (status != ScratchStatus::ok) return status;
            result = left < right ? -1 : right < left ? 1 : 0;
            return ScratchStatus::ok;
        }};
}

inline ScratchStoreConfig config() noexcept {
    return {2, kPageBytes, 1, 1, key_order(), ScratchOpenMode::create_empty};
}

struct ArenaAttachment {
    int fd = -1;
    struct sigaction previous_signal{};
    CommandScratchByteCap previous_cap;

    explicit ArenaAttachment(std::uint64_t bytes) {
        require(ScratchArena::attached() == nullptr, "fixture starts without an arena");
        require(::sigaction(SIGXFSZ, nullptr, &previous_signal) == 0,
                "read file-size signal disposition");
        char path[] = "./command-arena-XXXXXX";
        fd = ::mkstemp(path);
        require(fd >= 0, "create inherited arena fd");
        require(::unlink(path) == 0, "unlink inherited arena fd");
        require(::ftruncate(fd, static_cast<off_t>(bytes)) == 0,
                "size inherited fd before attachment/jail");
        ScratchArena::attach(fd, bytes);
        previous_cap = set_command_scratch_byte_cap(std::uint64_t{0});
    }
    ~ArenaAttachment() {
        set_command_scratch_byte_cap(std::move(previous_cap));
        ScratchArena::detach();
        ::close(fd);
        ::sigaction(SIGXFSZ, &previous_signal, nullptr);
    }
    ArenaAttachment(const ArenaAttachment&) = delete;
    ArenaAttachment& operator=(const ArenaAttachment&) = delete;
};

struct FailedWrites {
    std::uint64_t io_errors = 0;
    std::uint64_t requested_extent = 0;
    ScratchArenaStats latest{}, before{};
    ScratchArenaStats snapshot{};
    static void charge(void* context, const ScratchIoCharge& io) noexcept {
        auto& self = *static_cast<FailedWrites*>(context);
        const auto current = ScratchArena::attached()->stats();
        if (io.operation == ScratchIoOperation::write_page
            && io.status == ScratchStatus::io_error) {
            ++self.io_errors;
            self.requested_extent = (io.page + 1) * io.bytes;
            self.before = self.latest;
            self.snapshot = current;
        }
        self.latest = current;
    }
};

struct ObserverRegistration {
    ScratchIoObserver previous;
    explicit ObserverRegistration(ScratchIoObserver observer) noexcept
        : previous(set_command_scratch_io_observer(observer)) {}
    explicit ObserverRegistration(FailedWrites& failures) noexcept
        : ObserverRegistration(ScratchIoObserver{&failures, FailedWrites::charge}) {
        failures.latest = ScratchArena::attached()->stats();
    }
    ~ObserverRegistration() { set_command_scratch_io_observer(previous); }
};

inline ScratchStatus put(CommandScratchStore& store, std::uint64_t key,
                         const std::array<std::byte, 4 * kPageBytes>& image,
                         ScratchHandle& handle) noexcept {
    ScratchSpanSource k({reinterpret_cast<const std::byte*>(&key), sizeof key});
    ScratchSpanSource value({image.data(), image.size()});
    return store.put(k, value, handle);
}

} // namespace command_arena_test
