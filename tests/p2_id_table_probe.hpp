#pragma once

// Observe the real ScratchFile copy API, for file, slab and arena backends.
// Only the DiskIdTable definition below uses the forwarding subclass; no I/O
// is replaced and no file named by a command is opened by this fixture.
#include <pineforge/detail/scratch_file.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace p2_id_table {
struct Counters {
    std::size_t reads = 0;
    std::size_t writes = 0;
};
inline Counters io;

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Record {
    std::array<std::uint64_t, 5> words{};
    bool operator==(const Record& other) const noexcept { return words == other.words; }
};
static_assert(sizeof(Record) == 40, "cohort record width");

inline Record record(std::size_t i) {
    return {{{i + 1, i + 11, i + 21, i + 31, i + 41}}};
}

inline std::string id(std::size_t i) {
    // Fixed 32-byte, non-SSO keys, including NUL and an unsigned high byte.
    std::string key(32, 'x');
    key[1] = '\0';
    key[2] = static_cast<char>(0x80);
    const auto digits = std::to_string(i);
    key.replace(key.size() - digits.size(), digits.size(), digits);
    return key;
}

inline std::vector<std::string> ids(std::size_t count) {
    std::vector<std::string> keys;
    keys.reserve(count);
    for (std::size_t i = 0; i < count; ++i) keys.push_back(id(i));
    return keys;
}

inline std::shared_ptr<pineforge::detail::ScratchSlab> slab(
    std::size_t count, std::size_t key_bytes) {
    pineforge::detail::ScratchManifest manifest;
    manifest.U = count;
    manifest.B_U = key_bytes;
    return std::make_shared<pineforge::detail::ScratchSlab>(manifest);
}

inline std::size_t ceil_log2(std::size_t n) {
    std::size_t bits = 0;
    for (std::size_t power = 1; power < n; power *= 2) ++bits;
    return bits;
}
} // namespace p2_id_table

namespace pineforge::detail {
class P2ObservedScratchFile : public ScratchFile {
public:
    using ScratchFile::ScratchFile;
    void read(std::uint64_t offset, void* data, std::size_t length) const {
        ++p2_id_table::io.reads;
        ScratchFile::read(offset, data, length);
    }
    void write(std::uint64_t offset, const void* data, std::size_t length) {
        ++p2_id_table::io.writes;
        ScratchFile::write(offset, data, length);
    }
};
} // namespace pineforge::detail

#define ScratchFile P2ObservedScratchFile
#include <pineforge/detail/disk_id_table.hpp>
#undef ScratchFile
