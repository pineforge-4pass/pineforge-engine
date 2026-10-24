// CR150-CX: all calls below exist on c7b06a6a19. Set A alone must compile.
// Ordinary comparisons are backend-neutral; jail/directory clauses are the
// behavioral RED witnesses on that base. Execution belongs to the Linux lab.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <pineforge/detail/id_pair_store.hpp>
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/native_order.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace pd = pineforge::detail;
namespace no = pineforge::native_order;
namespace {
using Sink = pineforge::BrokerStateHashSink;
using Status = pd::ScratchStatus;
using Minima = pd::IdPairStore<std::uint64_t>;
using Cycles = pd::IdPairStore<std::int64_t, true>;
using Pair = std::pair<std::string, std::optional<std::string>>;
constexpr std::array<std::uint64_t, 7> seeds{{
    0, 1, 255, 256, UINT64_C(0x8000000000000000), UINT64_MAX,
    UINT64_C(0xd1342543de82ef95)
}};
#if defined(__linux__)
bool fail_next_pread = false;
std::uint64_t refused_reads = 0;
#endif

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

// Exact bytes, rather than only a checksum, travel from child to parent.
struct Transcript {
    std::string bytes;
    void u(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            bytes.push_back(static_cast<char>(value >> shift));
    }
    void s(const std::string& value) { u(value.size()); bytes += value; }
};

struct Environment {
    std::string name, old;
    bool had;
    Environment(const char* key, const std::string& value) : name(key) {
        const auto* previous = std::getenv(key);
        had = previous != nullptr;
        if (had) old = previous;
        require(::setenv(key, value.c_str(), 1) == 0, "set fixture environment");
    }
    ~Environment() {
        if (had) ::setenv(name.c_str(), old.c_str(), 1);
        else ::unsetenv(name.c_str());
    }
};

struct Directory {
    std::string path;
    Directory() {
        std::array<char, 4096> cwd{};
        require(::getcwd(cwd.data(), cwd.size()) != nullptr, "read test working directory");
        path = std::string(cwd.data()) + "/pineforge-privmoves-XXXXXX";
        require(::mkdtemp(path.data()) != nullptr, "create isolated scratch directory");
    }
    ~Directory() { ::chmod(path.c_str(), 0700); ::rmdir(path.c_str()); }
};

struct Backing {
    int fd = -1;
    std::uint64_t bytes = 160 * pd::ScratchArena::kChunkBytes;
    Backing(const std::string& directory, bool sealed) {
#if defined(__linux__)
        if (sealed) fd = ::memfd_create("privmoves-arena", MFD_ALLOW_SEALING);
#else
        require(!sealed, "sealed backend is Linux-only");
#endif
        if (!sealed) {
            auto path = directory + "/arena-XXXXXX";
            fd = ::mkstemp(path.data());
            require(fd >= 0 && ::unlink(path.c_str()) == 0, "create inherited regular arena");
        }
        require(fd >= 0 && ::ftruncate(fd, static_cast<off_t>(bytes)) == 0,
                "pre-size backing outside jail");
#if defined(__linux__)
        if (sealed)
            require(::fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0,
                    "seal inherited arena size");
#endif
        require(::fcntl(fd, F_SETFD, 0) == 0, "retain backing across child exec");
    }
    ~Backing() { if (fd >= 0) ::close(fd); }
};

std::shared_ptr<pd::ScratchSlab> slab_for() {
    pd::ScratchManifest manifest;
    manifest.K_e = 256; manifest.B_e = 100000;
    manifest.K_c = 128; manifest.B_c = 100000;
    manifest.U = 512; manifest.B_U = 100000;
    manifest.V = 27; manifest.B_V = 10000;
    manifest.R = 512;
    return std::make_shared<pd::ScratchSlab>(manifest);
}

std::string id(std::size_t i) {
    // Common prefixes, binary bytes, and a prefix of another key.
    if (i == 0) return "";
    if (i == 1) return std::string("a\0", 2);
    if (i == 2) return std::string("a\0b", 3);
    if (i < 48) return std::string(193 + i, 'p'); // Nested terminators create a deep saved path.
    return std::string(193, 'p') + static_cast<char>(i) + std::to_string(i);
}

void check_cycles(const Cycles& store, const std::map<std::string, std::int64_t>& expected,
                  const std::vector<std::string>& order, Transcript& out) {
    std::map<std::string, std::int64_t> rows;
    std::vector<std::string> observed_order, expected_order;
    for (const auto& key : order) if (expected.count(key)) expected_order.push_back(key);
    for (const auto& row : store) {
        require(!row.from_entry && rows.emplace(row.first, row.second).second,
                "cycle iteration has unique single keys");
        observed_order.push_back(row.first);
        out.s(row.first); out.u(static_cast<std::uint64_t>(row.second));
    }
    require(rows == expected && store.size() == expected.size(), "cycle contents equal map oracle");
    require(observed_order == expected_order, "cycles preserve first-seen order through resurrection");
    for (const auto& row : expected)
        require(store.lookup(row.first) == row.second, "cycle lookup survives collisions and rehash");
    for (const auto seed : seeds) {
        Sink actual, oracle;
        actual.h = oracle.h = seed;
        store.fold_sorted(actual);
        oracle.u(expected.size());
        for (const auto& row : expected) { oracle.s(row.first); oracle.i(row.second); }
        require(actual.h == oracle.h, "seven incoming-seed fold hashes equal independent sorted bytes");
        out.u(actual.h);
    }
}

void tables(bool bounded, const std::string& directory, Transcript& out) {
    auto slab = bounded ? slab_for() : nullptr;
    Minima minima, memory_minima;
    Cycles cycles, memory_cycles;
    memory_minima.fixture_use_memory_reference();
    memory_cycles.fixture_use_memory_reference();
    minima.set_directory(directory); cycles.set_directory(directory);
    if (slab) {
        minima.bind_scratch_tables(slab, pd::ScratchResource::erased_data, pd::ScratchResource::erased_index);
        cycles.bind_scratch_tables(slab, pd::ScratchResource::consumed_data, pd::ScratchResource::consumed_index);
    }
    minima.fixture_force_hash_collisions(7); cycles.fixture_force_hash_collisions(7);
    std::map<Pair, std::uint64_t> expected;
    std::map<std::string, std::int64_t> cycle_expected;
    std::vector<Pair> order;
    std::vector<std::string> cycle_order;
    const auto checkpoint = [&] {
        std::map<Pair, std::uint64_t> rows;
        std::vector<Pair> seen, live_order;
        for (const auto& key : order) if (expected.count(key)) live_order.push_back(key);
        Sink state, reference;
        state.u(minima.size()); reference.u(expected.size());
        for (const auto& row : minima) {
            const Pair key{row.first, row.from_entry};
            require(rows.emplace(key, row.second).second, "minima iteration is unique");
            seen.push_back(key);
            out.s(row.first); out.u(row.from_entry.has_value());
            if (row.from_entry) out.s(*row.from_entry);
            out.u(row.second);
        }
        require(rows == expected && seen == live_order, "minima contents and first-seen order equal oracle");
        for (const auto& row : expected) {
            const auto& key = row.first;
            const auto got = key.second ? minima.lookup(key.first, *key.second) : minima.lookup(key.first);
            const auto old = key.second ? memory_minima.lookup(key.first, *key.second)
                                        : memory_minima.lookup(key.first);
            require(got == row.second && got == old, "single/pair minima equal map and memory reference");
            state.s(key.first); state.u(key.second.has_value());
            reference.s(key.first); reference.u(key.second.has_value());
            if (key.second) { state.s(*key.second); reference.s(*key.second); }
            state.u(*got); reference.u(row.second);
        }
        require(state.h == reference.h, "minima state digest equals independent reference");
        out.u(state.h);
        check_cycles(cycles, cycle_expected, cycle_order, out);
        for (const auto& row : cycle_expected)
            require(memory_cycles.lookup(row.first) == cycles.lookup(row.first),
                    "cycle values equal the existing in-memory implementation");
        for (const auto seed : seeds) {
            Sink actual, old; actual.h = old.h = seed;
            cycles.fold_sorted(actual); memory_cycles.fold_sorted(old);
            require(actual.h == old.h, "maintained fold equals existing memory-reference fold");
        }
    };
    for (std::size_t i = 0; i < 80; ++i) {
        const auto first = id(i), second = id(79 - i);
        for (const Pair key : {Pair{first, std::nullopt}, Pair{first, second}}) {
            order.push_back(key);
            const auto assign = [&](std::uint64_t value) {
                if (key.second) {
                    minima.min_assign(key.first, *key.second, value);
                    memory_minima.min_assign(key.first, *key.second, value);
                } else {
                    minima.min_assign(key.first, value); memory_minima.min_assign(key.first, value);
                }
                auto inserted = expected.emplace(key, value);
                if (!inserted.second) inserted.first->second = std::min(inserted.first->second, value);
            };
            assign(10000 + i); assign(20000 + i); assign(1000 - i);
        }
        const auto joined = first + '\x1f' + second;
        cycle_order.push_back(joined);
        const auto value = -static_cast<std::int64_t>(i) - 1;
        require(cycles.insert_or_assign(joined, value), "new joined cycle inserted");
        memory_cycles.insert_or_assign(joined, value); cycle_expected[joined] = value;
        if (i == 15 || i == 16 || i == 31 || i == 32 || i == 79) checkpoint();
    }
    const auto high_water = minima.disk_bytes();
    for (std::size_t i = 0; i < order.size(); i += 3) {
        const auto& key = order[i];
        require(key.second ? minima.erase(key.first, *key.second) : minima.erase(key.first),
                "erase a live minimum");
        if (key.second) memory_minima.erase(key.first, *key.second);
        else memory_minima.erase(key.first);
        expected.erase(key);
    }
    for (std::size_t i = 0; i < cycle_order.size(); i += 3) {
        require(cycles.erase(cycle_order[i]), "erase a live folded cycle");
        memory_cycles.erase(cycle_order[i]); cycle_expected.erase(cycle_order[i]);
    }
    checkpoint();
    for (std::size_t i = 0; i < order.size(); i += 3) {
        const auto& key = order[i];
        if (key.second) {
            minima.min_assign(key.first, *key.second, i);
            memory_minima.min_assign(key.first, *key.second, i);
        } else {
            minima.min_assign(key.first, i); memory_minima.min_assign(key.first, i);
        }
        expected[key] = i;
    }
    for (std::size_t i = 0; i < cycle_order.size(); i += 3) {
        const auto value = std::numeric_limits<std::int64_t>::min() + static_cast<std::int64_t>(i);
        cycles.insert_or_assign(cycle_order[i], value);
        memory_cycles.insert_or_assign(cycle_order[i], value); cycle_expected[cycle_order[i]] = value;
    }
    require(minima.disk_bytes() == high_water, "minima resurrection reuses existing records");
    Minima moved_minima(std::move(minima)); minima = std::move(moved_minima);
    Cycles moved_cycles(std::move(cycles)); cycles = std::move(moved_cycles);
    checkpoint();
    minima.clear(); cycles.clear();
    require(minima.empty() && cycles.empty(), "clear resets both moved tables");
    require(bounded ? high_water == 0 : high_water > 160 * 40,
            "witness really populated the selected table backend");
}

void facade(const std::string& directory, Transcript& out) {
    pd::IdPairScratchFile::FixtureIO io;
    pd::IdPairScratchFile file;
    bool failed = false;
    try { file.resize(1); }
    catch (const pd::IdPairStoreError& error) { failed = error.code().value() == EBADF; }
    require(failed, "resize before open preserves the historical auxiliary EBADF");
    file.fixture_count_io(&io); file.open(directory); file.resize(1024);
    std::array<unsigned char, 1024> bytes{};
    file.read(0, bytes.data(), bytes.size());
    require(std::all_of(bytes.begin(), bytes.end(), [](auto byte) { return byte == 0; }),
            "fresh sparse index bytes are zero");
    io.fail_write = io.writes + 1; io.partial_write = 7;
    const std::array<unsigned char, 12> input{{1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23}};
    failed = false;
    try { file.write(101, input.data(), input.size()); }
    catch (const pd::IdPairStoreError& error) {
        failed = error.code().value() == EIO
            && pineforge::classify_run_failure(error).code == pineforge::RunFailureCode::scratch_io_failed
            && !pineforge::classify_run_failure(error).args;
    }
    require(failed && io.writes == 1 && io.write_bytes == 7, "actual fixture prefix and counters preserved");
    io.fail_write = 0;
    pd::IdPairScratchFile moved(std::move(file));
    require(!file.opened() && moved.fixture_io() == &io && file.fixture_io() == nullptr,
            "move transfers backing and observer exactly once");
    moved.read(101, bytes.data(), input.size());
    require(std::equal(input.begin(), input.begin() + 7, bytes.begin())
                && std::all_of(bytes.begin() + 7, bytes.begin() + 12, [](auto b) { return b == 0; }),
            "failed prefix contains the real input bytes only");
    failed = false;
    try { moved.read(1024, bytes.data(), 1); }
    catch (const pd::IdPairStoreError& error) { failed = error.code().value() == EIO; }
    require(failed, "read past logical end remains EIO in every backend");
    failed = false;
    try { moved.write(static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()), input.data(), 1); }
    catch (const pd::IdPairStoreError& error) { failed = error.code().value() == EFBIG; }
    require(failed, "offset overflow preserves IdPairStoreError and EFBIG");
    moved.write(101, input.data(), input.size()); moved.resize(2048);
    moved.read(101, bytes.data(), input.size());
    require(std::equal(input.begin(), input.end(), bytes.begin()), "growth and retry preserve bytes");
    out.bytes.append(reinterpret_cast<const char*>(bytes.data()), input.size());
    moved.reset(); require(!moved.opened(), "reset releases moved backing");
}

void recovery(const std::string& directory, Transcript& out) {
    // Fail every real write boundary, including undo before-images and the
    // deferred header/slot flush. Each trial checks the independently held
    // committed bytes and seven-seed fold before retrying.
    for (unsigned operation = 0; operation != 3; ++operation) {
        bool success = false, saw_failure = false;
        for (std::uint64_t fail = 1; !success; ++fail) {
            require(fail < 1024, "short-write enumeration must reach a successful operation");
            pd::IdPairScratchFile::FixtureIO io;
            Cycles store;
            store.set_directory(directory); store.fixture_count_io(&io);
            std::map<std::string, std::int64_t> expected;
            std::vector<std::string> order;
            const std::size_t population = operation == 0 ? 16 : 40;
            for (std::size_t i = 0; i < population; ++i) {
                order.push_back(id(i)); expected[id(i)] = static_cast<std::int64_t>(i);
                store.insert_or_assign(id(i), static_cast<std::int64_t>(i));
            }
            const auto key = operation == 0 ? id(80) : id(7);
            const auto mutate = [&] {
                if (operation == 2) store.erase(key);
                else store.insert_or_assign(key, -711);
            };
            io.fail_write = io.writes + fail; io.partial_write = fail % 2 ? 7 : 1025;
            try { mutate(); success = true; }
            catch (const pd::IdPairStoreError& error) {
                require(error.code().value() == EIO, "partial mutation reports exact injected errno");
                saw_failure = true;
            }
            io.fail_write = 0;
            Transcript before;
            if (!success) {
                check_cycles(store, expected, order, before);
                Cycles moved(std::move(store)); store = std::move(moved);
                io.fail_write = io.writes + 1;
                bool failed_again = false;
                try { mutate(); }
                catch (const pd::IdPairStoreError& error) { failed_again = error.code().value() == EIO; }
                require(failed_again, "retry can fail again during recovery or pending flush");
                io.fail_write = 0;
                Transcript after;
                check_cycles(store, expected, order, after);
                require(before.bytes == after.bytes, "repeated recovery failure preserves complete state");
                mutate();
            }
            if (operation == 2) expected.erase(key);
            else {
                if (!expected.count(key)) order.push_back(key);
                expected[key] = -711;
            }
            Transcript final;
            check_cycles(store, expected, order, final);
            // The same final contents must result at EVERY failure boundary.
            out.s(final.bytes);
        }
        require(saw_failure, "journal test actually injected a short write");
    }
}

template<std::size_t Size>
void segments(const std::string& directory, Transcript& out) {
    using Record = std::array<unsigned char, Size>;
    pd::IdPairFoldFile<Record, 4095> file;
    Record row{};
    for (std::uint64_t i = 0; i < 64; ++i) {
        for (std::size_t j = 0; j < Size; ++j) row[j] = static_cast<unsigned char>(i * 37 + j * 19);
        file.write(i, &row, directory);
    }
    auto moved = std::move(file);
    for (std::uint64_t i = 0; i < 64; ++i) {
        moved.read(i, &row);
        for (std::size_t j = 0; j < Size; ++j)
            require(row[j] == static_cast<unsigned char>(i * 37 + j * 19),
                    "small-offset segment preserves every original record byte");
        out.bytes.append(reinterpret_cast<const char*>(row.data()), row.size());
    }
    moved.clear(); require(!moved.opened(), "every lazy segment is released");
}

no::Request root_request() {
    no::Request request{no::Transact{1.0}, "moving", ""};
    request.trigger = no::Limit{90.0};
    return request;
}

void roots(bool bounded, const std::string& directory, Transcript& out) {
    no::WorkingRequestCore core{{"privmoves-roots", 1}};
    core.set_successor_roots_directory(directory);
    auto slab = bounded ? slab_for() : nullptr;
    if (slab) core.bind_scratch_slab(slab);
    std::uint64_t incarnation = 1, ordinal = 1;
    const auto retire = [&] { core.retire_history(core.last_ordinal()); };
    std::array<no::RequestHandle, 3> original{}, tails{}, oldest{};
    std::map<std::uint64_t, std::uint64_t> canonical;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> expected_rows;
    for (auto& root : original) {
        const auto expected_incarnation = std::max(incarnation, core.last_incarnation() + 1);
        auto result = core.apply_submit(root_request(), {}, incarnation, ordinal);
        require(result.result.status == no::SubmitStatus::Accepted && result.result.handle,
                "root submit accepted");
        root = *result.result.handle;
        require(root.incarnation == expected_incarnation, "root uses the supplied next incarnation");
        canonical.emplace(expected_incarnation, expected_incarnation);
        ++incarnation; ordinal += result.events.count; retire();
    }
    tails = original;
    for (std::size_t i = 0; i < 211; ++i) {
        const auto slot = i % tails.size();
        const auto previous = tails[slot];
        const auto expected_incarnation = std::max(incarnation, core.last_incarnation() + 1);
        no::ReplaceOptions options; options.keep_handle = i == 70 || i == 140;
        no::ReplaceResult result;
        if (i % 2) result = core.apply_replace(tails[slot], root_request(), {}, incarnation, ordinal, options).result;
        else {
            auto prepared = core.prepare_replace(tails[slot], root_request(), {}, incarnation, ordinal, options);
            auto installed = core.install_replace(std::move(prepared));
            require(std::holds_alternative<no::CommandInstalled<no::ReplaceResult>>(installed),
                    "prepared root replacement installed");
            result = std::get<no::CommandInstalled<no::ReplaceResult>>(std::move(installed)).result;
        }
        require(result.status == no::ReplaceStatus::Replaced && result.successor, "root replacement accepted");
        if (options.keep_handle) require(*result.successor == tails[slot], "kept handle has no new root row");
        else {
            require(result.successor->incarnation == expected_incarnation,
                    "replacement uses the supplied next incarnation");
            const auto root = canonical.at(previous.incarnation);
            canonical.emplace(expected_incarnation, root);
            expected_rows.emplace_back(expected_incarnation, root);
        }
        tails[slot] = *result.successor;
        if (i < 3) oldest[slot] = tails[slot];
        out.u(tails[slot].incarnation); out.u(core.last_incarnation()); out.u(core.last_ordinal());
        ++incarnation; ordinal = core.last_ordinal() + 1; retire();
    }
    const auto cohort = core.cohort_open();
    require(expected_rows.size() == 209, "two kept handles add no successor rows to the oracle");
    if (slab) {
        std::array<std::uint64_t, 192 * 2> prefix{};
        require(slab->region(pd::ScratchResource::successor_roots).logical_extent == 192 * 16,
                "slab witness crossed three complete root-tail batches");
        slab->read_at(pd::ScratchResource::successor_roots, 0, prefix.data(), sizeof(prefix));
        for (std::size_t i = 0; i < 192; ++i)
            require(prefix[2 * i] == expected_rows[i].first && prefix[2 * i + 1] == expected_rows[i].second,
                    "every slab prefix row equals the independent 16-byte root-pair oracle");
    }
    core.cohort_add(cohort, tails[1]);
    // Include the oldest on-disk successor and the current 64-row RAM tail.
    for (std::size_t i = 0; i < 3; ++i) {
        for (const auto handle : {original[i], oldest[i], tails[i]}) {
            const auto member = core.cohort_contains(cohort, handle);
            require(member == (i == 1), "spilled old-root lookup equals independent chain membership");
            out.u(handle.incarnation); out.u(member);
        }
        require(core.find_live(tails[i]) != nullptr, "current root tail remains live");
    }
    // Hash the complete semantic membership readback with the real state
    // sink, independently constructing each oracle bit from the operation map.
    for (const auto seed : seeds) {
        Sink actual, oracle; actual.h = oracle.h = seed;
        for (const auto& row : canonical) {
            const no::RequestHandle handle{original[0].run, row.first};
            const auto expected = row.second == original[1].incarnation;
            const auto observed = core.cohort_contains(cohort, handle);
            require(observed == expected, "every issued successor agrees with independent root map");
            actual.u(row.first); actual.u(observed);
            oracle.u(row.first); oracle.u(expected);
        }
        require(actual.h == oracle.h, "successor-root state digest equals independent chain oracle");
        out.u(actual.h);
    }
    core.cohort_remove(cohort, oldest[1]);
    require(!core.cohort_contains(cohort, tails[1]), "old disk successor removes canonical root");
    core.cohort_add(cohort, tails[2]);
    no::WorkingRequestCore moved(std::move(core));
    no::WorkingRequestCore assigned{{"discard", 1}};
    assigned = std::move(moved);
    require(assigned.cohort_contains(cohort, oldest[2]), "move keeps disk roots and authoritative tail");
    out.u(assigned.last_incarnation()); out.u(assigned.last_ordinal()); out.u(assigned.history_end());
    assigned.reset({"privmoves-roots", 2});
    require(!assigned.cohort_contains(cohort, oldest[2]), "reset forgets previous root history");
}

struct Openings : no::OpeningObservationReadback {
    no::RunIdentity run{"privmoves-openings", 1};
    std::uint64_t size = 129;
    std::uint64_t fail_at = UINT64_MAX;
    bool duplicate = false;
    std::vector<std::uint64_t> words;
    Openings() { populate(size); }
    void populate(std::uint64_t count) {
        size = count; words.clear();
        for (std::uint64_t i = 0; i < count; ++i) words.push_back((i * 37) % count + 1);
    }
    std::vector<std::uint64_t> sorted_reference() const {
        auto sorted = words; std::sort(sorted.begin(), sorted.end()); return sorted;
    }
    std::uint64_t count() const noexcept override { return size; }
    Status observation_at(std::uint64_t i, no::OpeningObservation& out) const noexcept override {
        if (i >= size) return Status::not_found;
        out = {{run, i + 1}, 7, no::PositionNonflat{7, no::Side::Long}, true};
        return Status::ok;
    }
    static Status read(void* context, std::uint64_t i, no::RequestHandle& out) {
        auto& input = *static_cast<Openings*>(context);
        if (i == input.fail_at) return Status::io_error;
        if (i >= input.size) return Status::not_found;
        out = {input.run, input.duplicate ? input.words.front() : input.words[static_cast<std::size_t>(i)]};
        return Status::ok;
    }
};

void read_frozen(const no::OpeningReadback& frozen, const no::RunIdentity& run,
                 const std::vector<std::uint64_t>& expected, Transcript& out) {
    const auto count = static_cast<std::uint64_t>(expected.size());
    require(frozen.count() == count, "frozen membership count preserved");
    for (std::uint64_t i = 0; i < count; ++i) {
        no::RequestHandle value;
        require(frozen.opening_at(i, value) == Status::ok
                    && value == (no::RequestHandle{run, expected[static_cast<std::size_t>(i)]}),
                "unsorted external membership equals independently sorted vector");
        out.u(value.incarnation);
    }
    no::RequestHandle unchanged{run, 991};
    require(frozen.opening_at(count, unchanged) == Status::not_found && unchanged.incarnation == 991,
            "frozen failed read preserves output and not_found");
    for (const auto seed : seeds) {
        Sink actual, oracle; actual.h = oracle.h = seed;
        actual.u(frozen.count()); oracle.u(count);
        for (std::uint64_t i = 0; i < count; ++i) {
            no::RequestHandle value;
            require(frozen.opening_at(i, value) == Status::ok, "read every word for membership digest");
            actual.u(value.incarnation); oracle.u(expected[static_cast<std::size_t>(i)]);
        }
        require(actual.h == oracle.h, "opening membership digest equals independent sorted-vector bytes");
        out.u(actual.h);
    }
}

void openings(bool bounded, const std::string& directory, Transcript& out) {
    Openings input;
    std::vector<std::shared_ptr<const no::OpeningReadback>> retained;
    std::vector<std::vector<std::uint64_t>> references;
    for (const auto count : {UINT64_C(1), UINT64_C(17), UINT64_C(129)}) {
        input.populate(count);
        no::OpeningInputCursor cursor(count, &input, Openings::read);
        std::shared_ptr<const no::OpeningReadback> frozen;
        require(no::freeze_openings(input.run, cursor, frozen) == Status::ok && frozen,
                "external membership freeze succeeds");
        retained.push_back(std::move(frozen));
        references.push_back(input.sorted_reference());
    }
    input.populate(129); input.fail_at = 64;
    no::OpeningInputCursor broken(input.size, &input, Openings::read);
    auto output = retained.back();
    require(no::freeze_openings(input.run, broken, output) == Status::io_error
                && output == retained.back(), "failed source read preserves retained owner");
    input.fail_at = UINT64_MAX; input.duplicate = true;
    require(no::freeze_openings(input.run, broken, output) == Status::programming_error
                && output == retained.back(), "duplicate rejection preserves retained owner");
    input.duplicate = false;
    {
        no::WorkingRequestCore core(input.run);
        core.set_successor_roots_directory(directory);
        if (bounded) core.bind_scratch_slab(slab_for());
        auto cursor = std::make_shared<no::OpeningInputCursor>(input.size, &input, Openings::read);
        no::Request request{no::Reduce{no::ExplicitUnits{1}}, "close", "frozen"};
        request.owner = no::BindOpenings{{}, 7, cursor};
        no::CommandContext command; command.external_openings = &input;
        std::uint64_t incarnation = 1000, ordinal = 1;
        auto installed = core.apply_submit(request, command, incarnation, ordinal);
        require(installed.result.status == no::SubmitStatus::Accepted && installed.result.handle,
                "core freezes external owner");
        const auto* live = core.find_live(*installed.result.handle);
        require(live != nullptr, "frozen request remains live");
        const auto& owner = std::get<no::BindOpenings>(live->request().owner);
        const auto& authority = std::get<no::OpeningsClose>(live->authority);
        require(owner.external_openings && owner.external_openings == authority.external_openings
                    && owner.external_openings != cursor, "definition and authority share one frozen file");
        retained.push_back(owner.external_openings);
        references.push_back(input.sorted_reference());
        core.reset({"privmoves-openings", 2});
    }
    // All source callbacks now fail, and the owning core is gone.
    input.fail_at = 0;
    for (std::size_t i = 0; i < retained.size(); ++i)
        read_frozen(*retained[i], input.run, references[i], out);
    auto shared = retained.back();
    retained.clear();
    read_frozen(*shared, input.run, references.back(), out);
#if defined(__linux__)
    no::RequestHandle unchanged{input.run, 999};
    const auto before = refused_reads;
    fail_next_pread = true;
    require(shared->opening_at(64, unchanged) == Status::io_error && unchanged.incarnation == 999,
            "a real backing read error preserves status and caller output");
    require(!fail_next_pread && refused_reads == before + 1,
            "backing read fault was consumed by the actual frozen store");
    read_frozen(*shared, input.run, references.back(), out);
#endif
}

Transcript sequence(const std::string& which, bool bounded, const std::string& directory) {
    Transcript out;
    if (which == "tables") { tables(bounded, directory, out); facade(directory, out); }
    else if (which == "fold") {
        // Exercise the real slab-fold branch when requested. The standalone
        // journal fault/segment fixtures below intentionally have no slab.
        tables(bounded, directory, out);
        recovery(directory, out);
        segments<136>(directory, out); segments<2088>(directory, out); segments<2096>(directory, out);
    } else if (which == "roots") roots(bounded, directory, out);
    else if (which == "openings") openings(bounded, directory, out);
    else throw std::runtime_error("unknown privmoves store");
    require(!out.bytes.empty(), "a witness must return actual observed bytes");
    return out;
}

bool track_creations = false, refuse_creations = false;
std::vector<std::string> created_paths;
std::uint64_t creation_refusals = 0;
[[maybe_unused]] bool observe_creation(const char* path) {
    if (track_creations) created_paths.emplace_back(path);
    if (!refuse_creations) return false;
    ++creation_refusals; errno = EACCES; return true;
}

std::set<std::string> tmp_entries() {
    DIR* directory = ::opendir("/tmp");
    require(directory != nullptr, "list /tmp before/after CR-260");
    std::set<std::string> entries;
    while (const auto* entry = ::readdir(directory)) {
        // Other processes share /tmp; only this backend's names are relevant.
        if (std::string(entry->d_name).find("pineforge-openings-") == 0) entries.insert(entry->d_name);
    }
    ::closedir(directory);
    return entries;
}

void compare(const std::string& which) {
    Directory scratch;
    Environment temporary("TMPDIR", scratch.path);
    const auto file = sequence(which, false, scratch.path);
    const auto slab = sequence(which, true, scratch.path);
    require(file.bytes == slab.bytes, "file/slab transcripts differ");
    for (const bool sealed : {false
#if defined(__linux__)
                              , true
#endif
    }) {
        Backing backing(scratch.path, sealed);
        pd::ScratchArena::attach(backing.fd, backing.bytes);
        const auto arena = sequence(which, false, scratch.path);
        const auto arena_slab = sequence(which, true, scratch.path);
        require(file.bytes == arena.bytes && file.bytes == arena_slab.bytes,
                "file/arena/slab exact contents, order and hash transcripts differ");
        require(pd::ScratchArena::attached()->stats().bytes_used == 0, "all moved owners release arena claims");
        pd::ScratchArena::detach();
    }
}

void directory_policy() {
    Directory scratch;
    require(scratch.path.compare(0, 5, "/tmp/") != 0, "CR-260 needs a non-/tmp directory");
    const auto before = tmp_entries();
    {
        Environment temporary("TMPDIR", scratch.path);
        created_paths.clear(); track_creations = true;
        Transcript out; openings(false, scratch.path, out);
        track_creations = false;
#if defined(__linux__)
        require(std::count_if(created_paths.begin(), created_paths.end(), [](const auto& path) {
                    return path.find("/pineforge-openings-") != std::string::npos;
                }) == 12, "observer sees both files for all six opening freezes");
        for (const auto& path : created_paths)
            require(path.compare(0, scratch.path.size() + 1, scratch.path + "/") == 0,
                    "CR-260: opening file bypassed configured non-/tmp scratch directory");
#endif
    }
    // Core directory wins over a deliberately unusable TMPDIR.
    {
        Environment temporary("TMPDIR", scratch.path + "/missing");
        Openings input;
        no::WorkingRequestCore core(input.run);
        core.set_successor_roots_directory(scratch.path);
        no::Request request{no::Reduce{no::ExplicitUnits{1}}, "close", "directory"};
        request.owner = no::BindOpenings{{}, 7,
            std::make_shared<no::OpeningInputCursor>(input.size, &input, Openings::read)};
        no::CommandContext command; command.external_openings = &input;
        std::uint64_t incarnation = 1000, ordinal = 1;
        created_paths.clear(); track_creations = true;
        const auto installed = core.apply_submit(request, command, incarnation, ordinal);
        track_creations = false;
        require(installed.result.status == no::SubmitStatus::Accepted, "configured core directory freeze succeeds");
#if defined(__linux__)
        require(std::count_if(created_paths.begin(), created_paths.end(), [](const auto& path) {
                    return path.find("/pineforge-openings-") != std::string::npos;
                }) == 2, "core freeze creates exactly retained and work opening files");
        for (const auto& path : created_paths)
            require(path.compare(0, scratch.path.size() + 1, scratch.path + "/") == 0,
                    "CR-260: core opening freeze ignored explicit scratch directory");
#endif
    }
    require(tmp_entries() == before, "CR-260 leaves no new opening file under /tmp");
}

#if defined(__linux__)
void child_write(int fd, const std::string& bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto count = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "write complete child transcript to pipe");
        done += static_cast<std::size_t>(count);
    }
}

void jailed_child(const std::string& which, bool bounded, int backing_fd,
                  std::uint64_t bytes, int output_fd, const std::string& locked) {
    Environment temporary("TMPDIR", locked);
    Environment commands("PINEFORGE_COMMAND_SCRATCH_DIR", locked);
    require(::chdir(locked.c_str()) == 0, "enter non-writable jail directory");
    struct stat directory{};
    require(::stat(".", &directory) == 0 && (directory.st_mode & 0222) == 0,
            "every configured jail directory has no write bits");
    if (::geteuid() == 0) {
        require(::setgroups(0, nullptr) == 0 && ::setgid(65534) == 0 && ::setuid(65534) == 0,
                "root test drops privilege so directory permissions are effective");
    }
    require(::access(".", W_OK) != 0, "jailed process truly has no writable working directory");
    require(std::signal(SIGXFSZ, SIG_IGN) != SIG_ERR, "ignore SIGXFSZ in actual child");
    struct rlimit limit{0, 0};
    require(::setrlimit(RLIMIT_FSIZE, &limit) == 0, "set actual child RLIMIT_FSIZE=0");
    struct rlimit observed{};
    require(::getrlimit(RLIMIT_FSIZE, &observed) == 0 && observed.rlim_cur == 0 && observed.rlim_max == 0,
            "zero soft and hard file-size limits are effective");
    errno = 0;
    const unsigned char byte = 19;
    require(::pwrite(backing_fd, &byte, 1, 0) == -1 && errno == EFBIG,
            "kernel refuses even an in-size write on inherited backing");
    pd::ScratchArena::attach(backing_fd, bytes);
    refuse_creations = true;
    auto probe = locked + "/must-refuse-empty-XXXXXX";
    require(::mkstemp(probe.data()) == -1 && errno == EACCES && creation_refusals == 1,
            "jail observes and refuses creation even before a file could grow");
    creation_refusals = 0; // Only the explicit observer self-test is discounted.
    const auto result = sequence(which, bounded, locked);
    require(creation_refusals == 0, "arena-attached store attempted private file creation");
    refuse_creations = false;
    if (!bounded || which != "roots")
        require(pd::ScratchArena::attached()->stats().high_water > 0,
                "jailed witness actually claimed arena chunks");
    require(pd::ScratchArena::attached()->stats().bytes_used == 0,
            "jailed sequence releases all retained owners");
    pd::ScratchArena::detach();
    child_write(output_fd, result.bytes);
}

void jail(const std::string& which) {
    Directory scratch, locked;
    require(::chmod(locked.path.c_str(), 0555) == 0, "remove every jail-directory write bit");
    Environment temporary("TMPDIR", scratch.path);
    for (const bool bounded : {false, true}) {
        const auto ordinary = sequence(which, bounded, scratch.path);
        for (const bool sealed : {false, true}) {
            Backing backing(scratch.path, sealed);
            int pipe_fds[2]{};
            require(::pipe(pipe_fds) == 0, "create transcript pipe");
            const auto child = ::fork();
            require(child >= 0, "fork real jail witness");
            if (child == 0) {
                ::close(pipe_fds[0]);
                const auto fd = std::to_string(backing.fd), size = std::to_string(backing.bytes);
                const auto output = std::to_string(pipe_fds[1]);
                ::execl("/proc/self/exe", "test_privmoves", "--jailed", which.c_str(),
                        bounded ? "slab" : "file", fd.c_str(), size.c_str(), output.c_str(),
                        locked.path.c_str(), static_cast<char*>(nullptr));
                ::_exit(120);
            }
            ::close(pipe_fds[1]);
            std::string actual;
            std::array<char, 8192> buffer{};
            for (;;) {
                const auto count = ::read(pipe_fds[0], buffer.data(), buffer.size());
                if (count < 0 && errno == EINTR) continue;
                require(count >= 0, "read child transcript");
                if (!count) break;
                actual.append(buffer.data(), static_cast<std::size_t>(count));
            }
            ::close(pipe_fds[0]);
            int status = 0; pid_t waited;
            do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
            require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                    "jailed child must complete all semantic checks");
            require(actual == ordinary.bytes, "jailed child differs from parent's complete ordinary transcript");
        }
    }
}
#endif
} // namespace

#if defined(__linux__)
extern "C" int __real_mkstemp(char*);
extern "C" int __real_open(const char*, int, ...);
extern "C" int __real_openat(int, const char*, int, ...);
extern "C" ssize_t __real_pread(int, void*, std::size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* destination, std::size_t length, off_t offset) {
    if (std::exchange(fail_next_pread, false)) {
        ++refused_reads; errno = EIO; return -1;
    }
    return __real_pread(fd, destination, length, offset);
}
extern "C" int __wrap_mkstemp(char* path) {
    if (observe_creation(path)) return -1;
    return __real_mkstemp(path);
}
extern "C" int __wrap_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args);
    }
    if (((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) && observe_creation(path)) return -1;
    return __real_open(path, flags, mode);
}
extern "C" int __wrap_openat(int directory, const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args);
    }
    if (((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) && observe_creation(path)) return -1;
    return __real_openat(directory, path, flags, mode);
}
#endif

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            compare("tables"); compare("fold"); compare("roots"); compare("openings");
        }
        else if (argc == 2 && std::string(argv[1]) == "directory") directory_policy();
        else if (argc == 3 && std::string(argv[1]) == "compare") compare(argv[2]);
#if defined(__linux__)
        else if (argc == 3 && std::string(argv[1]) == "jail") jail(argv[2]);
        else if (argc == 8 && std::string(argv[1]) == "--jailed")
            jailed_child(argv[2], std::string(argv[3]) == "slab", std::stoi(argv[4]),
                         std::stoull(argv[5]), std::stoi(argv[6]), argv[7]);
#endif
        else throw std::runtime_error("invalid privmoves test arguments");
        std::puts("test_privmoves: complete byte, order, minima and seven-seed hash comparisons");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_privmoves %s: %s\n", argc > 1 ? argv[1] : "compare", error.what());
        return 1;
    }
}
