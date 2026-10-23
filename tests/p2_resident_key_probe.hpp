#pragma once

#include "p2_id_table_probe.hpp"

// Preload every dependency before instrumenting only this table definition.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace p2_id_table {
inline std::size_t key_comparisons = 0;

// The ordinary string storage, conversions and ordering are preserved.
// Only whole-key comparisons add a counter; the disk index still uses the
// unmodified std::string API through the public base reference.
class CountedKey : public std::string {
public:
    using std::string::string;
    using std::string::compare;
    CountedKey() = default;
    CountedKey(const std::string& key) : std::string(key) {}
    CountedKey(std::string&& key) : std::string(std::move(key)) {}
    int compare(const CountedKey& other) const noexcept {
        ++key_comparisons;
        return std::string::compare(other);
    }
    bool operator==(const CountedKey& other) const noexcept {
        ++key_comparisons;
        return std::string::compare(other) == 0;
    }
};
} // namespace p2_id_table

// No declarations are added to namespace std. The table's std::string token
// alone resolves to the derived counting key; every other name resolves to
// the real standard library. This also compiles the unchanged base table.
namespace p2_resident_std {
using namespace std;
using string = p2_id_table::CountedKey;
}
#define std p2_resident_std
#include <pineforge/detail/disk_resident_id_table.hpp>
#undef std
