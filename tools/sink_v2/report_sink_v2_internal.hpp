#pragma once

#include "report_sink_v2_support.hpp"
#include "report_sink_v2_codec.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace pineforge { namespace report_sink { namespace v2 {

using Bytes = std::vector<std::uint8_t>;
using Digest = std::array<std::uint8_t, 32>;
constexpr std::uint64_t safe = UINT64_C(9007199254740991);
constexpr std::size_t block_limit = 32u << 20;
constexpr std::size_t column_limit = 16u << 20;
constexpr std::size_t page_bytes = 65536;
constexpr std::size_t page_head = 4096;
constexpr std::size_t page_payload = page_bytes - page_head;
constexpr std::array<char, 8> tags{{'E', 'T', 'O', 'V', 'K', 'X', 'H', 'I'}};
static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559,
              "report sink v2 requires IEEE-754 binary64");

[[noreturn]] inline void invalid(const char* text) {
    throw BlockError(BlockFailure::invalid_input, text);
}
[[noreturn]] inline void io(const char* operation, int number = errno) {
    throw BlockError(number == EFBIG ? BlockFailure::spool_bytes : BlockFailure::io,
        std::string("report sink v2 ") + operation
        + ": " + std::error_code(number, std::generic_category()).message(), number);
}
inline std::string_view view(const void* bytes, std::size_t size) {
    return {static_cast<const char*>(bytes), size};
}
inline std::string_view view(const Bytes& bytes) { return view(bytes.data(), bytes.size()); }
template<std::size_t N> inline std::string_view view(const std::array<std::uint8_t, N>& bytes) {
    return view(bytes.data(), bytes.size());
}
inline void put(std::uint8_t* out, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
inline std::uint64_t get(const std::uint8_t* in, unsigned width) {
    std::uint64_t result = 0;
    for (unsigned i = 0; i < width; ++i) result |= std::uint64_t{in[i]} << (8 * i);
    return result;
}
inline void append(Bytes& out, std::uint64_t value, unsigned width) {
    const auto at = out.size();
    out.resize(at + width);
    put(out.data() + at, value, width);
}
inline void varint(Bytes& out, std::uint64_t value) {
    if (value > safe) invalid("varint outside safe domain");
    do {
        const auto low = static_cast<std::uint8_t>(value & 127);
        value >>= 7;
        out.push_back(static_cast<std::uint8_t>(low | (value ? 128 : 0)));
    } while (value);
}
inline std::uint64_t zigzag(std::int64_t value) {
    return value < 0 ? std::uint64_t(-(value + 1)) * 2 + 1 : std::uint64_t(value) * 2;
}
inline std::uint64_t bits(double value) {
    std::uint64_t word;
    std::memcpy(&word, &value, sizeof word);
    return word;
}
inline double number(std::uint64_t word) {
    double value;
    std::memcpy(&value, &word, sizeof value);
    return value;
}
// Volatile stores round each operation to binary64, including on x87 hosts.
// These units also compile with fast math and FP contraction disabled.
inline double add64(double a, double b) { volatile double v = a + b; return v; }
inline double sub64(double a, double b) { volatile double v = a - b; return v; }
inline double mul64(double a, double b) { volatile double v = a * b; return v; }
inline double div64(double a, double b) { volatile double v = a / b; return v; }
inline std::int64_t signed_word(std::uint64_t word) {
    std::int64_t value;
    std::memcpy(&value, &word, sizeof value);
    return value;
}

class Crc {
    std::uint32_t state_ = UINT32_C(0xffffffff);
public:
    void update(std::string_view bytes) {
        static const auto table = [] {
            std::array<std::uint32_t, 256> result{};
            for (std::uint32_t i = 0; i < 256; ++i) {
                auto word = i;
                for (unsigned j = 0; j < 8; ++j)
                    word = (word >> 1) ^ ((word & 1) ? UINT32_C(0xedb88320) : 0);
                result[i] = word;
            }
            return result;
        }();
        for (const unsigned char byte : bytes)
            state_ = (state_ >> 8) ^ table[(state_ ^ byte) & 255];
    }
    std::uint32_t digest() const { return state_ ^ UINT32_C(0xffffffff); }
};
inline std::uint32_t crc(std::string_view bytes) { Crc c; c.update(bytes); return c.digest(); }
inline Digest sha(std::string_view bytes) { Sha256 s; s.update(bytes); return s.digest(); }
inline Digest parent(const Digest& left, const Digest& right) {
    Sha256 s;
    s.update(std::string_view("PFS2-NODE\0", 10));
    s.update(view(left));
    s.update(view(right));
    return s.digest();
}
inline std::string hex(const Digest& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text(64, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        text[2 * i] = digits[digest[i] >> 4];
        text[2 * i + 1] = digits[digest[i] & 15];
    }
    return text;
}

class Output {
    int fd_ = -1;
    bool owned_ = false;
    std::uint64_t size_ = 0;
    std::uint64_t immutable_end_ = 0;
    std::array<char, 65536> buffer_{};
    std::size_t used_ = 0;
    std::uint64_t limit_ = safe;
    std::atomic<bool> limit_failed_{false};
    void admit(std::uint64_t end) {
        if (end > limit_) {
            limit_failed_ = true;
            throw BlockError(BlockFailure::spool_bytes, "output exceeds spool byte or per-file cap");
        }
    }
public:
    ~Output() { if (fd_ >= 0) ::close(fd_); }
    void limit(std::optional<std::uint64_t> cap) {
        if (cap) limit_ = std::min(limit_, *cap);
        struct rlimit fsize{};
        if (::getrlimit(RLIMIT_FSIZE, &fsize) < 0) io("getrlimit fsize");
        if (fsize.rlim_cur != RLIM_INFINITY)
            limit_ = std::min(limit_, static_cast<std::uint64_t>(fsize.rlim_cur));
    }
    void open(const std::string& path, int staging_fd = -1) {
        // O_NONBLOCK avoids hanging on a FIFO before the regular-file check.
        // Check before truncating so refusal never damages a nonregular target.
        do { fd_ = staging_fd >= 0 ? ::fcntl(staging_fd, F_DUPFD_CLOEXEC, 0)
            : ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NONBLOCK, 0666); }
        while (fd_ < 0 && errno == EINTR);
        if (fd_ < 0) io("open");
        struct stat status{};
        if (::fstat(fd_, &status) < 0) io("stat staging output");
        if (!S_ISREG(status.st_mode) || ::lseek(fd_, 0, SEEK_SET) < 0)
            invalid("report sink v2 requires a seekable local regular staging file");
        if (::ftruncate(fd_, 0) < 0) io("truncate staging output");
        owned_ = true;
    }
    std::uint64_t size() const noexcept { return size_; }
    int fd() const noexcept { return fd_; }
    void freeze_data() { flush(); immutable_end_ = size_; }
    void read(std::uint64_t offset, void* destination, std::size_t length) const {
        if (used_ || offset > size_ || length > size_ - offset)
            invalid("output read before flush or outside file");
        auto* out = static_cast<char*>(destination);
        std::size_t at = 0;
        while (at < length) {
            const auto n = ::pread(fd_, out + at, length - at, static_cast<off_t>(offset + at));
            if (n < 0) { if (errno == EINTR) continue; io("staging read"); }
            if (n == 0) io("short staging read", EIO);
            at += static_cast<std::size_t>(n);
        }
    }
    void flush() {
        std::size_t at = 0;
        while (at < used_) {
            const auto n = ::pwrite(fd_, buffer_.data() + at, used_ - at,
                static_cast<off_t>(size_ - used_ + at));
            if (n < 0) { if (errno == EINTR) continue; limit_failed_ = errno == EFBIG; io("write"); }
            if (n == 0) io("write made no progress", EIO);
            at += static_cast<std::size_t>(n);
        }
        used_ = 0;
    }
    void write(std::string_view bytes) {
        if (bytes.size() > safe - size_) invalid("output exceeds safe offset domain");
        admit(size_ + bytes.size());
        while (!bytes.empty()) {
            const auto n = std::min(buffer_.size() - used_, bytes.size());
            std::memcpy(buffer_.data() + used_, bytes.data(), n);
            used_ += n;
            size_ += n;
            bytes.remove_prefix(n);
            if (used_ == buffer_.size()) flush();
        }
    }
    void reserve_footer(std::uint64_t end) {
        if (!immutable_end_ || end < size_ || end > safe) invalid("invalid footer reservation");
        admit(end);
        flush();
        if (::ftruncate(fd_, static_cast<off_t>(end)) < 0) {
            limit_failed_ = errno == EFBIG; io("reserve footer");
        }
        size_ = end;
    }
    void rewrite(std::uint64_t offset, std::string_view bytes) {
        if (used_ || offset < immutable_end_ || offset > size_ || bytes.size() > size_ - offset)
            invalid("rewrite outside unsealed footer");
        std::size_t at = 0;
        while (at < bytes.size()) {
            const auto n = ::pwrite(fd_, bytes.data() + at, bytes.size() - at,
                static_cast<off_t>(offset + at));
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EFBIG) limit_failed_ = true;
                io("footer rewrite");
            }
            if (!n) io("footer rewrite made no progress", EIO);
            at += static_cast<std::size_t>(n);
        }
    }
    Digest stored_digest() {
        flush();
        Sha256 hash;
        for (std::uint64_t at = 0; at < size_;) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer_.size(), size_ - at));
            read(at, buffer_.data(), n);
            hash.update(view(buffer_.data(), n));
            at += n;
        }
        return hash.digest();
    }
    void close() {
        flush();
        const int fd = fd_;
        fd_ = -1;
        if (::close(fd) < 0) io("close");
    }
    // If a final flush/close fails, invalidate a partly emitted tail even
    // when the OS accepted earlier writes. Reopen only our existing output.
    void invalidate(const std::string& path, std::uint64_t data_end) noexcept {
        if (!owned_) {
            if (fd_ >= 0) (void)::close(fd_);
            fd_ = -1;
            return;
        }
        used_ = 0;
        if (fd_ >= 0) {
            if (!limit_failed_) (void)::ftruncate(fd_, static_cast<off_t>(data_end));
            (void)::close(fd_);
            fd_ = -1;
        } else {
            const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
            if (fd >= 0) {
                if (!limit_failed_) (void)::ftruncate(fd, static_cast<off_t>(data_end));
                (void)::close(fd);
            }
        }
    }
};

struct Table {
    std::uint32_t width = 512;
    std::uint64_t count = 0;
    std::uint64_t offset = 0;
    std::uint32_t pages = 0;
    std::uint32_t last_page_bytes = 0;
    Digest root{};
};

constexpr std::size_t sort_capacity = 61680;
// One arena for ALL directions/stages, partitioned for independent ranks.
struct FinalArena {
    std::array<std::uint8_t, 1048576> tuples{};
    std::array<std::array<std::uint8_t, 65536>, 3> io{};
    std::array<std::array<std::uint8_t, 65536>, 2> pages{};
    std::array<Digest, 32> frontier{};
};
struct RankArena {
    std::uint8_t* tuples;
    std::size_t capacity;
    std::array<std::uint8_t*, 3> io;
    std::size_t io_bytes;
};
inline RankArena rank_arena(FinalArena& arena, unsigned lane = 0, unsigned lanes = 1) {
    const auto capacity = sort_capacity / lanes;
    const auto io_bytes = arena.io[0].size() / lanes;
    return {arena.tuples.data() + lane * capacity * 17, capacity,
        {{arena.io[0].data() + lane * io_bytes, arena.io[1].data() + lane * io_bytes,
          arena.io[2].data() + lane * io_bytes}}, io_bytes};
}
constexpr std::size_t final_control_bytes = 65536;
constexpr std::size_t final_overhead_bytes = 262144;
constexpr std::size_t final_allowance = 1836032;
static_assert(sizeof(FinalArena) + final_control_bytes + final_overhead_bytes + 131072
              == final_allowance, "section 10.7 fixed allowance");

struct Numeric { std::uint64_t word = 0; bool null = false; };
inline Numeric numeric_at(const Bytes& column, unsigned row) {
    const auto* p = column.data() + 9 * std::size_t{row};
    return {get(p + 1, 8), p[0] == 0};
}
// These buffers replace the encoder's body/column buffers after join. Their
// aggregate capacity is bounded by two Umax; no run-length-sized key cache.
struct DecodeScratch {
    Bytes raw, stored;
    std::array<Bytes, 3> columns;
    std::size_t capacity() const {
        std::size_t total = raw.capacity() + stored.capacity();
        for (const auto& c : columns) total += c.capacity();
        return total;
    }
};
struct Frame {
    std::uint64_t offset = 0, rec0 = 0;
    std::uint32_t length = 0, n = 0, ncols = 0, decoded = 0;
    char tag = 0;
    Digest digest{};
};

struct SinkFolds {
    std::uint64_t closed_count = 0, open_at_end_count = 0;
    double open_at_end_pnl = 0.0;
};

std::uint64_t table_bytes(const Table&);
std::uint64_t entry_offset(const Table&, std::uint64_t);
void plan_table(Table&, std::uint64_t& next);
Frame read_frame(const Output&, std::uint64_t, std::uint64_t data_end);
void read_numeric(const Output&, const Frame&, unsigned column, bool nullable, bool floating,
                  report_sink::ColumnCodec&, DecodeScratch&, unsigned destination);
void build_rank(Output&, Table&, const Table& trades, unsigned key, bool descending,
                report_sink::ColumnCodec&, DecodeScratch&, RankArena);
void seal_table(Table&, unsigned id, Output&, FinalArena&);
std::uint32_t scratch_backing(const std::string& directory, int staging_fd = -1);
SinkFolds validate_summary(std::string_view);

}}} // namespace pineforge::report_sink::v2
