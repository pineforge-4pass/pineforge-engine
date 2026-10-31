#pragma once

#include <pineforge/detail/cache_capacity.hpp>

#include <cassert>
#include <exception>
#include <limits>
#include <stdexcept>

namespace pineforge::detail {

inline std::size_t window_payload_bytes(std::size_t rows, std::size_t width) noexcept {
    return width && rows <= std::numeric_limits<std::size_t>::max() / width
        ? rows * width : 0;
}

// The constructor seams still express payload bytes. Charge the shared
// cache's complete lines as well, without shrinking the old payload default.
inline ReadThroughCachePolicy window_cache_policy(std::size_t page_size,
                                                  std::size_t payload_bytes) noexcept {
    return CacheCapacity::from_payload(page_size, payload_bytes).for_page(page_size);
}

// Bridge a throwing positional reader to the shared status boundary. Keep
// the original exception, including its typed code/arguments/errno. Contexts
// have stable owner addresses; no cache pointer escapes this copy-out port.
class ReadThroughWindow {
public:
    using Reader = void (*)(void*, std::uint64_t, void*, std::size_t);

    ReadThroughWindow(std::size_t page_size, std::size_t payload_bytes,
                      void* context, Reader reader) noexcept
        : ReadThroughWindow(page_size, CacheCapacity::from_payload(page_size, payload_bytes),
                            context, reader) {}
    ReadThroughWindow(std::size_t page_size, CacheCapacity capacity,
                      void* context, Reader reader) noexcept
        : context_(context), reader_(reader),
          cache_(capacity.for_page(page_size), {this, read, nullptr}) {}

    void get(std::uint64_t page, void* destination, std::size_t bytes) {
        failure_ = nullptr;
        const auto status = cache_.get({0, 0, page},
            {static_cast<std::byte*>(destination), bytes});
        if (status == ScratchStatus::ok) return;
        if (failure_) std::rethrow_exception(failure_);
        throw std::logic_error("invalid read-through window request");
    }
    void invalidate() noexcept {
        const auto status = cache_.invalidate_all();
        assert(status == ScratchStatus::ok);
        (void)status;
    }
    std::size_t page_size() const noexcept { return cache_.policy().page_size; }
    std::size_t payload_bytes() const noexcept { return cache_.slot_count() * page_size(); }
    ReadThroughCacheStats stats() const noexcept { return cache_.stats(); }

private:
    static ReadThroughCacheIoResult read(void* context, ReadThroughCacheKey key,
                                         ScratchBuffer destination) noexcept {
        auto& self = *static_cast<ReadThroughWindow*>(context);
        try {
            self.reader_(self.context_, key.page_number * self.page_size(),
                         destination.data, destination.size);
            return {ScratchStatus::ok, destination.size};
        } catch (...) {
            self.failure_ = std::current_exception();
            return {ScratchStatus::io_error, 0};
        }
    }

    void* context_;
    Reader reader_;
    std::exception_ptr failure_;
    ReadThroughCache cache_;
};

} // namespace pineforge::detail
