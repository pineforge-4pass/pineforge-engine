// CR-150 CX-D-P3. Generate the reference before measuring the core; every
// committed pair, old-origin lookup and stored row must agree in both modes.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "native_order_full_fold.hpp"

#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/report_sink.h>
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
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>
#ifdef __linux__
#include <sys/mman.h>
#endif

namespace no = pineforge::native_order;
using pineforge::detail::ScratchArena;
using pineforge::detail::ScratchFile;
using pineforge::detail::ScratchIoError;

namespace {

constexpr std::size_t kChains = 7;
constexpr std::size_t kTailRows = 64;
constexpr std::size_t kWorkRows = 300003;
constexpr std::size_t kWarmRows = 1024;
constexpr std::size_t kHostRows = 3 * kTailRows + 1;
constexpr std::uint64_t kChunk = ScratchArena::kChunkBytes;

struct Pair {
    std::uint64_t successor = 0;
    std::uint64_t root = 0;
};
static_assert(sizeof(Pair) == 16, "the reference uses the on-disk pair format");

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

constexpr std::uint64_t disk_rows(std::uint64_t rows) {
    return rows == 0 ? 0 : ((rows - 1) / kTailRows) * kTailRows;
}

constexpr std::uint64_t arena_bytes(std::uint64_t rows) {
    // A preparation can publish the last full tail and then be abandoned.
    const auto bytes = (rows / kTailRows) * kTailRows * sizeof(Pair);
    return ((bytes + kChunk - 1) / kChunk) * kChunk;
}

std::size_t chain_at(std::size_t index) {
    return (index * 5 + index / 11) % kChains;
}

std::vector<Pair> reference_pairs(std::size_t count) {
    std::vector<Pair> rows(count);
    for (std::size_t i = 0; i < count; ++i)
        rows[i] = Pair{static_cast<std::uint64_t>(kChains + i + 1),
                       static_cast<std::uint64_t>(chain_at(i) + 1)};
    return rows;
}

struct Environment {
    const char* key;
    bool present;
    std::string old;
    Environment(const char* name, const char* value)
        : key(name), present(std::getenv(name) != nullptr),
          old(present ? std::getenv(name) : "") {
        require((value ? ::setenv(key, value, 1) : ::unsetenv(key)) == 0,
                "set test environment");
    }
    ~Environment() {
        if (present) ::setenv(key, old.c_str(), 1);
        else ::unsetenv(key);
    }
};

struct Directory {
    std::string path = "/tmp/pineforge-dp3-XXXXXX";
    std::string report;
    Directory() {
        require(::mkdtemp(path.data()) != nullptr, "create test scratch directory");
        report = path + "/report.sink";
    }
    ~Directory() {
        ::unlink(report.c_str());
        ::rmdir(path.c_str());
    }
};

template<class F>
void each_fd(F&& visit) {
    DIR* directory = ::opendir("/dev/fd");
    require(directory != nullptr, "enumerate open descriptors");
    while (const auto* entry = ::readdir(directory)) {
        char* end = nullptr;
        const long number = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || number < 0
            || number > std::numeric_limits<int>::max()) continue;
        struct stat status{};
        const int fd = static_cast<int>(number);
        if (::fstat(fd, &status) == 0) visit(fd, status);
    }
    ::closedir(directory);
}

std::size_t regular_fds() {
    std::size_t count = 0;
    each_fd([&](int, const struct stat& status) {
        if (S_ISREG(status.st_mode)) ++count;
    });
    return count;
}

void read_at(int fd, std::uint64_t offset, void* data, std::size_t length) {
    auto* bytes = static_cast<unsigned char*>(data);
    std::size_t received = 0;
    while (received < length) {
        const ssize_t count = ::pread(fd, bytes + received, length - received,
                                      static_cast<off_t>(offset + received));
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "read every byte of a stored root pair");
        received += static_cast<std::size_t>(count);
    }
}

int root_fd(const Pair& first, std::uint64_t size) {
    int found = -1;
    each_fd([&](int fd, const struct stat& status) {
        if (!S_ISREG(status.st_mode) || status.st_nlink != 0
            || status.st_size != static_cast<off_t>(size)) return;
        Pair row;
        if (::pread(fd, &row, sizeof row, 0) == static_cast<ssize_t>(sizeof row)
            && row.successor == first.successor && row.root == first.root) {
            found = fd;
        }
    });
    require(found >= 0, "find the anonymous successor-root file");
    return found;
}

no::Request request() {
    no::Request value{no::Transact{1.0}, "moving", ""};
    value.trigger = no::Limit{50.0};
    return value;
}

struct Driver {
    no::WorkingRequestCore core{{"dp3-roots", 1}};
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    no::CommandContext context{};
    std::array<no::RequestHandle, kChains> tails{};
    std::array<no::CohortHandle, kChains> cohorts{};
    full_fold::Fold events;
    full_fold::Fold pairs;

    explicit Driver(const std::string& directory) {
        core.set_successor_roots_directory(directory);
        for (std::size_t i = 0; i < kChains; ++i) {
            const auto result = core.submit(request(), 0, incarnation, ordinal);
            require(result.status == no::SubmitStatus::Accepted && result.handle,
                    "reference chain opening accepted");
            tails[i] = *result.handle;
            require(tails[i].incarnation == i + 1, "reference opening incarnation");
            core.retire_history(core.last_ordinal());
            cohorts[i] = core.cohort_open();
            core.cohort_add(cohorts[i], tails[i]);
            core.retire_cohort_receipts(core.cohort_receipt_count());
        }
    }

    void replace(std::size_t index, const Pair& expected, bool direct) {
        const auto slot = chain_at(index);
        no::CommandInstalled<no::ReplaceResult> installed;
        if (direct) {
            installed = core.apply_replace(tails[slot], request(), context,
                                            incarnation, ordinal);
        } else {
            auto prepared = core.prepare_replace(tails[slot], request(), context,
                                                  incarnation, ordinal);
            // Spill and allocation happen in prepare, never in install.
            global_allocation::refusing = true;
            auto result = core.install_replace(std::move(prepared));
            global_allocation::refusing = false;
            require(std::holds_alternative<no::CommandInstalled<no::ReplaceResult>>(result),
                    "prepared replacement installs without allocation");
            installed = std::move(std::get<no::CommandInstalled<no::ReplaceResult>>(result));
        }
        require(installed.result.status == no::ReplaceStatus::Replaced
                    && installed.result.successor,
                "generated replacement accepted");
        const auto& event = core.history_at(installed.events.first_index);
        const auto* replaced = std::get_if<no::ReplacedEvent>(&event);
        require(replaced && replaced->successor_definition
                    && replaced->successor_definition->root,
                "replacement records a canonical root");
        const auto& definition = *replaced->successor_definition;
        require(definition.handle.incarnation == expected.successor
                    && definition.root->incarnation == expected.root,
                "every committed pair agrees with the independent reference");
        full_fold::fold(events, event);
        pairs.u(definition.handle.incarnation);
        pairs.u(definition.root->incarnation);
        tails[slot] = *installed.result.successor;
        ++incarnation;
        ++ordinal;
        core.retire_history(core.last_ordinal());
        require(core.history().empty(), "retire every replacement event");
    }

    no::RequestHandle handle(std::uint64_t value) const {
        return no::RequestHandle{core.identity(), value};
    }

    void lookup(const Pair& pair) const {
        const auto slot = static_cast<std::size_t>(pair.root - 1);
        require(core.cohort_contains(cohorts[slot], handle(pair.successor)),
                "every successor reads back its canonical root");
        require(!core.cohort_contains(cohorts[(slot + 1) % kChains], handle(pair.successor)),
                "a successor cannot read back a different chain's root");
    }
};

std::size_t peak_heap = 0;
void note_heap(std::size_t) {
    peak_heap = std::max(peak_heap, global_allocation::live_bytes);
}

struct Recording {
    Recording() {
        global_allocation::live_bytes = 0;
        peak_heap = 0;
        global_allocation::on_recorded = note_heap;
        global_allocation::recording = true;
    }
    ~Recording() {
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
    }
};

struct Replay {
    std::uint64_t events = 0;
    std::uint64_t pairs = 0;
    std::uint64_t readbacks = 0;
    std::uint64_t core = 0;
};

Replay replay(const std::vector<Pair>& expected, const std::string& directory,
              int backing_fd = -1, std::uint64_t backing_bytes = 0) {
    // Oracle storage is outside the allocation-counting window.
    // Warm the existing thread-local prepared-token pool outside it too:
    // those recycled blocks intentionally live until thread teardown.
    require(expected.size() > kWarmRows, "heap comparison has a full warm prefix");
    {
        Driver warm(directory);
        warm.replace(0, expected[0], false);
        warm.replace(1, expected[1], false);
    }
    const auto stored_count = static_cast<std::size_t>(disk_rows(expected.size()));
    std::vector<Pair> stored(stored_count);
    const no::RequestHandle foreign{{"foreign", 1}, expected.front().successor};
    const auto descriptors = regular_fds();
    Replay result;
    {
        Recording recording;
        Driver driver(directory);
        std::size_t warm_heap = 0;
        std::size_t warm_peak = 0;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            driver.replace(i, expected[i], i % 2 == 0);
            if (i + 1 == kWarmRows) {
                warm_heap = global_allocation::live_bytes;
                warm_peak = peak_heap;
            }
        }
        const auto chunk_count = arena_bytes(expected.size()) / kChunk;
        // ScratchArena's region vector has at most two uint32 entries of
        // capacity per chunk; the index itself still has exactly 64 RAM rows.
        const auto metadata_bound = static_cast<std::size_t>(
            2 * chunk_count * sizeof(std::uint32_t));
        require(global_allocation::live_bytes <= warm_heap + metadata_bound,
                "retained heap is fixed apart from the arena chunk table");
        require(peak_heap <= warm_peak + metadata_bound,
                "transient heap does not grow with successor pairs");
        for (const auto& pair : expected) driver.lookup(pair);
        require(!driver.core.cohort_contains(driver.cohorts[0], foreign),
                "foreign origins remain refused");
        require(!driver.core.cohort_contains(driver.cohorts[0],
                    driver.handle(expected.back().successor + 1)),
                "unissued origins remain refused");
        driver.core.set_successor_roots_directory(directory);
        bool changed = false;
        try { driver.core.set_successor_roots_directory(directory + "/changed"); }
        catch (const std::logic_error& error) {
            changed = std::strcmp(error.what(),
                "native successor roots directory changed after spill") == 0;
        }
        require(changed, "both backends keep the post-spill directory guard");
        const auto bytes = stored_count * sizeof(Pair);
        if (backing_fd < 0) {
            require(regular_fds() == descriptors + 1, "file mode owns one anonymous file");
            const int fd = root_fd(expected.front(), bytes);
            require((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "file mode keeps CLOEXEC");
            read_at(fd, 0, stored.data(), bytes);
        } else {
            require(regular_fds() == descriptors, "arena mode creates no regular file");
            std::array<Pair, kTailRows> batch{};
            std::size_t seen = 0;
            // Recover by successor key rather than assuming physical chunk
            // allocation order. Fresh unused backing and chunk slack are zero.
            for (std::uint64_t offset = 0; offset < backing_bytes; offset += sizeof batch) {
                read_at(backing_fd, offset, batch.data(), sizeof batch);
                for (const auto& pair : batch) {
                    if (pair.successor == 0) {
                        require(pair.root == 0, "unused arena rows remain zero");
                        continue;
                    }
                    require(pair.successor > kChains, "arena stores successor keys");
                    const auto index = pair.successor - kChains - 1;
                    require(index < stored.size(), "arena publishes complete batches only");
                    auto& destination = stored[static_cast<std::size_t>(index)];
                    require(destination.successor == 0, "each pair is stored once");
                    destination = pair;
                    ++seen;
                }
            }
            require(seen == stored.size(), "all published arena records read back");
            const auto stats = ScratchArena::attached()->stats();
            require(stats.bytes_used == arena_bytes(expected.size())
                        && stats.high_water == stats.bytes_used,
                    "claimed bytes equal the workload-derived rounded peak");
        }
        full_fold::Fold readbacks;
        for (std::size_t i = 0; i < stored.size(); ++i) {
            require(stored[i].successor == expected[i].successor
                        && stored[i].root == expected[i].root,
                    "every stored record is byte-identical to the reference");
            readbacks.u(stored[i].successor);
            readbacks.u(stored[i].root);
        }
        result = Replay{driver.events.h, driver.pairs.h, readbacks.h,
                        full_fold::core_value(driver.core)};
        driver.core.reset({"dp3-roots", 2});
        require(regular_fds() == descriptors, "reset closes the root backing immediately");
        if (backing_fd >= 0)
            require(ScratchArena::attached()->stats().bytes_used == 0,
                    "reset releases the old arena region before another run");
    }
    require(regular_fds() == descriptors, "destroying the index releases its backing");
    require(global_allocation::live_bytes == 0, "the core releases all recorded allocations");
    return result;
}

[[maybe_unused]] void same_replay(const Replay& actual, const Replay& expected) {
    require(actual.events == expected.events && actual.pairs == expected.pairs
                && actual.readbacks == expected.readbacks && actual.core == expected.core,
            "file and arena events, roots, readbacks and core digests agree");
}

template<class F>
void expect_file_io(F&& operation, int expected_errno, const char* message) {
    bool caught = false;
    try { operation(); }
    catch (const std::system_error& error) {
        const std::system_error expected(expected_errno, std::generic_category(), message);
        caught = error.code() == expected.code()
            && std::strcmp(error.what(), expected.what()) == 0
            && pineforge::classify_run_failure(error).code
                == pineforge::RunFailureCode::engine_unclassified_error;
    }
    require(caught, "legacy file errors retain errno, full text and classification");
}

struct FileSizeLimit {
    struct rlimit previous{};
    using Handler = void (*)(int);
    Handler handler = SIG_DFL;
    explicit FileSizeLimit(rlim_t bytes) {
        require(::getrlimit(RLIMIT_FSIZE, &previous) == 0, "read file-size limit");
        handler = std::signal(SIGXFSZ, SIG_IGN);
        require(handler != SIG_ERR, "ignore file-mode short-write signal");
        struct rlimit limit = previous;
        limit.rlim_cur = bytes;
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            std::signal(SIGXFSZ, handler);
            throw std::runtime_error("set file-mode short-write limit");
        }
    }
    ~FileSizeLimit() {
        ::setrlimit(RLIMIT_FSIZE, &previous);
        std::signal(SIGXFSZ, handler);
    }
};

void file_error_contract(const std::vector<Pair>& reference, const std::string& directory) {
    {
        Driver driver(directory + "/missing");
        for (std::size_t i = 0; i < kTailRows; ++i)
            driver.replace(i, reference[i], true);
        const auto before = full_fold::core_value(driver.core);
        expect_file_io([&] { driver.replace(kTailRows, reference[kTailRows], true); },
                       ENOENT, "native successor roots create");
        require(full_fold::core_value(driver.core) == before, "create failure cannot commit");
        driver.lookup(reference.front());
        driver.core.set_successor_roots_directory(directory);
        driver.replace(kTailRows, reference[kTailRows], false);
    }
    {
        Driver driver(directory);
        for (std::size_t i = 0; i < kTailRows; ++i)
            driver.replace(i, reference[i], false);
        const auto before = full_fold::core_value(driver.core);
        constexpr auto partial_bytes = 3 * sizeof(Pair);
        {
            FileSizeLimit limit(partial_bytes);
            expect_file_io([&] { driver.replace(kTailRows, reference[kTailRows], false); },
                           EFBIG, "native successor roots write");
        }
        const int partial = root_fd(reference.front(), partial_bytes);
        require(full_fold::core_value(driver.core) == before, "short write cannot commit");
        driver.lookup(reference.front());
        driver.replace(kTailRows, reference[kTailRows], false);
        require(root_fd(reference.front(), kTailRows * sizeof(Pair)) == partial,
                "retry overwrites the same short file at the same offset");
        driver.lookup(reference.front());
        require(::ftruncate(partial, 0) == 0, "truncate a published row for the read refusal");
        expect_file_io([&] { driver.lookup(reference.front()); },
                       EIO, "native successor roots read");
        driver.lookup(reference[kTailRows]);
    }
}

class KernelHost final : public pineforge::NativeStrategyHost {
public:
    std::size_t committed = 0;
    full_fold::Fold checkpoints;
    void on_native_run_begin() override {
        committed = 0;
        checkpoints = {};
    }
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {
        auto accepted = submit(request());
        require(accepted.status == no::SubmitStatus::Accepted && accepted.handle,
                "kernel opening accepted");
        auto tail = *accepted.handle;
        for (std::size_t i = 0; i < kHostRows; ++i) {
            const auto result = replace(tail, request());
            require(result.status == no::ReplaceStatus::Replaced && result.successor,
                    "kernel replacement accepted");
            tail = *result.successor;
            ++committed;
            checkpoints.u(native_continuation_hash());
            checkpoints.u(broker_state_hash());
        }
    }
};

pineforge::NativeRunSpec spec(std::uint64_t run) {
    pineforge::NativeRunSpec value;
    value.identity = {"dp3-kernel", run};
    value.input_tf = "1";
    value.script_tf = "1";
    value.tickerid = "TEST:DP3";
    value.timezone = "UTC";
    value.session = "24x7";
    value.slot_label_policy = pineforge::NativeSlotLabelPolicy::FeedTolerant;
    value.initial_capital = 1000000;
    value.point_value = 1;
    value.account_fx = 1;
    value.price_tick = 0.25;
    value.event_retention = pineforge::NativeEventRetention::Window;
    return value;
}

struct KernelReplay {
    std::uint64_t checkpoints;
    std::uint64_t continuation;
    std::uint64_t broker;
};

KernelReplay run_kernel(KernelHost& host, std::uint64_t run) {
    require(host.configure_native(spec(run)).status == pineforge::NativeSetupStatus::Applied,
            "configure the next kernel run");
    const pineforge::Bar bar{100, 101, 99, 100, 1, 1736121600000LL};
    host.run(&bar, 1);
    require(host.last_error().empty()
                && host.native_state().kind == pineforge::NativeLifecycleKind::Completed
                && host.committed == kHostRows,
            "kernel replay completes");
    return {host.checkpoints.h, host.native_continuation_hash(), host.broker_state_hash()};
}

[[maybe_unused]] void same_kernel(const KernelReplay& actual, const KernelReplay& expected) {
    require(actual.checkpoints == expected.checkpoints
                && actual.continuation == expected.continuation && actual.broker == expected.broker,
            "kernel chain-root and broker digests are backend-independent");
}

void production_directory_binding() {
    Directory first;
    Directory second;
    Environment temporary("TMPDIR", (first.path + "/missing").c_str());
    KernelHost host;
    for (std::uint64_t run = 1; run <= 2; ++run) {
        const auto& directory = run == 1 ? first : second;
        const pf_report_sink_v1 sink{sizeof(pf_report_sink_v1), 1,
            directory.report.c_str(), "dp3", directory.path.c_str()};
        require(host.set_report_sink_v1(&sink) == 0, "install the run's spill directory");
        (void)run_kernel(host, run);
        const int fd = root_fd(Pair{2, 1}, disk_rows(kHostRows) * sizeof(Pair));
        require(fd >= 0, "each reset rebinds the successor-root scratch directory");
        if (run == 1) {
            require(::unlink(first.report.c_str()) == 0 && ::rmdir(first.path.c_str()) == 0,
                    "remove the old spill directory before the next run");
        }
    }
}

void kernel_io_failure(bool arena) {
    Environment temporary("TMPDIR", "/pineforge-dp3-does-not-exist");
    ScratchFile blocker;
    if (arena) {
        blocker.open("dp3-kernel-blocker");
        blocker.ensure_size(ScratchArena::attached()->stats().bytes_total);
    }
    KernelHost host;
    require(host.configure_native(spec(1)).status == pineforge::NativeSetupStatus::Applied,
            "configure a kernel I/O refusal");
    const pineforge::Bar bar{100, 101, 99, 100, 1, 1736121600000LL};
    host.run(&bar, 1);
    const auto state = host.native_state();
    require(state.kind == pineforge::NativeLifecycleKind::Failed
                && state.failure.code == pineforge::NativeFailureCode::Allocation
                && state.failure.operation == pineforge::NativeFailureOperation::Command
                && host.committed == kTailRows,
            "scratch failure retains Allocation/Command at the first spill");
    require(pineforge::run_failure_value_of(host).code
                == (arena ? pineforge::RunFailureCode::report_sink_failed
                          : pineforge::RunFailureCode::engine_unclassified_error),
            "file mode keeps its legacy classification; arena refusal stays typed");
    require(host.last_error().find(arena ? "native successor roots write"
                                        : "native successor roots create") == 0,
            "kernel preserves successor-root operation messages");
}

#ifdef __linux__

struct Backing {
    int fd = -1;
    std::uint64_t bytes;
    explicit Backing(std::uint64_t size) : bytes(size) {
        fd = ::memfd_create("pineforge-dp3", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        require(fd >= 0, "parent creates a memfd");
        require(::ftruncate(fd, static_cast<off_t>(bytes)) == 0,
                "parent sizes the backing from the workload");
        require(::fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0,
                "parent seals the arena size");
    }
    ~Backing() { if (fd >= 0) ::close(fd); }
    Backing(const Backing&) = delete;
    Backing& operator=(const Backing&) = delete;
};

struct Attachment {
    explicit Attachment(const Backing& backing) { ScratchArena::attach(backing.fd, backing.bytes); }
    ~Attachment() { ScratchArena::detach(); }
};

void jail_limits() {
    require(std::signal(SIGXFSZ, SIG_IGN) != SIG_ERR, "ignore SIGXFSZ in the child");
    const struct rlimit file_limit{0, 0};
    require(::setrlimit(RLIMIT_FSIZE, &file_limit) == 0, "child sets RLIMIT_FSIZE=0");
    // Include sanitizer shadow mappings in the baseline; bound all further
    // address-space growth rather than skipping the jail under sanitizers.
    std::FILE* statm = std::fopen("/proc/self/statm", "r");
    require(statm != nullptr, "read child address-space baseline");
    unsigned long long pages = 0;
    const int parsed = std::fscanf(statm, "%llu", &pages);
    std::fclose(statm);
    const long page = ::sysconf(_SC_PAGESIZE);
    require(parsed == 1 && page > 0, "parse child address-space baseline");
    const auto baseline = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page);
    constexpr std::uint64_t headroom = 96ULL * 1024ULL * 1024ULL;
    struct rlimit limit{};
    require(::getrlimit(RLIMIT_AS, &limit) == 0, "read child address-space limit");
    const auto requested = static_cast<rlim_t>(baseline + headroom);
    limit.rlim_cur = std::min(limit.rlim_cur, requested);
    limit.rlim_max = std::min(limit.rlim_max, requested);
    require(::setrlimit(RLIMIT_AS, &limit) == 0, "child sets bounded RLIMIT_AS");
}

template<class F>
void jailed(F&& operation) {
    const pid_t child = ::fork();
    require(child >= 0, "fork a successor-root jail test");
    if (child == 0) {
        try {
            jail_limits();
            operation();
            ::_exit(0);
        } catch (const std::exception& error) {
            global_allocation::refusing = false;
            std::fprintf(stderr, "successor roots arena child: %s\n", error.what());
            ::_exit(1);
        }
    }
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "jailed child succeeds without SIGXFSZ, SIGBUS or allocation failure");
}

void exhaustion(const std::vector<Pair>& reference, const Backing& backing) {
    Attachment attached(backing);
    const auto descriptors = regular_fds();
    {
        Driver driver("/pineforge-dp3-no-files");
        const auto count = static_cast<std::size_t>(backing.bytes / sizeof(Pair) + kTailRows);
        require(count < reference.size(), "one missing chunk refuses within the workload");
        for (std::size_t i = 0; i < count; ++i) driver.replace(i, reference[i], i % 2 == 0);
        const auto before = full_fold::core_value(driver.core);
        const auto incarnation = driver.incarnation;
        const auto ordinal = driver.ordinal;
        const auto stats = ScratchArena::attached()->stats();
        require(stats.bytes_used == backing.bytes && stats.chunks_free == 0,
                "all smaller-arena chunks fill before the exact failing batch");
        for (const bool direct : {true, false}) {
            bool refused = false;
            try { driver.replace(count, reference[count], direct); }
            catch (const ScratchIoError& error) {
                refused = (error.code().value() == ENOSPC || error.code().value() == EFBIG)
                    && std::strstr(error.what(), "native successor roots write")
                    && pineforge::classify_run_failure(error).code
                        == pineforge::RunFailureCode::report_sink_failed;
            }
            require(refused, "the exact over-capacity write gives a typed refusal");
            require(full_fold::core_value(driver.core) == before
                        && driver.incarnation == incarnation && driver.ordinal == ordinal,
                    "failed flush publishes no event, incarnation or changed live row");
            driver.lookup(reference.front());
            driver.lookup(reference[count - kTailRows]);
            driver.lookup(reference[count - 1]);
            require(ScratchArena::attached()->stats().bytes_used == stats.bytes_used,
                    "retry cannot silently claim a file or RAM fallback");
        }
        require(regular_fds() == descriptors, "exhaustion creates no regular file");
    }
    const auto cleared = ScratchArena::attached()->stats();
    require(cleared.bytes_used == 0 && cleared.windows_mapped_bytes == 0,
            "index clear releases arena chunks and windows after a failed write");
}

void retry_after_release(const std::vector<Pair>& reference, const Backing& backing) {
    Attachment attached(backing);
    const auto descriptors = regular_fds();
    ScratchFile blocker;
    blocker.open("dp3-blocker");
    blocker.ensure_size(backing.bytes);
    {
        Driver driver("/pineforge-dp3-no-files");
        for (std::size_t i = 0; i < kTailRows; ++i)
            driver.replace(i, reference[i], i % 2 == 0);
        const auto before = full_fold::core_value(driver.core);
        bool refused = false;
        try { driver.replace(kTailRows, reference[kTailRows], false); }
        catch (const ScratchIoError&) { refused = true; }
        require(refused && full_fold::core_value(driver.core) == before,
                "unavailable first batch retains its authoritative tail");
        driver.lookup(reference.front());
        blocker.clear();
        {
            auto abandoned = driver.core.prepare_replace(
                driver.tails[chain_at(kTailRows)], request(), driver.context,
                driver.incarnation, driver.ordinal);
            require(static_cast<bool>(abandoned)
                        && full_fold::core_value(driver.core) == before,
                    "a completed preparation may spill without committing a successor");
            driver.lookup(reference.front());
        }
        driver.replace(kTailRows, reference[kTailRows], false);
        driver.lookup(reference.front());
        driver.lookup(reference[kTailRows]);
        std::array<Pair, kTailRows> stored{};
        read_at(backing.fd, 0, stored.data(), sizeof stored);
        for (std::size_t i = 0; i < stored.size(); ++i)
            require(stored[i].successor == reference[i].successor
                        && stored[i].root == reference[i].root,
                    "retry publishes exactly the original full batch at offset zero");
        require(ScratchArena::attached()->stats().bytes_used == kChunk,
                "retry does not skip the failed logical offset");
    }
    kernel_io_failure(true);
    require(regular_fds() == descriptors, "retry and kernel refusal create no regular file");
}

void arena_cases(const std::vector<Pair>& reference, const Replay& file,
                 const KernelReplay& kernel) {
    const auto bytes = arena_bytes(reference.size());
    require(bytes > kChunk, "the generated workload crosses multiple chunks");
    Backing full(bytes);
    Backing small(bytes - kChunk);
    Environment windows("PF_SCRATCH_WINDOW_BYTES", "65536");
    jailed([&] {
        Attachment attached(full);
        same_replay(replay(reference, "/pineforge-dp3-no-files", full.fd, full.bytes), file);
        require(ScratchArena::attached()->stats().bytes_used == 0,
                "successful replay releases every arena chunk");
        {
            const auto before = regular_fds();
            KernelHost host;
            same_kernel(run_kernel(host, 1), kernel);
            require(regular_fds() == before, "the kernel creates no regular scratch file");
        }
        require(ScratchArena::attached()->stats().bytes_used == 0,
                "kernel teardown releases its root region");
    });
    jailed([&] { exhaustion(reference, small); });
    jailed([&] { retry_after_release(reference, full); });
}

#endif

} // namespace

int main() {
    try {
        require(ScratchArena::attached() == nullptr, "file oracle starts without an arena");
        const auto reference = reference_pairs(kWorkRows);
        Directory directory;
        const auto file = replay(reference, directory.path);
        file_error_contract(reference, directory.path);
        KernelReplay kernel{};
        {
            KernelHost host;
            kernel = run_kernel(host, 1);
        }
        production_directory_binding();
        kernel_io_failure(false);
#ifdef __linux__
        arena_cases(reference, file, kernel);
#else
        std::puts("successor roots arena: SKIP Linux memfd/jail cases on this platform");
        (void)file;
        (void)kernel;
#endif
        std::puts("successor roots arena: PASS");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::refusing = false;
        std::fprintf(stderr, "successor roots arena: %s\n", error.what());
        return 1;
    }
}
