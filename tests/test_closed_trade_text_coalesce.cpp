// Linux physical-I/O specification; the oracle owns the appended text bytes.
#if !defined(__linux__)
#error "Physical ledger accounting requires Linux linker wrapping"
#endif
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <pineforge/engine.hpp>
#include <pineforge/detail/scratch_file.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

namespace text_io {
using Count = std::uint64_t;
enum class File { other, index, payload };
struct Counts {
    Count writes = 0, bytes = 0, reads = 0, index_writes = 0, alternate = 0;
    off_t first_offset = -1;
    int payload_fd = -1;
    bool classification_error = false, transfer_error = false;
};
bool measuring = false;
Counts counts;
bool fail_payload = false;
std::size_t prefix_remaining = 0;

// Resolve each descriptor anew so fd reuse cannot inherit a classification.
File identify(int fd) noexcept {
    char path[sizeof("/proc/self/fd/") + 3 * sizeof(int)]{};
    char name[4096]{};
    struct stat status{};
    const auto length = std::snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(path)
        || ::fstat(fd, &status) != 0) {
        counts.classification_error = true;
        return File::other;
    }
    const auto used = ::readlink(path, name, sizeof(name) - 1);
    if (used < 0 || static_cast<std::size_t>(used) == sizeof(name) - 1) {
        counts.classification_error = true;
        return File::other;
    }
    name[used] = '\0';
    const auto* slash = std::strrchr(name, '/');
    const auto* base = slash ? slash + 1 : name;
    constexpr const char* prefixes[]{"pineforge-trade-index", "pineforge-trade-payload"};
    for (std::size_t i = 0; i < std::size(prefixes); ++i) {
        const auto size = std::strlen(prefixes[i]);
        if (std::strncmp(base, prefixes[i], size) == 0 && base[size] == '-') {
            if (!S_ISREG(status.st_mode)) counts.classification_error = true;
            return i == 0 ? File::index : File::payload;
        }
    }
    return File::other;
}
void record(int fd, std::size_t bytes, ssize_t result, bool write, off_t offset) noexcept {
    if (!measuring) return;
    const int saved = errno;
    const auto file = identify(fd);
    if (file == File::index && write) ++counts.index_writes;
    if (file == File::payload) {
        if (write) {
            if (counts.writes++ == 0) counts.first_offset = offset;
            if (result > 0) counts.bytes += static_cast<Count>(result);
        } else {
            ++counts.reads;
        }
        counts.payload_fd = fd;
        counts.transfer_error |= result < 0 || static_cast<std::size_t>(result) != bytes;
    }
    errno = saved;
}
void alternate(int fd) noexcept {
    if (!measuring || fd < 0) return;
    const int saved = errno;
    if (identify(fd) != File::other) ++counts.alternate;
    errno = saved;
}
struct Measure {
    Measure() {
        if (measuring) throw std::runtime_error("Nested I/O measurement");
        counts = {};
        measuring = true;
    }
    ~Measure() { measuring = false; fail_payload = false; }
    Counts finish() { measuring = false; return counts; }
};
} // namespace text_io

extern "C" ssize_t __real_pwrite(int, const void*, std::size_t, off_t);
extern "C" ssize_t __real_pread(int, void*, std::size_t, off_t);
extern "C" int __real_ftruncate(int, off_t);
extern "C" ssize_t __real_write(int, const void*, std::size_t);
extern "C" ssize_t __real_read(int, void*, std::size_t);
extern "C" ssize_t __real_writev(int, const struct iovec*, int);
extern "C" ssize_t __real_readv(int, const struct iovec*, int);
extern "C" ssize_t __real_pwritev(int, const struct iovec*, int, off_t);
extern "C" ssize_t __real_preadv(int, const struct iovec*, int, off_t);
extern "C" void* __real_mmap(void*, std::size_t, int, int, int, off_t);

extern "C" ssize_t __wrap_pwrite(int fd, const void* data, std::size_t bytes, off_t offset) {
    auto requested = bytes;
    if (text_io::fail_payload && text_io::identify(fd) == text_io::File::payload) {
        if (text_io::prefix_remaining == 0) {
            text_io::fail_payload = false;
            errno = ENOSPC;
            text_io::record(fd, bytes, -1, true, offset);
            return -1;
        }
        requested = std::min(bytes, text_io::prefix_remaining);
    }
    const auto result = __real_pwrite(fd, data, requested, offset);
    if (text_io::fail_payload && result > 0
        && text_io::identify(fd) == text_io::File::payload)
        text_io::prefix_remaining -= static_cast<std::size_t>(result);
    text_io::record(fd, bytes, result, true, offset);
    return result;
}
extern "C" ssize_t __wrap_pread(int fd, void* data, std::size_t bytes, off_t offset) {
    const auto result = __real_pread(fd, data, bytes, offset);
    text_io::record(fd, bytes, result, false, offset);
    return result;
}
extern "C" int __wrap_ftruncate(int fd, off_t length) {
    const auto result = __real_ftruncate(fd, length);
    text_io::alternate(fd);
    return result;
}
extern "C" ssize_t __wrap_write(int fd, const void* data, std::size_t bytes) {
    const auto result = __real_write(fd, data, bytes);
    text_io::alternate(fd);
    return result;
}
extern "C" ssize_t __wrap_read(int fd, void* data, std::size_t bytes) {
    const auto result = __real_read(fd, data, bytes);
    text_io::alternate(fd);
    return result;
}
extern "C" ssize_t __wrap_writev(int fd, const struct iovec* data, int count) {
    const auto result = __real_writev(fd, data, count);
    text_io::alternate(fd);
    return result;
}
extern "C" ssize_t __wrap_readv(int fd, const struct iovec* data, int count) {
    const auto result = __real_readv(fd, data, count);
    text_io::alternate(fd);
    return result;
}
extern "C" ssize_t __wrap_pwritev(int fd, const struct iovec* data, int count, off_t offset) {
    const auto result = __real_pwritev(fd, data, count, offset);
    text_io::alternate(fd);
    return result;
}
extern "C" ssize_t __wrap_preadv(int fd, const struct iovec* data, int count, off_t offset) {
    const auto result = __real_preadv(fd, data, count, offset);
    text_io::alternate(fd);
    return result;
}
extern "C" void* __wrap_mmap(void* address, std::size_t length, int protection,
                             int flags, int fd, off_t offset) {
    auto* result = __real_mmap(address, length, protection, flags, fd, offset);
    if ((flags & MAP_ANONYMOUS) == 0) text_io::alternate(fd);
    return result;
}

namespace pineforge::detail {
struct TradeLedgerStorageProbe {
    static std::array<std::uint64_t, 2> bytes(const TradeLedger& ledger) {
        return ledger.scratch_storage_bytes();
    }
};
} // namespace pineforge::detail

namespace {
using namespace pineforge;
using text_io::Count;
constexpr auto kTailBytes = TradeLedger::kResidentRows * TradeLedger::kRecordBytes;
constexpr auto kBudget = detail::ScratchFile::kCacheBytes;
// Thirty-two descriptor windows exceed 2,000 rows and fit the decoded cache.
constexpr auto kRows = 32 * TradeLedger::kResidentRows;
static_assert(kRows >= 2000 && kRows < TradeLedger::kDefaultCacheRows);
int failures = 0;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", message);
    }
}
void valid(const text_io::Counts& io) {
    require(!io.classification_error && !io.transfer_error && io.alternate == 0,
            "Physical I/O classification, transfer or alternate-I/O failure");
}
std::array<const std::string*, 4> texts(const Trade& row) {
    return {{&row.entry_id, &row.entry_comment, &row.exit_comment, &row.exit_id}};
}
std::string packed(const Trade& row) {
    std::string result;
    for (const auto* text : texts(row)) result += *text;
    return result;
}
std::string text(std::size_t length, std::size_t seed) {
    std::string result(length, '\0');
    for (std::size_t i = 0; i < length; ++i)
        result[i] = static_cast<char>('A' + (seed + i) % 26);
    if (!result.empty()) result[result.size() / 2] = '\0';
    return result;
}
Trade row(std::size_t i) {
    Trade value{};
    const auto shape = i % 8;
    if (shape != 0) value.entry_id = text(17 + shape, i);
    if (shape >= 2) value.exit_id = text(19 + shape, i + 1);
    if (shape == 2) {
        auto& comment = (i / 8) % 2 == 0 ? value.entry_comment : value.exit_comment;
        comment = text(11, i + 2);
    }
    return value;
}
// A request adds one syscall plus one per budget boundary strictly inside it.
// A boundary at its end needs no split; the next request starts a new budget.
Count calls(Count start, Count bytes) {
    return bytes == 0 ? 0 : 1 + (start + bytes - 1) / kBudget - start / kBudget;
}
void same_texts(const TradeLedger& ledger, std::size_t i, const Trade& expected) {
    const auto actual = ledger.read(i);
    check(actual.entry_id == expected.entry_id, "Owning entry ID differs");
    check(actual.entry_comment == expected.entry_comment, "Owning entry comment differs");
    check(actual.exit_comment == expected.exit_comment, "Owning exit comment differs");
    check(actual.exit_id == expected.exit_id, "Owning exit ID differs");
    check(ledger.entry_id_at(i) == expected.entry_id, "Entry ID accessor differs");
    check(ledger.entry_comment_at(i) == expected.entry_comment, "Entry comment accessor differs");
    check(ledger.exit_comment_at(i) == expected.exit_comment, "Exit comment accessor differs");
    check(ledger.exit_id_at(i) == expected.exit_id, "Exit ID accessor differs");
    check(ledger.entry_id_equals(i, expected.entry_id), "Entry ID comparison differs");
    check(ledger.entry_comment_equals(i, expected.entry_comment), "Entry comment comparison differs");
    check(ledger.exit_comment_equals(i, expected.exit_comment), "Exit comment comparison differs");
    check(ledger.exit_id_equals(i, expected.exit_id), "Exit ID comparison differs");
}
void disk_bytes(const TradeLedger& ledger, int fd, const std::string& expected) {
    const auto sizes = detail::TradeLedgerStorageProbe::bytes(ledger);
    check(sizes[0] == ledger.size() * TradeLedger::kRecordBytes, "Index extent differs");
    check(sizes[1] == expected.size(), "Payload occupancy differs from appended bytes");
    struct stat status{};
    require(fd >= 0 && ::fstat(fd, &status) == 0, "Cannot inspect payload file");
    check(static_cast<Count>(status.st_size) == expected.size(), "Payload file size differs");
    std::string actual(expected.size(), '\0');
    std::size_t received = 0;
    while (received < actual.size()) {
        const auto count = __real_pread(fd, actual.data() + received,
                                       actual.size() - received, received);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "Cannot read physical payload bytes");
        received += static_cast<std::size_t>(count);
    }
    check(actual == expected, "Physical payload bytes differ from text oracle");
}
void append_population() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    std::vector<Trade> expected;
    std::string payload;
    Count nonempty_rows = 0, nonempty_texts = 0, coalesced = 0, separate = 0;
    // Each eight-row group has 7 nonempty rows and 14 nonempty fields.
    // Its byte widths are 0,18,51,42,44,46,48,50 (sum 299).
    for (std::size_t i = 0; i < kRows; ++i) {
        expected.push_back(row(i));
        const auto bytes = packed(expected.back());
        coalesced += calls(payload.size(), bytes.size());
        auto offset = payload.size();
        for (const auto* field : texts(expected.back())) {
            nonempty_texts += !field->empty();
            separate += calls(offset, field->size());
            offset += field->size();
        }
        nonempty_rows += !bytes.empty();
        payload += bytes;
    }
    // This fixture's only 64 KiB crossing is inside a row and an ID.
    // Check that geometry rather than silently counting aligned ends as cuts.
    const auto budget_splits = payload.size() / kBudget;
    require(budget_splits > 0 && payload.size() > kTailBytes, "Fixture never spills");
    require(coalesced == nonempty_rows + budget_splits, "Row boundary masks a split");
    require(separate == nonempty_texts + budget_splits, "Field boundary masks a split");
    require(separate > coalesced, "Uncoalesced mutant cannot break the call bound");
    text_io::Measure append;
    for (const auto& value : expected) ledger.push_back(value);
    const auto io = append.finish();
    valid(io);
    std::printf("payload_pwrite_calls actual=%llu expected=%llu uncoalesced=%llu bytes=%llu\n",
                static_cast<unsigned long long>(io.writes),
                static_cast<unsigned long long>(coalesced),
                static_cast<unsigned long long>(separate),
                static_cast<unsigned long long>(payload.size()));
    check(io.writes == coalesced, "Append payload pwrite count differs from row geometry");
    check(io.bytes == payload.size(), "Append wrote a different number of text bytes");
    disk_bytes(ledger, io.payload_fd, payload);
    for (int pass = 0; pass < 3; ++pass) {
        if (pass == 2) ledger.resize(ledger.size()); // Public decoded-cache invalidation.
        text_io::Measure read;
        for (std::size_t i = 0; i < expected.size(); ++i) same_texts(ledger, i, expected[i]);
        const auto reads = read.finish();
        valid(reads);
        check(reads.writes == 0 && reads.index_writes == 0, "Readers wrote ledger bytes");
        if (pass == 1) check(reads.reads == 0, "Warm text reads missed the decoded cache");
        else check(reads.reads > 0, "Cold text reads never reached the payload file");
    }
    disk_bytes(ledger, io.payload_fd, payload);
}
void bounded_and_replace_controls() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    std::vector<Trade> expected;
    Trade exact{};
    exact.entry_id = text(kTailBytes / 2, 1);
    exact.entry_comment = text(1, 2);
    exact.exit_comment = text(1, 3);
    exact.exit_id = text(kTailBytes - exact.entry_id.size() - 2, 4);
    expected.push_back(exact);
    auto over = exact;
    over.exit_id += 'X'; // Total just exceeds the cap; each individual text fits.
    expected.push_back(over);
    Trade huge{};
    huge.entry_id = text(kTailBytes + 1, 5);
    huge.exit_id = text(3, 6); // A single oversized field also refuses the copy.
    expected.push_back(huge);
    std::string payload;
    int fd = -1;
    for (const auto& value : expected) {
        const auto bytes = packed(value);
        Count wanted = 0;
        if (bytes.size() <= kTailBytes) wanted = calls(payload.size(), bytes.size());
        else {
            auto offset = payload.size();
            for (const auto* field : texts(value)) {
                wanted += calls(offset, field->size());
                offset += field->size();
            }
            require(wanted > calls(payload.size(), bytes.size()),
                    "Oversized-input mutant cannot detect a coalesced fallback");
        }
        text_io::Measure append;
        ledger.push_back(value);
        const auto io = append.finish();
        valid(io);
        check(io.writes == wanted && io.bytes == bytes.size(), "Bounded append route differs");
        payload += bytes;
        fd = io.payload_fd;
    }
    // Exactly two changed texts must retain two requests on replacement.
    // The two comments reuse their old extents and contribute no payload I/O.
    auto changed = exact;
    changed.entry_id = text(7, 7);
    changed.exit_id = text(9, 8);
    const auto wanted = calls(payload.size(), changed.entry_id.size())
        + calls(payload.size() + changed.entry_id.size(), changed.exit_id.size());
    text_io::Measure replace;
    ledger.replace(0, changed);
    const auto io = replace.finish();
    valid(io);
    check(io.writes == wanted, "Replacement coalesced changed texts or rewrote equal texts");
    check(io.bytes == changed.entry_id.size() + changed.exit_id.size(),
          "Replacement wrote bytes from equal texts");
    payload += changed.entry_id;
    payload += changed.exit_id;
    expected[0] = changed;
    ledger.resize(ledger.size());
    for (std::size_t i = 0; i < expected.size(); ++i) same_texts(ledger, i, expected[i]);
    disk_bytes(ledger, fd, payload);
}
void failed_append_is_unpublished() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    const auto seed = row(3);
    const auto added = row(2);
    ledger.push_back(seed);
    const auto published = ledger.size();
    same_texts(ledger, 0, seed);
    bool caught = false;
    text_io::Measure failing;
    text_io::prefix_remaining = added.entry_id.size() + 1;
    text_io::fail_payload = true;
    try { ledger.push_back(added); }
    catch (const detail::ScratchIoError& error) { caught = error.code().value() == ENOSPC; }
    const auto failed = failing.finish();
    require(caught && !text_io::fail_payload, "Injected partial-write failure was not reached");
    check(failed.bytes == added.entry_id.size() + 1, "Fault did not cross the first text");
    check(failed.index_writes == 0 && ledger.size() == published, "Failed append published a row");
    same_texts(ledger, 0, seed);
    ledger.resize(ledger.size());
    same_texts(ledger, 0, seed);
    text_io::Measure retry;
    ledger.push_back(added);
    const auto io = retry.finish();
    valid(io);
    check(io.first_offset == static_cast<off_t>(packed(seed).size()),
          "Failed coalesced write advanced the payload end");
    check(io.writes == calls(packed(seed).size(), packed(added).size()),
          "Retry did not use one bounded row request");
    same_texts(ledger, published, added);
    ledger.resize(ledger.size());
    same_texts(ledger, 0, seed);
    same_texts(ledger, published, added);
    disk_bytes(ledger, io.payload_fd, packed(seed) + packed(added));
}
} // namespace

int main() {
    try {
        append_population();
        bounded_and_replace_controls();
        failed_append_is_unpublished();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
