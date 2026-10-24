// CR150-CX admfold. Uses only frozen-base APIs: apply this test without the
// implementation change for the supervisor's RED run. No local execution.
#include <pineforge/detail/journal_codec.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/command_scratch_cache.hpp"
#include "../src/command_scratch_production.hpp"
#include "../src/source/admission_journal_codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
namespace ad = pineforge::admission;
namespace pd = pineforge::detail;
constexpr std::uint64_t seed = 1469598103934665603ULL;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();
constexpr std::size_t pressure_id_bytes = pd::CachedScratchBacking::kCacheBytes + 4096;

void require(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}
void ok(pd::ScratchStatus status, const char* why) {
    require(status == pd::ScratchStatus::ok, why);
}
template<class F> void invariant(F&& operation) {
    bool refused = false;
    try { operation(); }
    catch (const std::logic_error&) { refused = true; }
    require(refused, "invalid journal operation must retain its invariant refusal");
}

// The same single-pread accounting and zero-control calibration as
// test_disk_resident_id_table_resident_io.cpp. Only this I/O half is Linux-only.
struct Io { std::uint64_t reads = 0, writes = 0; };
class ProcessIo {
public:
    ProcessIo() {
#if defined(__linux__)
        fd_ = ::open("/proc/self/io", O_RDONLY | O_CLOEXEC);
        require(fd_ >= 0, "open /proc/self/io");
        const auto before = sample();
        const auto after = sample();
        const auto control = difference(before, after);
        require(control.reads == 0 && control.writes == 0,
                "back-to-back /proc/self/io control is not zero");
        const auto outer_before = sample();
        (void)sample();
        (void)sample();
        const auto outer_after = sample();
        const auto nested = difference(outer_before, outer_after, 2);
        require(nested.reads == 0 && nested.writes == 0,
                "nested /proc/self/io accounting control is not zero");
#else
        std::puts("SKIP admfold syscall counters: /proc/self/io requires Linux");
#endif
    }
    ProcessIo(const ProcessIo&) = delete;
    ProcessIo& operator=(const ProcessIo&) = delete;
    ~ProcessIo() {
#if defined(__linux__)
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    static constexpr bool enabled() noexcept {
#if defined(__linux__)
        return true;
#else
        return false;
#endif
    }
    Io sample() const {
#if defined(__linux__)
        char bytes[1024];
        const auto length = ::pread(fd_, bytes, sizeof(bytes) - 1, 0);
        require(length > 0 && static_cast<std::size_t>(length) < sizeof(bytes) - 1,
                "read /proc/self/io in one syscall");
        bytes[length] = '\0';
        const char* reads = std::strstr(bytes, "syscr: ");
        const char* writes = std::strstr(bytes, "syscw: ");
        require(reads && writes, "/proc/self/io lacks syscall counters");
        return {std::strtoull(reads + 7, nullptr, 10),
                std::strtoull(writes + 7, nullptr, 10)};
#else
        return {};
#endif
    }
    static Io difference(Io before, Io after, std::uint64_t inner_samples = 0) {
#if defined(__linux__)
        require(after.reads > before.reads && after.writes >= before.writes,
                "process I/O counters moved backwards");
        require(after.reads - before.reads > inner_samples,
                "counter window lacks its own snapshot reads");
        // The after snapshot includes the BEFORE snapshot's pread. Remove
        // only that counter read (and explicitly named nested samples).
        return {after.reads - before.reads - 1 - inner_samples,
                after.writes - before.writes};
#else
        (void)before; (void)after; (void)inner_samples;
        return {};
#endif
    }
private:
#if defined(__linux__)
    int fd_ = -1;
#endif
};

void zero_io(Io delta) {
    if (ProcessIo::enabled() && (delta.reads || delta.writes)) {
        std::fprintf(stderr, "admission fold syscr=%llu syscw=%llu\n",
                     static_cast<unsigned long long>(delta.reads),
                     static_cast<unsigned long long>(delta.writes));
        throw std::runtime_error("admission fold issued file I/O");
    }
}

pd::ScratchStatus numeric_order(void*, const pd::ScratchByteSource& a,
                                const pd::ScratchByteSource& b, int& order) noexcept {
    if (a.size() != 8 || b.size() != 8) return pd::ScratchStatus::programming_error;
    std::array<std::byte, 8> left{}, right{};
    auto status = a.read(0, {left.data(), left.size()});
    if (status != pd::ScratchStatus::ok) return status;
    status = b.read(0, {right.data(), right.size()});
    if (status != pd::ScratchStatus::ok) return status;
    order = 0;
    for (std::size_t i = 8; i-- != 0;) {
        if (left[i] != right[i]) {
            order = std::to_integer<unsigned>(left[i]) < std::to_integer<unsigned>(right[i]) ? -1 : 1;
            break;
        }
    }
    return pd::ScratchStatus::ok;
}

// Real production disk backing AND its default read cache, no fault wrapper
// and no arena. Wider records below prevent that cache from hiding readback.
// Geometry matches CommandScratchRun: two 512-byte pages, one journal/index
// slot. Common rows never change a resident limit or a scratch byte budget.
struct DiskStore {
    static constexpr std::size_t page_size = 512;
    std::unique_ptr<pd::ScratchBacking> backing = pd::make_command_scratch_backing(page_size);
    pd::CommandScratchStore store{
        {2, page_size, 1, 1, {0x61646d666f6c6401ULL, nullptr, &numeric_order},
         pd::ScratchOpenMode::create_empty}, backing.get()};
    DiskStore() {
        require(backing->kind() == pd::ScratchBackingKind::disk, "admission fixture must use a real file");
        ok(store.status(), "open admission store");
    }
};

// Existing explicit-instantiation access idiom from admission window tests.
// It observes the publication cache and invokes the exact production leaf.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
#define MEMBER(Tag, Owner, member) \
    struct Tag { friend auto access(Tag); }; \
    template struct Access<Tag, &Owner::member>
MEMBER(Cached, ad::Journal, folded_window_);
MEMBER(Valid, ad::Journal, fold_valid_);
MEMBER(Fold, pineforge::source::PineExecutionAdapter, fold_admission_events);
MEMBER(Count, pineforge::source::PineExecutionAdapter, admission_events_folded_);
MEMBER(Digest, pineforge::source::PineExecutionAdapter, admission_events_digest_);
MEMBER(Last, pineforge::source::PineExecutionAdapter, admission_events_last_);
#undef MEMBER

double binary64(std::uint64_t bits) {
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}
std::shared_ptr<const ad::CommandObservation> observation(std::uint64_t id, int bar,
                                                         std::size_t width = 137) {
    auto input = std::make_shared<ad::CommandObservation>();
    input->command = id;
    input->bar = bar;
    input->birth = pineforge::OrderBirth::direct_command(bar, 1234567 + bar);
    input->id = std::string(width, static_cast<char>('a' + id % 23)) + std::string("\0tail", 5);
    input->requested_quantity = 2.5;
    input->prices = {-0.0, binary64(0xfff8000000000041ULL)};
    input->oca_name = std::string("oca\0group", 9);
    input->configuration.long_margin = 100.0;
    input->configuration.short_margin = 100.0;
    input->configuration.default_quantity_value = -0.0;
    input->signal_close = std::numeric_limits<double>::infinity();
    input->original_sizing = ad::SizingObservation{2.5, -0.0, 100.0,
                                                  binary64(0x7ff8000000000083ULL), 1.0};
    return input;
}
ad::Event example(unsigned kind, std::uint64_t id, int bar, bool rejected) {
    auto origin = observation(1, bar, id % 11 == 0 ? 1031 : 137);
    const auto book = [&](std::uint64_t n) {
        ad::BookObservation row;
        row.incarnation = 70 + n; row.priority = -static_cast<std::int64_t>(n);
        row.bar = bar; row.buy = n % 2 != 0; row.id = origin->id;
        row.oca_name = origin->oca_name;
        row.prices = {-0.0, 0.0, binary64(0x7ff8000000000041ULL),
                      binary64(0xfff8000000000083ULL), 1.25};
        row.draft.bind(origin);
        row.draft.reviewed({11, ad::Checkpoint::TerminalGross, bar, 1});
        row.draft.sizing_revised({13, 97, bar, 1});
        return row;
    };
    if (kind == 0) {
        ad::CommandEvent event;
        event.observation = observation(id, bar, id % 11 == 0 ? 1031 : 137);
        event.outcome = rejected ? ad::Outcome::RejectedAffordability : ad::Outcome::Admitted;
        event.admitted_incarnation = 99 + id;
        event.before = {book(1), book(2)};
        event.removed = {81, 79};
        return event;
    }
    if (kind == 1) {
        ad::ReviewEvent event;
        event.receipt = {id, ad::Checkpoint::TerminalGross, bar, 0};
        event.configuration = origin->configuration;
        event.open_price = -0.0; event.position_side = -1; event.position_cycle = -71;
        event.book = {book(1), book(2)}; event.reviewed = {book(2), book(1)};
        event.resolutions = {{71, ad::ResolutionKind::Rejected, 0, -1, 0, missing},
                             {72, ad::ResolutionKind::PairedTransaction, 71, -2, -1, 0.5}};
        event.causes = {7, 3};
        return event;
    }
    ad::SizingEvent event;
    event.receipt = {id, 103, bar, 1}; event.incarnation = 991;
    event.before = {3.5, -0.0, 0.0, binary64(0x7ff8000000000041ULL), 1.25};
    event.after = {2.75, 991, 71.5, binary64(0xfff8000000000083ULL), 1.5};
    event.affordability_equity_before = 983.25;
    event.affordability_equity_after = 977.5;
    return event;
}

bool rejected(ad::Outcome outcome) {
    // Independent command-gate classification, not rejected_command().
    return outcome == ad::Outcome::RejectedIntradayCap
        || outcome == ad::Outcome::RejectedFrozenMarketCap
        || outcome == ad::Outcome::RejectedAffordability
        || outcome == ad::Outcome::RejectedPricedCap
        || outcome == ad::Outcome::OpeningRejectedReductionAdmitted;
}
ad::Journal::Prefix reference(const std::vector<ad::Event>& history) {
    // Full scan from the lifetime seed EVERY time. Neither Journal's fold nor
    // the codec's scanner/encoder/fold supplies any expected digest bytes.
    ad::Journal::Prefix result;
    for (const auto& event : history) {
        pineforge::BrokerStateHashSink sink;
        sink.h = result.digest;
        ad::reflect(event, "event", [&](const ad::Field& field) {
            sink.s(field.path); sink.u(field.value.index());
            std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::uint64_t>) sink.u(value);
                else if constexpr (std::is_same_v<T, std::int64_t>) sink.i(value);
                else if constexpr (std::is_same_v<T, double>) sink.d(value);
                else sink.s(value);
            }, field.value);
        });
        result.digest = sink.h;
        ++result.count;
        result.last_sequence = ad::sequence(event);
        if (const auto* command = std::get_if<ad::CommandEvent>(&event))
            if (rejected(command->outcome))
                result.last_rejected_command_bar = command->observation->bar;
    }
    return result;
}
void equal(const ad::Journal::Prefix& actual, const ad::Journal::Prefix& expected) {
    require(actual.count == expected.count, "admission lifetime event count differs");
    require(actual.digest == expected.digest, "independent admission field digest differs");
    require(actual.last_sequence == expected.last_sequence, "admission sequence frontier differs");
    require(actual.last_rejected_command_bar == expected.last_rejected_command_bar,
            "admission rejection bar differs");
}
void check(ad::Journal& journal, const std::vector<ad::Event>& history) {
    const auto expected = reference(history);
    equal(journal.folded_window(), expected);
    require(journal.last_rejected_command_bar() == expected.last_rejected_command_bar,
            "command rejection gate differs from independent history");
}
void publish(ad::Journal& journal, std::vector<ad::Event>& history, ad::Event event,
             bool encoded = false) {
    if (encoded) {
        const auto source = ad::encode_journal_record(event);
        ok(ad::append_journal_record(journal, source), "publish an encoded admission record");
    } else journal.append(event);
    history.push_back(std::move(event));
    std::sort(history.begin(), history.end(), [](const auto& a, const auto& b) {
        return ad::sequence(a) < ad::sequence(b);
    });
}
void check_stored(ad::Journal& journal, const std::vector<ad::Event>& history) {
    // Diagnostic readback is deliberately OUTSIDE every I/O window. Rebuild
    // the oracle using actual decoded store values plus the known retired
    // prefix, so a correct publication cache cannot hide a wrong stored tail.
    const auto retired = journal.retired_prefix().count;
    require(retired <= history.size(), "retired prefix exceeds the oracle history");
    std::vector<ad::Event> stored(history.begin(),
        history.begin() + static_cast<std::ptrdiff_t>(retired));
    const auto& tail = journal.events();
    stored.insert(stored.end(), tail.begin(), tail.end());
    equal(reference(stored), reference(history));
}
void calibrate_store_reads(ad::Journal& journal, ProcessIo& io) {
    const auto before = io.sample();
    const auto& events = journal.events(); // A real owning decode, outside fold windows.
    const auto after = io.sample();
    require(!events.empty()
            && ad::encode_journal_record(events.back()).size()
                > pd::CachedScratchBacking::kCacheBytes,
            "calibration needs a real record wider than the default backing cache");
    if (ProcessIo::enabled())
        require(ProcessIo::difference(before, after).reads > 0,
                "real store-read calibration did not issue a syscall");
}
void retire(ad::Journal& journal) {
    const auto prefix = journal.folded_window();
    journal.retire_prefix(prefix.count, prefix.digest, prefix.last_sequence);
}

ad::Journal::Prefix measured_fold(ad::Journal& journal, DiskStore& disk, ProcessIo& io) {
    const auto stats_before = disk.store.stats();
    const auto before = io.sample();
    const auto folded = journal.folded_window(); // ONLY operation inside the window.
    const auto after = io.sample();
    zero_io(ProcessIo::difference(before, after));
    const auto stats_after = disk.store.stats();
    require(stats_after.page_reads == stats_before.page_reads
            && stats_after.page_writes == stats_before.page_writes
            && stats_after.syncs == stats_before.syncs,
            "admission fold touched its backing");
    return folded;
}

void common_publication(ProcessIo& io, bool with_rejections, bool retire_each_bar) {
    DiskStore disk;
    ad::Journal journal;
    ad::Journal memory;
    ad::bind_journal_codec(journal, disk.store);
    equal(journal.folded_window(), {}); // Establish a valid EMPTY frontier.
    equal(memory.folded_window(), {});
    std::vector<ad::Event> history;
    constexpr int bars = 128;
    for (int bar = 0; bar < bars; ++bar) {
        if (bar % 13 == 12) {
            const auto gap = journal.next_sequence();
            require(memory.next_sequence() == gap, "memory/codec gap allocations differ");
            journal.abandon(gap);
            memory.abandon(gap);
        }
        const auto id = journal.next_sequence();
        // Rejected bars deliberately go backwards: the last SEQUENCE wins.
        auto event = example(static_cast<unsigned>(bar % 3), id, (bar * 7) % 31,
                             with_rejections && bar % 2 == 0);
        if (bar == 0)
            std::get<ad::CommandEvent>(event).observation =
                observation(id, 0, pressure_id_bytes);
        if (with_rejections && bar % 6 == 0) {
            constexpr ad::Outcome outcomes[] = {
                ad::Outcome::RejectedIntradayCap, ad::Outcome::RejectedFrozenMarketCap,
                ad::Outcome::RejectedAffordability, ad::Outcome::RejectedPricedCap,
                ad::Outcome::OpeningRejectedReductionAdmitted
            };
            std::get<ad::CommandEvent>(event).outcome = outcomes[(bar / 6) % 5];
        }
        if (!std::holds_alternative<ad::SizingEvent>(event))
            require(ad::encode_journal_record(event).size() > 2 * DiskStore::page_size,
                    "command/review must exceed the entire two-page resident cache");
        require(memory.next_sequence() == id, "memory/codec allocations differ");
        memory.append(event);
        publish(journal, history, std::move(event), bar % 2 != 0);
        if (bar == 0) calibrate_store_reads(journal, io);
        require(ad::journal_readback(journal).record_count() != 0,
                "zero-I/O row must have a codec-bound committed event");
        const auto at_publication = journal.*access(Cached{});
        const bool valid_at_publication = journal.*access(Valid{});
        const auto expected = reference(history);
        // RED on base: the FIRST fold after append reads the real disk store.
        // Repeating a previously folded query would be a false witness.
        equal(measured_fold(journal, disk, io), expected);
        // Also kills moving the fold back into the query while avoiding disk.
        require(valid_at_publication, "ordered publication invalidated a proved fold");
        equal(at_publication, expected);
        equal(memory.*access(Cached{}), expected);
        equal(memory.folded_window(), expected);
        require(journal.last_rejected_command_bar() == expected.last_rejected_command_bar,
                "publication did not advance the rejection gate");
        if (bar % 16 == 15) check_stored(journal, history);
        if (retire_each_bar) {
            retire(journal);
            retire(memory);
        }
    }
    require(disk.store.stats().page_writes != 0 && disk.backing->page_count() > 2,
            "admission common row never reached real backing pages");
}

void fallback_histories(ProcessIo& io) {
    DiskStore disk;
    ad::Journal journal;
    ad::bind_journal_codec(journal, disk.store);
    std::vector<ad::Event> history;
    // fold_valid_ starts false. Both appends must enter the first slow fold.
    for (unsigned kind : {0U, 1U})
        publish(journal, history, example(kind, journal.next_sequence(), 5, true));
    check(journal, history);
    retire(journal);

    const auto outer = journal.next_sequence();
    const auto inner = journal.next_sequence();
    const auto later = journal.next_sequence();
    publish(journal, history, example(0, inner, 29, true));
    check(journal, history);
    publish(journal, history, example(0, outer, 30, true)); // Invalidates the frontier.
    require(!(journal.*access(Valid{})), "earlier completion must invalidate the fold");
    publish(journal, history, example(2, later, 2, false)); // While fold_valid_ is false.
    check(journal, history); // Kills skipping outer while eagerly folding later.
    retire(journal);

    {
        const auto earlier = journal.next_sequence();
        const auto later_first = journal.next_sequence();
        const auto last = journal.next_sequence();
        publish(journal, history, example(0, later_first, 11, true));
        // No query between these publications: restoration leaves a valid
        // older frontier, but it no longer covers ALL already stored events.
        publish(journal, history, example(1, earlier, 11, false));
        require(journal.*access(Valid{}), "unobserved completion lost the valid older frontier");
        publish(journal, history, example(2, last, 11, false));
        check(journal, history); // Kills deleting ONLY the completeness/count guard.
        retire(journal);
    }

    const auto removed = journal.next_sequence();
    const auto kept_rejection = journal.next_sequence();
    const auto kept = journal.next_sequence();
    publish(journal, history, example(0, removed, 17, false));
    publish(journal, history, example(0, kept_rejection, 7, true));
    publish(journal, history, example(1, kept, 7, false));
    check(journal, history);
    journal.retain({kept_rejection, kept});
    history.erase(std::remove_if(history.begin(), history.end(), [&](const auto& event) {
        return ad::sequence(event) == removed;
    }), history.end());
    // A new rejection cannot bypass validation of the filtered old frontier.
    publish(journal, history, example(0, journal.next_sequence(), 3, true));
    check(journal, history); // Valid nonzero prefix + preserved old rejection.
    retire(journal);
    publish(journal, history, example(0, journal.next_sequence(), 1, true));
    equal(measured_fold(journal, disk, io), reference(history));
    require(journal.last_rejected_command_bar() == 1, "gate took greatest bar instead of latest sequence");

    journal.reset();
    history.clear();
    require(journal.sequence_frontier() == 1 && journal.event_count() == 0,
            "reset retained lifetime state");
    equal(journal.folded_window(), {});
    publish(journal, history, example(0, journal.next_sequence(), 0, false));
    equal(measured_fold(journal, disk, io), reference(history));
    require(journal.last_rejected_command_bar() == -1, "reset retained the old rejection gate");
}

void destructive_filter_refusals() {
    for (bool with_prefix : {false, true}) {
        DiskStore disk;
        ad::Journal journal;
        ad::bind_journal_codec(journal, disk.store);
        std::vector<ad::Event> history;
        (void)journal.folded_window();
        if (with_prefix) {
            publish(journal, history, example(0, journal.next_sequence(), 0, false));
            retire(journal);
        }
        const auto lost = journal.next_sequence();
        const auto kept = journal.next_sequence();
        publish(journal, history, example(0, lost, 7, true));
        publish(journal, history, example(1, kept, 7, false));
        check(journal, history);
        journal.retain({kept});
        invariant([&] { (void)journal.folded_window(); });
        require(journal.last_rejected_command_bar() == 7, "filter rewrote the old rejection gate");
        publish(journal, history, example(0, journal.next_sequence(), 9, true));
        invariant([&] { (void)journal.folded_window(); });
        require(journal.last_rejected_command_bar() == 7,
                "a later rejection repaired a discarded old rejection");
    }
}

void unobserved_filter_frontier() {
    for (bool out_of_order : {false, true}) {
        DiskStore disk;
        ad::Journal journal;
        ad::bind_journal_codec(journal, disk.store);
        std::vector<ad::Event> history;
        (void)journal.folded_window();
        publish(journal, history, example(0, journal.next_sequence(), 5, true));
        retire(journal);
        if (out_of_order) {
            const auto earlier = journal.next_sequence();
            const auto later = journal.next_sequence();
            publish(journal, history, example(0, later, 9, true));
            // No intervening fold query. The earlier event is still AFTER
            // the old queried frontier, so its original validity survives.
            publish(journal, history, example(0, earlier, 8, false));
            require(journal.*access(Valid{}), "unobserved tail invalidated the old queried prefix");
        } else {
            publish(journal, history, example(0, journal.next_sequence(), 9, true));
        }
        const auto kept = journal.next_sequence();
        publish(journal, history, example(1, kept, 7, false));
        // Filter BEFORE querying the just-appended rejection. The old path
        // permits this under a nonzero prefix; eager caching must do so too.
        const std::vector<ad::Event> expected{history.front(), history.back()};
        ad::Journal copied(journal); // Owning diagnostic copies are outside I/O windows.
        ad::Journal assigned;
        assigned = journal;
        ad::Journal moved(std::move(copied));
        ad::Journal move_assigned;
        move_assigned = std::move(assigned);
        for (auto* value : {&journal, &moved, &move_assigned}) {
            value->retain({kept});
            check(*value, expected);
            require(value->last_rejected_command_bar() == 5,
                    "unobserved removed rejection replaced the validated prefix gate");
        }
    }
}

void binding_after_history() {
    // Frozen-base bind_journal_codec REJECTS nonempty/mid-history binding.
    // Preserve that contract; migration is not silently introduced by a cache.
    DiskStore disk;
    ad::Journal journal;
    std::vector<ad::Event> history;
    publish(journal, history, example(0, journal.next_sequence(), 2, true));
    check(journal, history);
    invariant([&] { ad::bind_journal_codec(journal, disk.store); });
    check(journal, history);
    require(disk.store.record_count() == 0, "refused mid-history bind changed the store");
    journal.reset();
    history.clear();
    ad::bind_journal_codec(journal, disk.store);
    publish(journal, history, example(1, journal.next_sequence(), 0, false));
    check(journal, history);
}

void chunk_boundaries_and_canonical_numbers() {
    for (unsigned kind = 0; kind != 3; ++kind) {
        const auto event = example(kind, 11, 7, true);
        const auto expected = reference({event});
        const auto source = ad::encode_journal_record(event);
        std::array<std::byte, 97> small{};
        std::array<std::byte, 512> normal{};
        std::array<std::byte, 4096> large{};
        for (pd::ScratchBuffer page : {pd::ScratchBuffer{small.data(), small.size()},
                                      pd::ScratchBuffer{normal.data(), normal.size()},
                                      pd::ScratchBuffer{large.data(), large.size()}}) {
            auto digest = seed;
            ok(ad::fold_journal_record(source, page, digest), "fold chunked in-memory encoding");
            require(digest == expected.digest, "page geometry changed the normalized field fold");
        }
    }
}

void failed_publication() {
    // Real production cap failure, separately scoped from default-budget I/O
    // rows. The two-MiB value cannot fit the one-MiB disk-backing allowance.
    struct CapScope {
        pd::CommandScratchByteCap prior = pd::set_command_scratch_byte_cap(1024 * 1024);
        ~CapScope() { pd::set_command_scratch_byte_cap(std::move(prior)); }
    } cap;
    DiskStore disk;
    ad::Journal journal;
    ad::bind_journal_codec(journal, disk.store);
    std::vector<ad::Event> history;
    (void)journal.folded_window();
    publish(journal, history, example(0, journal.next_sequence(), 2, true));
    const auto stable = journal.folded_window();
    const auto stable_count = journal.event_count();
    ad::CommandEvent too_large;
    const auto id = journal.next_sequence();
    too_large.observation = observation(id, 3, 2 * 1024 * 1024);
    too_large.outcome = ad::Outcome::RejectedPricedCap;
    const ad::Event event = std::move(too_large);
    const auto source = ad::encode_journal_record(event);
    require(ad::append_journal_record(journal, source) == pd::ScratchStatus::io_error,
            "real capped scratch append must fail");
    require(!disk.store.transaction_active(), "failed append did not roll back");
    equal(journal.*access(Cached{}), stable);
    equal(journal.folded_window(), stable);
    require(journal.event_count() == stable_count && journal.has_unfinished_events(),
            "failed store publication consumed an event");
    require(journal.last_rejected_command_bar() == 2, "failed publication advanced the rejection gate");
    journal.abandon(id);
    publish(journal, history, example(0, journal.next_sequence(), 1, false));
    check(journal, history);
}

constexpr int host_bars = 384;
constexpr int warmup_bars = 16;
class AdmissionHost final : public pineforge::source::PineStrategyHost {
public:
    AdmissionHost(DiskStore& disk, ProcessIo& io) : disk_(disk), io_(io) {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 100'000.0;
        config.default_qty_type = static_cast<int>(pineforge::QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 1;
        config.margin_long = config.margin_short = 100.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }
    void on_source_bar(const pineforge::Bar&) override {
        require(callbacks >= 0 && callbacks < host_bars, "unexpected Pine host callback count");
        auto& journal = market_admission_journal();
        if (callbacks == 0) {
            // This base has no host-side codec installation. Bind before its
            // first command, after run reset, without changing production code.
            ad::bind_journal_codec(journal, disk_.store);
            (void)journal.folded_window();
        }
        const auto before_command = journal.event_count();
        // A real accepted, unfilled entry replacement produces an admission
        // command on EVERY bar; there is no growing live order population.
        strategy_entry(entry_id_, true, 1.0, missing, 1.0);
        require(journal.event_count() > before_command, "host bar produced no admission event");
        require(ad::journal_readback(journal).record_count() > 0, "host journal codec is inactive");
        if (callbacks == 0) calibrate_store_reads(journal, io_);
        ++event_bars;
        const bool measuring = callbacks >= warmup_bars;
        const auto stats_before = disk_.store.stats();
        const auto before = measuring ? io_.sample() : Io{};
        (adapter_.*access(Fold{}))(); // Exact leaf called by finish_admission_bar.
        const auto after = measuring ? io_.sample() : Io{};
        if (measuring) {
            deltas[static_cast<std::size_t>(callbacks)] = ProcessIo::difference(before, after);
            const auto stats_after = disk_.store.stats();
            backing_touches += stats_after.page_reads - stats_before.page_reads
                + stats_after.page_writes - stats_before.page_writes
                + stats_after.syncs - stats_before.syncs;
            ++measured;
        }
        require(adapter_.*access(Count{}) == journal.event_count(), "host fold missed the new command");
        require(adapter_.*access(Last{}) > previous_last_, "host fold did not advance its sequence");
        previous_last_ = adapter_.*access(Last{});
        require(adapter_.*access(Digest{}) != seed, "host admission digest stayed at its seed");
        ++callbacks;
    }
    int callbacks = 0, event_bars = 0, measured = 0;
    std::uint64_t backing_touches = 0;
    std::array<Io, host_bars> deltas{};
private:
    DiskStore& disk_;
    ProcessIo& io_;
    std::uint64_t previous_last_ = 0;
    // One fixed unfilled id, larger than the DEFAULT backing cache. No
    // budget override or cache bypass: an old readback fold must reach disk
    // on every measured bar, including after the journal retires its prefix.
    const std::string entry_id_ = "admfold-entry/" + std::string(pressure_id_bytes, 'e');
};

void pine_host(ProcessIo& io) {
    DiskStore disk; // Outlives the host's borrowed codec.
    AdmissionHost host(disk, io);
    std::vector<pineforge::Bar> bars;
    for (int i = 0; i < host_bars; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0, 1736121600000LL + i * 60000LL});
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    if (!host.last_error().empty())
        throw std::runtime_error("Pine admission host failed: " + host.last_error());
    require(host.callbacks == host_bars && host.event_bars == host_bars,
            "Pine host must publish on all 384 bars");
    require(host.measured == host_bars - warmup_bars, "Pine fold warmup hid measured bars");
    for (int bar = warmup_bars; bar < host_bars; ++bar)
        zero_io(host.deltas[static_cast<std::size_t>(bar)]);
    require(host.backing_touches == 0, "Pine fold touched the admission backing");
    require(host.market_admission_journal().events().empty(),
            "finish_admission_bar did not retire completed host events");
    require(host.market_admission_journal().retired_prefix().count >= host_bars,
            "host measured no lifetime admission history");
    require(host.trade_count() == 0, "unfilled-entry host unexpectedly traded");
    require(disk.store.stats().page_writes > 0, "host codec never wrote to its real file");
}
} // namespace

int main() {
    ProcessIo io;
    int failures = 0;
    const auto run = [&](const char* name, auto&& row) {
        try {
            row();
            std::printf("PASS admfold %s\n", name);
        } catch (const std::exception& error) {
            ++failures;
            std::fprintf(stderr, "FAIL admfold %s: %s\n", name, error.what());
        }
    };
    // Independent catches keep both required RED witnesses visible on base.
    run("codec common accepted", [&] { common_publication(io, false, false); });
    run("codec common rejected", [&] { common_publication(io, true, false); });
    run("codec retired accepted", [&] { common_publication(io, false, true); });
    run("codec retired rejected", [&] { common_publication(io, true, true); });
    run("invalid fold and legal filter", [&] { fallback_histories(io); });
    run("destructive filter refusals", destructive_filter_refusals);
    run("unobserved filter frontier and value semantics", unobserved_filter_frontier);
    run("binding after history", binding_after_history);
    run("encoded page equivalence", chunk_boundaries_and_canonical_numbers);
    run("failed store publication", failed_publication);
    run("Pine host isolated fold", [&] { pine_host(io); });
    return failures ? 1 : 0;
}
