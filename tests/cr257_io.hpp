#pragma once

// One Linux executable owns the forwarding linker wrappers. Each CTest
// selects a real workload in that executable, with setup outside its counters.
#include <pineforge/detail/scratch_file.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace cr257 {

using Count = std::uint64_t;
inline constexpr Count kSmallN = 10000;
inline constexpr Count kLargeN = 100000;
inline constexpr Count kReadFactor = 2; // Cold fill, plus one replacement read per page.
inline constexpr Count kCacheBytes = pineforge::detail::ScratchFile::kCacheBytes;

struct Calls {
    Count calls = 0;
    Count bytes = 0;       // Requested, never a fabricated transferred-byte count.
    Count transferred = 0;
    Count errors = 0;
    Count short_io = 0;
};

struct Io {
    Calls pread, pwrite, fadvise, fdatasync;
    Count dontneed = 0;
    Count random = 0;
    Count whole_file_advice = 0;
};

enum class FileClass : std::size_t {
    command, trade_payload, trade_index, ids, id_pairs, o1e2, other, count
};
inline constexpr std::size_t class_index(FileClass value) noexcept {
    return static_cast<std::size_t>(value);
}
inline constexpr std::array<const char*, class_index(FileClass::count)> kClassNames{{
    "pineforge-command", "pineforge-trade-payload", "pineforge-trade-index",
    "pineforge-ids", "pineforge-id-pairs", "pineforge-o1e2", "other"
}};
inline constexpr std::size_t kFdSlots = 32; // Fixed bookkeeping, independent of N.

struct AlternateReads {
    int fd = -1;
    std::uintmax_t device = 0, inode = 0;
    FileClass file_class = FileClass::other;
    Count read = 0, readv = 0, preadv = 0, preadv2 = 0, mmap = 0;
};

struct Counters {
    Io total;
    Io pages; // Platform-page-sized I/O on an unlinked regular descriptor.
    std::array<Io, kClassNames.size()> classes{};
    // Retain calls by fd AND identity before pread/pwrite identifies page_fd.
    // Reusing an fd cannot move old alternate reads into another file's class.
    std::array<AlternateReads, kFdSlots> alternate_reads{};
    int page_fd = -1;
    std::uintmax_t page_device = 0, page_inode = 0;
    bool multiple_page_fds = false;
    bool classification_error = false;
};

struct Context {
    Count n = 1;
    Count extent_pages = 0;
    Count records_changed = 0;
};

extern bool measuring;
extern std::size_t platform_page;
extern Counters counters;
extern int failures;

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

inline Count ceil_div(Count value, Count divisor) {
    require(divisor != 0, "CR-257 zero bound denominator");
    return value / divisor + (value % divisor != 0);
}

// An I/O contract failure records its measured and allowed numbers and lets
// the remaining contracts run. Setup/result-integrity failures throw instead.
void bound(const char* metric, Count actual, Count allowed, Context);
void class_bound(FileClass, const char* metric, Count actual, Count allowed, Context);
void total_info(const char* metric, Count actual, Context);
void validate(const Counters&);

class Measure {
public:
    Measure();
    ~Measure();
    Counters finish();
    Measure(const Measure&) = delete;
    Measure& operator=(const Measure&) = delete;
};

struct SourceRun {
    Context context;
    Counters io;
    Count page_reads = 0;
    Count page_writes = 0;
    Count sync_charges = 0;
    Count commits = 0;
    Count insert_commits = 0;
    Count records_created = 0;
};

SourceRun source_run(Count n);
SourceRun scope_erase_run(Count records);
void source_bounds(const SourceRun&);
void source_100k();
void exit_scope_io();
void scratch_file_io();

} // namespace cr257
