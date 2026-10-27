// LAB-o1bk: links the production factory from pineforge, with no fixture lease
// or local factory definition. Fail-before and execution belong to the remote
// lab; this edit-only lane has not compiled or run this translation unit.
#include "../src/source/pine_command_scratch_production.hpp"
#include "../src/command_scratch_cache.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
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
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(PINEFORGE_O1BK_WRAP_PWRITE)
namespace {
struct PwriteFault {
    int fd;
    int error;
    std::size_t calls = 0;
    std::size_t requested = 0;
    off_t offset = -1;
};
PwriteFault* selected_pwrite_fault = nullptr;
} // namespace

extern "C" ssize_t __real_pwrite(int, const void*, std::size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, std::size_t size, off_t offset) {
    if (selected_pwrite_fault && selected_pwrite_fault->fd == fd) {
        auto& fault = *selected_pwrite_fault;
        ++fault.calls;
        fault.requested = size;
        fault.offset = offset;
        errno = fault.error;
        return -1;
    }
    return __real_pwrite(fd, data, size, offset);
}
#endif

namespace {
using namespace pineforge::detail;
using pineforge::source::detail::make_command_scratch_backing;
using pineforge::source::detail::set_command_scratch_byte_cap;
using pineforge::source::detail::set_command_scratch_io_observer;
using ByteCap = decltype(set_command_scratch_byte_cap(std::uint64_t{}));

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

class ByteCapRegistration {
public:
    explicit ByteCapRegistration(std::uint64_t bytes)
        : previous_(set_command_scratch_byte_cap(bytes)) {}
    explicit ByteCapRegistration(ByteCap registration) noexcept
        : previous_(set_command_scratch_byte_cap(std::move(registration))) {}
    ~ByteCapRegistration() { set_command_scratch_byte_cap(std::move(previous_)); }
    ByteCapRegistration(const ByteCapRegistration&) = delete;
    ByteCapRegistration& operator=(const ByteCapRegistration&) = delete;

private:
    ByteCap previous_;
};

class ByteCapEnvironment {
public:
    explicit ByteCapEnvironment(const char* value) {
        const char* previous = std::getenv("PINEFORGE_COMMAND_SCRATCH_MAX_BYTES");
        had_previous_ = previous != nullptr;
        if (previous) previous_ = previous;
        const int result = value ? ::setenv("PINEFORGE_COMMAND_SCRATCH_MAX_BYTES", value, 1)
                                 : ::unsetenv("PINEFORGE_COMMAND_SCRATCH_MAX_BYTES");
        require(result == 0, "select scratch byte cap environment");
    }
    ~ByteCapEnvironment() {
        if (had_previous_) ::setenv("PINEFORGE_COMMAND_SCRATCH_MAX_BYTES", previous_.c_str(), 1);
        else ::unsetenv("PINEFORGE_COMMAND_SCRATCH_MAX_BYTES");
    }
    ByteCapEnvironment(const ByteCapEnvironment&) = delete;
    ByteCapEnvironment& operator=(const ByteCapEnvironment&) = delete;

private:
    bool had_previous_ = false;
    std::string previous_;
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

void require_extent(const ScratchBacking& backing, int fd, std::uint64_t pages) {
    struct stat file{};
    require(backing.page_count() == pages && ::fstat(fd, &file) == 0
            && file.st_size == static_cast<off_t>(pages * backing.page_size()),
            "reported extent and OS file size must agree with the charged extent");
}

void sparse_extent_and_durability(std::size_t page_size) {
    for (const std::size_t multiple : {std::size_t{1}, std::size_t{3}}) {
        const auto page = page_size * multiple;
        const auto before = open_descriptors();
        auto backing = make_command_scratch_backing(page, 0);
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

void byte_cap_exact_extent_and_charges(std::size_t page) {
    Charges charges;
    ObserverRegistration observer(charges.observer());
    constexpr std::uint64_t pages = 3;
    ByteCapRegistration cap(pages * page);
    const auto before = open_descriptors();
    auto backing = make_command_scratch_backing(page, 0);
    const int fd = created_descriptor(before);
    const auto image = pattern(page, 9), replacement = pattern(page, 10);
    std::vector<std::byte> out(page);
    for (ScratchPageId id = 0; id < pages; ++id) {
        require_status(backing->write_page(id, bytes(image)), ScratchStatus::ok,
                       "exact N-page cap permits each of N pages");
        require_charge(charges, static_cast<std::size_t>(id), ScratchIoOperation::write_page,
                       id, page, ScratchStatus::ok);
        require_extent(*backing, fd, id + 1);
    }
    require_status(backing->write_page(pages, bytes(replacement)), ScratchStatus::io_error,
                   "N+1 page is a typed scratch I/O failure");
    require_charge(charges, 3, ScratchIoOperation::write_page, pages, page, ScratchStatus::io_error);
    require_extent(*backing, fd, pages);
    require_status(backing->read_page(pages, buffer(out)), ScratchStatus::not_found,
                   "refused extension exposes no extra page");
    require_charge(charges, 4, ScratchIoOperation::read_page, pages, page, ScratchStatus::not_found);
    require_status(backing->read_page(0, buffer(out)), ScratchStatus::ok, "read below full cap");
    require_charge(charges, 5, ScratchIoOperation::read_page, 0, page, ScratchStatus::ok);
    require(out == image, "refusal preserves existing page bytes");
    require_status(backing->write_page(0, bytes(replacement)), ScratchStatus::ok,
                   "overwrite at full cap consumes no extra budget");
    require_charge(charges, 6, ScratchIoOperation::write_page, 0, page, ScratchStatus::ok);
    require_status(backing->read_page(0, buffer(out)), ScratchStatus::ok, "read capped overwrite");
    require_charge(charges, 7, ScratchIoOperation::read_page, 0, page, ScratchStatus::ok);
    require(out == replacement, "overwrite at full cap stores every requested byte");
    require_extent(*backing, fd, pages);
    backing.reset();
    auto later = make_command_scratch_backing(page);
    require_status(later->write_page(pages - 1, bytes(image)), ScratchStatus::ok,
                   "refused writes and overwrites leave no extra charge after destruction");
    require_charge(charges, 8, ScratchIoOperation::write_page, pages - 1, page, ScratchStatus::ok);
    require(later->page_count() == pages, "later backing can consume the complete released extent");
}

void byte_cap_shared_sparse_extents(std::size_t page) {
    ByteCapRegistration cap(std::uint64_t{5} * page);
    const auto before_a = open_descriptors();
    auto a = make_command_scratch_backing(page, 0);
    const int a_fd = created_descriptor(before_a);
    const auto before_b = open_descriptors();
    auto b = make_command_scratch_backing(2 * page, 0);
    const int b_fd = created_descriptor(before_b);
    const auto first = pattern(page, 11), second = pattern(2 * page, 12);
    require_status(a->write_page(2, bytes(first)), ScratchStatus::ok,
                   "sparse first backing holds three pages of the shared five-page byte cap");
    require_extent(*a, a_fd, 3);
    require_status(b->write_page(1, bytes(second)), ScratchStatus::io_error,
                   "second backing cannot exceed remaining bytes with its larger pages");
    require_extent(*b, b_fd, 0);
    require_status(b->write_page(0, bytes(second)), ScratchStatus::ok,
                   "refusal leaves exactly two base pages of budget for a smaller extension");
    require_extent(*b, b_fd, 1);
    require_status(a->write_page(3, bytes(first)), ScratchStatus::io_error,
                   "both live extents together exhaust the byte cap");
    require_extent(*a, a_fd, 3);
    std::vector<std::byte> out(2 * page);
    require_status(b->read_page(0, buffer(out)), ScratchStatus::ok, "read larger capped page");
    require(out == second, "shared byte accounting preserves differently sized page payloads");
}

void byte_cap_destruction_releases(std::size_t page) {
    ByteCapRegistration cap(std::uint64_t{3} * page);
    const auto image = pattern(page, 13);
    auto a = make_command_scratch_backing(page);
    auto b = make_command_scratch_backing(page);
    require_status(a->write_page(1, bytes(image)), ScratchStatus::ok, "first extent holds two pages");
    require_status(b->write_page(0, bytes(image)), ScratchStatus::ok, "second extent holds one page");
    require_status(b->write_page(1, bytes(image)), ScratchStatus::io_error, "live files exhaust cap");
    require(b->page_count() == 1, "full-budget refusal preserves surviving extent");
    a.reset();
    auto later = make_command_scratch_backing(page);
    require_status(later->write_page(1, bytes(image)), ScratchStatus::ok,
                   "destroyed backing releases its whole sparse extent for a later backing");
    require(later->page_count() == 2, "later backing acquires exactly the released two pages");
    require_status(b->write_page(1, bytes(image)), ScratchStatus::io_error,
                   "later backing's live bytes still count alongside the survivor");
    later.reset();
    require_status(b->write_page(2, bytes(image)), ScratchStatus::ok,
                   "second destruction releases enough bytes to extend the surviving backing");
    require(b->page_count() == 3, "survivor may consume the complete run cap after releases");
    std::vector<std::byte> out(page);
    require_status(b->read_page(2, buffer(out)), ScratchStatus::ok, "read survivor after budget reuse");
    require(out == image, "reused budget still writes the complete page");
}

void byte_cap_registration_snapshots(std::size_t page) {
    const auto image = pattern(page, 14);
    std::unique_ptr<ScratchBacking> a, b, restored;
    {
        ByteCapRegistration outer(std::uint64_t{2} * page);
        a = make_command_scratch_backing(page);
        require_status(a->write_page(0, bytes(image)), ScratchStatus::ok, "seed outer run");
        {
            ByteCapRegistration inner(std::uint64_t{2} * page);
            b = make_command_scratch_backing(page);
            require_status(b->write_page(1, bytes(image)), ScratchStatus::ok,
                           "same numeric cap starts an independent nested run");
            require_status(a->write_page(1, bytes(image)), ScratchStatus::ok,
                           "live backing retains the outer budget during nested registration");
            require_status(b->write_page(2, bytes(image)), ScratchStatus::io_error,
                           "nested backing retains its own full budget");
        }
        restored = make_command_scratch_backing(page);
        require_status(restored->write_page(0, bytes(image)), ScratchStatus::io_error,
                       "restored registration shares the outer run's existing live bytes");
        a.reset();
        require_status(restored->write_page(1, bytes(image)), ScratchStatus::ok,
                       "restored registration sees released outer bytes");
    }
    auto unregistered = make_command_scratch_backing(page);
    require_status(unregistered->write_page(7, bytes(image)), ScratchStatus::ok,
                   "leaving nested owners restores the unset registration");
    require_status(restored->write_page(2, bytes(image)), ScratchStatus::io_error,
                   "outer backing keeps its budget alive after its run owner leaves");
    require_status(b->write_page(2, bytes(image)), ScratchStatus::io_error,
                   "inner backing keeps its budget alive after its run owner leaves");
    require(restored->page_count() == 2 && b->page_count() == 2,
            "registration changes cannot retarget or extend capped live backings");
}

void byte_cap_unlimited(std::size_t page) {
    const auto image = pattern(page, 15);
    const auto exercise = [&] {
        auto a = make_command_scratch_backing(page);
        auto b = make_command_scratch_backing(page);
        require_status(a->write_page(4095, bytes(image)), ScratchStatus::ok,
                       "uncapped first backing retains the previous sparse extent range");
        require_status(b->write_page(8191, bytes(image)), ScratchStatus::ok,
                       "uncapped second backing has no shared byte ceiling");
        require(a->page_count() == 4096 && b->page_count() == 8192,
                "no-cap writes preserve the complete requested extents");
        std::vector<std::byte> out(page);
        require_status(b->read_page(8191, buffer(out)), ScratchStatus::ok, "read uncapped high page");
        require(out == image, "uncapped high page round trips without truncation");
    };
    {
        ByteCapRegistration unset(ByteCap{});
        exercise();
    }
    {
        ByteCapRegistration zero(std::uint64_t{0});
        exercise();
    }
}

void byte_cap_environment_default(std::size_t page) {
    ByteCapRegistration unset(ByteCap{});
    const auto selected = std::to_string(std::uint64_t{3} * page);
    ByteCapEnvironment environment(selected.c_str());
    const auto image = pattern(page, 16);
    auto a = make_command_scratch_backing(page);
    const auto before_b = open_descriptors();
    auto b = make_command_scratch_backing(page, 0);
    const int b_fd = created_descriptor(before_b);
    require_status(a->write_page(1, bytes(image)), ScratchStatus::ok,
                   "environment cap permits a two-page sparse extent");
    require_status(b->write_page(1, bytes(image)), ScratchStatus::io_error,
                   "environment default shares one byte cap across live backings");
    require_extent(*b, b_fd, 0);
    require_status(b->write_page(0, bytes(image)), ScratchStatus::ok,
                   "environment refusal preserves remaining budget");
    a.reset();
    require_status(b->write_page(2, bytes(image)), ScratchStatus::ok,
                   "environment budget releases destroyed extents");
    require_extent(*b, b_fd, 3);
    {
        ByteCapRegistration zero(std::uint64_t{0});
        auto unlimited = make_command_scratch_backing(page);
        require_status(unlimited->write_page(7, bytes(image)), ScratchStatus::ok,
                       "explicit zero selects no cap even with an environment default");
        require(unlimited->page_count() == 8, "explicit zero does not clamp the requested extent");
    }
    {
        ByteCapRegistration explicit_cap(static_cast<std::uint64_t>(page));
        auto explicit_backing = make_command_scratch_backing(page);
        require_status(explicit_backing->write_page(0, bytes(image)), ScratchStatus::ok,
                       "explicit run cap overrides the environment default");
        require_status(explicit_backing->write_page(1, bytes(image)), ScratchStatus::io_error,
                       "explicit cap remains smaller than the environment default");
    }
    {
        ByteCapEnvironment removed(nullptr);
        auto unlimited = make_command_scratch_backing(page);
        require_status(unlimited->write_page(7, bytes(image)), ScratchStatus::ok,
                       "unset environment and registration supply no default cap");
        require_status(b->write_page(3, bytes(image)), ScratchStatus::io_error,
                       "live backing retains its environment budget after the variable is removed");
        require_extent(*b, b_fd, 3);
    }
    auto later = make_command_scratch_backing(page);
    require_status(later->write_page(0, bytes(image)), ScratchStatus::io_error,
                   "restored environment default still shares the live default budget");
    b.reset();
    require_status(later->write_page(2, bytes(image)), ScratchStatus::ok,
                   "later default backing can reuse the complete released byte cap");
    require(later->page_count() == 3, "environment cap allows its exact extent after release");
}

void byte_cap_invalid_environment(std::size_t page) {
    const auto exercise = [&] {
        for (const char* invalid : {"", "0", "000", "-1", "+1", " 1", "1 ", "1x",
                                    "18446744073709551616"}) {
            ByteCapEnvironment environment(invalid);
            const auto before = open_descriptors();
            bool rejected = false;
            try {
                auto backing = make_command_scratch_backing(page);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected, "bad or non-positive environment cap must throw invalid_argument");
            require(open_descriptors() == before, "invalid cap must not retain a backing descriptor");
        }
    };
    {
        ByteCapRegistration unset(ByteCap{});
        exercise();
    }
    {
        ByteCapRegistration explicit_cap(static_cast<std::uint64_t>(page));
        exercise();
    }
    {
        ByteCapRegistration zero(std::uint64_t{0});
        exercise();
    }
}

void byte_cap_failed_extension_accounting(std::size_t page) {
    Charges charges;
    ObserverRegistration observer(charges.observer());
    ByteCapRegistration cap(std::uint64_t{2} * page);
    const auto before = open_descriptors();
    auto backing = make_command_scratch_backing(page, 0);
    const int fd = created_descriptor(before);
    const auto image = pattern(page, 17);
    require_status(backing->write_page(0, bytes(image)), ScratchStatus::ok, "seed capped syscall fault");
    require_charge(charges, 0, ScratchIoOperation::write_page, 0, page, ScratchStatus::ok);
    require(::close(fd) == 0, "force capped extension syscall failure");
    // No descriptor-creating calls before reset(): the failed close in the
    // backing destructor must not see a reused descriptor.
    require_status(backing->write_page(1, bytes(image)), ScratchStatus::io_error,
                   "ftruncate failure releases the reserved extension bytes");
    require_charge(charges, 1, ScratchIoOperation::write_page, 1, page, ScratchStatus::io_error);
    require(backing->page_count() == 1, "failed capped syscall preserves the prior extent");
    backing.reset();
    auto later = make_command_scratch_backing(page);
    require_status(later->write_page(1, bytes(image)), ScratchStatus::ok,
                   "failed syscall and destruction leave the full cap available");
    require_charge(charges, 2, ScratchIoOperation::write_page, 1, page, ScratchStatus::ok);
    require(later->page_count() == 2, "successful retry consumes exactly the full two-page cap");
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

void backing_errno_mapping() {
    struct Mapping {
        int error;
        ScratchStatus status;
        const char* name;
    };
    const std::array<Mapping, 5> mappings{{
        {ENOSPC, ScratchStatus::io_error, "ENOSPC is a backing I/O fault"},
        {EDQUOT, ScratchStatus::io_error, "EDQUOT is a backing I/O fault"},
        {EFBIG, ScratchStatus::io_error, "EFBIG is a backing I/O fault"},
        {EIO, ScratchStatus::io_error, "EIO is a backing I/O fault"},
        {EBADF, ScratchStatus::io_error, "unrelated errno remains a backing I/O fault"},
    }};
    for (const auto& mapping : mappings)
        require_status(pineforge::detail::command_scratch_io_status(mapping.error),
                       mapping.status, mapping.name);
}

void write_past_file_size_limit(std::size_t page) {
    struct sigaction disposition{};
    disposition.sa_handler = SIG_IGN;
    require(sigemptyset(&disposition.sa_mask) == 0
            && ::sigaction(SIGXFSZ, &disposition, nullptr) == 0,
            "child ignores SIGXFSZ so the real syscall can report EFBIG");
    struct rlimit limit{};
    require(::getrlimit(RLIMIT_FSIZE, &limit) == 0, "read child file-size limit");
    const auto cap = static_cast<rlim_t>(page);
    require(static_cast<std::uintmax_t>(cap) == static_cast<std::uintmax_t>(page)
            && (limit.rlim_max == RLIM_INFINITY || cap <= limit.rlim_max),
            "one page must fit the inherited hard file-size limit");
    limit.rlim_cur = cap;
    require(::setrlimit(RLIMIT_FSIZE, &limit) == 0
            && ::getrlimit(RLIMIT_FSIZE, &limit) == 0 && limit.rlim_cur == cap,
            "apply and verify the child's real one-page file-size cap");

    ByteCapRegistration uncapped(std::uint64_t{0});
    Charges charges;
    ObserverRegistration observer(charges.observer());
    const auto before = open_descriptors();
    auto backing = make_command_scratch_backing(page, 0);
    const int fd = created_descriptor(before);
    const auto image = pattern(page, 18);
    require_status(backing->write_page(0, bytes(image)), ScratchStatus::ok,
                   "first page fits the OS cap without a synthetic byte cap");
    require_charge(charges, 0, ScratchIoOperation::write_page, 0, page, ScratchStatus::ok);
    require_extent(*backing, fd, 1);
    const auto next_page_end = std::uintmax_t{2} * image.size();
    require(image.size() == page && next_page_end > static_cast<std::uintmax_t>(limit.rlim_cur),
            "page one ends at two pages, strictly beyond the applied one-page cap");
    errno = 0;
    const auto status = backing->write_page(1, bytes(image));
    const int error = errno;
    require(error == EFBIG, "production extension must hit the real OS EFBIG fault");
    require_status(status, ScratchStatus::io_error, "production EFBIG must return io_error");
    require_charge(charges, 1, ScratchIoOperation::write_page, 1, page, ScratchStatus::io_error);
    require_extent(*backing, fd, 1);
    std::vector<std::byte> out(page);
    require_status(backing->read_page(0, buffer(out)), ScratchStatus::ok,
                   "existing page remains readable after EFBIG");
    require(out == image, "EFBIG preserves the existing page bytes");
    require_status(backing->read_page(1, buffer(out)), ScratchStatus::not_found,
                   "failed OS extension cannot expose an unwritten page");
}

void file_size_limit_is_io_error(std::size_t page) {
    const pid_t child = ::fork();
    require(child >= 0, "fork a child for the file-size limit fault");
    if (child == 0) {
        try {
            write_past_file_size_limit(page);
            ::_exit(0);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL file_size_limit child page_size=%zu: %s\n", page, error.what());
            std::fflush(stderr);
            ::_exit(1);
        }
    }
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == child, "wait for the file-size limit child");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("file-size limit child failed: wait status " + std::to_string(status));
}

#if defined(PINEFORGE_O1BK_WRAP_PWRITE)
void production_pwrite_faults_are_io_errors(std::size_t page) {
    ByteCapRegistration uncapped(std::uint64_t{0});
    for (const int error : {ENOSPC, EIO}) {
        Charges charges;
        ObserverRegistration observer(charges.observer());
        const auto before = open_descriptors();
        auto backing = make_command_scratch_backing(page, 0);
        const int fd = created_descriptor(before);
        const auto image = pattern(page, 19), replacement = pattern(page, 20);
        require_status(backing->write_page(0, bytes(image)), ScratchStatus::ok,
                       "seed real production backing before interposing pwrite");
        require_charge(charges, 0, ScratchIoOperation::write_page, 0, page, ScratchStatus::ok);

        PwriteFault fault{fd, error};
        selected_pwrite_fault = &fault;
        errno = 0;
        const auto status = backing->write_page(0, bytes(replacement));
        const int observed_error = errno;
        // write_page is noexcept: disarm before any throwing assertion.
        selected_pwrite_fault = nullptr;
        require(fault.calls == 1 && fault.requested == page && fault.offset == 0,
                "production backing reached exactly one full-page positional write");
        require(observed_error == error, "interposed production pwrite raised the selected errno");
        require_status(status, ScratchStatus::io_error, "production ENOSPC/EIO must return io_error");
        require_charge(charges, 1, ScratchIoOperation::write_page, 0, page, ScratchStatus::io_error);
        require_extent(*backing, fd, 1);
        std::vector<std::byte> out(page);
        require_status(backing->read_page(0, buffer(out)), ScratchStatus::ok,
                       "read production backing after interposed fault");
        require(out == image, "failed pwrite preserves the prior page bytes");
        require_status(backing->write_page(0, bytes(replacement)), ScratchStatus::ok,
                       "disarmed pwrite returns to the real OS backing");
        require_status(backing->read_page(0, buffer(out)), ScratchStatus::ok,
                       "read production backing after disarming");
        require(out == replacement, "real pwrite stores the replacement after the injected fault");
    }
}
#endif

void short_reads_and_syscall_errors(std::size_t page) {
    Charges charges;
    ObserverRegistration registration(charges.observer());
    const auto before = open_descriptors();
    auto backing = make_command_scratch_backing(page);
    const int fd = created_descriptor(before);
    const auto image = pattern(page, 8);
    const auto conflicting_image = pattern(page, 18);
    require(page >= 512 && page <= CachedScratchBacking::kCacheBytes,
            "fault fixture must exercise a cached page geometry");
    const ScratchPageId conflicting_page = CachedScratchBacking::kCacheBytes / page;
    std::vector<std::byte> out(page);
    require_status(backing->write_page(0, bytes(image)), ScratchStatus::ok, "seed fault test");
    require_charge(charges, 0, ScratchIoOperation::write_page, 0, page, ScratchStatus::ok);
    // id == slot count maps to page zero's slot, including non-power-of-two
    // geometries. A real conflicting write makes each faulted read a miss.
    require_status(backing->write_page(conflicting_page, bytes(conflicting_image)),
                   ScratchStatus::ok, "evict page zero before short read");
    require_charge(charges, 1, ScratchIoOperation::write_page, conflicting_page, page, ScratchStatus::ok);
    require(::ftruncate(fd, static_cast<off_t>(page - 1)) == 0, "force a short OS read");
    require_status(backing->read_page(0, buffer(out)), ScratchStatus::io_error, "short read is an I/O error");
    require_charge(charges, 2, ScratchIoOperation::read_page, 0, page, ScratchStatus::io_error);
    require_status(backing->write_page(conflicting_page, bytes(conflicting_image)),
                   ScratchStatus::ok, "keep page zero evicted before EOF");
    require_charge(charges, 3, ScratchIoOperation::write_page, conflicting_page, page, ScratchStatus::ok);
    require(::ftruncate(fd, 0) == 0, "force EOF inside advertised extent");
    require_status(backing->read_page(0, buffer(out)), ScratchStatus::io_error, "early EOF is an I/O error");
    require_charge(charges, 4, ScratchIoOperation::read_page, 0, page, ScratchStatus::io_error);
    require_status(backing->write_page(0, bytes(image)), ScratchStatus::ok, "restore page after fault");
    require_charge(charges, 5, ScratchIoOperation::write_page, 0, page, ScratchStatus::ok);
    require_status(backing->write_page(conflicting_page, bytes(conflicting_image)),
                   ScratchStatus::ok, "evict page zero before descriptor fault");
    require_charge(charges, 6, ScratchIoOperation::write_page, conflicting_page, page, ScratchStatus::ok);
    require_extent(*backing, fd, conflicting_page + 1);

    // No descriptor-creating calls occur between this fault and reset(), so the
    // destructor cannot close a reused descriptor when it sees EBADF.
    require(::close(fd) == 0, "force invalid-descriptor syscall failures");
    require_status(backing->sync(ScratchDurability::buffered), ScratchStatus::ok, "buffered is a no-op");
    require_charge(charges, 7, ScratchIoOperation::sync, scratch_no_page, 0, ScratchStatus::ok);
    require_status(backing->sync(ScratchDurability::data), ScratchStatus::io_error, "fdatasync error surfaces");
    require_charge(charges, 8, ScratchIoOperation::sync, scratch_no_page, 0, ScratchStatus::io_error);
    require_status(backing->sync(ScratchDurability::full), ScratchStatus::io_error, "fsync error surfaces");
    require_charge(charges, 9, ScratchIoOperation::sync, scratch_no_page, 0, ScratchStatus::io_error);
    // Successful page bytes remain cached after an OS fault; only a miss
    // must reach the failed descriptor. Assert this for every tested geometry.
    std::fill(out.begin(), out.end(), std::byte{0x5a});
    require_status(backing->read_page(conflicting_page, buffer(out)), ScratchStatus::ok,
                   "cached page survives the closed descriptor");
    require_charge(charges, 10, ScratchIoOperation::read_page, conflicting_page, page, ScratchStatus::ok);
    require(out == conflicting_image, "cache hit after OS fault retains the complete successful write");
    require_status(backing->read_page(0, buffer(out)), ScratchStatus::io_error, "pread error surfaces");
    require_charge(charges, 11, ScratchIoOperation::read_page, 0, page, ScratchStatus::io_error);
    require_status(backing->write_page(0, bytes(image)), ScratchStatus::io_error, "pwrite error surfaces");
    require_charge(charges, 12, ScratchIoOperation::write_page, 0, page, ScratchStatus::io_error);
    require_status(backing->write_page(conflicting_page + 1, bytes(image)), ScratchStatus::io_error,
                   "ftruncate error surfaces");
    require_charge(charges, 13, ScratchIoOperation::write_page, conflicting_page + 1, page,
                   ScratchStatus::io_error);
    require(backing->page_count() == conflicting_page + 1, "failed extension cannot invent pages");
    backing.reset();
}

} // namespace

int main() {
    try {
        const long platform_page = ::sysconf(_SC_PAGESIZE);
        require(platform_page > 1, "query platform page size");
        TemporaryDirectory directory;
        DirectorySelection selection(directory.path);
        ByteCapEnvironment no_cap_environment(nullptr);
        const auto initial = set_command_scratch_io_observer({});
        require(initial.context == nullptr && initial.charge == nullptr, "observer registration defaults empty");
        const auto initial_cap = set_command_scratch_byte_cap(ByteCap{});
        require(!initial_cap, "byte cap registration defaults unset");
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
            run("byte_cap_exact_extent_and_charges", [&] { byte_cap_exact_extent_and_charges(page); });
            run("byte_cap_shared_sparse_extents", [&] { byte_cap_shared_sparse_extents(page); });
            run("byte_cap_destruction_releases", [&] { byte_cap_destruction_releases(page); });
            run("byte_cap_registration_snapshots", [&] { byte_cap_registration_snapshots(page); });
            run("byte_cap_unlimited", [&] { byte_cap_unlimited(page); });
            run("byte_cap_environment_default", [&] { byte_cap_environment_default(page); });
            run("byte_cap_invalid_environment", [&] { byte_cap_invalid_environment(page); });
            run("byte_cap_failed_extension_accounting", [&] { byte_cap_failed_extension_accounting(page); });
            run("selected_directory_failure", [&] { selected_directory_failure(page, directory.path); });
            run("backing_errno_mapping", [&] { backing_errno_mapping(); });
            run("file_size_limit_is_io_error", [&] { file_size_limit_is_io_error(page); });
#if defined(PINEFORGE_O1BK_WRAP_PWRITE)
            run("production_pwrite_faults_are_io_errors", [&] { production_pwrite_faults_are_io_errors(page); });
#endif
            run("short_reads_and_syscall_errors", [&] { short_reads_and_syscall_errors(page); });
        }
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL test_o1bk_production_backing setup: %s\n", error.what());
        return 1;
    }
}
