// CR-150 L19m: independent old containers, real retained-byte measurements,
// and fault injection into the production scratch store. No hash pins.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/detail/adapter_exposure_storage.hpp>
#include <pineforge/native_order_identity.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/resource.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
namespace storage = pineforge::detail;
using Handle = pineforge::native_order::RequestHandle;
using Units = std::unordered_map<std::uint64_t, double>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Directory {
    std::string path = "/tmp/pineforge-growth-test-XXXXXX";
    Directory() { require(::mkdtemp(path.data()) != nullptr, "create scratch directory"); }
    ~Directory() { ::rmdir(path.c_str()); }
};

struct Recording {
    Recording() {
        require(!global_allocation::recording, "allocation windows must not nest");
        global_allocation::live_bytes = 0;
        global_allocation::recording = true;
    }
    ~Recording() { global_allocation::recording = false; }
};

struct Exposure {
    std::vector<Handle> opened;
    Units units;
};

Handle handle(std::uint64_t incarnation) {
    Handle result;
    result.incarnation = incarnation;
    return result;
}

void same_exposure(const Exposure& actual, const Exposure& old) {
    require(actual.opened == old.opened, "opened order and complete handle identities match");
    require(actual.units == old.units, "all provenance quantities match the old map");
}

void exposure_lifecycles_match_old_containers() {
    Exposure actual, old;
    for (std::uint64_t cycle = 0; cycle < 128; ++cycle) {
        for (std::uint64_t offset = 1; offset <= 9; ++offset) {
            const auto origin = cycle * 9 + offset;
            actual.opened.push_back(handle(origin));
            old.opened.push_back(handle(origin));
            actual.units[origin] += static_cast<double>(offset);
            old.units[origin] += static_cast<double>(offset);
            storage::release_empty_exposure_capacity(actual.opened);
            storage::release_empty_exposure_capacity(actual.units);
            same_exposure(actual, old);
        }
        // A partial reduction cannot retire a handle or an outstanding unit.
        const auto first = actual.opened.front().incarnation;
        actual.units.at(first) -= 0.5;
        old.units.at(first) -= 0.5;
        storage::release_empty_exposure_capacity(actual.units);
        same_exposure(actual, old);
        // Consume in a different order from insertion, including the last
        // handle. Reusing the objects exercises a reopened source cohort.
        while (!old.opened.empty()) {
            const auto origin = old.opened.back().incarnation;
            actual.units.erase(origin);
            old.units.erase(origin);
            actual.opened.pop_back();
            old.opened.pop_back();
            storage::release_empty_exposure_capacity(actual.units);
            storage::release_empty_exposure_capacity(actual.opened);
            same_exposure(actual, old);
        }
        require(actual.opened.capacity() == std::vector<Handle>{}.capacity(),
                "drained cohort retains no handle allocation");
        require(actual.units.bucket_count() == Units{}.bucket_count(),
                "drained cohort retains no historical buckets");
        // Reversal/flat use the old clear semantics, including on nonempty
        // state. They must release storage without changing logical values.
        actual.opened.push_back(handle(first));
        old.opened.push_back(handle(first));
        actual.units[first] = old.units[first] = 2.0;
        storage::clear_exposure_storage(actual.opened);
        storage::clear_exposure_storage(actual.units);
        old.opened.clear();
        old.units.clear();
        same_exposure(actual, old);
    }
}

std::size_t retained_closed_exposures(std::size_t ids, bool release, bool map) {
    // Cohort headers belong to L19l. Allocate them outside the measured
    // window so this witness isolates the two measured child allocations,
    // even while an origin/placement keeps every parent row resident.
    std::vector<Exposure> cohorts(ids);
    const Handle opening = handle(1); // SessionKey initialization is outside.
    Recording recording;
    for (auto& cohort : cohorts) {
        if (map) {
            cohort.units[opening.incarnation] = 1.0;
            cohort.units.erase(opening.incarnation);
            if (release) storage::release_empty_exposure_capacity(cohort.units);
        } else {
            cohort.opened.push_back(opening);
            cohort.opened.erase(cohort.opened.begin());
            if (release) storage::release_empty_exposure_capacity(cohort.opened);
        }
    }
    return global_allocation::live_bytes;
}

void closed_id_heap_is_flat() {
    for (const bool map : {false, true}) {
        const auto old_low = retained_closed_exposures(10, false, map);
        const auto old_high = retained_closed_exposures(10000, false, map);
        const auto low = retained_closed_exposures(10, true, map);
        const auto high = retained_closed_exposures(10000, true, map);
        require(old_high > old_low, "old containers must reproduce historical-id growth");
        require(high == low && high == 0, "empty exposure children grow with completed ids");
    }
}

void phases_match_old_vector() {
    Directory directory;
    storage::ExitPhaseStore actual, fixture;
    actual.set_directory(directory.path);
    fixture.fixture_use_memory_reference();
    std::vector<std::uint8_t> old;
    for (std::size_t i = 0; i < 10000; ++i) {
        const auto index = i * 3; // None-filled gaps are logical/hash inputs.
        const auto phase = static_cast<std::uint8_t>(i % 6);
        old.resize(index + 1, 0);
        actual.resize(index + 1, 0);
        fixture.resize(index + 1, 0);
        old[index] = phase;
        actual.set(index, phase);
        fixture.set(index, phase);
    }
    require(actual.disk_bytes() == old.size() && fixture.disk_bytes() == 0,
            "production bytes really use disk and the old fixture uses RAM");
    for (std::size_t i = 0; i < old.size(); ++i)
        require(actual[i] == old[i] && fixture[i] == old[i], "exact vector byte/gap replay");
    const auto bytes = actual.disk_bytes();
    for (std::size_t i = 0; i < 10000; ++i) {
        const auto index = (i * 7919) % old.size();
        const auto phase = static_cast<std::uint8_t>((i + 3) % 6);
        old[index] = phase;
        actual.set(index, phase); // Old finalized prefix, not just a tail.
        fixture.set(index, phase);
        require(actual[index] == old[index], "write invalidates the cached page");
    }
    // A same-bar permutation reads every source byte before overwriting any
    // target. Cross read-page boundaries and then refold the entire prefix.
    const std::vector<std::size_t> indices{8192, 4095, 0, 4096, 1, 16384};
    std::vector<std::uint8_t> saved;
    for (const auto index : indices) saved.push_back(old[index]);
    for (std::size_t i = 0; i < saved.size(); ++i) {
        old[i] = saved[i];
        actual.set(i, saved[i]);
        fixture.set(i, saved[i]);
    }
    require(actual.disk_bytes() == bytes, "rewrites must not append phase history");
    for (std::size_t i = old.size(); i > 0; --i)
        require(actual[i - 1] == old[i - 1] && fixture[i - 1] == old[i - 1],
                "permuted historical replay matches the complete old vector");
    storage::ExitPhaseStore moved(std::move(actual));
    require(actual.empty() && actual.disk_bytes() == 0, "move resets source ownership");
    require(moved.size() == old.size() && moved[0] == old[0], "move keeps historical phases");
    actual = std::move(moved);
    require(moved.empty() && actual[8192] == old[8192], "move assignment keeps byte authority");
    actual.resize(7, 0);
    old.resize(7);
    actual.resize(12000, 4);
    old.resize(12000, 4);
    for (std::size_t i = 0; i < old.size(); ++i)
        require(actual[i] == old[i], "shrink/regrow fills like the old vector");
    actual.clear();
    fixture.clear();
    require(actual.empty() && actual.disk_bytes() == 0 && fixture.empty(), "clear resets all phases");
    actual.resize(1, 5);
    fixture.resize(1, 5);
    require(actual[0] == 5 && fixture[0] == 5 && fixture.disk_bytes() == 0,
            "reset preserves directory/backend and starts a fresh phase sequence");
}

void phase_heap_is_flat() {
    Directory directory;
    storage::ExitPhaseStore phases;
    phases.set_directory(directory.path);
    Recording recording;
    phases.resize(10, 0);
    phases.set(0, 1);
    require(phases[0] == 1, "prime real page read before measuring");
    const auto low = global_allocation::live_bytes;
    phases.resize(10000, 0);
    phases.set(9999, 5);
    for (std::size_t i = 0; i < phases.size(); ++i) (void)phases[i];
    const auto high = global_allocation::live_bytes;
    require(high == low, "phase store has no per-trade retained heap");
}

int descriptor(std::uint64_t bytes) {
    const long limit = ::sysconf(_SC_OPEN_MAX);
    const long end = limit > 0 ? std::min(limit, 65536L) : 1024;
    for (int fd = 3; fd < end; ++fd) {
        struct stat status{};
        if (::fstat(fd, &status) == 0 && S_ISREG(status.st_mode)
            && status.st_nlink == 0 && status.st_size >= 0
            && static_cast<std::uint64_t>(status.st_size) == bytes) return fd;
    }
    throw std::runtime_error("find test-owned anonymous phase file");
}

template<class F>
void io_failure(F&& action) {
    bool failed = false;
    try { action(); }
    catch (const std::system_error& error) {
        failed = pineforge::classify_run_failure(error).code
            == pineforge::RunFailureCode::report_sink_failed;
    }
    require(failed, "scratch error must preserve report_sink_failed");
}

void scratch_ownership_and_failures() {
    Directory directory;
    storage::ExitPhaseStore phases;
    phases.set_directory(directory.path + "/missing");
    io_failure([&] { phases.resize(1, 0); });
    require(phases.empty(), "failed open did not publish a phase");
    phases.set_directory(directory.path);
    phases.resize(10003, 3);
    const int fd = descriptor(phases.disk_bytes());
    struct stat status{};
    require(::fstat(fd, &status) == 0 && (status.st_mode & 0777) == 0600,
            "phase scratch permissions are private");
    require((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "phase scratch closes on exec");
    require(::lseek(fd, 7, SEEK_SET) == 7 && phases[0] == 3, "positional phase read");
    phases.set(0, 4);
    require(::lseek(fd, 0, SEEK_CUR) == 7, "phase IO must not share a file cursor");
    bool locked = false;
    try { phases.set_directory(directory.path + "/other"); }
    catch (const std::logic_error&) { locked = true; }
    require(locked, "directory cannot change after opening");
    // Truncate beneath an uncached read. EOF is a run failure, never None.
    require(::ftruncate(fd, 0) == 0, "inject short pread");
    io_failure([&] { (void)phases[9000]; });
    phases.clear();
    require(::fcntl(fd, F_GETFD) == -1 && errno == EBADF, "clear closes phase fd");
    phases.resize(10007, 2);
    const int replacement = descriptor(phases.disk_bytes());
    const std::string path = "/dev/fd/" + std::to_string(replacement);
    const int read_only = ::open(path.c_str(), O_RDONLY);
    require(read_only >= 0, "open test phase file read only");
    const int replaced = ::dup2(read_only, replacement);
    ::close(read_only);
    require(replaced == replacement, "inject failing pwrite into owned phase fd");
    io_failure([&] { phases.set(0, 5); });
    io_failure([&] { phases.resize(10008, 0); });
    require(phases.size() == 10007 && phases[0] == 2,
            "failed extension publishes neither size nor a new historical value");
}

struct FileLimit {
    struct rlimit previous{};
    using Handler = void (*)(int);
    Handler handler = SIG_ERR;
    FileLimit() {
        require(::getrlimit(RLIMIT_FSIZE, &previous) == 0, "read file-size limit");
        handler = ::signal(SIGXFSZ, SIG_IGN);
        require(handler != SIG_ERR, "ignore file-limit signal during injection");
    }
    ~FileLimit() {
        ::setrlimit(RLIMIT_FSIZE, &previous);
        if (handler != SIG_ERR) ::signal(SIGXFSZ, handler);
    }
    void set(rlim_t bytes) const {
        auto limit = previous;
        limit.rlim_cur = bytes;
        require(::setrlimit(RLIMIT_FSIZE, &limit) == 0, "set file-size limit");
    }
};

void partial_extension_does_not_publish() {
    Directory directory;
    storage::ExitPhaseStore phases;
    phases.set_directory(directory.path);
    phases.resize(17, 2);
    {
        FileLimit limit;
        limit.set(std::min<rlim_t>(limit.previous.rlim_cur, 4126));
        io_failure([&] { phases.resize(9000, 5); }); // crosses a complete page
        require(phases.size() == 17 && phases[16] == 2,
                "partial extension leaves old logical bytes and count intact");
    }
    phases.resize(9000, 4);
    require(phases[16] == 2 && phases[17] == 4 && phases[8999] == 4,
            "retry overwrites the entire unpublished extension");
    io_failure([&] { phases.resize(std::numeric_limits<std::size_t>::max(), 0); });
}
} // namespace

int main() {
    try {
        exposure_lifecycles_match_old_containers();
        closed_id_heap_is_flat();
        phases_match_old_vector();
        phase_heap_is_flat();
        scratch_ownership_and_failures();
        partial_extension_does_not_publish();
        std::puts("test_adapter_growth_storage: old containers, heap bounds and IO failures agree");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_adapter_growth_storage: %s\n", error.what());
        return 1;
    }
}
