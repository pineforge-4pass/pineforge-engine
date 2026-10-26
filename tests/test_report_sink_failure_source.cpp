// A7 source-side restore and sealed-T translators, through a real source host.
// The frozen batch-C compiles these calls; its behavioral red is the lost
// reason at the actual corruption / socket failure. Lab records fail-before.
#include "report_sink_failure_fixture.hpp"
#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <dirent.h>

namespace {
using namespace cr150_w1;
namespace detail = pineforge::detail;
constexpr auto chunk = detail::ScratchArena::kChunkBytes;
constexpr const char* retired_id = "W1-retired-corrupted-cohort";

// The same explicit-instantiation access seam as test_id_adapter_storage: no
// changed class definition and no production friend. Used only to assert the
// setup assumption below (the target is on disk, not resident).
using Adapter = pineforge::source::PineExecutionAdapter;
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Cohorts { friend auto access(Cohorts); };
template struct Access<Cohorts, &Adapter::cohorts_by_id_>;

// Locate by the real AVL-node layout AND its unique key, never by an fd
// number/prefix or an English error. Both file mode and arena mode use the
// same 56-byte links + 40-byte CohortFacts::Record; offset zero is a sentinel.
bool poison_at(int fd, std::uint64_t base, const std::string& id) {
    constexpr auto links_bytes = 7 * sizeof(std::uint64_t);
    constexpr auto record_bytes = 5 * sizeof(std::uint64_t);
    std::array<std::uint64_t, 7> links{};
    if (::pread(fd, links.data(), links_bytes, static_cast<off_t>(base + 1))
            != static_cast<ssize_t>(links_bytes)
        || links[4] != id.size() || links[5] == 0 || links[5] > 128)
        return false;
    std::string key(id.size(), '\0');
    const auto key_offset = base + 1 + links_bytes + record_bytes;
    if (::pread(fd, key.data(), key.size(), static_cast<off_t>(key_offset))
            != static_cast<ssize_t>(key.size()) || key != id)
        return false;
    std::uint64_t handle = 0;
    const auto record_offset = base + 1 + links_bytes;
    require(::pread(fd, &handle, sizeof handle, static_cast<off_t>(record_offset))
                == static_cast<ssize_t>(sizeof handle)
                && handle != 0,
            "poison witness found a valid production cohort record");
    const std::uint64_t invalid_handle = 0;
    require(::pwrite(fd, &invalid_handle, sizeof invalid_handle, static_cast<off_t>(record_offset))
                == static_cast<ssize_t>(sizeof invalid_handle),
            "corrupt the real retired cohort handle");
    return true;
}

void poison_cohort(int arena_fd) {
    if (arena_fd >= 0) {
        const auto supplied = detail::ScratchArena::attached()->stats().bytes_supplied;
        for (std::uint64_t base = 0; base + chunk <= supplied; base += chunk)
            if (poison_at(arena_fd, base, retired_id)) return;
    } else {
#if defined(__linux__)
        DIR* descriptors = ::opendir("/proc/self/fd");
#else
        DIR* descriptors = ::opendir("/dev/fd");
#endif
        require(descriptors != nullptr, "inspect real open scratch descriptors");
        bool found = false;
        while (const auto* entry = ::readdir(descriptors)) {
            char* end = nullptr;
            const long number = std::strtol(entry->d_name, &end, 10);
            if (!*entry->d_name || *end || number < 0 || number > std::numeric_limits<int>::max())
                continue;
            struct stat status{};
            const int fd = static_cast<int>(number);
            if (::fstat(fd, &status) == 0 && S_ISREG(status.st_mode) && poison_at(fd, 0, retired_id)) {
                found = true;
                break;
            }
        }
        ::closedir(descriptors);
        if (found) return;
    }
    throw std::runtime_error("production retired cohort record was not materialized");
}

enum class Mode { corrupt_cohort, sealed_trade, cohort_exhaustion, exit_rank_exhaustion };

class Host final : public pineforge::source::PineStrategyHost {
public:
    Mode mode;
    Socket* socket = nullptr;
    int arena_fd = -1;
    int callbacks = 0;
    bool poisoned = false;
    std::string active;
    explicit Host(Mode selected_mode) : mode(selected_mode) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.process_orders_on_close = true;
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        // CR-347 round 3 made the implicit cohort spill amortised (once per
        // interval, 64 at least); this fixture needs the retired target on
        // disk after the all_sorted walk, as the eager spill used to leave it.
        // Only the spill timing changes; the codec rejection is still real.
        adapter_.fixture_cohort_implicit_spill_interval(1);
    }
    void on_source_bar(const pineforge::Bar&) override {
        const int index = pine_bar_index();
        ++callbacks;
        const auto na = std::numeric_limits<double>::quiet_NaN();
        if (mode == Mode::cohort_exhaustion) {
            // Header and value fit the first chunk, then this key needs one
            // additional chunk. The refusal must name the production cohort.
            strategy_entry(std::string(static_cast<std::size_t>(chunk), 'c'), true, na, na, 1.0);
            return;
        }
        if (mode == Mode::corrupt_cohort && index == 32) {
            // all_sorted()'s Range pins its first row even after the loop
            // iterator advances. A different cohort must sort first: then
            // advancing past our retired target spills it before poisoning.
            // Otherwise the next find() overwrites the poison with the valid
            // resident record before the real codec can see it.
            require(!active.empty() && active < retired_id,
                    "poison target must follow another cohort in hash order");
            pineforge::BrokerStateHashSink hash;
            adapter_.hash_state(hash);
            // Setup assumption, named so a later spill change fails HERE and
            // not as "callbacks=64": the corrupted record is the only copy.
            const auto& cohorts = adapter_.*access(Cohorts{});
            require(cohorts.find_resident(retired_id) == cohorts.end(),
                    "poison target must be spilled (not resident) before it is corrupted");
            poison_cohort(arena_fd);
            poisoned = true;
            // Mutation: replace row.handle == 0 with false in production
            // CohortFacts::restore_record; the checks below must turn red.
            strategy_entry(retired_id, true, na, na, 1.0);
            return;
        }
        if (index % 4 == 0) {
            if (index == 0) {
                active = mode == Mode::sealed_trade ? std::string(64 * 1024 + 1, 't') : retired_id;
                if (socket) socket->close_peer();
            } else {
                active = "W1-later-" + std::to_string(index);
            }
            strategy_entry(active, true, na, na, 1.0);
        } else if (index % 4 == 1) {
            strategy_close(active);
        }
    }
};

void cohort_restore_translator(bool arena) {
    Directory directory;
    Environment temporary("TMPDIR", directory.path.c_str());
    const auto path = directory.file("arena");
    Fd backing;
    if (arena) {
        backing.value = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        const auto supplied = 8 * chunk + 13;
        require(backing.value >= 0 && ::ftruncate(backing.value, static_cast<off_t>(supplied)) == 0,
                "create real cohort arena");
        detail::ScratchArena::attach(backing.value, supplied);
    }
    {
        Host host(Mode::corrupt_cohort);
        host.arena_fd = backing.value;
        run(host, tape(64));
        std::fprintf(stderr,
            "C21 cohort restore: backend=%s poisoned=%d callbacks=%d last_run_status=%d "
            "code=%s args=%s last_error=%s\n",
            arena ? "arena" : "file", host.poisoned, host.callbacks, host.last_run_status(),
            pineforge::run_failure_code_of(host), pineforge::run_failure_args_of(host),
            host.last_error().c_str());
        // No fill recalculations: callbacks at indices 0..32 total 32 + 1.
        // A real codec rejection at recurrence must prevent index 33.
        require(host.poisoned && host.callbacks == 33,
                "real cohort codec rejection must interrupt its recurrence");
        expect_engine_failure(host, "{}", pineforge::RunFailureCode::scratch_io_failed,
                              pineforge::RunFailureClass::resource);
    }
    if (arena) detail::ScratchArena::detach();
}

void sealed_trade_writer_translator() {
    Directory directory;
    Environment temporary("TMPDIR", directory.path.c_str());
    Socket socket;
    Host host(Mode::sealed_trade);
    host.socket = &socket;
    host.set_trade_history_streamable(true);
    install_fd(host, socket.writer.value);
    const auto bars = tape(64);
    run(host, bars);
    // The E lines of this short tape fit one buffer. The first final T's long
    // id forces a real write at sealing, before fill_report is ever called.
    require(host.callbacks >= 10 && host.callbacks < static_cast<int>(bars.size()),
            "sealed T must fail mid-run after the protected exit groups become final");
    expect_engine_failure(host, reason_args("sink_peer_closed"));
    require(::fcntl(socket.writer.value, F_GETFD) >= 0, "sealed T translator closed borrowed fd");
}

void production_table_identity(bool large_key) {
    Directory directory;
    Environment temporary("TMPDIR", directory.path.c_str());
    const auto path = directory.file("arena");
    Fd backing(::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600));
    const auto supplied = chunk + 17;
    require(backing.value >= 0 && ::ftruncate(backing.value, static_cast<off_t>(supplied)) == 0,
            "create real source exhaustion backing");
    detail::ScratchArena::attach(backing.value, supplied);
    {
        Host host(large_key ? Mode::cohort_exhaustion : Mode::exit_rank_exhaustion);
        run(host, tape(2));
        // A large cohort key spills into a second chunk during submission.
        // A short key fits; its fill then needs the separate exit-rank store.
        require(host.callbacks == 1, "production table must fail during its first entry");
        expect_engine_failure(host, exhaustion_args(supplied, chunk, chunk, chunk,
                                                    large_key ? "cohort" : "exit_rank"));
    }
    detail::ScratchArena::detach();
}

} // namespace

int main() {
    try {
        Environment arena_fd("PF_SCRATCH_ARENA_FD", nullptr);
        Environment arena_bytes("PF_SCRATCH_ARENA_BYTES", nullptr);
        Environment window("PF_SCRATCH_WINDOW_BYTES", nullptr);
        cohort_restore_translator(false);
        cohort_restore_translator(true);
        sealed_trade_writer_translator();
        production_table_identity(true);
        production_table_identity(false);
        std::puts("source sink reasons: actual cohort restore and sealed T failures checked");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_report_sink_failure_source: %s\n", error.what());
        return 1;
    }
}
