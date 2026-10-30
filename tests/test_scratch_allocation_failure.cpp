#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "command_scratch_arena_fixture.hpp"
#include "global_allocation_replacement.hpp"
#include "../src/command_scratch_cache.hpp"

#include <pineforge/detail/o1_store.hpp>
#include <pineforge/detail/script_store.hpp>
#include <pineforge/detail/scratch_errors.hpp>
#include <pineforge/native_order.hpp>
#include <pineforge/source/exit_scratch.hpp>
#include <pineforge/source/market_admission.hpp>

#include <cerrno>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <utility>

namespace scratch_allocation_test {
using namespace command_arena_test;
using pineforge::RunFailureCode;
namespace no = pineforge::native_order;
namespace ad = pineforge::admission;
namespace exit_scratch = pineforge::source::detail;

constexpr auto kDisabled = std::numeric_limits<std::size_t>::max();

struct AllocationFault {
    const std::size_t before = global_allocation::refused;
    unsigned arms = 0;
    bool clean = true;

    void arm() noexcept {
        clean = clean && !global_allocation::refusing
            && global_allocation::fail_after == kDisabled;
        ++arms;
        global_allocation::fail_after = 0;
    }
    void finish() {
        const bool consumed = global_allocation::fail_after == kDisabled;
        global_allocation::fail_after = kDisabled;
        require(clean && arms == 1, "arm exactly once after operation setup");
        const auto faults = global_allocation::refused - before;
        if (faults != 1)
            throw std::runtime_error("expected one allocation refusal, observed "
                                     + std::to_string(faults));
        require(consumed, "the one-shot allocation fault consumed and disarmed");
    }
    ~AllocationFault() { global_allocation::fail_after = kDisabled; }
};

struct SyscallState {
    AllocationFault* create_allocation = nullptr;
    AllocationFault* read_allocation = nullptr;
    bool fail_create = false;
    bool fail_read = false;
    bool kernel_map_exhaustion = false;
    unsigned creates = 0;
    unsigned reads = 0;
    unsigned maps = 0;
    int map_errno = 0;
    bool map_failed = false;
};
SyscallState syscalls;

struct SyscallScope {
    SyscallScope() {
        require(!syscalls.create_allocation && !syscalls.read_allocation
                    && !syscalls.fail_create && !syscalls.fail_read
                    && !syscalls.kernel_map_exhaustion,
                "syscall faults start disarmed");
        syscalls = {};
    }
    ~SyscallScope() { syscalls = {}; }
};
} // namespace scratch_allocation_test

// These symbols belong only to this Linux test executable. Healthy calls
// reach libc's equivalent creator or the real positional/mapping syscall.
// Arming at a failed transfer occurs after all owner/file setup allocations.
extern "C" int mkstemp(char* pattern) {
    using namespace scratch_allocation_test;
    if (syscalls.fail_create && std::strstr(pattern, "pineforge-openings")) {
        syscalls.fail_create = false;
        ++syscalls.creates;
        if (auto* fault = std::exchange(syscalls.create_allocation, nullptr)) fault->arm();
        errno = EIO;
        return -1;
    }
    return ::mkostemp(pattern, 0);
}

extern "C" ssize_t pread(int fd, void* data, std::size_t length, off_t offset) {
    using namespace scratch_allocation_test;
    auto* fault = std::exchange(syscalls.read_allocation, nullptr);
    if (std::exchange(syscalls.fail_read, false) || fault) {
        ++syscalls.reads;
        if (fault) fault->arm();
        errno = EIO;
        return -1;
    }
    return static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, length, offset));
}

extern "C" void* mmap(void* address, std::size_t length, int protection,
                      int flags, int fd, off_t offset) noexcept {
    using namespace scratch_allocation_test;
    if (std::exchange(syscalls.kernel_map_exhaustion, false)) {
        ++syscalls.maps;
        // Linux rejects an address-space-sized mapping with real ENOMEM.
        // No userspace allocation is refused and no errno is fabricated.
        const auto result = ::syscall(SYS_mmap, nullptr,
            std::numeric_limits<std::size_t>::max(), PROT_NONE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        syscalls.map_errno = errno;
        syscalls.map_failed = result == -1;
        return reinterpret_cast<void*>(result);
    }
    return reinterpret_cast<void*>(
        ::syscall(SYS_mmap, address, length, protection, flags, fd, offset));
}

namespace scratch_allocation_test {
struct Environment {
    const char* name;
    const bool present;
    const std::string previous;
    Environment(const char* key, const char* value)
        : name(key), present(std::getenv(key) != nullptr),
          previous(present ? std::getenv(key) : "") {
        require((value ? ::setenv(name, value, 1) : ::unsetenv(name)) == 0,
                "set scratch test environment");
    }
    ~Environment() {
        if (present) ::setenv(name, previous.c_str(), 1);
        else ::unsetenv(name);
    }
};

void require_code(const std::exception& error, RunFailureCode expected) {
    const auto observed = pineforge::classify_run_failure(error).code;
    if (observed != expected)
        throw std::runtime_error(std::string("expected ")
            + pineforge::run_failure_code_name(expected) + ", observed "
            + pineforge::run_failure_code_name(observed));
    require(dynamic_cast<const pineforge::RunFailureInfo*>(&error) != nullptr,
            "the exception carries a typed run failure");
}

const char* status_name(ScratchStatus status) noexcept {
    switch (status) {
    case ScratchStatus::ok: return "ok";
    case ScratchStatus::end: return "end";
    case ScratchStatus::cancelled: return "cancelled";
    case ScratchStatus::not_found: return "not_found";
    case ScratchStatus::programming_error: return "programming_error";
    case ScratchStatus::io_error: return "io_error";
    case ScratchStatus::corrupt: return "corrupt";
    }
    return "unknown status";
}

struct Outcome {
    ScratchStatus status = ScratchStatus::ok;
    std::exception_ptr error;
};
template<class Operation>
Outcome observe(Operation operation) {
    Outcome result;
    try { result.status = operation(); }
    catch (...) { result.error = std::current_exception(); }
    return result;
}

void require_memory(const Outcome& result, const ReportScratchIoError* carried = nullptr) {
    if (result.error) {
        try { std::rethrow_exception(result.error); }
        catch (const std::exception& error) {
            require_code(error, RunFailureCode::out_of_memory);
        }
        return;
    }
    if (carried) {
        require(result.status != ScratchStatus::ok, "memory exhaustion cannot report success");
        require_code(*carried, RunFailureCode::out_of_memory);
        return;
    }
    // The opening API has no failure accessor. This assertion records its
    // strongest current observable at the throwing freeze boundary.
    throw std::runtime_error(std::string("expected out_of_memory, observed ")
        + status_name(result.status) + " with null carried failure");
}

void fill_arena(ScratchFile& filler) {
    filler.open("scratch-allocation-filler");
    filler.ensure_size(ScratchArena::attached()->stats().bytes_total);
    require(ScratchArena::attached()->stats().chunks_free == 0,
            "fill every arena chunk before arming");
}

void require_reason(const ReportScratchIoError& error, const char* reason) {
    require_code(error, RunFailureCode::report_sink_failed);
    const auto& args = error.run_failure().args;
    require(args && args->find(std::string("\"reason\":\"") + reason + "\"") != std::string::npos,
            "retain the original arena reason");
}

struct PreparedArenaFd {
    int fd = -1;
    struct sigaction previous{};
    PreparedArenaFd() {
        require(!ScratchArena::attached(), "prepare a detached arena descriptor");
        require(::sigaction(SIGXFSZ, nullptr, &previous) == 0, "save file-size signal");
        char path[] = "./scratch-allocation-XXXXXX";
        fd = ::mkstemp(path);
        require(fd >= 0, "create arena descriptor before arming");
        require(::unlink(path) == 0 && ::ftruncate(fd, kSmallArenaBytes) == 0,
                "unlink and size arena descriptor before arming");
    }
    ~PreparedArenaFd() {
        ScratchArena::detach();
        ::close(fd);
        ::sigaction(SIGXFSZ, &previous, nullptr);
    }
};

struct Input {
    no::RunIdentity run{"scratch-allocation", 1};
    AllocationFault* fault = nullptr;
    bool allocate_in_reader = false;
    unsigned calls = 0;
    static ScratchStatus read(void* context, std::uint64_t index, no::RequestHandle& out) {
        auto& self = *static_cast<Input*>(context);
        ++self.calls;
        if (index != 0) return ScratchStatus::not_found;
        out = no::RequestHandle{self.run, 1};
        if (self.allocate_in_reader) {
            if (self.fault) self.fault->arm();
            // A real allocation in the user callback, within the cursor's
            // catch. The same callback allocates successfully when unarmed.
            void* block = ::operator new(128);
            ::operator delete(block);
        } else if (self.fault) {
            // The next allocation belongs to the following opening write.
            self.fault->arm();
        }
        return ScratchStatus::ok;
    }
};

struct MemoryStore {
    std::array<std::byte, 256 * kPageBytes> bytes{};
    FixedMemoryScratchBacking backing{{bytes.data(), bytes.size()}, kPageBytes};
    CommandScratchStore store{config(), &backing};
    MemoryStore() { require(store.status() == ScratchStatus::ok, "open codec workspace unarmed"); }
};

struct NativeJournal {
    MemoryStore memory;
    no::RunIdentity run{"scratch-journal", 1};
    no::WorkingRequestCore core{run};
    const std::string label = std::string(128, 'n');
    NativeJournal() {
        no::bind_journal_codec(core, memory.store);
        const no::CommandEvent event = no::RejectedEvent{
            1, no::Request{no::Transact{1.0}, label, ""}};
        const auto source = no::encode_journal_record(event);
        require(no::append_journal_record(core, source) == ScratchStatus::ok
                    && core.history_end() == 1,
                "append a real uncached native record before arming");
    }
};

struct AdmissionJournal {
    MemoryStore memory;
    ad::Journal journal;
    const std::string id = std::string(128, 'a');
    AdmissionJournal() {
        ad::bind_journal_codec(journal, memory.store);
        auto observation = std::make_shared<ad::CommandObservation>();
        observation->command = journal.next_sequence();
        observation->id = id;
        journal.append(ad::CommandEvent{observation});
        require(journal.event_count() == 1, "append a real admission record before arming");
    }
};

struct SpilledCore {
    no::RunIdentity run{"scratch-carried-allocation", 1};
    no::WorkingRequestCore core{run};
    SpilledCore() {
        no::Request request{no::Transact{1.0}, "rest", ""};
        request.trigger = no::Limit{50.0};
        std::uint64_t incarnation = 1, ordinal = 1;
        // The fixed resident bound is 4096. The next request creates a real
        // spill, whose public read-failure slot can now retain a cause.
        for (unsigned i = 0; i != 4097; ++i)
            require(core.submit(request, 0, incarnation, ordinal).status == no::SubmitStatus::Accepted,
                    "populate the real spilled book before arming");
        require(core.book_spilled(), "the public book failure slot requires a real spill");
        core.rethrow_book_read_failure();
    }
};

pineforge::detail::o1::Shape object_shape() noexcept {
    pineforge::detail::o1::Shape shape;
    shape.resident_pages = 2;
    shape.journal_slots = 1;
    shape.index_slots = 1;
    return shape;
}

void script_allocation_failure() {
    ArenaAttachment attachment(kSmallArenaBytes);
    ScratchFile filler;
    fill_arena(filler);
    auto backing = make_script_scratch_backing();
    std::array<std::byte, 4096> page{};
    AllocationFault fault;
    fault.arm();
    const auto result = observe([&] { return backing->write_page(0, {page.data(), page.size()}); });
    fault.finish();
    require(backing->page_count() == 0, "failed script write publishes no page");
    require_memory(result, backing->carried_failure());
}

void cache_allocation_failure() {
    ArenaAttachment attachment(kSmallArenaBytes);
    // Construct the owned backing first. The stack decorator's first new is
    // its nothrow page buffer, with no make_unique allocation in front of it.
    auto inner = make_script_scratch_backing();
    AllocationFault fault;
    fault.arm();
    const auto result = observe([&] {
        CachedScratchBacking cached(std::move(inner));
        return ScratchStatus::ok;
    });
    fault.finish();
    require_memory(result);
}

void chunk_directory_allocation_failure() {
    ArenaAttachment attachment(kSmallArenaBytes);
    ScratchFile file(ScratchFile::IoRaiser::auxiliary);
    file.open("scratch-allocation-table");
    AllocationFault fault;
    fault.arm();
    const auto result = observe([&] { file.ensure_size(1); return ScratchStatus::ok; });
    fault.finish();
    require(ScratchArena::attached()->stats().bytes_used == 0,
            "failed chunk directory allocation claims no chunk");
    require_memory(result);
}

void arena_metadata_allocation_failure() {
    PreparedArenaFd arena;
    AllocationFault fault;
    fault.arm();
    const auto result = observe([&] {
        ScratchArena::attach(arena.fd, kSmallArenaBytes);
        return ScratchStatus::ok;
    });
    fault.finish();
    require(!ScratchArena::attached(), "failed arena attachment publishes no singleton");
    require_memory(result);
}

void bounded_directory_allocation_failure() {
    ArenaAttachment attachment(kSmallArenaBytes);
    ScratchFile file(ScratchFile::IoRaiser::auxiliary);
    AllocationFault fault;
    fault.arm();
    const auto result = observe([&] {
        file.open_bounded("scratch-allocation-bounded", ScratchArena::kChunkBytes);
        return ScratchStatus::ok;
    });
    fault.finish();
    require(!file.opened(), "failed bounded directory publishes no region");
    require_memory(result);
}

void opening_create_allocation_failure() {
    require(!ScratchArena::attached(), "opening creation uses the real disk path");
    SyscallScope scope;
    Input input;
    const no::OpeningInputCursor cursor(1, &input, &Input::read);
    std::shared_ptr<const no::OpeningReadback> frozen;
    AllocationFault fault;
    syscalls.create_allocation = &fault;
    syscalls.fail_create = true;
    const auto result = observe([&] { return no::freeze_openings(input.run, cursor, frozen); });
    fault.finish();
    require(syscalls.creates == 1 && input.calls == 0 && !frozen,
            "fault only the create diagnostic after the retained owner exists");
    require_memory(result);
}

void opening_write_allocation_failure() {
    ArenaAttachment attachment(kSmallArenaBytes);
    ScratchFile filler;
    fill_arena(filler);
    Input input;
    const no::OpeningInputCursor cursor(1, &input, &Input::read);
    std::shared_ptr<const no::OpeningReadback> frozen;
    AllocationFault fault;
    input.fault = &fault;
    const auto result = observe([&] { return no::freeze_openings(input.run, cursor, frozen); });
    fault.finish();
    require(input.calls == 1 && !frozen && ScratchArena::attached()->stats().chunks_free == 0,
            "fault the first opening write after both files and the input handle exist");
    require_memory(result);
}

void opening_read_allocation_failure() {
    ArenaAttachment attachment(kSmallArenaBytes);
    SyscallScope scope;
    Input input;
    const no::OpeningInputCursor cursor(1, &input, &Input::read);
    std::shared_ptr<const no::OpeningReadback> original;
    require(no::freeze_openings(input.run, cursor, original) == ScratchStatus::ok,
            "freeze and populate the read source before arming");
    auto output = original;
    AllocationFault fault;
    syscalls.read_allocation = &fault;
    const auto result = observe([&] { return no::freeze_openings(input.run, *original, output); });
    fault.finish();
    require(syscalls.reads == 1 && output == original,
            "fault the frozen opening read diagnostic and retain the output owner");
    require_memory(result);
}

void opening_cursor_allocation_failure() {
    ArenaAttachment attachment(kSmallArenaBytes);
    Input input;
    input.allocate_in_reader = true;
    const no::OpeningInputCursor cursor(1, &input, &Input::read);
    std::shared_ptr<const no::OpeningReadback> frozen;
    AllocationFault fault;
    input.fault = &fault;
    const auto result = observe([&] { return no::freeze_openings(input.run, cursor, frozen); });
    fault.finish();
    require(input.calls == 1 && !frozen, "allocation really occurs within the cursor callback");
    require_memory(result);
}

void workspace_allocation_failure() {
    std::array<std::byte, 16 * kPageBytes> bytes{};
    FixedMemoryScratchBacking backing({bytes.data(), bytes.size()}, kPageBytes);
    const auto geometry = config();
    AllocationFault fault;
    fault.arm();
    CommandScratchStore store(geometry, &backing);
    fault.finish();
    const auto result = observe([&] {
        exit_scratch::exit_scratch_check(store.status(), store);
        return ScratchStatus::ok;
    });
    require_memory(result, store.carried_failure());

    require(store.status() == ScratchStatus::io_error
                && store.begin() == ScratchStatus::programming_error
                && store.page_size() == 0 && store.record_count() == 0
                && !store.transaction_active(),
            "the allocation marker refuses mutation and exposes no workspace");
    std::array<std::byte, kPageBytes> page{};
    page.fill(std::byte{0x5a});
    const auto untouched = page;
    std::uint64_t digest = 0x123456789abcdef0ULL;
    require(store.state_digest({page.data(), page.size()}, digest)
                == ScratchStatus::programming_error
                && digest == 0x123456789abcdef0ULL && page == untouched
                && backing.page_count() == 0,
            "the allocation marker leaves digest output, buffer and backing untouched");

    std::array<std::byte, 16 * kPageBytes> healthy_bytes{};
    FixedMemoryScratchBacking healthy_backing(
        {healthy_bytes.data(), healthy_bytes.size()}, kPageBytes);
    CommandScratchStore healthy(geometry, &healthy_backing);
    require(healthy.status() == ScratchStatus::ok && !healthy.carried_failure()
                && healthy.page_size() == kPageBytes && healthy.record_count() == 0
                && healthy.begin() == ScratchStatus::ok && healthy.transaction_active(),
            "a later unarmed store has its own healthy transaction state");
    require(store.status() == ScratchStatus::io_error && store.carried_failure()
                && store.page_size() == 0 && store.record_count() == 0
                && !store.transaction_active()
                && store.begin() == ScratchStatus::programming_error,
            "a healthy store cannot mutate the shared allocation marker");
    require(healthy.rollback() == ScratchStatus::ok && !healthy.transaction_active(),
            "the independent healthy transaction rolls back normally");
}

void object_store_allocation_failure() {
    ArenaAttachment attachment(kSmallArenaBytes);
    ScratchFile filler;
    fill_arena(filler);
    auto backing = make_script_scratch_backing();
    auto store = pineforge::detail::o1::Store::open(object_shape(), *backing);
    auto transaction = store->begin();
    AllocationFault fault;
    fault.arm();
    const auto result = observe([&] {
        transaction->commit();
        return ScratchStatus::ok;
    });
    fault.finish();
    require(backing->page_count() == 0, "failed object-store commit publishes no backing page");
    require_memory(result);
    // Rollback blanks the possibly tainted superblock slot. Make the backing
    // serviceable only after checking the failed commit and its original cause.
    filler.clear();
    transaction->rollback();
}

void native_decode_allocation_failure() {
    NativeJournal journal;
    AllocationFault fault;
    fault.arm();
    const auto result = observe([&] {
        (void)journal.core.history_at(0);
        return ScratchStatus::ok;
    });
    fault.finish();
    require(journal.core.history_end() == 1, "failed native decode retains the record");
    require_memory(result);
}

void admission_decode_allocation_failure() {
    AdmissionJournal journal;
    AllocationFault fault;
    fault.arm();
    const auto result = observe([&] {
        (void)journal.journal.events();
        return ScratchStatus::ok;
    });
    fault.finish();
    require(journal.journal.event_count() == 1, "failed admission decode retains the record");
    require_memory(result);
}

void carried_book_allocation_failure() {
    SpilledCore book;
    const std::system_error base(ENOMEM, std::generic_category(), "scratch allocation transport");
    const pineforge::RunFailureArgs args{{"reason", "arena_io"}};
    AllocationFault fault;
    fault.arm();
    // Copying the prepared system_error base does not allocate. Refuse the
    // diagnostic canonicalizer's allocation: it carries out_of_memory in a
    // system_error, exactly the shape a generic I/O catch must preserve.
    const pineforge::coded<std::system_error> incoming(
        RunFailureCode::report_sink_failed, args, base);
    fault.finish();
    require_code(incoming, RunFailureCode::out_of_memory);
    require(book.core.carry_book_read_failure(std::make_exception_ptr(incoming)),
            "retain the allocation failure through the public book channel");
    const auto result = observe([&] {
        book.core.rethrow_book_read_failure();
        return ScratchStatus::ok;
    });
    require_memory(result);
}

void unarmed_arena_operations() {
    ArenaAttachment attachment(8 * ScratchArena::kChunkBytes);
    const auto refused = global_allocation::refused;
    auto backing = make_script_scratch_backing();
    std::array<std::byte, 4096> page{}, read{};
    page.fill(std::byte{0x6b});
    require(backing->write_page(0, {page.data(), page.size()}) == ScratchStatus::ok
                && backing->read_page(0, {read.data(), read.size()}) == ScratchStatus::ok
                && page == read && !backing->carried_failure(),
            "unarmed script page round-trips");
    auto inner = make_script_scratch_backing();
    CachedScratchBacking cached(std::move(inner));
    require(cached.write_page(0, {page.data(), page.size()}) == ScratchStatus::ok,
            "unarmed cache buffer and write succeed");
    ScratchFile growing(ScratchFile::IoRaiser::auxiliary);
    growing.open("scratch-allocation-table");
    growing.ensure_size(1);
    ScratchFile bounded(ScratchFile::IoRaiser::auxiliary);
    bounded.open_bounded("scratch-allocation-bounded", ScratchArena::kChunkBytes);
    bounded.ensure_size(1);
    require(growing.storage_bytes() == ScratchArena::kChunkBytes
                && bounded.storage_bytes() == ScratchArena::kChunkBytes,
            "unarmed chunk and bounded directories claim real chunks");
    require(global_allocation::refused == refused, "healthy arena operations refuse no allocation");
}

void check_openings_unarmed() {
    const auto refused = global_allocation::refused;
    Input input;
    input.allocate_in_reader = true;
    const no::OpeningInputCursor cursor(1, &input, &Input::read);
    std::shared_ptr<const no::OpeningReadback> frozen;
    require(no::freeze_openings(input.run, cursor, frozen) == ScratchStatus::ok && frozen,
            "unarmed owner creation, allocating cursor and opening write succeed");
    no::RequestHandle handle;
    require(frozen->opening_at(0, handle) == ScratchStatus::ok
                && handle == no::RequestHandle{input.run, 1} && input.calls == 1,
            "unarmed opening read returns the same handle");
    require(global_allocation::refused == refused, "healthy opening operations refuse no allocation");
}

void unarmed_disk_openings() { check_openings_unarmed(); }
void unarmed_arena_openings() {
    ArenaAttachment attachment(kSmallArenaBytes);
    check_openings_unarmed();
}

void unarmed_journal_decodes() {
    const auto refused = global_allocation::refused;
    NativeJournal native;
    require(std::get<no::RejectedEvent>(native.core.history_at(0)).request.label == native.label,
            "unarmed native decode restores allocated text");
    AdmissionJournal admission;
    const auto& events = admission.journal.events();
    require(events.size() == 1
                && std::get<ad::CommandEvent>(events.front()).observation->id == admission.id,
            "unarmed admission decode restores allocated text and observation");
    require(global_allocation::refused == refused, "healthy codecs refuse no allocation");
}

void unarmed_object_store() {
    ArenaAttachment attachment(kSmallArenaBytes);
    const auto refused = global_allocation::refused;
    auto backing = make_script_scratch_backing();
    auto store = pineforge::detail::o1::Store::open(object_shape(), *backing);
    auto transaction = store->begin();
    transaction->commit();
    require(backing->page_count() == 2 && !backing->carried_failure(),
            "unarmed object-store commit writes and publishes the real superblock");
    require(global_allocation::refused == refused, "healthy object store refuses no allocation");
}

void unarmed_script_exhaustion() {
    ArenaAttachment attachment(kSmallArenaBytes);
    ScratchFile filler;
    fill_arena(filler);
    const auto refused = global_allocation::refused;
    auto backing = make_script_scratch_backing();
    std::array<std::byte, 4096> page{};
    require(backing->write_page(0, {page.data(), page.size()}) == ScratchStatus::io_error
                && backing->carried_failure(),
            "genuine script arena exhaustion retains a carrier");
    require_reason(*backing->carried_failure(), "arena_exhausted");
    require(global_allocation::refused == refused, "genuine exhaustion refuses no allocation");
}

void unusable_table_directory() {
    require(!ScratchArena::attached(), "directory control uses disk");
    const auto refused = global_allocation::refused;
    ScratchFile file(ScratchFile::IoRaiser::auxiliary);
    file.set_directory("/dev/null");
    bool caught = false;
    try { file.open("scratch-allocation-table"); }
    catch (const std::system_error& error) {
        caught = true;
        require_code(error, RunFailureCode::scratch_io_failed);
        require(error.code().value() == ENOTDIR, "genuine invalid directory retains ENOTDIR");
    }
    require(caught && global_allocation::refused == refused,
            "directory I/O failure needs no allocation refusal");
}

void unarmed_opening_create_io_failure() {
    SyscallScope scope;
    Input input;
    const no::OpeningInputCursor cursor(1, &input, &Input::read);
    std::shared_ptr<const no::OpeningReadback> frozen;
    const auto refused = global_allocation::refused;
    syscalls.fail_create = true;
    require(no::freeze_openings(input.run, cursor, frozen) == ScratchStatus::io_error
                && syscalls.creates == 1 && !frozen && input.calls == 0,
            "a genuine opening-create I/O failure remains an I/O status");
    require(global_allocation::refused == refused, "the create error diagnostic allocates normally");
}

void unarmed_opening_read_io_failure() {
    ArenaAttachment attachment(kSmallArenaBytes);
    SyscallScope scope;
    Input input;
    const no::OpeningInputCursor cursor(1, &input, &Input::read);
    std::shared_ptr<const no::OpeningReadback> frozen;
    require(no::freeze_openings(input.run, cursor, frozen) == ScratchStatus::ok,
            "prepare the real frozen read before injecting a transfer error");
    no::RequestHandle handle{input.run, 99};
    const auto refused = global_allocation::refused;
    syscalls.fail_read = true;
    require(frozen->opening_at(0, handle) == ScratchStatus::io_error
                && syscalls.reads == 1 && handle.incarnation == 99,
            "an opening read I/O failure retains status and leaves its output unchanged");
    require(global_allocation::refused == refused, "the read error diagnostic allocates normally");
}

void kernel_mapping_enomem() {
    for (const auto raiser : {ScratchFile::IoRaiser::auxiliary, ScratchFile::IoRaiser::report_sink}) {
        ArenaAttachment attachment(kSmallArenaBytes);
        SyscallScope scope;
        ScratchFile file(raiser);
        file.open("scratch-allocation-map");
        const auto refused = global_allocation::refused;
        syscalls.kernel_map_exhaustion = true;
        bool caught = false;
        try { file.ensure_size(1); }
        catch (const std::system_error& error) {
            caught = true;
            require(syscalls.maps == 1 && syscalls.map_failed && syscalls.map_errno == ENOMEM,
                    "a real kernel mapping request returns ENOMEM exactly once");
            require(error.code().value() == ENOMEM, "preserve real mmap errno");
            require_code(error, raiser == ScratchFile::IoRaiser::auxiliary
                ? RunFailureCode::scratch_io_failed : RunFailureCode::report_sink_failed);
            if (const auto* report = dynamic_cast<const ReportScratchIoError*>(&error))
                require_reason(*report, "arena_io");
        }
        require(caught && global_allocation::refused == refused
                    && global_allocation::fail_after == kDisabled
                    && ScratchArena::attached()->stats().bytes_used == 0,
                "kernel ENOMEM stays I/O and rolls back the chunk without refusing new");
    }
}

void carried_kernel_mapping_enomem() {
    SpilledCore book;
    ArenaAttachment attachment(kSmallArenaBytes);
    SyscallScope scope;
    ScratchFile file;
    file.open("scratch-allocation-map");
    const auto refused = global_allocation::refused;
    syscalls.kernel_map_exhaustion = true;
    const auto original = observe([&] {
        file.ensure_size(1);
        return ScratchStatus::ok;
    });
    require(original.error && syscalls.maps == 1 && syscalls.map_failed
                && syscalls.map_errno == ENOMEM,
            "capture a real mmap ENOMEM for the book channel");
    require(book.core.carry_book_read_failure(original.error), "carry the actual mapping failure");
    bool caught = false;
    try { book.core.rethrow_book_read_failure(); }
    catch (const std::system_error& error) {
        caught = true;
        require_code(error, RunFailureCode::scratch_io_failed);
        require(error.code().value() == ENOMEM, "book I/O translation retains kernel ENOMEM");
    }
    require(caught && global_allocation::refused == refused,
            "a real mapping refusal remains book I/O without refusing an allocation");
}
} // namespace scratch_allocation_test

int main() {
    using namespace scratch_allocation_test;
    try {
        Environment byte_cap("PINEFORGE_COMMAND_SCRATCH_MAX_BYTES", nullptr);
        Environment window_cap("PF_SCRATCH_WINDOW_BYTES", nullptr);
        Environment directory("TMPDIR", ".");
        struct Row { const char* name; void (*run)(); };
        const std::array<Row, 25> rows{{
            {"script_allocation_failure", script_allocation_failure},
            {"cache_allocation_failure", cache_allocation_failure},
            {"chunk_directory_allocation_failure", chunk_directory_allocation_failure},
            {"arena_metadata_allocation_failure", arena_metadata_allocation_failure},
            {"bounded_directory_allocation_failure", bounded_directory_allocation_failure},
            {"opening_create_allocation_failure", opening_create_allocation_failure},
            {"opening_write_allocation_failure", opening_write_allocation_failure},
            {"opening_read_allocation_failure", opening_read_allocation_failure},
            {"opening_cursor_allocation_failure", opening_cursor_allocation_failure},
            {"workspace_allocation_failure", workspace_allocation_failure},
            {"object_store_allocation_failure", object_store_allocation_failure},
            {"native_decode_allocation_failure", native_decode_allocation_failure},
            {"admission_decode_allocation_failure", admission_decode_allocation_failure},
            {"carried_book_allocation_failure", carried_book_allocation_failure},
            {"unarmed_arena_operations", unarmed_arena_operations},
            {"unarmed_disk_openings", unarmed_disk_openings},
            {"unarmed_arena_openings", unarmed_arena_openings},
            {"unarmed_journal_decodes", unarmed_journal_decodes},
            {"unarmed_object_store", unarmed_object_store},
            {"unarmed_script_exhaustion", unarmed_script_exhaustion},
            {"unusable_table_directory", unusable_table_directory},
            {"unarmed_opening_create_io_failure", unarmed_opening_create_io_failure},
            {"unarmed_opening_read_io_failure", unarmed_opening_read_io_failure},
            {"kernel_mapping_enomem", kernel_mapping_enomem},
            {"carried_kernel_mapping_enomem", carried_kernel_mapping_enomem},
        }};
        int failures = 0;
        for (const auto& row : rows) {
            try {
                row.run();
                std::printf("PASS %s\n", row.name);
            } catch (const std::exception& error) {
                ++failures;
                std::fprintf(stderr, "FAIL %s: %s\n", row.name, error.what());
            } catch (...) {
                ++failures;
                std::fprintf(stderr, "FAIL %s: unknown exception\n", row.name);
            }
        }
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL scratch allocation setup: %s\n", error.what());
        return 1;
    }
}
