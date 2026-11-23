// The field writer must accept literals without constructing a temporary
// std::string. The identity oracle below is the base writer's byte contract:
// each string contributes its uint64 length in native byte order followed by
// exactly its characters.
#include "global_allocation_replacement.hpp"

#include <pineforge/engine.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

int failures = 0;
int checks = 0;

#define CHECK(condition) do {                                                   \
    ++checks;                                                                   \
    if (!(condition)) {                                                         \
        ++failures;                                                             \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
    }                                                                            \
} while (false)

// Frozen from the base definition `u(v.size()); bytes(v.data(), v.size())`,
// for the two fixed inputs below. The target platforms serialize uint64_t in
// little-endian order, as the base implementation does.
constexpr std::array<unsigned char, 65> kBaseSerialized = {
    26, 0, 0, 0, 0, 0, 0, 0,
    'p', 'i', 'n', 'e', 'f', 'o', 'r', 'g', 'e', '-', 'b', 'r', 'o', 'k', 'e',
    'r', '-', 's', 't', 'a', 't', 'e', '/', 'v', '1', '9',
    23, 0, 0, 0, 0, 0, 0, 0,
    'n', 'a', 't', 'i', 'v', 'e', '-', 'c', '-', 'h', 'o', 's', 't', ':',
    'e', 'x', 't', 'e', 'n', 's', 'i', 'o', 'n'
};

std::uint64_t fnv1a(const std::array<unsigned char, 65>& bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

template <typename Sink, typename = void>
struct accepts_string_view : std::false_type {};

template <typename Sink>
struct accepts_string_view<Sink, std::void_t<
    decltype(std::declval<Sink&>().s(std::declval<std::string_view>()))>>
    : std::true_type {};

template <typename Sink>
void string_view_caller() {
    if constexpr (accepts_string_view<Sink>::value) {
        Sink from_view;
        Sink view_literal;
        from_view.s(std::string_view("view caller"));
        view_literal.s("view caller");
        CHECK(from_view.h == view_literal.h);
    }
}

void identity_bytes() {
    pineforge::BrokerStateHashSink actual;
    actual.s("pineforge-broker-state/v19");
    actual.s("native-c-host:extension");
    CHECK(actual.h == fnv1a(kBaseSerialized));
}

void literal_sites_do_not_allocate() {
    const std::size_t before = global_allocation::allocations;
    pineforge::BrokerStateHashSink sink;
    // These are representative production domain tags:
    // src/engine_state_hash.cpp:66,
    // src/source/pine_state_hash.cpp:73, and
    // src/native_c_host.cpp:1730.
    sink.s("pineforge-broker-state/v19");
    sink.s("pineforge-source-runtime-request/v1");
    sink.s("native-c-host:extension");
    CHECK(global_allocation::allocations - before == 0);
}

void existing_callers_keep_their_bytes() {
    const std::string string_value = "string caller";
    pineforge::BrokerStateHashSink from_string;
    pineforge::BrokerStateHashSink from_literal;
    from_string.s(string_value);
    from_literal.s("string caller");
    CHECK(from_string.h == from_literal.h);
    // The base's std::string-only overload remains compilable here; this
    // detection is false on the base and true after the string_view fix.
    CHECK(accepts_string_view<pineforge::BrokerStateHashSink>::value);
    string_view_caller<pineforge::BrokerStateHashSink>();
}

}  // namespace

int main() {
    identity_bytes();
    literal_sites_do_not_allocate();
    existing_callers_keep_their_bytes();
    std::printf("test_broker_state_sink_string_view: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
