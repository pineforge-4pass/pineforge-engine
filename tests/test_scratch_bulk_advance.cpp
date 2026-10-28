// CommandScratchStore::advance_handles consumes handle numbers without
// creating records. Normal execution checks it against count successful puts
// followed by erases; --mutate-off-by-one must be rejected.
// All store operations use the public interface. Only the overflow fixture
// edits a closed store's backing image, to hand-set its allocation counter.
#include <pineforge/detail/scratch_store.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace {
using namespace pineforge::detail;
using Word = std::uint64_t;
constexpr std::size_t kPageBytes = 512;
constexpr Word kOrderId = 0x5334454731000001ULL;
constexpr Word kHandleLimit = Word{1} << 62;
constexpr Word kProbeKey = std::numeric_limits<Word>::max();
Word current_case = 0;

static_assert(noexcept(std::declval<CommandScratchStore&>().advance_handles(Word{})),
              "bulk handle advancement must preserve the status-only contract");

void require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "scratch bulk advance: %s (case=%llu)\n", message,
                 static_cast<unsigned long long>(current_case));
    std::exit(EXIT_FAILURE);
}
void ok(ScratchStatus status, const char* message) {
    require(status == ScratchStatus::ok, message);
}

void encode(std::byte* bytes, Word value) noexcept {
    for (unsigned i = 0; i != 8; ++i) bytes[i] = std::byte((value >> (8 * i)) & 255);
}
Word decode(const std::byte* bytes) noexcept {
    Word value = 0;
    for (unsigned i = 0; i != 8; ++i)
        value |= Word(std::to_integer<unsigned char>(bytes[i])) << (8 * i);
    return value;
}
ScratchStatus numeric_order(void*, const ScratchByteSource& a, const ScratchByteSource& b,
                            int& result) noexcept {
    if (a.size() != 8 || b.size() != 8) return ScratchStatus::programming_error;
    std::array<std::byte, 8> left{}, right{};
    auto status = a.read(0, {left.data(), left.size()});
    if (status != ScratchStatus::ok) return status;
    status = b.read(0, {right.data(), right.size()});
    if (status != ScratchStatus::ok) return status;
    const Word x = decode(left.data()), y = decode(right.data());
    result = x < y ? -1 : x > y ? 1 : 0;
    return ScratchStatus::ok;
}
ScratchStoreConfig config(ScratchOpenMode mode) {
    return {2, kPageBytes, 1, 1, {kOrderId, nullptr, numeric_order}, mode};
}

struct Rig {
    // Test backing provision, independent of the store's resident workspace.
    std::vector<std::byte> bytes;
    FixedMemoryScratchBacking backing;
    std::unique_ptr<CommandScratchStore> store;
    Word resident;
    Rig() : bytes(kPageBytes * 4096), backing({bytes.data(), bytes.size()}, kPageBytes),
            store(std::make_unique<CommandScratchStore>(config(ScratchOpenMode::create_empty),
                                                        &backing)),
            resident(store->stats().resident_bytes) {
        ok(backing.status(), "fixture backing opens");
        ok(store->status(), "store opens");
    }
    void reopen() {
        store.reset();
        store = std::make_unique<CommandScratchStore>(config(ScratchOpenMode::reopen), &backing);
        ok(store->status(), "committed allocation state reopens");
    }
};

ScratchStatus put_word(CommandScratchStore& store, Word key, Word value, ScratchHandle& handle) {
    std::array<std::byte, 8> k{}, v{};
    encode(k.data(), key);
    encode(v.data(), value);
    const ScratchSpanSource ks({k.data(), k.size()}), vs({v.data(), v.size()});
    return store.put(ks, vs, handle);
}

struct Row { ScratchHandle handle; Word value; };
struct Model {
    std::map<Word, Row> rows;
    Word next_handle = 1;
    ScratchHandle put(Word key, Word value) {
        auto found = rows.find(key);
        if (found != rows.end()) {
            found->second.value = value;
            return found->second.handle;
        }
        const auto handle = next_handle++;
        rows.emplace(key, Row{handle, value});
        return handle;
    }
};

// Independent FNV-1a oracle for the contract's explicit LE64 byte stream.
// Consuming n fresh handles and erasing their rows changes ONLY next_handle
// from h to h+n: count, ordered rows and every surviving handle stay fixed.
struct Fnv {
    Word value = 14695981039346656037ULL;
    void bytes(const std::byte* data, std::size_t length) {
        for (std::size_t i = 0; i != length; ++i) {
            value ^= std::to_integer<unsigned char>(data[i]);
            value *= 1099511628211ULL;
        }
    }
    void word(Word value) {
        std::array<std::byte, 8> raw{};
        encode(raw.data(), value);
        bytes(raw.data(), raw.size());
    }
};
Word expected_digest(const Model& model, bool state) {
    Fnv fold;
    if (state) { fold.word(kOrderId); fold.word(model.next_handle); }
    fold.word(model.rows.size());
    for (const auto& item : model.rows) {
        if (state) fold.word(item.second.handle);
        fold.word(8); fold.word(item.first);
        fold.word(8); fold.word(item.second.value);
    }
    return fold.value;
}
Word digest(const CommandScratchStore& store, bool state) {
    std::array<std::byte, kPageBytes> page{};
    Word result = 0;
    ok(state ? store.state_digest({page.data(), page.size()}, result)
             : store.ordered_digest({page.data(), page.size()}, result), "digest succeeds");
    return result;
}
Word read_word(const CommandScratchStore& store, ScratchHandle handle, ScratchField field) {
    std::array<std::byte, 8> bytes{};
    ok(store.read(handle, field, 0, {bytes.data(), bytes.size()}), "row bytes readable");
    return decode(bytes.data());
}
void verify_rows(const Rig& rig, const Model& model) {
    const auto& store = *rig.store;
    ok(store.status(), "status unchanged");
    require(store.carried_failure() == nullptr, "no manufactured backing failure");
    require(store.page_size() == kPageBytes, "page geometry unchanged");
    require(store.stats().resident_bytes == rig.resident, "resident workspace stays fixed");
    require(store.record_count() == model.rows.size(), "record_count unchanged by consumption");
    require(digest(store, false) == expected_digest(model, false),
            "ordered_digest differs from independent row oracle");
    ScratchHandle cursor = kProbeKey;
    const auto first = store.first(cursor);
    require(first == (model.rows.empty() ? ScratchStatus::end : ScratchStatus::ok),
            "ordered first status");
    for (const auto& item : model.rows) {
        require(cursor == item.second.handle, "ordered handle sequence unchanged");
        ScratchRecordInfo info{};
        ok(store.point(cursor, info), "surviving handle remains present");
        require(info.handle == cursor && info.key_bytes == 8 && info.value_bytes == 8,
                "surviving record metadata unchanged");
        require(read_word(store, cursor, ScratchField::key) == item.first
                    && read_word(store, cursor, ScratchField::value) == item.second.value,
                "surviving key/value unchanged");
        std::array<std::byte, 8> raw{};
        encode(raw.data(), item.first);
        const ScratchSpanSource key({raw.data(), raw.size()});
        ScratchHandle found = 0;
        ok(store.lower_bound(key, found), "lower_bound finds survivor");
        require(found == cursor, "lower_bound handle unchanged");
        const auto status = store.next(cursor, cursor);
        require(status == ScratchStatus::ok || status == ScratchStatus::end, "ordered next status");
    }
    require(cursor == scratch_no_handle, "ordered walk has no extra rows");
}
void probe_next(Rig& rig, const Model& model) {
    auto& store = *rig.store;
    const Word before = digest(store, true);
    ok(store.begin(), "next-handle probe begins");
    ScratchHandle handle = scratch_no_handle;
    const auto status = put_word(store, kProbeKey, current_case, handle);
    if (model.next_handle < kHandleLimit) {
        ok(status, "next fresh put succeeds");
        require(handle == model.next_handle, "next put handle differs from allocation oracle");
    } else {
        require(status == ScratchStatus::programming_error && handle == scratch_no_handle,
                "exhausted next put refuses without publishing a handle");
    }
    ok(store.rollback(), "next-handle probe rolls back");
    require(digest(store, true) == before, "probe rollback restores allocation state");
}

struct Pair {
    Rig bulk, singles;
    Model model;
    void begin() { ok(bulk.store->begin(), "bulk begin"); ok(singles.store->begin(), "oracle begin"); }
    void commit() {
        ok(bulk.store->commit(ScratchDurability::buffered), "bulk commit");
        ok(singles.store->commit(ScratchDurability::buffered), "oracle commit");
    }
    void rollback() {
        ok(bulk.store->rollback(), "bulk rollback"); ok(singles.store->rollback(), "oracle rollback");
    }
    void put(Word key, Word value) {
        const ScratchHandle expected = model.put(key, value);
        ScratchHandle a = 0, b = 0;
        ok(put_word(*bulk.store, key, value, a), "bulk ordinary put");
        ok(put_word(*singles.store, key, value, b), "oracle ordinary put");
        require(a == expected && b == expected, "ordinary put handle differs from allocation oracle");
    }
    void erase(Word key) {
        const auto found = model.rows.find(key);
        if (found == model.rows.end()) return;
        ok(bulk.store->erase(found->second.handle), "bulk ordinary erase");
        ok(singles.store->erase(found->second.handle), "oracle ordinary erase");
        model.rows.erase(found);
    }
    void verify() {
        require(!bulk.store->transaction_active() && !singles.store->transaction_active(),
                "successful implicit operation leaves no transaction");
        require(digest(*bulk.store, true) == digest(*singles.store, true),
                "state_digest differs from put/erase oracle");
        require(digest(*bulk.store, true) == expected_digest(model, true),
                "state_digest differs from independent allocation/row oracle");
        verify_rows(bulk, model); verify_rows(singles, model);
        probe_next(bulk, model); probe_next(singles, model);
    }
};

struct Rng {
    Word state;
    Word next() {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        return state;
    }
};
void random_prior(Pair& pair, Rng& rng) {
    pair.begin();
    pair.put(0, rng.next()); // Never erased: every random case has live rows.
    pair.put(1, rng.next());
    pair.erase(1);          // Every case also has committed, erased history.
    const Word count = 16 + rng.next() % 32;
    for (Word i = 0; i != count; ++i) {
        const Word key = 2 + rng.next() % 48, value = rng.next();
        pair.put(key, value);
        if (i % 3 == 0) pair.erase(2 + rng.next() % 48);
    }
    pair.commit();
    const Model saved = pair.model;
    pair.begin();
    const Word aborted = 4 + rng.next() % 12;
    for (Word i = 0; i != aborted; ++i) {
        const Word key = 128 + rng.next() % 64, value = rng.next();
        pair.put(key, value);
        pair.erase(2 + rng.next() % 48);
    }
    pair.rollback();
    pair.model = saved;
    pair.begin();
    for (Word i = 0; i != 12; ++i) {
        const Word key = 2 + rng.next() % 48, value = rng.next();
        pair.put(key, value);
        pair.erase(2 + rng.next() % 48);
    }
    pair.commit();
    pair.verify();
}

// Independent operational oracle: fresh put, erase its handle, repeat.
// A failed put leaves its output unchanged and its transaction rollback-only.
ScratchStatus single_advances(CommandScratchStore& store, Word count, Word next) {
    const bool own = !store.transaction_active();
    if (count == 0) return ScratchStatus::ok;
    if (own) ok(store.begin(), "oracle implicit begin");
    for (Word i = 0; i != count; ++i) {
        ScratchHandle handle = scratch_no_handle;
        const auto status = put_word(store, kProbeKey, i, handle);
        if (status != ScratchStatus::ok) {
            require(handle == scratch_no_handle, "failed oracle put keeps output unchanged");
            return status;
        }
        require(handle == next + i, "single put consumes exactly one handle");
        ok(store.erase(handle), "single allocation's row is erased");
    }
    return own ? store.commit(ScratchDurability::buffered) : ScratchStatus::ok;
}
void active_digest_refused(const CommandScratchStore& store) {
    std::array<std::byte, kPageBytes> page{};
    Word result = kProbeKey;
    require(store.state_digest({page.data(), page.size()}, result) == ScratchStatus::programming_error
                && result == kProbeKey, "active state_digest stays illegal and leaves output alone");
}
void property_cases(bool mutate) {
    for (current_case = 0; current_case != 64; ++current_case) {
        Rng rng{0x391B533445000001ULL + current_case};
        Pair pair;
        random_prior(pair, rng);
        const Model before = pair.model;
        const Word count = current_case < 2 ? current_case : 2 + rng.next() % 127;
        const bool explicit_transaction = current_case % 3 != 0;
        if (explicit_transaction) { pair.begin(); pair.put(256, rng.next()); }
        const Word start = pair.model.next_handle;
        ok(pair.bulk.store->advance_handles(count + (mutate ? 1 : 0)), "bulk advance succeeds");
        ok(single_advances(*pair.singles.store, count, start), "single advances succeed");
        pair.model.next_handle += count;
        if (explicit_transaction) {
            // Observe the next issued handle BEFORE rollback can hide a mutation.
            pair.put(257, rng.next());
            verify_rows(pair.bulk, pair.model); verify_rows(pair.singles, pair.model);
            active_digest_refused(*pair.bulk.store); active_digest_refused(*pair.singles.store);
            if (current_case % 3 == 2) { pair.rollback(); pair.model = before; }
            else pair.commit();
        }
        pair.verify();
        pair.bulk.reopen(); pair.singles.reopen();
        pair.verify();
        // Also compare the literal n one-handle API calls. The independent
        // model and the preceding real put/erase oracle prevent shared bugs
        // in the bulk and unit-call implementations from validating each other.
        ok(pair.bulk.store->advance_handles(count), "second bulk advance succeeds");
        for (Word i = 0; i != count; ++i)
            ok(pair.singles.store->advance_handles(1), "unit advance succeeds");
        pair.model.next_handle += count;
        pair.verify();
        pair.bulk.reopen(); pair.singles.reopen();
        pair.verify();
    }
}

// Hand-set a CLOSED store's counter via the public backing interface. The
// version-1 superblock has 18 LE64 fields, next_handle at field 5, followed by
// FNV-1a over those 18 fields. No Core/private access or product test seam.
void set_counter(Rig& rig, Word next_handle) {
    rig.store.reset();
    unsigned changed = 0;
    for (ScratchPageId slot = 0; slot != 2; ++slot) {
        std::array<std::byte, kPageBytes> page{};
        ok(rig.backing.read_page(slot, {page.data(), page.size()}), "read fixture superblock");
        if (decode(page.data()) != 0x3142535243534650ULL) continue;
        require(decode(page.data() + 8) == 1 && decode(page.data() + 24) == kOrderId,
                "fixture edits only its version/order superblock");
        Fnv before; before.bytes(page.data(), 18 * 8);
        require(before.value == decode(page.data() + 18 * 8), "fixture starts with valid checksum");
        encode(page.data() + 5 * 8, next_handle);
        Fnv after; after.bytes(page.data(), 18 * 8);
        encode(page.data() + 18 * 8, after.value);
        ok(rig.backing.write_page(slot, {page.data(), page.size()}), "write fixture counter/checksum");
        ++changed;
    }
    require(changed != 0, "boundary fixture changed a committed superblock");
    rig.reopen();
}
void rollback_only(CommandScratchStore& store) {
    require(store.transaction_active(), "exhaustion leaves an active transaction");
    require(store.advance_handles(0) == ScratchStatus::programming_error
                && store.advance_handles(1) == ScratchStatus::programming_error,
            "rollback-only transaction refuses even zero advancement");
    require(store.commit(ScratchDurability::buffered) == ScratchStatus::programming_error,
            "exhaustion cannot commit a partial allocation");
    active_digest_refused(store);
    std::array<std::byte, kPageBytes> page{};
    Word result = kProbeKey;
    require(store.ordered_digest({page.data(), page.size()}, result) == ScratchStatus::programming_error
                && result == kProbeKey, "rollback-only ordered_digest remains unavailable");
}
void boundary_case(Word count, bool explicit_transaction, bool abort_success) {
    Pair pair;
    Rng rng{0x391B0F10A0000001ULL};
    random_prior(pair, rng);
    // L=2^62; at L-3 the last legal handles are L-3, L-2, L-1.
    // Three consumptions reach L; the fourth fails, just like put_impl.
    set_counter(pair.bulk, kHandleLimit - 3); set_counter(pair.singles, kHandleLimit - 3);
    pair.model.next_handle = kHandleLimit - 3;
    pair.verify();
    const Model before = pair.model;
    if (explicit_transaction) pair.begin();
    const auto bulk = pair.bulk.store->advance_handles(count);
    const auto singles = single_advances(*pair.singles.store, count, pair.model.next_handle);
    require(bulk == singles, "bulk exhaustion status differs from real single puts");
    if (count <= 3) {
        ok(bulk, "advance to the exact handle limit succeeds");
        pair.model.next_handle += count;
        if (explicit_transaction) {
            if (abort_success) { pair.rollback(); pair.model = before; }
            else pair.commit();
        }
    } else {
        require(bulk == ScratchStatus::programming_error, "exhaustion uses existing coded status");
        require(pair.bulk.store->record_count() == before.rows.size()
                    && pair.singles.store->record_count() == before.rows.size(),
                "failed consumption introduces no live rows");
        rollback_only(*pair.bulk.store); rollback_only(*pair.singles.store);
        pair.rollback();
    }
    pair.verify();
    if (pair.model.next_handle == kHandleLimit) {
        ok(pair.bulk.store->advance_handles(0), "zero remains legal at exhaustion");
        require(pair.bulk.store->advance_handles(1) == ScratchStatus::programming_error
                    && single_advances(*pair.singles.store, 1, kHandleLimit)
                        == ScratchStatus::programming_error,
                "already exhausted counter refuses the first new allocation");
        rollback_only(*pair.bulk.store); rollback_only(*pair.singles.store);
        pair.rollback();
        pair.verify();
    }
    pair.bulk.reopen(); pair.singles.reopen();
    pair.verify();
}
void zero_and_reentrancy() {
    Pair pair;
    Rng rng{0x391B0A1100000001ULL};
    random_prior(pair, rng);
    for (bool active : {false, true}) {
        if (active) pair.begin();
        const auto before = pair.bulk.store->stats();
        ok(pair.bulk.store->advance_handles(0), "zero advancement succeeds");
        const auto after = pair.bulk.store->stats();
        require(before.page_reads == after.page_reads && before.page_writes == after.page_writes
                    && before.syncs == after.syncs && before.backing_pages == after.backing_pages,
                "zero advancement performs no backing work");
        struct Context { CommandScratchStore* store; Word calls = 0; } context{pair.bulk.store.get()};
        const auto visitor = [](void* raw, const ScratchRecordView&) noexcept {
            auto& context = *static_cast<Context*>(raw);
            ++context.calls;
            require(context.store->advance_handles(1) == ScratchStatus::programming_error,
                    "callback cannot mutate allocation state");
            return true;
        };
        std::array<std::byte, kPageBytes> page{};
        ok(pair.bulk.store->for_each({page.data(), page.size()}, visitor, &context), "reentrant walk");
        require(context.calls == pair.model.rows.size(), "reentrant witness visited real rows");
        require(pair.bulk.store->transaction_active() == active, "callback refusal changes no transaction");
        if (active) pair.commit(); // Refusal must not poison the open transaction.
        pair.verify();
    }
    CommandScratchStore unusable(config(ScratchOpenMode::create_empty), nullptr);
    require(unusable.advance_handles(0) == ScratchStatus::programming_error
                && unusable.advance_handles(1) == ScratchStatus::programming_error
                && !unusable.transaction_active(), "unusable store refuses without mutation");
}
} // namespace

int main(int argc, char** argv) {
    const bool mutate = argc == 2 && std::strcmp(argv[1], "--mutate-off-by-one") == 0;
    require(argc == 1 || mutate, "expected no argument or --mutate-off-by-one");
    // Mutation uses the SAME equality oracle, with one extra handle consumed
    // only on the bulk side. It must exit nonzero, never invert a failed check.
    property_cases(mutate);
    for (bool active : {false, true}) {
        for (Word count : {Word{0}, Word{3}, Word{4}, std::numeric_limits<Word>::max()}) {
            ++current_case;
            boundary_case(count, active, false);
        }
    }
    ++current_case; boundary_case(3, true, true);
    ++current_case; zero_and_reentrancy();
    std::puts("scratch bulk advance: ok");
    return EXIT_SUCCESS;
}
