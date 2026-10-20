// CR-150 bounded successor roots. Against the d91c96d47d header the expected
// first diagnostic is no WorkingRequestCore member named bind_scratch_slab.
// Compile/run this complete unit remotely; that red is predicted, not observed.
#include "global_allocation_replacement.hpp"

#include <pineforge/detail/scratch_slab.hpp>
#include <pineforge/native_order.hpp>
#include <pineforge/run_failure.hpp>

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
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace no = pineforge::native_order;
namespace pd = pineforge::detail;

namespace {

// Transcribed from d91c96d47d's native_order.hpp: InstanceBinding at 2202-2205,
// then every WorkingRequestCore data member in declaration order at 2296-2344,
// including ReceiptKey's fields. Private nested types are mirrored locally;
// no private-member access or values from a compiler/implementation run are
// used. The class has no base or virtual members. Adding the non-virtual
// bind_scratch_slab member function changes none of these data declarations.
// Keep this pre-change mirror fixed: sizeof/alignof are compared against the
// real class in each remote compiler/configuration, never against a self-pin.
struct WorkingRequestCorePreBindLayout {
    struct InstanceBinding {
        uint64_t generation = 1;
        bool expired = false;
    };

    no::RunIdentity identity_;
    std::shared_ptr<InstanceBinding> instance_;
    std::vector<no::LiveRequest> live_;
    std::vector<no::CommandEvent> history_;
    uint64_t last_ordinal_ = 0;
    uint64_t last_incarnation_ = 0;
    uint64_t epoch_ = 0;
    std::vector<std::pair<uint64_t, std::size_t>> ordinal_index_;
    std::size_t history_base_ = 0;
    uint64_t retired_through_ = 0;
    struct SuccessorRootIndex;
    std::vector<std::pair<uint64_t, uint64_t>> issued_;
    std::shared_ptr<SuccessorRootIndex> successor_roots_;
    std::vector<std::pair<uint64_t, uint64_t>> repriced_;
    struct ReceiptKey {
        no::EventId cause;
        no::RequestHandle recipient;
        no::GroupEffect effect = no::GroupEffect::Reduce;
        uint64_t outcome_ordinal = 0;
    };
    std::vector<ReceiptKey> receipts_;
    std::size_t receipts_base_ = 0;
    std::uint64_t next_cohort_handle_ = 1;
    std::map<std::uint64_t, no::CohortRoster> cohorts_;
    std::vector<no::CohortReceipt> cohort_receipts_;
    std::size_t cohort_receipts_base_ = 0;
};

static_assert(sizeof(no::WorkingRequestCore) == sizeof(WorkingRequestCorePreBindLayout),
              "slab binding must preserve the pre-change WorkingRequestCore size");
static_assert(alignof(no::WorkingRequestCore) == alignof(WorkingRequestCorePreBindLayout),
              "slab binding must preserve the pre-change WorkingRequestCore alignment");

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct RefuseAllocations {
    RefuseAllocations() { global_allocation::refusing = true; }
    ~RefuseAllocations() { global_allocation::refusing = false; }
};

no::Request request() {
    no::Request value{no::Transact{1.0}, "moving", ""};
    value.trigger = no::Limit{90.0};
    return value;
}

std::shared_ptr<pd::ScratchSlab> slab_for(std::uint64_t roots, bool neighbors = false) {
    pd::ScratchManifest manifest{};
    manifest.R = roots;
    if (neighbors) {
        // K_q and B_q stay zero: the q store is removed and the layout refuses a nonzero q.
        manifest.K_e = manifest.K_c = manifest.U = manifest.V = 1;
        manifest.B_e = 23;
        manifest.B_c = 29;
        manifest.B_U = 5;
        manifest.B_V = 7;
    }
    return std::make_shared<pd::ScratchSlab>(manifest);
}

struct Driver {
    no::WorkingRequestCore core{{"roots", 1}};
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    no::CommandContext context{};

    explicit Driver(const std::shared_ptr<pd::ScratchSlab>& owner = {}) {
        core.bind_scratch_slab(owner);
    }

    void retire() {
        core.retire_history(core.last_ordinal());
        require(core.history().empty(), "command journal retired");
    }

    no::RequestHandle submit(bool direct = false) {
        no::SubmitResult result;
        if (direct) {
            result = core.apply_submit(request(), context, incarnation, ordinal).result;
        } else {
            auto prepared = core.prepare_submit(request(), context, incarnation, ordinal);
            RefuseAllocations guard;
            auto installed = core.install_submit(std::move(prepared));
            require(std::holds_alternative<no::CommandInstalled<no::SubmitResult>>(installed),
                    "prepared submit installed without allocation");
            result = std::move(std::get<no::CommandInstalled<no::SubmitResult>>(installed).result);
        }
        require(result.status == no::SubmitStatus::Accepted && result.handle, "submit accepted");
        ++incarnation;
        ++ordinal;
        retire();
        return *result.handle;
    }

    no::RequestHandle replace(no::RequestHandle target, bool direct, no::ReplaceOptions options = {}) {
        no::ReplaceResult result;
        if (direct) {
            result = core.apply_replace(target, request(), context, incarnation, ordinal, options).result;
        } else {
            auto prepared = core.prepare_replace(target, request(), context, incarnation, ordinal, options);
            RefuseAllocations guard;
            auto installed = core.install_replace(std::move(prepared));
            require(std::holds_alternative<no::CommandInstalled<no::ReplaceResult>>(installed),
                    "prepared replacement installed without allocation");
            result = std::move(std::get<no::CommandInstalled<no::ReplaceResult>>(installed).result);
        }
        require(result.status == no::ReplaceStatus::Replaced && result.successor, "replace accepted");
        ++incarnation;
        ++ordinal;
        retire();
        return *result.successor;
    }
};

template<class Operation>
void expect_capacity(Operation&& operation) {
    bool failed = false;
    try {
        operation();
    } catch (const pd::ScratchCapacityError& error) {
        const auto value = pineforge::classify_run_failure(error);
        require(value.code == pineforge::RunFailureCode::scratch_capacity_exceeded,
                "root capacity has its typed code");
        require(value.args && *value.args == "{\"resource\":\"successor_roots\"}",
                "root capacity has only the closed resource argument");
        require(pineforge::run_failure_code_class(value.code) == pineforge::RunFailureClass::resource
                && !pineforge::run_failure_code_retryable(value.code), "capacity is nonretryable resource");
        failed = true;
    }
    require(failed, "expected scratch_capacity_exceeded before mutation; operation succeeded");
}

template<class Operation>
void expect_io(Operation&& operation, int expected_errno, const char* message) {
    bool failed = false;
    try {
        operation();
    } catch (const std::system_error& error) {
        require(error.code() == std::error_code(expected_errno, std::generic_category()),
                "scratch I/O preserves errno and category");
        const std::system_error expected(expected_errno, std::generic_category(), message);
        require(std::strcmp(error.what(), expected.what()) == 0, "scratch I/O preserves English");
        const auto value = pineforge::classify_run_failure(error);
        require(value.code == pineforge::RunFailureCode::scratch_io_failed,
                "root disk failures carry scratch_io_failed");
        require(!value.args || *value.args == "{}", "scratch I/O has no computed arguments");
        require(pineforge::run_failure_code_class(value.code) == pineforge::RunFailureClass::resource
                && !pineforge::run_failure_code_retryable(value.code), "I/O is nonretryable resource");
        failed = true;
    }
    require(failed, "expected real scratch I/O failure");
}

void binding_owns_the_slab_before_commands() {
    auto owner = slab_for(65);
    std::weak_ptr<pd::ScratchSlab> lifetime = owner;
    Driver driver(owner);
    require(owner.use_count() == 2, "binding eagerly retains one owner before any command");
    owner.reset();
    require(!lifetime.expired(), "caller can release its slab owner");
    auto tail = driver.submit();
    no::RequestHandle first;
    for (std::size_t i = 0; i < 65; ++i) {
        tail = driver.replace(tail, i % 2 == 0);
        if (i == 0) first = tail;
    }
    const auto cohort = driver.core.cohort_open();
    driver.core.cohort_add(cohort, tail);
    {
        RefuseAllocations guard;
        require(driver.core.cohort_contains(cohort, first), "slab prefix lookup allocates nothing");
    }
    no::WorkingRequestCore moved(std::move(driver.core));
    no::WorkingRequestCore destination({"discard", 1});
    destination = std::move(moved);
    require(!lifetime.expired() && destination.cohort_contains(cohort, first), "moves retain old readers");
    destination.reset({"roots", 2});
    require(lifetime.expired(), "reset retires the root helper and its owner");

    auto detached = slab_for(0);
    std::weak_ptr<pd::ScratchSlab> detached_lifetime = detached;
    driver.core.reset({"roots", 3});
    driver.core.bind_scratch_slab(detached);
    detached.reset();
    driver.core.bind_scratch_slab({});
    require(detached_lifetime.expired(), "empty owner detaches the preceding RAM binding");
    driver.core.reset({"roots", 4});
}

void old_successor_matches_disk(bool direct) {
    // A genuine rooted replacement adds one 16-byte row; 16,384 replacements
    // need exactly R=16,384, independent of the single live request.
    constexpr std::size_t replacements = 16384;
    auto owner = slab_for(replacements);
    Driver ram(owner);
    Driver disk;
    const auto root = ram.submit();
    auto ram_tail = root;
    auto disk_tail = disk.submit();
    require(disk_tail == root, "same original incarnation");
    no::RequestHandle oldest;
    for (std::size_t i = 0; i < replacements; ++i) {
        ram_tail = ram.replace(ram_tail, direct);
        disk_tail = disk.replace(disk_tail, direct);
        require(ram_tail == disk_tail, "disk/RAM replacement identities agree");
        if (i == 0) oldest = ram_tail;
    }
    const auto ram_cohort = ram.core.cohort_open();
    const auto disk_cohort = disk.core.cohort_open();
    ram.core.cohort_add(ram_cohort, ram_tail);
    disk.core.cohort_add(disk_cohort, disk_tail);
    for (const auto& alias : {root, oldest, ram_tail}) {
        require(ram.core.cohort_contains(ram_cohort, alias), "old successor resolves its canonical root");
        require(ram.core.cohort_contains(ram_cohort, alias)
                    == disk.core.cohort_contains(disk_cohort, alias), "disk/RAM canonical membership agrees");
    }
    ram.core.cohort_remove(ram_cohort, oldest);
    disk.core.cohort_remove(disk_cohort, oldest);
    require(!ram.core.cohort_contains(ram_cohort, ram_tail)
            && !disk.core.cohort_contains(disk_cohort, disk_tail), "old alias removes the canonical root");
    // Flushes occur BEFORE rows 65,129,...; at 16,384 rows the last 64 are
    // still inline. This prefix extent is derived from that cadence.
    const auto prefix_rows = replacements - 64;
    require(owner->region(pd::ScratchResource::successor_roots).logical_extent == 16 * prefix_rows,
            "only complete tail batches publish a prefix");
    for (std::size_t i = 0; i < prefix_rows; ++i) {
        std::array<std::uint64_t, 2> row{};
        owner->read_at(pd::ScratchResource::successor_roots, 16 * i, row.data(), sizeof(row));
        // Original is incarnation 1; successor i is i+2 and all root at 1.
        require(row[0] == i + 2 && row[1] == root.incarnation, "prefix keeps the exact 16-byte row codec");
    }
}

void interleaved_roots_match_disk() {
    auto owner = slab_for(2054);
    Driver ram(owner);
    Driver disk;
    std::array<no::RequestHandle, 3> roots{ram.submit(), ram.submit(), ram.submit()};
    std::array<no::RequestHandle, 3> disk_tails{disk.submit(), disk.submit(), disk.submit()};
    auto tails = roots;
    std::array<no::RequestHandle, 3> oldest{};
    std::array<no::RequestHandle, 3> previous{};
    no::RequestHandle plain;
    for (std::size_t i = 0; i < 2054; ++i) {
        const auto slot = i % tails.size();
        previous[slot] = tails[slot];
        tails[slot] = ram.replace(tails[slot], i % 2 == 0);
        disk_tails[slot] = disk.replace(disk_tails[slot], i % 2 == 0);
        require(tails[slot] == disk_tails[slot], "interleaved successor identities agree");
        if (i < tails.size()) oldest[slot] = tails[slot];
        if (i == 99) {
            plain = ram.submit();
            require(disk.submit() == plain, "rootless key gap agrees");
            (void)ram.core.cancel(plain, ram.ordinal);
            (void)disk.core.cancel(plain, disk.ordinal);
            ram.retire();
            disk.retire();
        }
    }
    const auto cohort = ram.core.cohort_open();
    const auto disk_cohort = disk.core.cohort_open();
    ram.core.cohort_add(cohort, tails[1]);
    disk.core.cohort_add(disk_cohort, disk_tails[1]);
    const std::array<no::RequestHandle, 9> queries{{
        roots[1], oldest[1], previous[1], tails[1], oldest[0], oldest[2], plain,
        {{"other", 1}, oldest[1].incarnation}, {{"roots", 1}, ram.incarnation + 7},
    }};
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const bool expected = i < 4; // Exactly the four aliases of chain 1 were enrolled.
        require(ram.core.cohort_contains(cohort, queries[i]) == expected, "interleaved canonical query");
        require(ram.core.cohort_contains(cohort, queries[i])
                    == disk.core.cohort_contains(disk_cohort, queries[i]), "disk/RAM terminal queries agree");
    }
    ram.core.cohort_remove(cohort, oldest[1]);
    disk.core.cohort_remove(disk_cohort, oldest[1]);
    require(!ram.core.cohort_contains(cohort, tails[1])
            && !disk.core.cohort_contains(disk_cohort, disk_tails[1]), "interleaved root removal agrees");
    ram.core.cohort_add(cohort, tails[2]);
    (void)ram.core.cancel(tails[2], ram.ordinal);
    ram.retire();
    require(ram.core.cohort_contains(cohort, tails[2]), "cancelled terminal alias still names its root");
    ram.core.cohort_remove(cohort, oldest[2]);
    require(!ram.core.cohort_contains(cohort, tails[2]), "terminal alias removes an observable dead chain");
}

struct RegionImage {
    std::uint64_t capacity = 0;
    std::uint64_t extent = 0;
    std::vector<std::uint8_t> bytes;
};
using SlabImage = std::array<RegionImage, pd::kScratchRegionCount>;

SlabImage image(const pd::ScratchSlab& slab) {
    SlabImage result;
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto& region = slab.regions()[i];
        result[i].capacity = region.capacity;
        result[i].extent = region.logical_extent;
        result[i].bytes.resize(static_cast<std::size_t>(region.capacity));
        slab.read_at(region, 0, result[i].bytes.data(), result[i].bytes.size());
    }
    return result;
}

void same_image(const pd::ScratchSlab& slab, const SlabImage& before) {
    const auto after = image(slab);
    for (std::size_t i = 0; i < before.size(); ++i) {
        require(before[i].capacity == after[i].capacity && before[i].extent == after[i].extent
                && before[i].bytes == after[i].bytes, "refused root leaves all region bytes and extents intact");
    }
}

void exact_limit_is_atomic(std::uint64_t limit, bool direct) {
    auto owner = slab_for(limit, true);
    Driver driver(owner);
    const auto root = driver.submit(direct);
    auto tail = root;
    auto first = root;
    for (std::uint64_t i = 0; i < limit; ++i) {
        tail = driver.replace(tail, direct);
        if (i == 0) first = tail;
    }
    const auto cohort = driver.core.cohort_open();
    driver.core.cohort_add(cohort, tail);
    // Publish readable zeroed spare bytes only for this test's full snapshot.
    // This does not add logical root rows. Normal flush extents are separately
    // checked above; neighboring regions get nonzero canaries through I/O.
    for (const auto& region : owner->regions()) {
        if (region.resource != pd::ScratchResource::successor_roots) {
            std::array<std::uint8_t, 128> canary{};
            canary.fill(static_cast<std::uint8_t>(region.resource));
            for (std::uint64_t offset = 0; offset < region.capacity;) {
                const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(
                        canary.size(), region.capacity - offset));
                owner->write_at(region, offset, canary.data(), length);
                offset += length;
            }
        }
        owner->extend(region.resource, region.capacity);
    }
    // An already prepared cancellation is an independent epoch witness:
    // a capacity refusal must not invalidate it, even with a full tail.
    auto pending_cancel = driver.core.prepare_cancel(tail, driver.ordinal);
    const auto before = image(*owner);
    const auto incarnation = driver.incarnation;
    const auto ordinal = driver.ordinal;
    const auto last_incarnation = driver.core.last_incarnation();
    const auto last_ordinal = driver.core.last_ordinal();
    const auto history_base = driver.core.history_base();
    const auto history_end = driver.core.history_end();
    const auto receipt_count = driver.core.cohort_receipt_count();
    const auto definition = driver.core.find_live(tail)->definition;
    expect_capacity([&] {
        if (direct) {
            (void)driver.core.apply_replace(tail, request(), driver.context, driver.incarnation, driver.ordinal);
        } else {
            (void)driver.core.prepare_replace(tail, request(), driver.context, driver.incarnation, driver.ordinal);
        }
    });
    require(driver.incarnation == incarnation && driver.ordinal == ordinal
            && driver.core.last_incarnation() == last_incarnation && driver.core.last_ordinal() == last_ordinal
            && driver.core.history_base() == history_base && driver.core.history_end() == history_end
            && driver.core.history().empty() && driver.core.live().size() == 1
            && driver.core.find_live(tail) && driver.core.find_live(tail)->definition == definition
            && driver.core.cohort_receipt_count() == receipt_count, "capacity refusal precedes core mutation");
    require(driver.core.cohort_contains(cohort, first) && driver.core.cohort_contains(cohort, root),
            "capacity refusal retains both prefix and inline aliases");
    same_image(*owner, before);
    {
        RefuseAllocations guard;
        const auto installed = driver.core.install_cancel(std::move(pending_cancel));
        require(std::holds_alternative<no::CommandInstalled<no::CancelResult>>(installed),
                "capacity refusal leaves the previous prepared epoch valid");
    }
    require(!driver.core.find_live(tail) && driver.core.cohort_contains(cohort, tail),
            "refused flush did not drop the terminal tail's canonical root");
}

void rootless_and_kept_plans_are_free(bool direct, std::uint64_t limit) {
    auto owner = slab_for(limit);
    Driver driver(owner);
    auto tail = driver.submit(direct);
    no::RequestHandle first = tail;
    for (std::uint64_t i = 0; i < limit; ++i) {
        tail = driver.replace(tail, direct);
        if (i == 0) first = tail;
    }
    // Even R=0 admits rootless incarnations. At R=64 either submission may
    // flush all 64 rows but must not claim a nonexistent sixty-fifth root.
    const auto plain_prepared = driver.submit(false);
    const auto plain_direct = driver.submit(true);
    require(plain_prepared != plain_direct, "rootless plans still issue distinct incarnations");
    no::ReplaceOptions options;
    options.keep_handle = true;
    require(driver.replace(tail, false, options) == tail, "prepared kept handle consumes no root row");
    require(driver.replace(tail, true, options) == tail, "direct kept handle consumes no root row");
    const auto cohort = driver.core.cohort_open();
    driver.core.cohort_add(cohort, tail);
    require(driver.core.cohort_contains(cohort, first), "rootless/kept plans preserve chain lookup");
    const auto flushed_rows = 64 * (limit / 64);
    require(owner->region(pd::ScratchResource::successor_roots).logical_extent == 16 * flushed_rows,
            "rootless preparations publish full batches without charging another root");
    expect_capacity([&] {
        (void)driver.core.prepare_replace(tail, request(), driver.context, driver.incarnation, driver.ordinal);
    });
}

void abandoned_ticket_does_not_consume_capacity() {
    auto owner = slab_for(1);
    Driver driver(owner);
    const auto root = driver.submit();
    {
        auto abandoned = driver.core.prepare_replace(root, request(), driver.context,
                                                      driver.incarnation, driver.ordinal);
        require(static_cast<bool>(abandoned), "a real replacement was prepared");
    }
    require(owner->region(pd::ScratchResource::successor_roots).logical_extent == 0
            && driver.core.last_incarnation() == root.incarnation, "reservation publishes no logical row");
    const auto tail = driver.replace(root, false);
    require(tail.incarnation == root.incarnation + 1, "chosen ticket consumes the single row");
    expect_capacity([&] {
        (void)driver.core.prepare_replace(tail, request(), driver.context, driver.incarnation, driver.ordinal);
    });
}

struct ScratchDirectory {
    std::string path = "/tmp/pineforge-roots-ram-XXXXXX";
    ScratchDirectory() { require(::mkdtemp(path.data()) != nullptr, "test scratch directory created"); }
    ~ScratchDirectory() { ::rmdir(path.c_str()); }
};

int root_descriptor(off_t expected_bytes) {
    DIR* descriptors = ::opendir("/dev/fd");
    require(descriptors != nullptr, "test descriptor directory opened");
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
    require(found >= 0, "test anonymous root file found");
    return found;
}

struct FileSizeLimit {
    struct rlimit previous{};
    using Handler = void (*)(int);
    Handler handler = SIG_DFL;
    explicit FileSizeLimit(rlim_t bytes) {
        require(::getrlimit(RLIMIT_FSIZE, &previous) == 0, "read file-size limit");
        handler = std::signal(SIGXFSZ, SIG_IGN);
        require(handler != SIG_ERR, "install test file-size signal disposition");
        struct rlimit limit = previous;
        limit.rlim_cur = bytes;
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

void ram_needs_neither_directory_nor_file_extent() {
    ScratchDirectory scratch;
    auto owner = slab_for(129);
    Driver driver(owner);
    driver.core.set_successor_roots_directory(scratch.path + "/missing");
    FileSizeLimit limit(0);
    auto tail = driver.submit();
    no::RequestHandle first;
    for (std::size_t i = 0; i < 129; ++i) {
        tail = driver.replace(tail, i % 2 == 0);
        if (i == 0) first = tail;
    }
    const auto cohort = driver.core.cohort_open();
    driver.core.cohort_add(cohort, tail);
    require(driver.core.cohort_contains(cohort, first), "RAM performs prefix reads with no writable directory");
    // Two full tails have flushed: 2*64*16 bytes; the 129th row is inline.
    require(owner->region(pd::ScratchResource::successor_roots).logical_extent == 2 * 64 * 16,
            "RAM flush reaches the fixed Roots region");
}

void disk_failures_are_typed() {
    ScratchDirectory scratch;
    Driver driver;
    driver.core.set_successor_roots_directory(scratch.path + "/missing");
    auto tail = driver.submit();
    no::RequestHandle first;
    for (std::size_t i = 0; i < 64; ++i) {
        tail = driver.replace(tail, false);
        if (i == 0) first = tail;
    }
    expect_io([&] { (void)driver.replace(tail, true); }, ENOENT, "native successor roots create");
    driver.core.set_successor_roots_directory(scratch.path);
    {
        // 96 bytes admit six rows of a 64-row batch, then EFBIG. The tail
        // remains authoritative until the entire original batch is retried.
        FileSizeLimit limit(96);
        expect_io([&] { (void)driver.replace(tail, false); }, EFBIG, "native successor roots write");
    }
    const int partial = root_descriptor(96);
    const auto cohort = driver.core.cohort_open();
    driver.core.cohort_add(cohort, tail);
    require(driver.core.cohort_contains(cohort, first), "failed partial flush retains the inline roots");
    tail = driver.replace(tail, true);
    require(root_descriptor(64 * 16) == partial, "disk retry overwrites the original partial prefix");
    require(driver.core.cohort_contains(cohort, first), "successful disk retry preserves old aliases");
    // The first binary-search probe for 2 is row 32. A zero root is corrupt
    // under root<successor and must surface as typed I/O, not an absent alias.
    const std::array<std::uint64_t, 2> corrupt{{34, 0}};
    require(::pwrite(partial, corrupt.data(), sizeof(corrupt), 32 * 16)
                == static_cast<ssize_t>(sizeof(corrupt)), "test corrupts a disk prefix row");
    expect_io([&] { (void)driver.core.cohort_contains(cohort, first); }, EIO,
              "native successor roots invalid row");
    require(::ftruncate(partial, 0) == 0, "test truncates the disk prefix");
    expect_io([&] { (void)driver.core.cohort_contains(cohort, first); }, EIO,
              "native successor roots read");
}

} // namespace

int main() {
    try {
        binding_owns_the_slab_before_commands();
        old_successor_matches_disk(true);
        old_successor_matches_disk(false);
        interleaved_roots_match_disk();
        // Every cap below 64 must fail before the 65th-row flush trigger;
        // 64 and 128 test refusal BEFORE a flush, 65 tests a nonempty prefix.
        for (const std::uint64_t limit : {0, 1, 7, 63, 64, 65, 128}) {
            exact_limit_is_atomic(limit, true);
            exact_limit_is_atomic(limit, false);
        }
        for (const std::uint64_t limit : {0, 1, 64}) {
            rootless_and_kept_plans_are_free(true, limit);
            rootless_and_kept_plans_are_free(false, limit);
        }
        abandoned_ticket_does_not_consume_capacity();
        ram_needs_neither_directory_nor_file_extent();
        disk_failures_are_typed();
        std::puts("successor roots RAM: PASS");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::refusing = false;
        std::fprintf(stderr, "successor roots RAM: %s\n", error.what());
        return 1;
    }
}
