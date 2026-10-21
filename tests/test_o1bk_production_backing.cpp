// LAB-o1bk: links the production factory from pineforge, with no fixture lease
// or local factory definition. Fail-before and execution belong to the remote
// lab; this edit-only lane has not compiled or run this translation unit.
#include "../src/source/pine_command_scratch_production.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
using namespace pineforge::detail;
using pineforge::source::detail::make_command_scratch_backing;
using pineforge::source::detail::set_command_scratch_io_observer;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void require_status(ScratchStatus actual, ScratchStatus expected, const char* message) {
    if (actual != expected)
        throw std::runtime_error(std::string(message) + ": status "
            + std::to_string(static_cast<std::uint32_t>(actual)) + ", expected "
            + std::to_string(static_cast<std::uint32_t>(expected)));
}

struct CloseDirectory {
    void operator()(DIR* directory) const noexcept { ::closedir(directory); }
};
using Directory = std::unique_ptr<DIR, CloseDirectory>;

Directory open_directory(const char* path) {
    Directory result(::opendir(path));
    require(result != nullptr, "open test directory");
    return result;
}

void require_empty_directory(const std::string& path) {
    auto directory = open_directory(path.c_str());
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(directory.get());
        if (!entry) {
            require(errno == 0, "read scratch directory");
            return;
        }
        require(std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0,
                "scratch file must have no directory entry, even while live");
    }
}

struct TemporaryDirectory {
    std::string path = "./o1bk-scratch-XXXXXX";
    TemporaryDirectory() { require(::mkdtemp(path.data()) != nullptr, "create scratch test directory"); }
    ~TemporaryDirectory() { ::rmdir(path.c_str()); }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
};

class DirectorySelection {
public:
    explicit DirectorySelection(const std::string& path) {
        const char* previous = std::getenv("PINEFORGE_COMMAND_SCRATCH_DIR");
        had_previous_ = previous != nullptr;
        if (previous) previous_ = previous;
        require(::setenv("PINEFORGE_COMMAND_SCRATCH_DIR", path.c_str(), 1) == 0,
                "select scratch directory");
    }
    ~DirectorySelection() {
        if (had_previous_) ::setenv("PINEFORGE_COMMAND_SCRATCH_DIR", previous_.c_str(), 1);
        else ::unsetenv("PINEFORGE_COMMAND_SCRATCH_DIR");
    }
    DirectorySelection(const DirectorySelection&) = delete;
    DirectorySelection& operator=(const DirectorySelection&) = delete;

private:
    bool had_previous_ = false;
    std::string previous_;
};

class ObserverRegistration {
public:
    explicit ObserverRegistration(ScratchIoObserver observer) noexcept
        : previous_(set_command_scratch_io_observer(observer)) {}
    ~ObserverRegistration() { set_command_scratch_io_observer(previous_); }
    ObserverRegistration(const ObserverRegistration&) = delete;
    ObserverRegistration& operator=(const ObserverRegistration&) = delete;

private:
    ScratchIoObserver previous_;
};

struct Charges {
    std::array<ScratchIoCharge, 32> records{};
    std::size_t count = 0;
    bool overflow = false;
    static void receive(void* context, const ScratchIoCharge& record) noexcept {
        auto& self = *static_cast<Charges*>(context);
        if (self.count == self.records.size()) self.overflow = true;
        else self.records[self.count++] = record;
    }
    ScratchIoObserver observer() noexcept { return {this, &receive}; }
};

void require_charge(const Charges& charges, std::size_t index, ScratchIoOperation operation,
                    ScratchPageId page, std::size_t bytes, ScratchStatus status) {
    require(!charges.overflow && charges.count == index + 1, "exactly one charge per operation");
    const auto& record = charges.records[index];
    require(record.kind == ScratchBackingKind::disk && record.operation == operation
            && record.page == page && record.bytes == bytes && record.status == status,
            "charge preserves medium, operation, page, requested bytes and result");
}

std::vector<std::byte> pattern(std::size_t size, unsigned seed) {
    std::vector<std::byte> result(size);
    for (std::size_t i = 0; i < size; ++i)
        result[i] = static_cast<std::byte>((i * 13u + (i / 251u) * 17u + seed * 29u) & 0xffu);
    return result;
}

ScratchBytes bytes(const std::vector<std::byte>& value) { return {value.data(), value.size()}; }
ScratchBuffer buffer(std::vector<std::byte>& value) { return {value.data(), value.size()}; }

// /dev/fd is available on the Linux lab hosts (and macOS). Observe the public
// OS resource rather than adding a production descriptor/fault-injection API.
std::vector<int> open_descriptors() {
    auto directory = open_directory("/dev/fd");
    const int scanner = ::dirfd(directory.get());
    std::vector<int> result;
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(directory.get());
        if (!entry) {
            require(errno == 0, "enumerate descriptors");
            break;
        }
        char* end = nullptr;
        const long value = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end || value < 0
            || value > std::numeric_limits<int>::max() || value == scanner) continue;
        result.push_back(static_cast<int>(value));
    }
    std::sort(result.begin(), result.end());
    return result;
}

int created_descriptor(const std::vector<int>& before) {
    int created = -1;
    for (const int fd : open_descriptors()) {
        if (std::binary_search(before.begin(), before.end(), fd)) continue;
        require(created == -1, "one factory call must create exactly one descriptor");
        created = fd;
    }
    require(created >= 0, "production factory must create a real file descriptor");
    return created;
}

void sparse_extent_and_durability(std::size_t page_size) {
    for (const std::size_t multiple : {std::size_t{1}, std::size_t{3}}) {
        const auto page = page_size * multiple;
        const auto before = open_descriptors();
        auto backing = make_command_scratch_backing(page);
        const int fd = created_descriptor(before);
        require(backing && backing->kind() == ScratchBackingKind::disk,
                "factory must select the production disk backing");
        require(backing->page_size() == page && backing->page_count() == 0,
                "fresh file has requested geometry and an empty extent");
        const std::array<ScratchPageId, 3> ids{{0, 7, 4095}};
        const std::array<std::vector<std::byte>, 3> images{{
            pattern(page, 1), pattern(page, 2), pattern(page, 3)}};
        std::vector<std::byte> out(page);
        require_status(backing->read_page(0, buffer(out)), ScratchStatus::not_found,
                       "empty file has no page zero");
        for (std::size_t i = 0; i < ids.size(); ++i) {
            require_status(backing->write_page(ids[i], bytes(images[i])), ScratchStatus::ok,
                           "write page 0, 7 or 4095 beyond the prior extent");
            require(backing->page_count() == ids[i] + 1, "extent advances to written page plus one");
            struct stat file{};
            require(::fstat(fd, &file) == 0
                    && file.st_size == static_cast<off_t>(page * (ids[i] + 1)),
                    "OS file extent uses the requested page size without rounding");
        }
        for (const auto durability : {ScratchDurability::buffered, ScratchDurability::data,
                                      ScratchDurability::full}) {
            require_status(backing->sync(durability), ScratchStatus::ok, "file sync succeeds");
            for (std::size_t i = 0; i < ids.size(); ++i) {
                require_status(backing->read_page(ids[i], buffer(out)), ScratchStatus::ok,
                               "read page 0, 7 or 4095 after sync");
                require(out == images[i], "every written page round trips byte-exact after sync");
                std::fill(out.begin(), out.end(), std::byte{0x5a});
                require(::pread(fd, out.data(), page, static_cast<off_t>(page * ids[i]))
                            == static_cast<ssize_t>(page),
                        "OS read at the requested page offset returns a whole page");
                require(out == images[i], "OS file stores each page at its requested byte offset");
            }
        }
        for (const ScratchPageId hole : {ScratchPageId{1}, ScratchPageId{6},
                                        ScratchPageId{8}, ScratchPageId{4094}}) {
            std::fill(out.begin(), out.end(), std::byte{0x5a});
            require_status(backing->read_page(hole, buffer(out)), ScratchStatus::ok, "read sparse gap");
            require(std::all_of(out.begin(), out.end(), [](std::byte v) { return v == std::byte{0}; }),
                    "file extension zero-fills every gap byte");
        }
        require_status(backing->read_page(4096, buffer(out)), ScratchStatus::not_found,
                       "page 4096 is beyond the actual extent");
        require_status(backing->write_page(0, bytes(images[1])), ScratchStatus::ok, "overwrite low page");
        require(backing->page_count() == 4096, "overwrite never shrinks the extent");
        require_status(backing->read_page(0, buffer(out)), ScratchStatus::ok, "read overwritten page");
        require(out == images[1], "overwrite changes the stored bytes");
    }
}

void exclusive_backings(std::size_t page) {
    auto a = make_command_scratch_backing(page);
    auto b = make_command_scratch_backing(page);
    const auto first = pattern(page, 4), second = pattern(page, 5);
    std::vector<std::byte> out(page);
    const auto b_extent = b->page_count();
    require(b_extent == 0, "second factory call starts empty");
    require_status(a->write_page(3, bytes(first)), ScratchStatus::ok, "extend A");
    require(a->page_count() == 4 && b->page_count() == b_extent, "extending A cannot extend B");
    require_status(b->read_page(3, buffer(out)), ScratchStatus::not_found, "B cannot see A's page");
    require_status(b->write_page(3, bytes(second)), ScratchStatus::ok, "extend B independently");
    require_status(a->read_page(3, buffer(out)), ScratchStatus::ok, "A survives B's same-page write");
    require(out == first, "A and B never share file contents");
    a.reset();
    require_status(b->read_page(3, buffer(out)), ScratchStatus::ok, "B survives destruction of A");
    require(out == second, "B retains its exclusive bytes");
}

void anonymous_file_lifecycle(std::size_t page, const std::string& directory) {
    const auto before = open_descriptors();
    auto backing = make_command_scratch_backing(page);
    const int fd = created_descriptor(before);
    struct stat file{}, parent{};
    require(::fstat(fd, &file) == 0 && ::stat(directory.c_str(), &parent) == 0, "stat backing file");
    require(S_ISREG(file.st_mode) && file.st_nlink == 0 && file.st_size == 0
            && file.st_dev == parent.st_dev, "fresh backing is an unlinked regular file on selected medium");
    const int flags = ::fcntl(fd, F_GETFD);
    require(flags >= 0 && (flags & FD_CLOEXEC) != 0, "scratch descriptor is close-on-exec");
    require_empty_directory(directory);
    const auto image = pattern(page, 6);
    require_status(backing->write_page(4095, bytes(image)), ScratchStatus::ok, "extend anonymous file");
    require(::fstat(fd, &file) == 0 && file.st_size == static_cast<off_t>(page * 4096)
            && backing->page_count() == 4096, "reported extent agrees with the OS file size");
    require_empty_directory(directory);
    backing.reset();
    errno = 0;
    require(::fcntl(fd, F_GETFD) == -1 && errno == EBADF, "destruction closes the exclusive descriptor");
    require_empty_directory(directory);
}

void observer_registration_and_charges(std::size_t page) {
    Charges first, second;
    std::unique_ptr<ScratchBacking> a, b;
    const auto image = pattern(page, 7);
    std::vector<std::byte> out(page);
    {
        ObserverRegistration outer(first.observer());
        a = make_command_scratch_backing(page);
        require(first.count == 0, "factory creation is not a page operation");
        require_status(a->write_page(0, bytes(image)), ScratchStatus::ok, "observed write");
        require_charge(first, 0, ScratchIoOperation::write_page, 0, page, ScratchStatus::ok);
        require_status(a->read_page(0, buffer(out)), ScratchStatus::ok, "observed read");
        require(out == image, "observed read retains bytes");
        require_charge(first, 1, ScratchIoOperation::read_page, 0, page, ScratchStatus::ok);
        require_status(a->read_page(1, buffer(out)), ScratchStatus::not_found, "observed absent page");
        require_charge(first, 2, ScratchIoOperation::read_page, 1, page, ScratchStatus::not_found);
        require_status(a->write_page(0, {image.data(), page - 1}), ScratchStatus::programming_error,
                       "observed short source");
        require_charge(first, 3, ScratchIoOperation::write_page, 0, page - 1, ScratchStatus::programming_error);
        require_status(a->read_page(0, {out.data(), page - 1}), ScratchStatus::programming_error,
                       "observed short destination");
        require_charge(first, 4, ScratchIoOperation::read_page, 0, page - 1, ScratchStatus::programming_error);
        require_status(a->write_page(scratch_no_page, bytes(image)), ScratchStatus::programming_error,
                       "observed sentinel page");
        require_charge(first, 5, ScratchIoOperation::write_page, scratch_no_page, page,
                       ScratchStatus::programming_error);
        require_status(a->write_page(scratch_no_page - 1, bytes(image)), ScratchStatus::programming_error,
                       "unrepresentable file offset");
        require_charge(first, 6, ScratchIoOperation::write_page, scratch_no_page - 1, page,
                       ScratchStatus::programming_error);
        std::size_t index = 7;
        for (const auto durability : {ScratchDurability::buffered, ScratchDurability::data,
                                      ScratchDurability::full}) {
            require_status(a->sync(durability), ScratchStatus::ok, "observed sync");
            require_charge(first, index++, ScratchIoOperation::sync, scratch_no_page, 0, ScratchStatus::ok);
        }
        require_status(a->sync(static_cast<ScratchDurability>(99)), ScratchStatus::programming_error,
                       "observed invalid durability");
        require_charge(first, 10, ScratchIoOperation::sync, scratch_no_page, 0,
                       ScratchStatus::programming_error);
        {
            ObserverRegistration inner(second.observer());
            b = make_command_scratch_backing(page);
            require_status(b->read_page(0, buffer(out)), ScratchStatus::not_found, "inner observer");
            require_charge(second, 0, ScratchIoOperation::read_page, 0, page, ScratchStatus::not_found);
            require_status(a->read_page(0, buffer(out)), ScratchStatus::ok, "live A retains outer observer");
            require_charge(first, 11, ScratchIoOperation::read_page, 0, page, ScratchStatus::ok);
        }
        auto restored = make_command_scratch_backing(page);
        require_status(restored->write_page(3, bytes(image)), ScratchStatus::ok, "restored outer observer");
        require_charge(first, 12, ScratchIoOperation::write_page, 3, page, ScratchStatus::ok);
    }
    auto unobserved = make_command_scratch_backing(page);
    require_status(unobserved->read_page(0, buffer(out)), ScratchStatus::not_found, "unobserved absent page");
    require_status(unobserved->write_page(0, bytes(image)), ScratchStatus::ok, "unobserved write");
    require_status(unobserved->read_page(0, buffer(out)), ScratchStatus::ok, "unobserved read");
    for (const auto durability : {ScratchDurability::buffered, ScratchDurability::data, ScratchDurability::full})
        require_status(unobserved->sync(durability), ScratchStatus::ok, "unobserved sync");
    require(first.count == 13 && second.count == 1, "no records when no observer is installed");
    require_status(a->read_page(0, buffer(out)), ScratchStatus::ok, "observer snapshot outlives registration");
    require_charge(first, 13, ScratchIoOperation::read_page, 0, page, ScratchStatus::ok);
    require_status(b->write_page(0, bytes(image)), ScratchStatus::ok, "inner snapshot outlives registration");
    require_charge(second, 1, ScratchIoOperation::write_page, 0, page, ScratchStatus::ok);
}

void invalid_page_geometry() {
    const auto offset_limit = static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max());
    require(offset_limit < static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()),
            "test size_t must represent a page size above off_t max");
    for (const std::size_t invalid : {std::size_t{0}, static_cast<std::size_t>(offset_limit + 1)}) {
        bool rejected = false;
        try {
            auto backing = make_command_scratch_backing(invalid);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "zero or page size above off_t max must throw invalid_argument");
    }
}

void selected_directory_failure(std::size_t page, const std::string& directory) {
    DirectorySelection missing(directory + "/missing");
    bool rejected = false;
    try {
        auto backing = make_command_scratch_backing(page);
    } catch (const std::system_error& error) {
        rejected = error.code().value() == ENOENT;
    }
    require(rejected, "failed selected directory must surface errno instead of falling back");
}

void short_reads_and_syscall_errors(std::size_t page) {
    Charges charges;
    ObserverRegistration registration(charges.observer());
    const auto before = open_descriptors();
    auto backing = make_command_scratch_backing(page);
    const int fd = created_descriptor(before);
    const auto image = pattern(page, 8);
    std::vector<std::byte> out(page);
    require_status(backing->write_page(0, bytes(image)), ScratchStatus::ok, "seed fault test");
    require_charge(charges, 0, ScratchIoOperation::write_page, 0, page, ScratchStatus::ok);
    require(::ftruncate(fd, static_cast<off_t>(page - 1)) == 0, "force a short OS read");
    require_status(backing->read_page(0, buffer(out)), ScratchStatus::io_error, "short read is an I/O error");
    require_charge(charges, 1, ScratchIoOperation::read_page, 0, page, ScratchStatus::io_error);
    require(::ftruncate(fd, 0) == 0, "force EOF inside advertised extent");
    require_status(backing->read_page(0, buffer(out)), ScratchStatus::io_error, "early EOF is an I/O error");
    require_charge(charges, 2, ScratchIoOperation::read_page, 0, page, ScratchStatus::io_error);
    require_status(backing->write_page(0, bytes(image)), ScratchStatus::ok, "restore page after fault");
    require_charge(charges, 3, ScratchIoOperation::write_page, 0, page, ScratchStatus::ok);

    // No descriptor-creating calls occur between this fault and reset(), so the
    // destructor cannot close a reused descriptor when it sees EBADF.
    require(::close(fd) == 0, "force invalid-descriptor syscall failures");
    require_status(backing->sync(ScratchDurability::buffered), ScratchStatus::ok, "buffered is a no-op");
    require_charge(charges, 4, ScratchIoOperation::sync, scratch_no_page, 0, ScratchStatus::ok);
    require_status(backing->sync(ScratchDurability::data), ScratchStatus::io_error, "fdatasync error surfaces");
    require_charge(charges, 5, ScratchIoOperation::sync, scratch_no_page, 0, ScratchStatus::io_error);
    require_status(backing->sync(ScratchDurability::full), ScratchStatus::io_error, "fsync error surfaces");
    require_charge(charges, 6, ScratchIoOperation::sync, scratch_no_page, 0, ScratchStatus::io_error);
    require_status(backing->read_page(0, buffer(out)), ScratchStatus::io_error, "pread error surfaces");
    require_charge(charges, 7, ScratchIoOperation::read_page, 0, page, ScratchStatus::io_error);
    require_status(backing->write_page(0, bytes(image)), ScratchStatus::io_error, "pwrite error surfaces");
    require_charge(charges, 8, ScratchIoOperation::write_page, 0, page, ScratchStatus::io_error);
    require_status(backing->write_page(7, bytes(image)), ScratchStatus::io_error, "ftruncate error surfaces");
    require_charge(charges, 9, ScratchIoOperation::write_page, 7, page, ScratchStatus::io_error);
    require(backing->page_count() == 1, "failed extension cannot invent pages");
    backing.reset();
}

} // namespace

int main() {
    try {
        const long platform_page = ::sysconf(_SC_PAGESIZE);
        require(platform_page > 1, "query platform page size");
        TemporaryDirectory directory;
        DirectorySelection selection(directory.path);
        const auto initial = set_command_scratch_io_observer({});
        require(initial.context == nullptr && initial.charge == nullptr, "observer registration defaults empty");
        int failures = 0;
        for (const std::size_t page : {std::size_t{512}, static_cast<std::size_t>(platform_page),
                                      std::size_t{1536}}) {
            require(page <= std::numeric_limits<std::size_t>::max() / (3 * 4096),
                    "test geometry must fit size_t");
            const auto run = [&](const char* name, auto test) {
                try {
                    test();
                    require_empty_directory(directory.path);
                    std::printf("PASS %s page_size=%zu\n", name, page);
                } catch (const std::exception& error) {
                    std::fprintf(stderr, "FAIL %s page_size=%zu: %s\n", name, page, error.what());
                    ++failures;
                }
            };
            run("invalid_page_geometry", [&] { invalid_page_geometry(); });
            run("sparse_extent_and_durability", [&] { sparse_extent_and_durability(page); });
            run("exclusive_backings", [&] { exclusive_backings(page); });
            run("anonymous_file_lifecycle", [&] { anonymous_file_lifecycle(page, directory.path); });
            run("observer_registration_and_charges", [&] { observer_registration_and_charges(page); });
            run("selected_directory_failure", [&] { selected_directory_failure(page, directory.path); });
            run("short_reads_and_syscall_errors", [&] { short_reads_and_syscall_errors(page); });
        }
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL test_o1bk_production_backing setup: %s\n", error.what());
        return 1;
    }
}
