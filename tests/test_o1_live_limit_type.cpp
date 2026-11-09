// T5: the internal checked native policy's conversion and layout contract.
// No Store or objects: all behavior here belongs to the field's exact type.
#include <pineforge/detail/o1_store.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace {
using pineforge::detail::o1::LiveObjectLimit;
using pineforge::detail::o1::Shape;

enum Unscoped { one = 1 };
enum class Scoped : unsigned { one = 1 };

template <typename... Integers>
constexpr bool accepts_integrals() {
  return ((std::is_constructible<LiveObjectLimit, Integers>::value &&
           std::is_assignable<LiveObjectLimit&, Integers>::value) && ...);
}

static_assert(accepts_integrals<
    char, signed char, unsigned char, wchar_t, char16_t, char32_t,
    short, unsigned short, int, unsigned int, long, unsigned long,
    long long, unsigned long long, std::int8_t, std::uint8_t,
    std::int16_t, std::uint16_t, std::int32_t, std::uint32_t,
    std::int64_t, std::uint64_t, std::size_t>());

template <typename... Types>
constexpr bool rejects_nonintegrals() {
  return ((!std::is_constructible<LiveObjectLimit, Types>::value &&
           !std::is_assignable<LiveObjectLimit&, Types>::value) && ...);
}

static_assert(rejects_nonintegrals<
    bool, float, double, long double, Unscoped, Scoped, void*, int*,
    const char*, std::nullptr_t>());
static_assert(std::is_trivially_copyable<LiveObjectLimit>::value);
static_assert(std::is_standard_layout<LiveObjectLimit>::value);
static_assert(std::is_trivially_copyable<Shape>::value);
static_assert(std::is_standard_layout<Shape>::value);
static_assert(sizeof(LiveObjectLimit) == 8 && alignof(LiveObjectLimit) == 8);
static_assert(sizeof(Shape) == 32 && alignof(Shape) == 8);
static_assert(offsetof(Shape, max_live_objects) == 24);
static_assert(std::is_same<
    decltype(std::declval<const LiveObjectLimit&>().operator std::uint64_t()),
    std::uint64_t>::value);
static_assert(noexcept(static_cast<std::uint64_t>(
    std::declval<const LiveObjectLimit&>())));

constexpr Shape defaults{};
constexpr Shape partial{4096, 8};
constexpr Shape finite{4096, 8, 1024, 8, 8, 17};
constexpr LiveObjectLimit maximum{UINT64_MAX};
static_assert(defaults.max_live_objects == 0);
static_assert(partial.max_live_objects == 0);
static_assert(finite.max_live_objects == 17);
static_assert(maximum == UINT64_MAX);
static_assert(finite.max_live_objects + 1 == 18);

constexpr bool constexpr_assignment() {
  LiveObjectLimit value;
  value = std::numeric_limits<std::int64_t>::max();
  return value == static_cast<std::uint64_t>(
                      std::numeric_limits<std::int64_t>::max());
}
static_assert(constexpr_assignment());

void require_negative_message(const std::invalid_argument& error) {
  const std::string message(error.what());
  assert(message.find("max_live_objects") != std::string::npos);
  assert(message.find("negative") != std::string::npos);
  assert(message.find("refused") != std::string::npos);
}

template <typename Integer>
void check_integer() {
  LiveObjectLimit value{Integer{1}};
  assert(value == 1);
  value = std::numeric_limits<Integer>::max();
  assert(value == static_cast<std::uint64_t>(
                      std::numeric_limits<Integer>::max()));
  value = Integer{0};
  assert(value == 0);

  if constexpr (std::is_signed<Integer>::value) {
    const Integer negatives[] = {Integer{-1}, std::numeric_limits<Integer>::min()};
    for (const Integer negative : negatives) {
      value = UINT64_C(37);
      bool assignment_refused = false;
      try {
        value = negative;
      } catch (const std::invalid_argument& error) {
        require_negative_message(error);
        assignment_refused = true;
      }
      assert(assignment_refused);
      assert(value == 37); // Strong guarantee at the assignment boundary.

      bool construction_refused = false;
      try {
        const LiveObjectLimit rejected{negative};
        (void)rejected;
      } catch (const std::invalid_argument& error) {
        require_negative_message(error);
        construction_refused = true;
      }
      assert(construction_refused);
    }
  }
}

template <typename... Integers>
void check_integers() {
  (check_integer<Integers>(), ...);
}
} // namespace

int main() {
  check_integers<
      char, signed char, unsigned char, wchar_t, char16_t, char32_t,
      short, unsigned short, int, unsigned int, long, unsigned long,
      long long, unsigned long long, std::int8_t, std::uint8_t,
      std::int16_t, std::uint16_t, std::int32_t, std::uint32_t,
      std::int64_t, std::uint64_t, std::size_t>();

  Shape shape{4096, 8};
  std::uint64_t wide = UINT64_MAX;
  std::size_t size = 31;
  int integer = 32;
  long long signed_wide = 33;
  unsigned unsigned_value = 34;
  shape.max_live_objects = wide;
  const std::uint64_t read = shape.max_live_objects;
  assert(read == UINT64_MAX && shape.max_live_objects == wide);
  shape.max_live_objects = size;
  assert(shape.max_live_objects == size);
  shape.max_live_objects = integer;
  assert(shape.max_live_objects == integer);
  shape.max_live_objects = signed_wide;
  assert(shape.max_live_objects == signed_wide);
  shape.max_live_objects = unsigned_value;
  assert(shape.max_live_objects + 1 == 35);
  Shape all_fields{4096, 8, 1024, 8, 8, integer};
  assert(all_fields.max_live_objects == 32);
}
