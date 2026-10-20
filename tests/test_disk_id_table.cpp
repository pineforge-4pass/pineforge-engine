// CR-150 L19l. The reference owns the old in-memory ordering/fold; no pins.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/detail/disk_id_table.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <fcntl.h>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/resource.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {
void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

using Table = pineforge::detail::DiskIdTable<std::int64_t>;

struct Directory {
    std::string path = "/tmp/pineforge-ids-test-XXXXXX";
    Directory() { require(::mkdtemp(path.data()) != nullptr, "create scratch directory"); }
    ~Directory() { ::rmdir(path.c_str()); }
};

std::uint64_t append(std::uint64_t h, const void* data, std::size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}

std::uint64_t fold(std::uint64_t h, const std::string& id, std::int64_t value) {
    const std::uint64_t n = id.size();
    h = append(h, &n, sizeof n);
    h = append(h, id.data(), id.size());
    return append(h, &value, sizeof value);
}

std::string id_for(std::size_t n) {
    // Include non-SSO strings and embedded NULs; the disk key is length based.
    return "entry-with-a-long-id/" + std::to_string(n) + std::string("\0tail", 5);
}

void insert_find_recur_and_folds() {
    Directory directory;
    Table table;
    table.set_directory(directory.path);
    std::map<std::string, std::int64_t> reference;
    std::vector<std::string> order;
    for (std::size_t i = 0; i < 10000; ++i) {
        const auto id = id_for((i * 7919) % 10000);
        const auto value = static_cast<std::int64_t>(i);
        const auto placed = table.insert(id, value);
        require(placed.second, "new id inserted");
        reference.emplace(id, value);
        order.push_back(id);
        if (i % 97 == 0) {
            require(!table.insert(id, -1).second, "recurring id is not inserted twice");
            require(table.get(placed.first) == value, "recurrence preserves old value");
        }
    }
    require(table.size() == reference.size(), "logical count equals reference");
    require(!table.find("absent"), "absent key stays absent");
    const auto bytes_before_updates = table.file_bytes();
    for (const auto& id : order) {
        const auto found = table.find(id);
        require(found.has_value(), "old id found after many insertions");
        table.set(*found, -reference.at(id) - 1);
        reference.at(id) = -reference.at(id) - 1;
    }
    require(table.file_bytes() == bytes_before_updates, "updates do not append history");
    std::uint64_t expected = 1469598103934665603ULL;
    for (const auto& row : reference) expected = fold(expected, row.first, row.second);
    std::uint64_t actual = 1469598103934665603ULL;
    std::size_t visited = 0;
    table.for_each_sorted([&](const auto& id, auto value) {
        actual = fold(actual, id, value);
        ++visited;
    });
    require(visited == reference.size() && actual == expected, "sorted fold is byte identical");
    expected = actual = 1469598103934665603ULL;
    for (const auto& id : order) expected = fold(expected, id, reference.at(id));
    table.for_each_inserted([&](const auto& id, auto value) {
        actual = fold(actual, id, value);
    });
    require(actual == expected, "first-insertion order and size survive recurrence");
    Table moved(std::move(table));
    require(moved.size() == reference.size() && table.size() == 0, "move transfers ownership");
    require(moved.get(*moved.find(order.front())) == reference.at(order.front()), "move keeps lookup");
    moved.clear();
    require(moved.size() == 0 && !moved.find(order.front()), "reset forgets old ids");
    require(moved.insert("fresh", 7).second, "reset can create a new file");
}

void flat_heap() {
    global_allocation::recording = true;
    {
        Table table;
        for (std::size_t i = 0; i < 10; ++i) table.insert(id_for(i), static_cast<std::int64_t>(i));
        const auto low = global_allocation::live_bytes;
        for (std::size_t i = 10; i < 10000; ++i) table.insert(id_for(i), static_cast<std::int64_t>(i));
        const auto high = global_allocation::live_bytes;
        require(high <= low + 1024, "disk id table retains per-id heap");
    }
    global_allocation::recording = false;
}

// Find this test's unlinked table by its exact length. No production fd seam.
int descriptor(std::uint64_t bytes) {
    const long limit = ::sysconf(_SC_OPEN_MAX);
    const long end = limit > 0 ? std::min(limit, 65536L) : 1024;
    for (int fd = 3; fd < end; ++fd) {
        struct stat status{};
        if (::fstat(fd, &status) == 0 && S_ISREG(status.st_mode) && status.st_nlink == 0
            && status.st_size >= 0 && static_cast<std::uint64_t>(status.st_size) == bytes) {
            return fd;
        }
    }
    throw std::runtime_error("find anonymous table descriptor");
}

struct FileLimit {
    struct rlimit previous{};
    using Handler = void (*)(int);
    Handler handler = SIG_ERR;
    FileLimit() {
        require(::getrlimit(RLIMIT_FSIZE, &previous) == 0, "read file-size limit");
        handler = ::signal(SIGXFSZ, SIG_IGN);
        require(handler != SIG_ERR, "ignore file-limit signal");
        auto limit = previous;
        limit.rlim_cur = 0;
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            ::signal(SIGXFSZ, handler);
            throw std::runtime_error("set file-size limit");
        }
    }
    ~FileLimit() {
        ::setrlimit(RLIMIT_FSIZE, &previous);
        ::signal(SIGXFSZ, handler);
    }
};

template<class F> void io_failure(F action) {
    bool failed = false;
    try { action(); }
    catch (const std::system_error& error) {
        failed = pineforge::classify_run_failure(error).code
            == pineforge::RunFailureCode::report_sink_failed;
    }
    require(failed, "I/O error must carry report_sink_failed");
}

void file_ownership_and_failures() {
    Directory directory;
    Table table;
    table.set_directory(directory.path + "/missing");
    io_failure([&] { table.insert("first", 1); });
    require(table.size() == 0, "open failure did not publish an id");
    table.set_directory(directory.path);
    const auto at = table.insert("first", 1).first;
    const int fd = descriptor(table.file_bytes());
    require((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "close on exec");
    require(::lseek(fd, 3, SEEK_SET) == 3, "set unrelated shared cursor");
    require(table.get(at) == 1 && table.find("first") == at, "read exact row");
    require(::lseek(fd, 0, SEEK_CUR) == 3, "reads use pread");
    // A zero file-size limit rejects pwrite on Linux and Darwin, including
    // overwrites in an existing file. Reads remain available for rollback.
    {
        FileLimit limit;
        io_failure([&] { table.set(at, 2); });
    }
    require(table.get(at) == 1, "failed write did not silently alter the value");
    table.clear();
    table.set_directory(directory.path);
    const auto other = table.insert("other", 9).first;
    const int truncated = descriptor(table.file_bytes());
    require(::ftruncate(truncated, 0) == 0, "truncate table to inject short pread");
    io_failure([&] { (void)table.get(other); });
}
} // namespace

int main() {
    try {
        insert_find_recur_and_folds();
        flat_heap();
        file_ownership_and_failures();
        std::puts("disk id table: OK");
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
