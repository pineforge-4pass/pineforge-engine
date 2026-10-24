// Edit-only specification; compile/run and the frozen-header diagnostic belong
// to the lab. The companion POOC unit also compiles on frozen2 and is a runtime
// RED there. This unit exercises the new cache admission/budget API directly.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "../src/command_scratch_cache.hpp"
#include "../src/command_scratch_production.hpp"
#include "../src/source/exit_scratch_versions_production.hpp"
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/detail/scratch_store.hpp>
#include <pineforge/source/pine_adapter.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
namespace pd = pineforge::detail;
namespace sd = pineforge::source::detail;
using Cache = pd::CachedScratchBacking;
using Status = pd::ScratchStatus;
using Bytes = std::vector<std::byte>;
constexpr std::size_t page_bytes = 512;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void ok(Status status, const char* message) { require(status == Status::ok, message); }
pd::ScratchBytes span(const Bytes& value) { return {value.data(), value.size()}; }
pd::ScratchBuffer buffer(Bytes& value) { return {value.data(), value.size()}; }
Bytes pattern(std::size_t size, unsigned seed) {
    Bytes result(size);
    for (std::size_t i = 0; i < size; ++i)
        result[i] = static_cast<std::byte>((i * 37 + seed * 19) % 251);
    return result;
}

// Caller-owned fixed medium and fault controls outlive the decorated backing.
// This is an ordering/fault double, not a claim of durable test-memory storage.
// All operation bodies copy actual bytes; none allocate or throw.
struct Medium {
    const std::size_t page;
    Bytes bytes;
    std::uint64_t extent = 0, reads = 0, writes = 0, syncs = 0, failed_writes = 0;
    std::uint64_t writes_at_sync = 0, free_writes = 0;
    pd::ScratchDurability last_sync = pd::ScratchDurability::buffered;
    bool fail_write = false, short_write = false, fail_sync = false;
    bool carry = true, failure_active = false;
    pd::ReportScratchIoError failure{ENOSPC, "deferred command page write"};
    explicit Medium(std::size_t size = page_bytes, std::size_t pages = 4096)
        : page(size), bytes(size * pages) {}
    bool equals(std::size_t id, const Bytes& expected) const {
        return expected.size() == page
            && std::memcmp(bytes.data() + id * page, expected.data(), page) == 0;
    }
};

class Backing final : public pd::ScratchBacking {
public:
    Medium& medium;
    explicit Backing(Medium& value) : ScratchBacking(pd::ScratchBackingKind::test_memory, value.page),
                                      medium(value) {}
    std::uint64_t page_count() const noexcept override { return medium.extent; }
    const pd::ReportScratchIoError* carried_failure() const noexcept override {
        return medium.carry && medium.failure_active ? &medium.failure : nullptr;
    }
    Status admit(pd::ScratchPageId id) noexcept {
        medium.failure_active = false;
        if (id >= medium.bytes.size() / medium.page) return Status::programming_error;
        if (id >= medium.extent) {
            const auto begin = static_cast<std::size_t>(medium.extent) * medium.page;
            const auto end = static_cast<std::size_t>(id + 1) * medium.page;
            std::memset(medium.bytes.data() + begin, 0, end - begin);
            medium.extent = id + 1;
        }
        return Status::ok;
    }
    static Status admission(pd::ScratchBacking& backing, pd::ScratchPageId id) noexcept {
        return static_cast<Backing&>(backing).admit(id);
    }
protected:
    Status do_read_page(pd::ScratchPageId id, pd::ScratchBuffer out) const noexcept override {
        ++medium.reads;
        medium.failure_active = false;
        if (id >= medium.extent) return Status::not_found;
        std::memcpy(out.data, medium.bytes.data() + id * medium.page, medium.page);
        return Status::ok;
    }
    Status do_write_page(pd::ScratchPageId id, pd::ScratchBytes in) noexcept override {
        ++medium.writes;
        const auto admitted = admit(id);
        if (admitted != Status::ok) return admitted;
        if (medium.fail_write) {
            ++medium.failed_writes;
            medium.failure_active = true;
            if (medium.short_write)
                std::memcpy(medium.bytes.data() + id * medium.page, in.data, medium.page / 2);
            return Status::io_error;
        }
        std::memcpy(medium.bytes.data() + id * medium.page, in.data, medium.page);
        medium.free_writes += in.data[0] == std::byte{8} && in.data[1] == std::byte{0}
            && in.data[2] == std::byte{0} && in.data[3] == std::byte{0};
        return Status::ok;
    }
    Status do_sync(pd::ScratchDurability durability) noexcept override {
        ++medium.syncs;
        medium.writes_at_sync = medium.writes;
        medium.last_sync = durability;
        medium.failure_active = medium.fail_sync;
        return medium.fail_sync ? Status::io_error : Status::ok;
    }
};

struct Charges {
    std::array<std::uint64_t, 3> calls{}, bytes{};
    bool failed = false;
    pd::ScratchIoObserver observer() { return {this, &receive}; }
    static void receive(void* context, const pd::ScratchIoCharge& io) noexcept {
        auto& self = *static_cast<Charges*>(context);
        const auto op = static_cast<std::size_t>(io.operation);
        if (op >= self.calls.size()) { self.failed = true; return; }
        ++self.calls[op];
        self.bytes[op] += io.bytes;
        self.failed |= io.kind != pd::ScratchBackingKind::test_memory || io.status != Status::ok;
    }
};
struct NoAllocation {
    const std::size_t before = global_allocation::allocations;
    const bool previous = global_allocation::refusing;
    NoAllocation() { global_allocation::refusing = true; }
    ~NoAllocation() { global_allocation::refusing = previous; }
};

void budget_accounting() {
    for (const std::size_t geometry : {std::size_t{512}, std::size_t{4096},
                                       std::size_t{16384}, std::size_t{1024 * 1024}}) {
        const auto normal = Cache::default_entries(geometry), maximum = Cache::max_entries(geometry);
        require(normal > 0 && normal <= maximum, "default budget is admissible");
        for (const auto entries : {std::size_t{2}, normal, maximum}) {
            Medium medium(geometry, 4);
            auto inner = std::make_unique<Backing>(medium);
            const auto live_before = global_allocation::live_bytes;
            const auto allocations_before = global_allocation::allocations;
            global_allocation::recording = true;
            {
                Cache cache(std::move(inner), entries, {}, &Backing::admission);
                // Measure the allocator, independently of the accounting getter.
                const auto measured = global_allocation::live_bytes - live_before;
                const auto formula = Cache::resident_bytes(entries, geometry);
                require(global_allocation::allocations == allocations_before + 1
                            && measured == cache.allocated_bytes()
                            && sizeof(Cache) + measured == formula
                            && formula <= Cache::kMaxResidentBytes
                            && cache.resident_entries() == entries
                            && Cache::slot_metadata_bytes() >= sizeof(pd::ScratchPageId) + sizeof(bool),
                        "measured small/default/max resident bytes exceed the published formula");
                const auto allocations = global_allocation::allocations;
                // Buffers are already caller-owned; even first admission, hits
                // and explicit drain must work with the allocator refusing.
                Bytes image(geometry, std::byte{0x4b}), out(geometry);
                Status write, read, sync;
                {
                    NoAllocation no_allocation;
                    write = cache.write_page(0, span(image));
                    read = cache.read_page(0, buffer(out));
                    sync = cache.drain();
                }
                ok(write, "budget write"); ok(read, "budget hit"); ok(sync, "budget drain");
                require(out == image && medium.writes == 1
                            && global_allocation::allocations == allocations + 2,
                        "cache I/O allocated or failed to move real bytes");
            }
            require(global_allocation::live_bytes == live_before, "cache/buffer allocation leaked");
            global_allocation::recording = false;
        }
    }
    auto rejected = [](auto operation) {
        bool caught = false;
        try { operation(); }
        catch (const std::invalid_argument& error) {
            const auto* typed = dynamic_cast<const RunFailureInfo*>(&error);
            caught = typed && typed->run_failure_code() == RunFailureCode::scratch_capacity_exceeded
                && typed->run_failure().args && *typed->run_failure().args == "{\"resource\":\"layout\"}";
        }
        require(caught, "bad resident budget must have its typed layout refusal");
    };
    for (const auto bad : {std::size_t{0}, Cache::kMaxEntries + 1,
                           std::numeric_limits<std::size_t>::max()}) {
        rejected([&] {
            pd::set_command_scratch_resident_entries(
                pd::CommandScratchResidentKind::placement_snapshots, bad);
        });
        rejected([&] {
            Medium medium;
            Cache cache(std::make_unique<Backing>(medium), bad, {}, &Backing::admission);
        });
    }
    rejected([&] {
        Medium medium;
        Cache cache(std::make_unique<Backing>(medium), Cache::max_entries(page_bytes) + 1,
                    {}, &Backing::admission);
    });
    const auto old = pd::set_command_scratch_resident_entries(
        pd::CommandScratchResidentKind::journal_pages, std::size_t{2});
    require(pd::set_command_scratch_resident_entries(
                pd::CommandScratchResidentKind::journal_pages, old) == std::optional<std::size_t>{2},
            "resident registration is restorable");
}

void page_spill_sync_and_close(std::size_t geometry) {
    Medium medium(geometry, 16);
    const Bytes a = pattern(geometry, 1), b = pattern(geometry, 2), c = pattern(geometry, 3);
    Bytes out(geometry);
    {
        Cache cache(std::make_unique<Backing>(medium), 2, {}, &Backing::admission);
        ok(cache.write_page(0, span(a)), "resident page A");
        ok(cache.read_page(0, buffer(out)), "resident A hit");
        require(out == a && medium.writes == 0 && medium.reads == 0,
                "resident write/hit reached the inner backing");
        ok(cache.write_page(2, span(b)), "conflict evicts A");
        const bool a_persisted = medium.equals(0, a);
        ok(cache.read_page(0, buffer(out)), "read miss evicts B then reads A");
        require(out == a && a_persisted && medium.equals(2, b)
                    && medium.writes == 2 && medium.reads == 1,
                "RED: dirty eviction lost bytes subsequently read from the backing");
        ok(cache.write_page(1, span(c)), "resident C");
        ok(cache.sync(pd::ScratchDurability::data), "data sync");
        require(medium.equals(1, c) && medium.writes == 3 && medium.writes_at_sync == 3
                    && medium.last_sync == pd::ScratchDurability::data,
                "RED: data sync did not flush dirty bytes before the barrier");
        ok(cache.write_page(4, span(b)), "resident page before out-of-extent read");
        const auto writes = medium.writes;
        require(cache.read_page(5, buffer(out)) == Status::not_found && medium.writes == writes,
                "out-of-extent read flushed an unrelated victim");
        ok(cache.read_page(3, buffer(out)), "admitted zero hole");
        require(std::all_of(out.begin(), out.end(), [](std::byte value) {
                    return value == std::byte{0};
                }), "immediate admission lost zero-filled holes");
        ok(cache.sync(pd::ScratchDurability::full), "full sync");
        require(medium.equals(4, b) && medium.writes_at_sync == medium.writes
                    && medium.last_sync == pd::ScratchDurability::full,
                "full sync lost a resident page");
        ok(cache.write_page(1, span(a)), "page retained until close");
    }
    require(medium.equals(1, a), "orderly destruction lost the last dirty page");
}

void deferred_faults_keep_dirty(bool torn, bool typed) {
    Medium medium;
    medium.short_write = torn;
    medium.carry = typed;
    Cache cache(std::make_unique<Backing>(medium), 2, {}, &Backing::admission);
    const Bytes a = pattern(page_bytes, 11), b = pattern(page_bytes, 12);
    Bytes out(page_bytes);
    ok(cache.write_page(0, span(a)), "resident seed before fault");
    medium.fail_write = true;
    require(cache.write_page(2, span(b)) == Status::io_error && medium.failed_writes == 1,
            "eviction must encounter the injected real backing fault");
    const auto* carried = cache.carried_failure();
    require(typed ? carried == &medium.failure : carried == nullptr,
            "deferred eviction lost or invented a carried typed failure");
    ok(cache.read_page(0, buffer(out)), "victim stays resident after a failed eviction");
    require(out == a && cache.carried_failure() == nullptr,
            "failed eviction replaced bytes or a successful hit retained a stale diagnostic");
    require(cache.drain() == Status::io_error && medium.failed_writes == 2,
            "failed eviction cleared dirty and made later drain silently succeed");
    medium.fail_write = false;
    ok(cache.drain(), "retry a failed dirty drain");
    require(medium.equals(0, a), "retry did not repair the entire torn page");
    ok(cache.write_page(2, span(b)), "retry conflicting page");
    medium.fail_sync = true;
    require(cache.sync(pd::ScratchDurability::full) == Status::io_error
                && medium.equals(2, b), "flush must precede even a failed sync hook");
    require(typed ? cache.carried_failure() == &medium.failure : cache.carried_failure() == nullptr,
            "sync failure lost its diagnostic");
    medium.fail_sync = false;
    ok(cache.sync(pd::ScratchDurability::full), "retry full sync");
}

pd::ScratchStoreConfig config(pd::ScratchOpenMode mode = pd::ScratchOpenMode::create_empty) {
    return {2, page_bytes, 1, 1, sd::exit_scratch_key_order(), mode};
}
pd::ScratchHandle put(pd::CommandScratchStore& store, std::uint64_t key, const Bytes& value) {
    const sd::ExitScratchKey encoded(17, 0, key);
    const pd::ScratchSpanSource payload(span(value));
    pd::ScratchHandle handle = 0;
    ok(store.put(encoded, payload, handle), "store put");
    return handle;
}
std::uint64_t digest(pd::CommandScratchStore& store) {
    std::array<std::byte, page_bytes> workspace{};
    std::uint64_t result = 0;
    ok(store.state_digest({workspace.data(), workspace.size()}, result), "store state digest");
    return result;
}
struct Canonical {
    Bytes bytes;
    template<class T> void add(const T& value) {
        const auto* start = reinterpret_cast<const std::byte*>(&value);
        bytes.insert(bytes.end(), start, start + sizeof(value));
    }
    void add(const Bytes& value) {
        add(static_cast<std::uint64_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
};
Bytes records(pd::CommandScratchStore& store) {
    Canonical result;
    result.add(store.record_count());
    result.add(digest(store)); // Includes allocation history even when empty.
    pd::ScratchHandle handle = 0;
    auto status = store.first(handle);
    while (status == Status::ok) {
        pd::ScratchRecordInfo info;
        ok(store.point(handle, info), "record info");
        Bytes key(static_cast<std::size_t>(info.key_bytes)), value(static_cast<std::size_t>(info.value_bytes));
        ok(store.read(handle, pd::ScratchField::key, 0, buffer(key)), "record key bytes");
        ok(store.read(handle, pd::ScratchField::value, 0, buffer(value)), "record value bytes");
        result.add(handle); result.add(key); result.add(value);
        status = store.next(handle, handle);
    }
    require(status == Status::end, "record walk failed");
    return result.bytes;
}

struct StoreRun {
    Bytes exact_records, raw_pages;
    std::array<std::uint64_t, 3> calls{}, bytes{};
    std::uint64_t reads = 0, writes = 0;
};
StoreRun store_run(std::size_t budget, bool write_through) {
    Medium medium;
    Charges charges;
    auto inner = std::make_unique<Backing>(medium);
    auto* raw = inner.get();
    Cache cache(std::move(inner), budget, charges.observer(), &Backing::admission, write_through);
    StoreRun result;
    {
        pd::CommandScratchStore store(config(), &cache);
        ok(store.status(), "store create");
        std::array<pd::ScratchHandle, 32> handles{};
        ok(store.begin(), "initial begin");
        for (std::size_t i = 0; i < handles.size(); ++i) {
            const auto value = pattern(2049 + i, static_cast<unsigned>(i));
            handles[i] = put(store, i, value);
        }
        ok(store.commit(pd::ScratchDurability::buffered), "initial buffered commit");
        const auto before = records(store);
        const auto edit = [&] {
            for (std::size_t i = 0; i < 16; ++i)
                require(put(store, i, pattern(4097 + i, static_cast<unsigned>(i + 43))) == handles[i],
                        "replacement changed a handle");
            for (std::size_t i = 16; i < 24; ++i) ok(store.erase(handles[i]), "erase for rollback");
            for (std::size_t i = 32; i < 40; ++i)
                (void)put(store, i, pattern(3073 + i, static_cast<unsigned>(i)));
        };
        ok(store.begin(), "edit before rollback");
        edit();
        ok(store.rollback(), "rollback spilled journal and dirty targets");
        require(records(store) == before, "rollback changed bytes/handles/allocation history");
        ok(store.begin(), "repeat rolled-back edit");
        edit();
        ok(store.commit(pd::ScratchDurability::data), "data commit with two barriers");
        require(medium.syncs == 2 && medium.last_sync == pd::ScratchDurability::data
                    && medium.writes_at_sync == medium.writes,
                "stronger commit failed to drain before both durability barriers");
        result.exact_records = records(store);
        ok(store.begin(), "abandoned transaction");
        (void)put(store, 100, pattern(8193, 99));
        // Store destructor must roll back before backing close/reopen.
    }
    {
        pd::CommandScratchStore reopened(config(pd::ScratchOpenMode::reopen), &cache);
        ok(reopened.status(), "same-decorator buffered reopen");
        require(records(reopened) == result.exact_records,
                "store destruction/reopen lost the last successful commit");
    }
    ok(cache.drain(), "checked drain before bypass reader");
    {
        // Read-only bypass is legal ONLY after a checked drain. It must see
        // the same handles and every key/value byte, with no cached help.
        pd::CommandScratchStore disk(config(pd::ScratchOpenMode::reopen), raw);
        ok(disk.status(), "drained raw reopen");
        require(records(disk) == result.exact_records, "drain lost bytes needed by a bypassing reader");
    }
    result.raw_pages.assign(medium.bytes.begin(), medium.bytes.begin()
        + static_cast<std::ptrdiff_t>(medium.extent * page_bytes));
    result.calls = charges.calls; result.bytes = charges.bytes;
    result.reads = medium.reads; result.writes = medium.writes;
    require(!charges.failed && result.calls[1] > 0, "store oracle missed actual logical writes");
    return result;
}

void store_spill_oracle() {
    const auto oracle = store_run(512, true);
    for (const auto pages : {std::size_t{1}, std::size_t{2}, std::size_t{512}}) {
        const auto run = store_run(pages, false);
        require(run.exact_records == oracle.exact_records && run.raw_pages == oracle.raw_pages
                    && run.calls == oracle.calls && run.bytes == oracle.bytes,
                "spill/rollback/reopen differs from write-through bytes or logical counters");
        require(run.writes > 0 && run.reads > 0, "spill oracle never reached its inner backing");
    }
}

void store_failure_rollback(bool torn) {
    Medium medium;
    Cache cache(std::make_unique<Backing>(medium), 2, {}, &Backing::admission);
    pd::CommandScratchStore store(config(), &cache);
    ok(store.status(), "fault store create");
    const Bytes initial = pattern(2049, 1), changed = pattern(8193, 2);
    ok(store.begin(), "fault seed begin");
    (void)put(store, 1, initial);
    ok(store.commit(pd::ScratchDurability::buffered), "fault seed commit");
    const auto before = records(store);
    ok(cache.drain(), "seed physical bytes before fault");
    ok(store.begin(), "begin failing spilled edit");
    medium.fail_write = true;
    medium.short_write = torn;
    const sd::ExitScratchKey key(17, 0, 1);
    const pd::ScratchSpanSource payload(span(changed));
    pd::ScratchHandle ignored = 0;
    auto status = store.put(key, payload, ignored);
    if (status == Status::ok) status = store.commit(pd::ScratchDurability::data);
    require(status == Status::io_error && medium.failed_writes > 0 && store.carried_failure(),
            "injected eviction/sync write fault missed the actual store");
    const auto args = store.carried_failure()->run_failure().args;
    medium.fail_write = false;
    ok(store.rollback(), "rollback after deferred physical failure");
    require(records(store) == before && store.carried_failure()
                && store.carried_failure()->run_failure().args == args,
            "rollback changed committed bytes or discarded the original typed fault");
    ok(store.begin(), "clear original diagnostic at next begin");
    require(store.carried_failure() == nullptr, "new begin retained a stale fault");
    ok(store.rollback(), "finish fault witness");
}

void checked_store_close_retains_failure() {
    Medium medium;
    Charges charges;
    Cache cache(std::make_unique<Backing>(medium), 256, charges.observer(), &Backing::admission);
    pd::CommandScratchStore store(config(), &cache);
    ok(store.status(), "checked-close store create");
    ok(store.begin(), "checked-close seed begin");
    (void)put(store, 1, pattern(2049, 3));
    ok(store.commit(pd::ScratchDurability::buffered), "checked-close seed commit");
    const auto before = records(store);
    const auto stats = store.stats();
    const auto calls = charges.calls, bytes = charges.bytes;
    require(medium.writes == 0 && medium.extent <= cache.resident_entries(),
            "checked-close witness must have genuinely deferred resident pages");
    medium.fail_write = true;
    Status failed, retried;
    {
        NoAllocation no_allocation;
        failed = store.drain();
        medium.fail_write = false;
        retried = store.drain();
    }
    require(failed == Status::io_error && medium.failed_writes == 1
                && store.carried_failure()
                && store.carried_failure()->run_failure().args == medium.failure.run_failure().args,
            "checked close failed to retain the original deferred typed write error");
    ok(retried, "retry checked store close");
    require(charges.calls == calls && charges.bytes == bytes
                && store.stats().page_reads == stats.page_reads
                && store.stats().page_writes == stats.page_writes
                && store.stats().syncs == stats.syncs,
            "physical close changed logical I/O counters or observer charges");
    require(records(store) == before, "checked close changed logical state or record bytes");
    ok(store.begin(), "active-drain refusal begin");
    const auto writes = medium.writes;
    require(store.drain() == Status::programming_error && medium.writes == writes,
            "drain must not implicitly commit or flush an active store transaction");
    ok(store.rollback(), "active-drain refusal rollback");

    // The optional interface must leave ordinary immediate backings alone.
    Medium immediate;
    Backing backing(immediate);
    pd::CommandScratchStore ordinary(config(), &backing);
    ok(ordinary.status(), "immediate store create");
    const auto immediate_stats = ordinary.stats();
    ok(ordinary.drain(), "immediate backing has nothing deferred");
    require(immediate.reads == 0 && immediate.writes == 0 && immediate.syncs == 0
                && ordinary.stats().page_reads == immediate_stats.page_reads,
            "drain invented I/O for an immediate backing");
}

void erased_scope_never_writes() {
    Medium medium;
    Cache cache(std::make_unique<Backing>(medium), 256, {}, &Backing::admission);
    pd::CommandScratchStore store(config(), &cache);
    sd::ExitScratchVersionsProduction versions(store);
    sd::ExitScratchContext context{store, {}};
    context.versions = &versions;
    ok(store.status(), "erased-scope store create");
    source::PlacementSnapshot value;
    value.source_id = "held";
    value.from_entry = "entry";
    value.family = source::PineOrderFamily::ExitLimit;
    value.comment.assign(2049, 's'); // Forces actual overflow pages to be freed.
    value.exit_levels.limit = 150.0;
    value.command_sequence = 1;
    for (std::uint64_t i = 0; i < 16; ++i) {
        {
            sd::ExitScratchScope scope(context);
            const auto version = versions.placement(scope.scope(), value);
            const auto frozen = versions.placement(version);
            require(frozen.source_id == value.source_id && frozen.comment == value.comment
                        && frozen.command_sequence == value.command_sequence
                        && frozen.exit_levels.limit == value.exit_levels.limit,
                    "production placement version did not preserve its input");
            ++value.command_sequence;
        }
        require(store.record_count() == 0 && !store.transaction_active()
                    && cache.page_count() <= cache.resident_entries(),
                "scope did not erase its real counter/version within the fixed budget");
        require(medium.writes == 0 && medium.free_writes == 0,
                "RED: an erased resident scope/page was physically written");
    }
    ok(cache.drain(), "free-list metadata drained for orderly close");
    require(medium.writes > 0 && medium.free_writes > 0,
            "free-list pages must still exist for continuation/reopen");
}
} // namespace

int main() {
    try {
        for (const std::size_t geometry : {std::size_t{512}, std::size_t{1536}, std::size_t{4096}})
            page_spill_sync_and_close(geometry);
        budget_accounting();
        for (const bool torn : {false, true}) {
            for (const bool typed : {false, true}) deferred_faults_keep_dirty(torn, typed);
            store_failure_rollback(torn);
        }
        checked_store_close_retains_failure();
        store_spill_oracle();
        erased_scope_never_writes();
        return 0;
    } catch (const std::exception& error) {
        global_allocation::refusing = false;
        global_allocation::recording = false;
        std::fprintf(stderr, "FAIL scratch write-behind: %s\n", error.what());
        return 1;
    }
}
