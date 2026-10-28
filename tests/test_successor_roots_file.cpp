// Successor lookup survives journal retirement without a
// per-replacement heap slope. The heap witness uses only the base API.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/native_order.hpp>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <variant>

namespace no = pineforge::native_order;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

no::Request request() {
    no::Request value{no::Transact{1.0}, "moving", ""};
    value.trigger = no::Limit{90.0};
    return value;
}

struct Driver {
    no::WorkingRequestCore core{{"roots", 1}};
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    no::CommandContext context{};

    no::RequestHandle submit() {
        const auto result = core.submit(request(), 0, incarnation, ordinal);
        require(result.status == no::SubmitStatus::Accepted, "submit accepted");
        core.retire_history(core.last_ordinal());
        return *result.handle;
    }

    no::RequestHandle replace(no::RequestHandle target, bool direct, no::ReplaceOptions options = {}) {
        no::ReplaceResult result;
        if (direct) {
            auto installed = core.apply_replace(target, request(), context, incarnation, ordinal, options);
            result = std::move(installed.result);
        } else {
            auto prepared = core.prepare_replace(target, request(), context, incarnation, ordinal, options);
            // Installing a prepared replacement remains allocation-free,
            // including at a spill boundary.
            global_allocation::refusing = true;
            auto installed = core.install_replace(std::move(prepared));
            global_allocation::refusing = false;
            require(std::holds_alternative<no::CommandInstalled<no::ReplaceResult>>(installed),
                    "prepared replacement installed");
            result = std::move(std::get<no::CommandInstalled<no::ReplaceResult>>(installed).result);
        }
        require(result.status == no::ReplaceStatus::Replaced, "replace accepted");
        ++incarnation;
        ++ordinal;
        core.retire_history(core.last_ordinal());
        require(core.history().empty(), "replacement journal retired");
        return *result.successor;
    }
};

struct Recording {
    Recording() {
        global_allocation::recording = true;
    }
    ~Recording() { global_allocation::recording = false; }
};

void replacement_heap_is_flat(bool direct) {
    Recording recording;
    Driver driver;
    const auto root = driver.submit();
    auto tail = root;
    no::RequestHandle old_successor;
    for (std::size_t i = 0; i < 1024; ++i) {
        tail = driver.replace(tail, direct);
        if (i == 0) old_successor = tail;
    }
    const std::size_t low = global_allocation::live_bytes;
    for (std::size_t i = 1024; i < 16384; ++i) tail = driver.replace(tail, direct);
    const std::size_t high = global_allocation::live_bytes;
    std::printf("successor roots %s: 1024=%zu 16384=%zu retained bytes\n",
                direct ? "direct" : "prepared", low, high);
    // Other core state is bounded here: one live request, one dense issued
    // range, no group/cohort receipts, and an empty journal after each step.
    // The base retains >= 16 * (16384 - 1024) bytes of successor pairs.
    require(high <= low + 4096, "successor roots retain per-replacement heap bytes");
    const auto cohort = driver.core.cohort_open();
    driver.core.cohort_add(cohort, tail);
    require(driver.core.cohort_contains(cohort, root), "root still in cohort");
    require(driver.core.cohort_contains(cohort, old_successor), "old spilled successor found");
    driver.core.cohort_remove(cohort, old_successor);
    require(!driver.core.cohort_contains(cohort, tail), "old successor removes canonical root");
}

void sorted_lookup_preserves_interleaved_roots() {
    Driver driver;
    std::array<no::RequestHandle, 3> roots{driver.submit(), driver.submit(), driver.submit()};
    auto tails = roots;
    std::array<no::RequestHandle, 3> oldest{};
    std::array<no::RequestHandle, 3> previous{};
    no::RequestHandle plain;
    for (std::size_t i = 0; i < 2054; ++i) {
        const std::size_t slot = i % tails.size();
        previous[slot] = tails[slot];
        tails[slot] = driver.replace(tails[slot], i % 2 == 0);
        if (i < tails.size()) oldest[slot] = tails[slot];
        if (i == 99) {
            plain = driver.submit();
            (void)driver.core.cancel(plain, driver.ordinal);
            driver.core.retire_history(driver.core.last_ordinal());
        }
    }
    const auto cohort = driver.core.cohort_open();
    driver.core.cohort_add(cohort, tails[1]);
    require(driver.core.cohort_contains(cohort, roots[1]), "second root found");
    require(driver.core.cohort_contains(cohort, oldest[1]), "interleaved old root found");
    require(driver.core.cohort_contains(cohort, previous[1]), "terminal root in RAM tail found");
    require(!driver.core.cohort_contains(cohort, oldest[0]), "first chain stays distinct");
    require(!driver.core.cohort_contains(cohort, oldest[2]), "third chain stays distinct");
    require(!driver.core.cohort_contains(cohort, plain), "plain terminal handle in a disk-key gap");
    require(!driver.core.cohort_contains(cohort, {{"other", 1}, oldest[1].incarnation}),
            "foreign-run handle refused");
    require(!driver.core.cohort_contains(cohort, {{"roots", 1}, driver.incarnation + 7}),
            "unissued handle refused");
    driver.core.cohort_remove(cohort, oldest[1]);
    require(!driver.core.cohort_contains(cohort, tails[1]), "old root removed");
    driver.core.cohort_add(cohort, tails[2]);
    no::WorkingRequestCore moved(std::move(driver.core));
    require(moved.cohort_contains(cohort, oldest[2]), "move keeps spilled roots");
    no::WorkingRequestCore destination({"discard", 1});
    destination = std::move(moved);
    require(destination.cohort_contains(cohort, oldest[2]), "move assignment keeps spilled roots");
    destination.reset({"roots", 2});
    require(!destination.cohort_contains(cohort, oldest[2]), "reset forgets previous roots");
}

struct ScratchDirectory {
    std::string path = "/tmp/pineforge-roots-test-XXXXXX";
    ScratchDirectory() { require(::mkdtemp(path.data()) != nullptr, "test scratch directory created"); }
    ~ScratchDirectory() { ::rmdir(path.c_str()); }
};

// Inspect only the test core's anonymous file: it has the known first pair
// (2,1), no directory links, and the byte length this case expects. /dev/fd
// is available on both verification Linux hosts and Darwin.
int root_file_descriptor(off_t expected_bytes) {
    DIR* descriptors = ::opendir("/dev/fd");
    require(descriptors != nullptr, "descriptor directory opened");
    int found = -1;
    while (const auto* entry = ::readdir(descriptors)) {
        char* end = nullptr;
        const long number = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || number < 0
            || number > std::numeric_limits<int>::max()) continue;
        const int fd = static_cast<int>(number);
        struct stat status{};
        std::array<std::uint64_t, 2> row{};
        if (::fstat(fd, &status) == 0 && S_ISREG(status.st_mode)
            && status.st_nlink == 0 && status.st_size == expected_bytes
            && ::pread(fd, row.data(), sizeof(row), 0) == static_cast<ssize_t>(sizeof(row))
            && row[0] == 2 && row[1] == 1) {
            found = fd;
            break;
        }
    }
    ::closedir(descriptors);
    require(found >= 0, "anonymous successor root file found");
    return found;
}

void file_layout_cursor_and_ownership() {
    ScratchDirectory scratch;
    Driver driver;
    driver.core.set_successor_roots_directory(scratch.path);
    auto tail = driver.submit();
    no::RequestHandle first;
    for (std::size_t i = 0; i < 65; ++i) {
        tail = driver.replace(tail, i % 2 == 0);
        if (i == 0) first = tail;
    }
    const int fd = root_file_descriptor(64 * 16);
    require((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "scratch descriptor closes on exec");
    std::array<std::uint64_t, 128> rows{};
    require(::pread(fd, rows.data(), sizeof(rows), 0) == static_cast<ssize_t>(sizeof(rows)),
            "complete sorted batch read");
    for (std::size_t i = 0; i < 64; ++i) {
        require(rows[2 * i] == i + 2 && rows[2 * i + 1] == 1,
                "file is exactly the committed 16-byte root pairs");
    }
    require(::lseek(fd, 7, SEEK_SET) == 7, "test changes file cursor");
    const auto cohort = driver.core.cohort_open();
    driver.core.cohort_add(cohort, tail);
    require(driver.core.cohort_contains(cohort, first), "pread finds old root");
    require(::lseek(fd, 0, SEEK_CUR) == 7, "lookup leaves shared file cursor unchanged");

    auto abandoned = driver.core.prepare_replace(tail, request(), driver.context,
                                                 driver.incarnation, driver.ordinal);
    auto chosen = driver.core.prepare_replace(tail, request(), driver.context,
                                              driver.incarnation, driver.ordinal);
    const auto stale = driver.core.install_replace(std::move(abandoned));
    require(std::holds_alternative<no::InstallError>(stale), "abandoned preparation is stale");
    global_allocation::refusing = true;
    auto installed = driver.core.install_replace(std::move(chosen));
    global_allocation::refusing = false;
    require(std::holds_alternative<no::CommandInstalled<no::ReplaceResult>>(installed),
            "chosen preparation installs without allocation");
    tail = *std::get<no::CommandInstalled<no::ReplaceResult>>(installed).result.successor;
    ++driver.incarnation;
    ++driver.ordinal;
    driver.core.retire_history(driver.core.last_ordinal());
    no::ReplaceOptions kept;
    kept.keep_handle = true;
    const auto same = driver.replace(tail, true, kept);
    require(same == tail, "keep-handle reprice emits no successor");
    require(driver.core.cohort_contains(cohort, first), "reprice preserves old roots");

    bool changed = false;
    try {
        driver.core.set_successor_roots_directory(scratch.path + "/elsewhere");
    } catch (const std::logic_error&) {
        changed = true;
    }
    require(changed, "active scratch directory cannot silently change");
    no::WorkingRequestCore moved(std::move(driver.core));
    require(moved.cohort_contains(cohort, first), "move owns file and tail");
    no::WorkingRequestCore destination({"discard", 1});
    destination = std::move(moved);
    require(destination.cohort_contains(cohort, first), "assignment transfers file");
    destination.reset({"roots", 2});
    errno = 0;
    require(::fcntl(fd, F_GETFD) == -1 && errno == EBADF, "reset closes the anonymous file");
    driver.core.reset({"roots", 3});
    require(driver.core.history_end() == 0, "moved-from core can start a new run");
}

void failed_create_does_not_commit() {
    ScratchDirectory scratch;
    Driver driver;
    driver.core.set_successor_roots_directory(scratch.path + "/missing");
    auto tail = driver.submit();
    no::RequestHandle first;
    for (std::size_t i = 0; i < 64; ++i) {
        tail = driver.replace(tail, true);
        if (i == 0) first = tail;
    }
    const auto cohort = driver.core.cohort_open();
    driver.core.cohort_add(cohort, tail);
    const auto events = driver.core.history_end();
    const auto incarnation = driver.incarnation;
    const auto ordinal = driver.ordinal;
    const auto last_incarnation = driver.core.last_incarnation();
    const auto last_ordinal = driver.core.last_ordinal();
    bool failed = false;
    try {
        (void)driver.replace(tail, true);
    } catch (const std::system_error& error) {
        failed = error.code().value() == ENOENT;
    }
    require(failed, "configured directory create failure propagates");
    require(driver.incarnation == incarnation && driver.ordinal == ordinal
            && driver.core.last_incarnation() == last_incarnation && driver.core.last_ordinal() == last_ordinal
            && driver.core.history_end() == events && driver.core.find_live(tail),
            "create failure precedes all logical mutation");
    require(driver.core.cohort_contains(cohort, first), "failed create retains authoritative tail");
    driver.core.set_successor_roots_directory(scratch.path);
    tail = driver.replace(tail, false);
    require(driver.core.cohort_contains(cohort, tail), "create can be retried");
    require(driver.core.cohort_contains(cohort, first), "retry preserves old root identity");
}

struct FileSizeLimit {
    struct rlimit previous{};
    using Handler = void (*)(int);
    Handler handler = SIG_DFL;
    FileSizeLimit() {
        require(::getrlimit(RLIMIT_FSIZE, &previous) == 0, "read test file-size limit");
        handler = std::signal(SIGXFSZ, SIG_IGN);
        require(handler != SIG_ERR, "ignore test file-size signal");
        struct rlimit limit = previous;
        limit.rlim_cur = 96;
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            std::signal(SIGXFSZ, handler);
            throw std::runtime_error("set test file-size limit");
        }
    }
    ~FileSizeLimit() {
        ::setrlimit(RLIMIT_FSIZE, &previous);
        std::signal(SIGXFSZ, handler);
    }
};

void partial_write_is_retryable() {
    ScratchDirectory scratch;
    Driver driver;
    driver.core.set_successor_roots_directory(scratch.path);
    auto tail = driver.submit();
    no::RequestHandle first;
    for (std::size_t i = 0; i < 64; ++i) {
        tail = driver.replace(tail, false);
        if (i == 0) first = tail;
    }
    const auto cohort = driver.core.cohort_open();
    driver.core.cohort_add(cohort, tail);
    const auto events = driver.core.history_end();
    const auto incarnation = driver.incarnation;
    const auto ordinal = driver.ordinal;
    const auto last_incarnation = driver.core.last_incarnation();
    const auto last_ordinal = driver.core.last_ordinal();
    bool failed = false;
    {
        FileSizeLimit limit;
        try {
            (void)driver.replace(tail, false);
        } catch (const std::system_error& error) {
            failed = error.code().value() == EFBIG;
        }
    }
    require(failed, "short batch followed by EFBIG propagates");
    const int fd = root_file_descriptor(96);
    require(driver.incarnation == incarnation && driver.ordinal == ordinal
            && driver.core.last_incarnation() == last_incarnation && driver.core.last_ordinal() == last_ordinal
            && driver.core.history_end() == events && driver.core.find_live(tail),
            "partial write precedes all logical mutation");
    require(driver.core.cohort_contains(cohort, first), "partial batch is not published to readers");
    tail = driver.replace(tail, false);
    require(root_file_descriptor(64 * 16) == fd, "retry overwrites the partial batch in place");
    require(driver.core.cohort_contains(cohort, first)
            && driver.core.cohort_contains(cohort, tail), "retry preserves both disk and tail lookups");
}

void truncated_read_fails() {
    ScratchDirectory scratch;
    Driver driver;
    driver.core.set_successor_roots_directory(scratch.path);
    auto tail = driver.submit();
    no::RequestHandle last_on_disk;
    for (std::size_t i = 0; i < 65; ++i) {
        tail = driver.replace(tail, true);
        if (i == 63) last_on_disk = tail;
    }
    const auto cohort = driver.core.cohort_open();
    driver.core.cohort_add(cohort, tail);
    require(driver.core.cohort_contains(cohort, last_on_disk), "last disk row is readable");
    const int fd = root_file_descriptor(64 * 16);
    require(::ftruncate(fd, 63 * 16 + 8) == 0, "test cuts the last row midway");
    bool failed = false;
    try {
        (void)driver.core.cohort_contains(cohort, last_on_disk);
    } catch (const std::system_error& error) {
        failed = error.code().value() == EIO;
    }
    require(failed, "truncated read fails instead of inventing a different root");
}

}  // namespace

int main() {
    try {
        replacement_heap_is_flat(true);
        replacement_heap_is_flat(false);
        sorted_lookup_preserves_interleaved_roots();
        file_layout_cursor_and_ownership();
        failed_create_does_not_commit();
        partial_write_is_retryable();
        truncated_read_fails();
        std::puts("successor roots file: PASS");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::refusing = false;
        std::fprintf(stderr, "successor roots file: %s\n", error.what());
        return 1;
    }
}
