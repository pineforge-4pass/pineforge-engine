#include <pineforge/array.hpp>
#include <pineforge/run_failure.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace pineforge::detail::script_array {

void index_error(const char* method, std::int64_t raw, std::uint64_t size, bool missing) {
    const auto text = std::string("Index ") +
        (missing ? "na" : std::to_string(raw)) +
        " is out of bounds. Array size is " + std::to_string(size);
    throw coded<std::runtime_error>(RunFailureCode::pine_array_error,
        {{"reason", "index_out_of_bounds"}, {"method", method}}, text);
}

void index_error_double(const char* method, double raw, std::uint64_t size) {
    const auto spelling = std::isnan(raw) ? std::string("na")
        : !std::isfinite(raw) ? std::string(raw > 0 ? "inf" : "-inf")
        : std::to_string(raw);
    const auto text = "Index " + spelling + " is out of bounds. Array size is " +
                      std::to_string(size);
    throw coded<std::runtime_error>(RunFailureCode::pine_array_error,
        {{"reason", "index_out_of_bounds"}, {"method", method}}, text);
}

std::int64_t checked_index_double(double raw, std::uint64_t size, const char* method) {
    // 2^63 itself is representable as binary64, INT64_MAX is not. An inclusive
    // comparison to double(INT64_MAX) would admit an undefined integer cast.
    if (!std::isfinite(raw) || raw < -0x1p63 || raw >= 0x1p63)
        index_error_double(method, raw, size);
    // A floating -2^63 is a finite index, not the integer na sentinel. It
    // cannot normalize into any valid array (size <= INT64_MAX), so reject
    // it here while its numeric spelling is still distinguishable from na.
    if (raw == -0x1p63)
        index_error(method, std::numeric_limits<std::int64_t>::min(), size);
    return static_cast<std::int64_t>(raw);
}

void empty_array_error(const char* method) {
    const auto text = std::string("Cannot use ") + method + "() if array is empty.";
    throw coded<std::runtime_error>(RunFailureCode::pine_array_error,
        {{"reason", "empty_array_access"}, {"method", method}}, text);
}

void size_error() {
    throw coded<std::runtime_error>(RunFailureCode::pine_array_error,
        {{"reason", "size_invalid"}, {"method", "new"}},
        "cannot create std::vector larger than max_size()");
}

void invariant(const char* text) {
    throw coded<std::runtime_error>(RunFailureCode::engine_invariant, {}, text);
}

void inverted_range_error(const char* method) {
    const auto text = std::string("Index 'from' should be less than index 'to'.");
    throw coded<std::runtime_error>(RunFailureCode::pine_array_error,
        {{"reason", "slice_range_inverted"}, {"method", method}}, text);
}

o1::Id clone_bytes(const o1::Store& source, o1::Id id, o1::Store& target) {
    const auto bytes = source.size(id);
    const auto copy = target.create(bytes, 1);
    unsigned char buffer[kPayloadPageBytes];
    for (std::uint64_t offset = 0; offset < bytes;) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(sizeof buffer, bytes - offset));
        read_exact(source, id, offset, buffer, count);
        write_exact(target, copy, offset, buffer, count);
        offset += count;
    }
    return copy;
}

o1::Id store_string(o1::Store& store, const std::string& value) {
    if (value.empty()) return o1::kNullId;
    const auto id = store.create(value.size(), 1);
    write_exact(store, id, 0, value.data(), value.size());
    return id;
}

std::string load_string(const o1::Store& store, o1::Id id) {
    if (id == o1::kNullId) return {};
    const auto count = store.size(id);
    if (count > std::string{}.max_size()) size_error();
    std::string result(static_cast<std::size_t>(count), '\0');
    if (count != 0) read_exact(store, id, 0, &result[0], result.size());
    return result;
}

} // namespace pineforge::detail::script_array
