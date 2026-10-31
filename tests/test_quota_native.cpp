#include <pineforge/detail/disk_id_table.hpp>
#include <pineforge/detail/run_quota.hpp>
#include <pineforge/detail/scratch_file.hpp>
#include "../src/command_scratch_production.hpp"
#include "../src/command_scratch_cache.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace pineforge;
using namespace pineforge::detail;

namespace {

// Compute the pre-quota footprint from its field widths and alignment,
// including the private arena region's vector, extent, optional cap and label.
// This pins object size, not a count of allocations elsewhere in open().
struct FieldLayout {
    std::size_t bytes;
    std::size_t alignment;
};
template<class T> constexpr FieldLayout field_layout() {
    return {sizeof(T), alignof(T)};
}
template<std::size_t Count>
constexpr FieldLayout aggregate_layout(const std::array<FieldLayout, Count>& fields) {
    FieldLayout result{0, 1};
    for (const auto field : fields) {
        result.bytes += (field.alignment - result.bytes % field.alignment) % field.alignment;
        result.bytes += field.bytes;
        result.alignment = std::max(result.alignment, field.alignment);
    }
    result.bytes += (result.alignment - result.bytes % result.alignment) % result.alignment;
    return result;
}
constexpr auto unmanaged_region_layout = aggregate_layout(std::array<FieldLayout, 4>{{
    field_layout<std::vector<std::uint32_t>>(), field_layout<std::uint64_t>(),
    field_layout<std::optional<std::uint64_t>>(), field_layout<ScratchStore>()
}});
constexpr auto unmanaged_file_layout = aggregate_layout(std::array<FieldLayout, 10>{{
    field_layout<int>(), field_layout<ScratchFile::IoRaiser>(), field_layout<std::string>(),
    field_layout<std::shared_ptr<ScratchSlab>>(), field_layout<ScratchResource>(),
    field_layout<bool>(), field_layout<ScratchArena*>(), unmanaged_region_layout,
    field_layout<std::size_t>(), field_layout<std::size_t>()
}});
static_assert(sizeof(ScratchFile) == unmanaged_file_layout.bytes,
              "quota bookkeeping must fit the pre-quota unmanaged ScratchFile footprint");
static_assert(sizeof(std::unique_ptr<ScratchFileQuota>) == sizeof(void*),
              "per-file quota ownership must cost only one pointer");
static_assert(ScratchFile::kCacheBytes <= std::numeric_limits<std::uint32_t>::max(),
              "cache-window counters must represent the complete window");

std::uint64_t block_size(const char* directory) {
    struct statvfs filesystem{};
    assert(::statvfs(directory, &filesystem) == 0);
    const auto block = filesystem.f_bsize;
    assert(block != 0);
    return block;
}

struct stat file_stat(int fd) {
    struct stat result{};
    assert(::fstat(fd, &result) == 0 && result.st_size >= 0);
    return result;
}

void same_failure(const RunFailureValue& actual, const RunFailureValue& expected) {
    assert(actual.code == expected.code);
    assert(actual.args && expected.args && *actual.args == *expected.args);
}

void usage(RunQuota& quota, QuotaResource resource, std::uint64_t committed,
           std::uint64_t peak, std::uint64_t limit) {
    const auto snapshot = quota.snapshot(resource);
    assert(snapshot.failure.code == RunFailureCode::none);
    assert(snapshot.message[QuotaSlot::committed] == committed);
    assert(snapshot.message[QuotaSlot::reserved] == 0);
    assert(snapshot.message[QuotaSlot::used] == committed);
    assert(snapshot.message[QuotaSlot::peak] == peak);
    assert(snapshot.message[QuotaSlot::limit] == limit);
}

void refusal(RunQuota& quota, const RunFailureValue& caught, const char* resource,
             std::uint64_t used, std::uint64_t requested, std::uint64_t limit,
             std::uint64_t peak) {
    const auto expected = make_run_failure(RunFailureCode::run_resource_quota_exceeded,
        {{"stage", "execution"}, {"artifact", "table"}, {"resource", resource},
         {"used", used}, {"requested", requested}, {"limit", limit}, {"peak", peak}});
    assert(expected.code == RunFailureCode::run_resource_quota_exceeded);
    same_failure(caught, expected);
    const auto snapshot = quota.refusal();
    assert(snapshot);
    same_failure(snapshot->failure, expected);
    assert(snapshot->message[QuotaSlot::used] == used);
    assert(snapshot->message[QuotaSlot::requested] == requested);
    assert(snapshot->message[QuotaSlot::limit] == limit);
    assert(snapshot->message[QuotaSlot::peak] == peak);
    assert(snapshot->message[QuotaSlot::committed] == used);
    assert(snapshot->message[QuotaSlot::reserved] == 0);
}

void grow(ScratchFile& file, const std::string& operation, std::uint64_t end) {
    const unsigned char byte = 91;
    if (operation == "ensure") file.ensure_size(end);
    else if (operation == "truncate") file.truncate(end);
    else if (operation == "write") file.write(end - sizeof byte, &byte, sizeof byte);
    else {
        assert(operation == "write_some");
        assert(file.write_some(end - sizeof byte, &byte, sizeof byte) == sizeof byte);
    }
}

void two_files(const char* directory, const std::string& operation, bool managed,
               std::uint64_t limit) {
    auto quota = RunQuota::from_environment();
    const auto block = block_size(directory);
    const auto end = block + sizeof(unsigned char);
    const auto initial = sizeof(unsigned char);
    const auto rounded = [&](std::uint64_t size) { return quota_rounded_extent(size, block); };
    std::array<ScratchFile, 2> files;
    for (auto& file : files) {
        file.set_directory(directory);
        file.open("quota-table");
    }
    files.front().ensure_size(end);
    grow(files.back(), "write", initial);
    const auto before = file_stat(files.back().descriptor());
    const auto used = rounded(end) + rounded(initial);
    assert(rounded(end) <= limit && used == limit);
    if (managed) {
        bool caught = false;
        try { grow(files.back(), operation, end); }
        catch (const coded<std::runtime_error>& error) {
            caught = true;
            refusal(quota, error.run_failure(), "scratch_bytes", used,
                    rounded(end) - rounded(initial), limit, used);
        }
        assert(caught);
        usage(quota, QuotaResource::scratch_files, files.size(), files.size(), files.size());
        const auto after = file_stat(files.back().descriptor());
        assert(after.st_size == before.st_size);
        assert(after.st_dev == before.st_dev && after.st_ino == before.st_ino);
    } else {
        grow(files.back(), operation, end);
        assert(file_stat(files.back().descriptor()).st_size == static_cast<off_t>(end));
        assert(!quota.refusal());
    }
}

void create_refusal(const char* directory, std::uint64_t file_limit, bool command) {
    auto quota = RunQuota::from_environment();
    ScratchFile first;
    first.set_directory(directory);
    first.open("quota-first");
    const auto before = file_stat(first.descriptor());
    const std::uint64_t requested_files = 1;
    bool caught = false;
    try {
        if (command) {
            auto second = make_command_scratch_backing(512);
            (void)second;
        } else {
            ScratchFile second;
            second.set_directory(directory);
            second.open("quota-second");
        }
    } catch (const coded<std::runtime_error>& error) {
        caught = true;
        refusal(quota, error.run_failure(), "scratch_files", file_limit,
                requested_files, file_limit, file_limit);
    }
    assert(caught && file_stat(first.descriptor()).st_size == before.st_size);
}

void sparse_and_lifetime(const char* directory, std::uint64_t limit) {
    auto quota = RunQuota::from_environment();
    const auto block = block_size(directory);
    const auto end = 9 * block + sizeof(unsigned char);
    const auto charge = quota_rounded_extent(end, block);
    ScratchFile file;
    file.set_directory(directory);
    file.open("quota-sparse");
    const int fd = file.descriptor();
    assert(file_stat(fd).st_nlink == 0);
    file.ensure_size(end);
    assert(file_stat(fd).st_size == static_cast<off_t>(end));
    for (const auto offset : {std::uint64_t{0}, end / 2, end - 1}) {
        unsigned char byte = 255;
        file.read(offset, &byte, sizeof byte);
        assert(byte == 0);
    }
    usage(quota, QuotaResource::scratch_bytes, charge, charge, limit);
    file.ensure_size(end / 2); // A monotone sizing no-op cannot refund storage.
    usage(quota, QuotaResource::scratch_bytes, charge, charge, limit);
    const auto kept = block - 1;
    file.truncate(kept);
    assert(file_stat(fd).st_size == static_cast<off_t>(kept));
    const auto remaining = quota_rounded_extent(kept, block);
    usage(quota, QuotaResource::scratch_bytes, remaining, charge, limit);
    const int alias = ::dup(fd);
    assert(alias >= 0);
    ScratchFile moved(std::move(file));
    assert(!file.opened());
    moved.clear();
    assert(file_stat(alias).st_size == static_cast<off_t>(kept));
    usage(quota, QuotaResource::scratch_bytes, remaining, charge, limit);
    usage(quota, QuotaResource::scratch_files, 1, 1, 1);
    assert(::close(alias) == 0);
    // Descriptor escape leaves last-alias proof/refund with the host lease.
    usage(quota, QuotaResource::scratch_bytes, remaining, charge, limit);
}

void moved_lifetime(const char* directory, std::uint64_t limit, std::uint64_t file_limit) {
    auto quota = RunQuota::from_environment();
    const auto block = block_size(directory);
    const auto end = block + sizeof(unsigned char);
    const auto charge = quota_rounded_extent(end, block);
    const auto replaced_charge = quota_rounded_extent(sizeof(unsigned char), block);
    const auto peak = charge + replaced_charge;
    assert(limit == peak && file_limit == 2);
    {
        ScratchFile assigned;
        assigned.set_directory(directory);
        assigned.open("quota-replaced");
        assigned.ensure_size(sizeof(unsigned char));
        {
            ScratchFile file;
            file.set_directory(directory);
            file.open("quota-move");
            file.ensure_size(end);
            usage(quota, QuotaResource::scratch_bytes, peak, peak, limit);
            usage(quota, QuotaResource::scratch_files, 2, 2, file_limit);

            ScratchFile moved(std::move(file));
            assert(!file.opened() && moved.opened());
            file.clear();
            usage(quota, QuotaResource::scratch_bytes, peak, peak, limit);
            usage(quota, QuotaResource::scratch_files, 2, 2, file_limit);

            // Assignment must retire its old backing and transfer the new
            // backing's sole reservation owner. Never borrow either fd.
            assigned = std::move(moved);
            assert(!moved.opened() && assigned.opened());
            moved.clear();
            usage(quota, QuotaResource::scratch_bytes, charge, peak, limit);
            usage(quota, QuotaResource::scratch_files, 1, 2, file_limit);
        }
        // Both moved-from objects are now destroyed; the transferred charge
        // must remain until the sole live owner closes it.
        usage(quota, QuotaResource::scratch_bytes, charge, peak, limit);
        usage(quota, QuotaResource::scratch_files, 1, 2, file_limit);
        assigned.clear();
        usage(quota, QuotaResource::scratch_bytes, 0, peak, limit);
        usage(quota, QuotaResource::scratch_files, 0, 2, file_limit);
        assigned.clear();
        usage(quota, QuotaResource::scratch_bytes, 0, peak, limit);
        usage(quota, QuotaResource::scratch_files, 0, 2, file_limit);

        // Reopening proves that close returned reusable capacity; scope
        // destruction must also release exactly one file and its bytes.
        assigned.open("quota-reopened");
        assigned.ensure_size(end);
        usage(quota, QuotaResource::scratch_bytes, charge, peak, limit);
        usage(quota, QuotaResource::scratch_files, 1, 2, file_limit);
    }
    usage(quota, QuotaResource::scratch_bytes, 0, peak, limit);
    usage(quota, QuotaResource::scratch_files, 0, 2, file_limit);
}

void id_table_refund(const char* directory) {
    assert(mode() == QuotaMode::managed && ScratchArena::attached() == nullptr);
    auto quota = RunQuota::from_environment();
    usage(quota, QuotaResource::scratch_files, 0, 0, 1);
    const auto before = quota.snapshot(QuotaResource::scratch_bytes);
    assert(before.failure.code == RunFailureCode::none);
    assert(before.message[QuotaSlot::committed] == 0);
    assert(before.message[QuotaSlot::reserved] == 0);
    assert(before.message[QuotaSlot::used] == 0);

    DiskIdTable<std::uint64_t> table(4096);
    table.set_directory(directory);
    const auto inserted = table.insert("row", 37);
    assert(inserted.first == 1 && inserted.second);
    assert(!table.arena_backed() && table.size() == 1 && table.file_bytes() == 68);
    const auto found = table.find("row");
    assert(found && *found == 1);
    assert(table.get(1) == 37);
    table.set(1, 91);
    assert(table.get(1) == 91);
    usage(quota, QuotaResource::scratch_files, 1, 1, 1);

    table.clear();
    usage(quota, QuotaResource::scratch_files, 0, 1, 1);
    const auto after = quota.snapshot(QuotaResource::scratch_bytes);
    assert(after.failure.code == RunFailureCode::none);
    assert(after.message[QuotaSlot::committed] == 0);
    assert(after.message[QuotaSlot::reserved] == 0);
    assert(after.message[QuotaSlot::used] == 0);
}

void borrowed_file_lifetime(const char* directory) {
    assert(mode() == QuotaMode::managed && ScratchArena::attached() == nullptr);
    auto quota = RunQuota::from_environment();
    usage(quota, QuotaResource::scratch_files, 0, 0, 1);
    ScratchFile file;
    file.set_directory(directory);
    file.open("quota-borrowed");
    const int alias = ::dup(file.descriptor());
    assert(alias >= 0 && file_stat(alias).st_nlink == 0);
    usage(quota, QuotaResource::scratch_files, 1, 1, 1);
    file.clear();
    assert(file_stat(alias).st_size == 0);
    usage(quota, QuotaResource::scratch_files, 1, 1, 1);
    usage(quota, QuotaResource::scratch_bytes, 0, 0, 0);
    assert(::close(alias) == 0);
    // Last-alias proof still belongs to the host after a real descriptor loan.
    usage(quota, QuotaResource::scratch_files, 1, 1, 1);
}

class FileSizeLimit {
public:
    explicit FileSizeLimit(std::uint64_t bytes) {
        assert(::getrlimit(RLIMIT_FSIZE, &previous_) == 0);
        assert(previous_.rlim_cur == RLIM_INFINITY || previous_.rlim_cur >= bytes);
        struct sigaction ignored{};
        ignored.sa_handler = SIG_IGN;
        assert(::sigemptyset(&ignored.sa_mask) == 0);
        assert(::sigaction(SIGXFSZ, &ignored, &signal_) == 0);
        auto selected = previous_;
        selected.rlim_cur = static_cast<rlim_t>(bytes);
        assert(::setrlimit(RLIMIT_FSIZE, &selected) == 0);
    }
    ~FileSizeLimit() {
        assert(::setrlimit(RLIMIT_FSIZE, &previous_) == 0);
        assert(::sigaction(SIGXFSZ, &signal_, nullptr) == 0);
    }
private:
    struct rlimit previous_{};
    struct sigaction signal_{};
};

void partial_write(const char* directory, bool single, std::uint64_t limit) {
    auto quota = RunQuota::from_environment();
    const auto block = block_size(directory);
    const auto requested = 2 * block + sizeof(unsigned char);
    const auto realized = block + sizeof(unsigned char);
    std::vector<unsigned char> bytes(requested, 37);
    ScratchFile file;
    file.set_directory(directory);
    file.open("quota-partial");
    const int fd = file.descriptor();
    {
        FileSizeLimit cap(realized);
        if (single) {
            const auto count = file.write_some(0, bytes.data(), bytes.size());
            assert(count == static_cast<ssize_t>(realized));
            errno = 0;
            assert(file.write_some(realized, bytes.data(), sizeof(unsigned char)) == -1);
            assert(errno == EFBIG);
        } else {
            bool caught = false;
            try { file.write(0, bytes.data(), bytes.size()); }
            catch (const ReportScratchIoError& error) {
                caught = true;
                assert(error.code().value() == EFBIG);
                same_failure(error.run_failure(), make_run_failure(
                    RunFailureCode::report_sink_failed, {{"reason", "scratch_io"}}));
            }
            assert(caught);
        }
        assert(file_stat(fd).st_size == static_cast<off_t>(realized));
        bool caught = false;
        try { file.truncate(requested); }
        catch (const ReportScratchIoError& error) {
            caught = true;
            assert(error.code().value() == EFBIG);
            same_failure(error.run_failure(), make_run_failure(
                RunFailureCode::report_sink_failed, {{"reason", "scratch_io"}}));
        }
        assert(caught && file_stat(fd).st_size == static_cast<off_t>(realized));
    }
    usage(quota, QuotaResource::scratch_bytes, quota_rounded_extent(realized, block),
          quota_rounded_extent(requested, block), limit);
    assert(!quota.refusal());
    // Cancelled unrealized growth must be available for another real backing.
    ScratchFile other;
    other.set_directory(directory);
    other.open("quota-remainder");
    const auto spare = limit - quota_rounded_extent(realized, block);
    other.ensure_size(spare);
    usage(quota, QuotaResource::scratch_bytes, limit, limit, limit);
}

class ByteCap {
public:
    explicit ByteCap(std::uint64_t bytes) : previous_(set_command_scratch_byte_cap(bytes)) {}
    ~ByteCap() { set_command_scratch_byte_cap(std::move(previous_)); }
private:
    CommandScratchByteCap previous_;
};

// Observe the real private command descriptor without adding a product seam.
int command_descriptor() {
    DIR* directory = ::opendir("/proc/self/fd");
    assert(directory);
    int found = -1;
    while (const auto* entry = ::readdir(directory)) {
        char* end = nullptr;
        const auto number = std::strtol(entry->d_name, &end, 10);
        if (!end || *end || number < 0 || number > std::numeric_limits<int>::max()) continue;
        const auto path = std::string("/proc/self/fd/") + entry->d_name;
        std::array<char, 4096> target{};
        const auto count = ::readlink(path.c_str(), target.data(), target.size());
        if (count <= 0) continue;
        const std::string name(target.data(), static_cast<std::size_t>(count));
        if (name.find("/pineforge-command-") != std::string::npos) {
            assert(found == -1);
            found = static_cast<int>(number);
        }
    }
    assert(::closedir(directory) == 0 && found >= 0);
    return found;
}

std::size_t command_page(std::uint64_t block) {
    const auto page = std::min<std::uint64_t>(block, CachedScratchBacking::kCacheBytes / 4);
    assert(page >= 512);
    return static_cast<std::size_t>(page);
}

void command_spill(const char* directory, const std::string& which, bool managed,
                   std::uint64_t limit) {
    auto quota = RunQuota::from_environment();
    const auto block = block_size(directory);
    const auto page = command_page(block);
    const std::size_t prefix_pages = 2;
    // Cross a rounded disk boundary even when a filesystem block is larger
    // than the resident window. Sparse holes belong to the requested extent.
    const auto spill_page = std::max<std::uint64_t>(prefix_pages, block / page);
    const auto end = (spill_page + 1) * page;
    const auto peak = quota_rounded_extent(end, block);
    // The local cap is sufficient in the shared-refusal case; internal zero
    // must also leave the shared quota enabled.
    ByteCap cap(which == "local" ? prefix_pages * page
                : which == "command-zero-cap" ? 0 : 2 * end);
    std::vector<std::byte> bytes(page, std::byte{63});
    const ScratchBytes image{bytes.data(), bytes.size()};
    auto backing = make_command_scratch_backing(page, prefix_pages * page);
    const int fd = command_descriptor();
    for (ScratchPageId id = 0; id < prefix_pages; ++id)
        assert(backing->write_page(id, image) == ScratchStatus::ok);
    assert(file_stat(fd).st_size == 0);
    if (managed) usage(quota, QuotaResource::scratch_bytes, 0, 0, limit);
    const auto status = backing->write_page(spill_page, image);
    if (managed && which == "local") {
        assert(status == ScratchStatus::io_error && !quota.refusal());
        assert(file_stat(fd).st_size == 0 && backing->page_count() == prefix_pages);
        usage(quota, QuotaResource::scratch_bytes, 0, 0, limit);
    } else if (managed && (which == "command-refuse" || which == "command-zero-cap")) {
        assert(status == ScratchStatus::io_error);
        const auto before = prefix_pages * page;
        assert(file_stat(fd).st_size == static_cast<off_t>(before));
        assert(backing->page_count() == prefix_pages);
        const auto snapshot = quota.refusal();
        assert(snapshot);
        refusal(quota, snapshot->failure, "scratch_bytes", quota_rounded_extent(before, block),
                peak - quota_rounded_extent(before, block), limit,
                quota_rounded_extent(before, block));
        same_failure(run_quota_failure(), snapshot->failure);
    } else {
        assert(status == ScratchStatus::ok);
        assert(file_stat(fd).st_size == static_cast<off_t>(end));
        assert(backing->write_page(0, image) == ScratchStatus::ok);
        assert(backing->page_count() == spill_page + 1);
        std::vector<std::byte> readback(page);
        assert(backing->read_page(0, {readback.data(), readback.size()}) == ScratchStatus::ok);
        assert(readback == bytes);
        if (managed) usage(quota, QuotaResource::scratch_bytes, peak, peak, limit);
        backing.reset();
        if (managed) {
            usage(quota, QuotaResource::scratch_bytes, 0, peak, limit);
            usage(quota, QuotaResource::scratch_files, 0, 1, 1);
            auto replacement = make_command_scratch_backing(page, 0);
            assert(replacement->write_page(spill_page, image) == ScratchStatus::ok);
            usage(quota, QuotaResource::scratch_bytes, peak, peak, limit);
        }
    }
}

void truncate_then_write_error(const char* directory, std::uint64_t limit) {
    auto quota = RunQuota::from_environment();
    const auto block = block_size(directory);
    const auto page = command_page(block);
    ByteCap cap(2 * page);
    auto backing = make_command_scratch_backing(page, 0);
    const int fd = command_descriptor();
    void* inaccessible = ::mmap(nullptr, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(inaccessible != MAP_FAILED);
    const auto status = backing->write_page(0, {static_cast<const std::byte*>(inaccessible), page});
    assert(::munmap(inaccessible, page) == 0);
    assert(status == ScratchStatus::io_error);
    assert(file_stat(fd).st_size == static_cast<off_t>(page));
    assert(backing->page_count() == 1 && !quota.refusal());
    const auto charge = quota_rounded_extent(page, block);
    usage(quota, QuotaResource::scratch_bytes, charge, charge, limit);
}

void missing_channel(const char* directory, bool command) {
    assert(mode() == QuotaMode::managed);
    bool caught = false;
    try {
        if (command) {
            auto backing = make_command_scratch_backing(512);
            (void)backing;
        } else {
            ScratchFile file;
            file.set_directory(directory);
            file.open("quota-missing");
        }
    } catch (const coded<std::runtime_error>& error) {
        caught = true;
        same_failure(error.run_failure(), make_run_failure(
            RunFailureCode::run_quota_configuration_invalid,
            {{"stage", "admission"}, {"reason", "protocol"}, {"setting", "channel"}}));
    }
    assert(caught);
}

// The host uses the actual ledger/serve implementation and Linux credentials.
// This socket observer supplies the host-only inode confirmation by opening
// /proc/<actor>/fd while the actor is blocked awaiting its COMMIT reply.
// It changes no request, reply, limit, reservation or accounting verdict.
const char host[] = R"PY(
import importlib
import os
from pathlib import Path
import resource
import socket
import subprocess
import sys
import tempfile
import threading

program, module_dir = sys.argv[1:]
marker = [socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET) for _ in range(2)]
os.environ.update(PF_RUN_QUOTA_MODE="managed",
                  PF_RUN_QUOTA_NATIVE_FD=str(marker[0][1].fileno()),
                  PF_RUN_QUOTA_PYTHON_FD=str(marker[1][1].fileno()))
sys.path.insert(0, module_dir)
q = importlib.import_module("run_quota")
assert q.mode() == "managed"
base = dict(os.environ)
for name in ("PF_RUN_QUOTA_MODE", "PF_RUN_QUOTA_NATIVE_FD", "PF_RUN_QUOTA_PYTHON_FD",
             "PF_RUN_SCRATCH_BYTES", "PF_RUN_SCRATCH_FILES", "PF_RUN_OUTPUT_BYTES",
             "PF_RUN_DESCRIPTOR_LIMIT", "PINEFORGE_COMMAND_SCRATCH_MAX_BYTES",
             "PF_SCRATCH_ARENA_FD", "PF_SCRATCH_ARENA_BYTES", "PF_SCRATCH_ARENA_JAILED"):
    base.pop(name, None)
disk = subprocess.check_output([program, "--directory"], env=base, text=True)[:-1]

class ObservedSocket:
    def __init__(self, channel, ledger, actor):
        self.channel, self.ledger, self.actor = channel, ledger, actor
        self.messages, self.requests, self.tickets = 0, {}, {}
    def __getattr__(self, name):
        return getattr(self.channel, name)
    def recvmsg(self, *args):
        result = self.channel.recvmsg(*args)
        if result[0]:
            self.messages += 1
            request = q.Message.decode(result[0])
            self.requests[request.sequence] = request
            if request.opcode == q.Opcode.COMMIT:
                original = self.tickets[request[q.Slot.TICKET]]
                identity = request[q.Slot.DEVICE], request[q.Slot.INODE]
                assert identity[1], "physical-file commit omitted its inode"
                matched = False
                for entry in Path(f"/proc/{self.actor.pid}/fd").iterdir():
                    try:
                        info = entry.stat()
                        if (info.st_dev, info.st_ino) != identity:
                            continue
                        fd = os.open(entry, os.O_RDONLY | os.O_CLOEXEC)
                    except OSError:
                        continue
                    try:
                        info = os.fstat(fd)
                        if (info.st_dev, info.st_ino) != identity:
                            continue
                        confirmed = self.ledger.confirm_object(
                            self.actor, original[q.Slot.OBJECT], original[q.Slot.GENERATION], fd)
                        assert confirmed == identity
                        matched = True
                        break
                    finally:
                        os.close(fd)
                assert matched, "host could not observe the admitted backing"
        return result
    def sendall(self, data):
        reply = q.Message.decode(data)
        request = self.requests.pop(reply.sequence)
        if not reply[q.Slot.STATUS]:
            if request.opcode == q.Opcode.RESERVE:
                self.tickets[reply[q.Slot.TICKET]] = request
            elif request.opcode == q.Opcode.CANCEL:
                self.tickets.pop(request[q.Slot.TICKET])
        self.channel.sendall(data)

with tempfile.TemporaryDirectory(prefix="quota-native-", dir=disk) as directory:
    geometry = os.statvfs(directory)
    block = geometry.f_bsize
    native_block, page = map(int, subprocess.check_output(
        [program, "--geometry", directory], env=base, text=True).split())
    assert native_block == block
    spill_page = max(2, block // page)
    rounded = lambda extent: q.rounded_extent(extent, block)
    cases = [(f"two-{operation}", rounded(block + 1) + rounded(1), 2)
             for operation in ("ensure", "truncate", "write", "write_some")]
    cases += [("create", 0, 1), ("command-create", 0, 1),
              ("sparse", rounded(9 * block + 1), 1),
              ("move", rounded(block + 1) + rounded(1), 2),
              ("id-table-refund", rounded(68), 1), ("borrowed-file", 0, 1),
              ("partial", rounded(2 * block + 1), 2),
              ("partial-some", rounded(2 * block + 1), 2),
              ("command", rounded((spill_page + 1) * page), 1),
              ("command-refuse", rounded(2 * page), 1),
              ("command-zero-cap", rounded(2 * page), 1),
              ("local", 0, 1), ("truncate-error", rounded(page), 1),
              ("missing", 0, 0), ("command-missing", 0, 0),
              ("unmanaged", rounded(block + 1) + rounded(1), 2)]
    for case, byte_limit, file_limit in cases:
        server, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        python_server, python_child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        q.prepare_socket(server)
        environment = dict(base, PINEFORGE_COMMAND_SCRATCH_DIR=directory)
        if case == "unmanaged":
            # Finite/zero limit text alone cannot opt into management. The
            # real endpoint is inherited and explicitly tried by from_fd().
            environment.update(PF_RUN_SCRATCH_BYTES="0", PF_RUN_SCRATCH_FILES="0",
                               PF_QUOTA_TEST_CHANNEL=str(child.fileno()))
        elif case in ("missing", "command-missing"):
            environment["PF_RUN_QUOTA_MODE"] = "managed"
        else:
            environment.update(PF_RUN_QUOTA_MODE="managed",
                PF_RUN_QUOTA_NATIVE_FD=str(child.fileno()),
                PF_RUN_QUOTA_PYTHON_FD=str(python_child.fileno()))
        process = subprocess.Popen([program, "--case", case, directory, str(byte_limit),
                                    str(file_limit)], env=environment,
            pass_fds=(child.fileno(), python_child.fileno()),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        child.close()
        python_child.close()
        # The child waits before any operation so pidfd/process-generation
        # authentication is registered while it is still alive.
        soft, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
        assert 0 <= soft <= q.MAX_COUNTER
        ledger = q.RunQuota(q.Amounts(byte_limit, file_limit, 0, soft))
        actor = ledger.open_owner(pid=process.pid, uid=os.getuid(), gid=os.getgid(),
            generation=1, role=q.Role.WORKER, soft_nofile=soft)
        observed = ObservedSocket(server, ledger, actor)
        errors = []
        def serve():
            try:
                q.serve(ledger, observed, actor)
            except BaseException as error:
                errors.append(error)
            finally:
                server.close()
        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        output, error = process.communicate("go\n")
        thread.join()
        try:
            assert process.returncode == 0, (case, process.returncode, output, error, errors)
            assert not errors, (case, errors)
            assert not output and not error, (case, output, error)
            if case in ("unmanaged", "missing", "command-missing"):
                assert observed.messages == 0, (case, observed.messages)
            else:
                assert observed.messages > 0, case
                denied = case.startswith("two-") or case in (
                    "create", "command-create", "command-refuse", "command-zero-cap")
                if denied:
                    assert ledger.refusal is not None, case
                    assert ledger.refusal.code == "run_resource_quota_exceeded", case
                else:
                    assert ledger.refusal is None, (case, ledger.refusal)
            assert not list(Path(directory).iterdir()), case
        finally:
            server.close()
            python_server.close()
            ledger.close()
for pair in marker:
    for channel in pair:
        channel.close()
)PY";

} // namespace

int main(int argc, char** argv) {
    static_cast<void>(mode());
    try {
        if (argc == 2 && std::string(argv[1]) == "--directory") {
            std::string note;
            int error = 0;
            const auto directory = resolve_scratch_directory("", note, error);
            assert(!directory.empty());
            std::cout << directory << '\n';
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--geometry") {
            const auto block = block_size(argv[2]);
            std::cout << block << ' ' << command_page(block) << '\n';
            return 0;
        }
        if (argc == 6 && std::string(argv[1]) == "--case") {
            const std::string which = argv[2];
            const char* directory = argv[3];
            const auto limit = std::stoull(argv[4]);
            const auto files = std::stoull(argv[5]);
            std::string ready;
            assert(std::getline(std::cin, ready) && ready == "go");
            if (which.compare(0, 4, "two-") == 0)
                two_files(directory, which.substr(4), true, limit);
            else if (which == "create" || which == "command-create")
                create_refusal(directory, files, which == "command-create");
            else if (which == "sparse") sparse_and_lifetime(directory, limit);
            else if (which == "move") moved_lifetime(directory, limit, files);
            else if (which == "id-table-refund") id_table_refund(directory);
            else if (which == "borrowed-file") borrowed_file_lifetime(directory);
            else if (which == "partial" || which == "partial-some")
                partial_write(directory, which == "partial-some", limit);
            else if (which == "truncate-error") truncate_then_write_error(directory, limit);
            else if (which == "missing" || which == "command-missing")
                missing_channel(directory, which == "command-missing");
            else if (which == "unmanaged") {
                assert(mode() == QuotaMode::unmanaged);
                const char* channel = std::getenv("PF_QUOTA_TEST_CHANNEL");
                assert(channel);
                auto explicit_client = RunQuota::from_fd(std::stoi(channel));
                explicit_client.check();
                for (const auto* operation : {"ensure", "truncate", "write", "write_some"})
                    two_files(directory, operation, false, limit);
                command_spill(directory, "command", false, limit);
            } else {
                assert(which == "command" || which == "command-refuse"
                       || which == "command-zero-cap" || which == "local");
                command_spill(directory, which, true, limit);
            }
            if (which != "missing" && which != "command-missing"
                && run_quota_failure().code == RunFailureCode::none) {
                auto quota = RunQuota::from_environment();
                assert(quota.finish().failure.code == RunFailureCode::none);
            }
            return 0;
        }
        assert(argc == 1);
        const std::string source = __FILE__;
        const auto slash = source.find_last_of('/');
        assert(slash != std::string::npos);
        const auto module_dir = source.substr(0, slash + 1) + "../docker";
        ::execlp("python3", "python3", "-B", "-c", host, argv[0], module_dir.c_str(),
                 static_cast<char*>(nullptr));
        std::cerr << "cannot launch native quota host fixture\n";
        return 1;
    } catch (const std::exception& error) {
        const auto failure = classify_run_failure(error);
        std::cerr << run_failure_code_name(failure.code) << ' '
                  << (failure.args ? *failure.args : "{}") << ": " << error.what() << '\n';
        return 1;
    }
}
