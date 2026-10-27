#pragma once

#include <pineforge/native_toolkit.hpp>
#include <pineforge/detail/scratch_arena.hpp>
#if __has_include(<pineforge/detail/order_book_storage.hpp>)
#include <pineforge/detail/order_book_storage.hpp>
#endif
#include "native_current_fixture.hpp"

#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <type_traits>
#include <unistd.h>

namespace order_book_test {
using Book = pineforge::native_toolkit::OrderBook<std::string>;
inline constexpr std::size_t kResidentLimit = 64;
#if __has_include(<pineforge/detail/order_book_storage.hpp>)
static_assert(kResidentLimit ==
              pineforge::detail::OrderBookStorage<std::string>::resident_capacity,
              "test bound must match the actual record window");
#endif

template<class T, class = void> struct HasResidentSize : std::false_type {};
template<class T> struct HasResidentSize<T, std::void_t<
    decltype(std::declval<const T&>().resident_size())>> : std::true_type {};
template<class T> std::size_t resident(const T& book) {
    if constexpr (HasResidentSize<T>::value) return book.resident_size();
    // Red-base compatibility ONLY: feae33694's std::map has one resident
    // node per logical binding. This selects size() on that unchanged class.
    else return book.size();
}

inline std::string key(std::size_t index) {
    auto digits = std::to_string(index);
    return std::string(16 - digits.size(), '0') + digits;
}
inline pineforge::native_order::Request resting() {
    auto request = r4_test::tx(1.0, "constant");
    request.trigger = pineforge::native_order::Limit{1.0};
    return request;
}

// Own a regular anonymous arena backing. Every book/host must die before this
// object. The fixed budget is independent of B and includes other host stores.
struct Arena {
    int fd = -1;
    struct sigaction previous{};
    explicit Arena(std::uint64_t bytes = 256 * pineforge::detail::ScratchArena::kChunkBytes) {
        using namespace r4_test;
        REQUIRE(pineforge::detail::ScratchArena::attached() == nullptr);
        REQUIRE(::sigaction(SIGXFSZ, nullptr, &previous) == 0);
        char name[] = "./order-book-arena-XXXXXX";
        fd = ::mkstemp(name);
        REQUIRE(fd >= 0);
        const bool unlinked = ::unlink(name) == 0;
        const bool sized = ::ftruncate(fd, static_cast<off_t>(bytes)) == 0;
        if (!unlinked || !sized) {
            ::close(fd);
            fd = -1;
            REQUIRE(false);
        }
        try { pineforge::detail::ScratchArena::attach(fd, bytes); }
        catch (...) { ::close(fd); fd = -1; throw; }
    }
    ~Arena() {
        pineforge::detail::ScratchArena::detach();
        ::close(fd);
        ::sigaction(SIGXFSZ, &previous, nullptr);
    }
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
};
} // namespace order_book_test
