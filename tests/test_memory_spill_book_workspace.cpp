// Spilled-book workspace follows request growth, matching and retirement.
// Complete BookSpill by including the real implementation
// verbatim. The byte-size check uses a sizeof model of the store layout; there is
// no injected index row, product seam or alternate matcher.
#include "../src/native_order.cpp"
#include "../src/command_scratch_cache.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace memory_spill_book_workspace {
using namespace pineforge;
namespace no = pineforge::native_order;
using Word = std::uint64_t;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Spill { friend auto access(Spill); };
struct BookStore { friend auto access(BookStore); };
struct StoreState { friend auto access(StoreState); };
struct BackingInner { friend auto access(BackingInner); };
template struct Access<Spill, &no::WorkingRequestCore::book_spill_>;
template struct Access<BookStore, &NativeExecutionConsumer::match_book_scratch_>;
template struct Access<StoreState, &detail::CommandScratchStore::state_>;
template struct Access<BackingInner, &DormantBookScratchBacking::inner_>;

std::size_t checks = 0, failures = 0;
std::string phase;
struct UnsafeRead {};
bool check(bool passed, const char* expression, int line) {
    ++checks;
    if (!passed) {
        ++failures;
        std::fprintf(stderr, "FAIL [book-workspace] %s:%d: %s (phase=%s)\n",
                     __FILE__, line, expression, phase.c_str());
    }
    return passed;
}
#define MEM_CHECK(expression) check(bool(expression), #expression, __LINE__)
// Only an unsafe prerequisite ends a trial. Ordinary failed equalities keep
// going, including the empty-index and live-index release trials.
#define MEM_NEED(expression) do { \
    if (!MEM_CHECK(expression)) throw UnsafeRead{}; \
} while (false)

template<class Action>
void trial(const std::string& name, Action action) {
    phase = name;
    try { action(); }
    catch (const UnsafeRead&) {} // Already reported; continue the next trial.
    catch (const std::exception& error) { check(false, error.what(), __LINE__); }
    catch (...) { check(false, "unexpected exception", __LINE__); }
}

constexpr std::size_t batch = 512;
constexpr std::int64_t start = 1736121600000LL;
constexpr Word sign = Word{1} << 63;

Word bits(double value) {
    static_assert(sizeof(double) == sizeof(Word));
    Word result = 0;
    std::memcpy(&result, &value, sizeof result);
    return result;
}
Word big_endian(const std::byte* bytes) {
    Word result = 0;
    for (unsigned i = 0; i != sizeof(Word); ++i)
        result = result * 256 + std::to_integer<unsigned char>(bytes[i]);
    return result;
}

// Semantic rows: trigger/direction class, price bits, queue priority, handle.
// The oracle uses only issued prices and accepted handles. With no replacement,
// LiveRequest::priority is the original handle incarnation.
using Row = std::tuple<unsigned, Word, Word, Word>;
using Rows = std::multiset<Row>;
struct Issued {
    no::RequestHandle handle;
    double price;
    bool pending = true;
};

NativeRunSpec spec() {
    NativeRunSpec s;
    s.identity = {"book-workspace", 1};
    s.input_tf = s.script_tf = "1";
    s.ticker = "MOCK"; s.tickerid = "TEST:BOOK";
    s.type = "crypto"; s.currency = "USD";
    s.timezone = "UTC"; s.session = "24x7";
    s.initial_capital = 1000000.0;
    s.point_value = s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.close_execution = NativeCloseExecution::AfterCalculation;
    s.report_policy = NativeReportPolicy::KernelRecorded;
    return s;
}

class BookHost final : public NativeStrategyHost {
public:
    explicit BookHost(std::size_t population) : population_(population) {}
    std::vector<Issued> issued;
    std::size_t callbacks = 0, fills = 0;
    NativeExecutionConsumer& consumer() {
        return as_native_consumer(execution_consumer());
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        // The previous batch has an entire bar to join through MatchDormantPoint.
        // Candidate-free submission can then spill older dormant requests,
        // rather than spill an active row and disable the acceleration path.
        const auto end = std::min(population_, issued.size() + batch);
        while (issued.size() < end) {
            const double price = 40.0 + static_cast<double>(issued.size() % 29) * 0.25;
            no::Request request{no::Transact{1.0}, "resting-order", ""};
            request.trigger = no::Limit{price};
            const auto accepted = submit(request);
            MEM_NEED(accepted.status == no::SubmitStatus::Accepted && accepted.handle);
            issued.push_back({*accepted.handle, price, true});
        }
    }
    void on_native_applied(const no::ExecutionAppliedEvent&,
                           const NativeDecisionContext&) override { ++fills; }
private:
    std::size_t population_;
};

Rows model(const BookHost& host) {
    Rows expected;
    for (const auto& row : host.issued)
        if (row.pending)
            expected.emplace(0u, bits(row.price),
                             row.handle.incarnation, row.handle.incarnation);
    return expected;
}

Rows observe(detail::CommandScratchStore& store, const Rows& expected,
             std::size_t issued_count) {
    MEM_CHECK(store.status() == detail::ScratchStatus::ok);
    MEM_CHECK(!store.transaction_active());
    MEM_CHECK(store.record_count() == expected.size());
    Rows actual;
    detail::ScratchHandle cursor = detail::scratch_no_handle;
    auto status = store.first(cursor);
    // A damaged cursor must fail finitely. No legitimate phase can contain
    // more records than requests ever issued, even after a failed erase.
    std::size_t walked = 0;
    while (status == detail::ScratchStatus::ok && walked <= issued_count) {
        ++walked;
        detail::ScratchRecordInfo info;
        MEM_NEED(store.point(cursor, info) == detail::ScratchStatus::ok);
        // Source schema: one class byte, one double ordering word, one
        // priority word; the value is one incarnation word.
        std::array<std::byte, 1 + sizeof(double) + sizeof(Word)> key{};
        std::array<std::byte, sizeof(Word)> value{};
        const bool shape = info.key_bytes == key.size() && info.value_bytes == value.size();
        MEM_CHECK(shape);
        if (shape) {
            MEM_NEED(store.read(cursor, detail::ScratchField::key, 0,
                              {key.data(), key.size()}) == detail::ScratchStatus::ok);
            MEM_NEED(store.read(cursor, detail::ScratchField::value, 0,
                              {value.data(), value.size()}) == detail::ScratchStatus::ok);
            // All issued levels are positive buy limits. Their reverse numeric
            // ordering is (2^63 - 1) - IEEE price bits. Decode by subtraction,
            // independently of BookSpill's sign/xor/complement encoder.
            const Word price_bits = (sign - 1) - big_endian(key.data() + 1);
            actual.emplace(std::to_integer<unsigned char>(key[0]), price_bits,
                           big_endian(key.data() + 1 + sizeof(double)),
                           big_endian(value.data()));
        }
        detail::ScratchHandle next = detail::scratch_no_handle;
        status = store.next(cursor, next);
        cursor = next;
    }
    MEM_CHECK(status == detail::ScratchStatus::end);
    MEM_CHECK(actual == expected);
    return actual;
}

std::size_t store_workspace_bytes() {
    // Independent layout arithmetic from StoreImpl's scratch-record format and
    // RunCommandScratch's {2,512,1,1} geometry. Two slot descriptors, four hash
    // buckets (minimum four, >= twice the slot count), two cached pages and
    // six work pages. Do not call required_bytes()/stats() for the oracle.
    using Core = detail::scratch_impl::Core;
    const auto core = ((sizeof(Core) + 15) / 16) * 16;
    const auto slots = ((2 * sizeof(Core::Slot) + 15) / 16) * 16;
    const auto buckets = ((4 * sizeof(std::int32_t) + 63) / 64) * 64;
    return core + slots + buckets + (2 + 6) * 512;
}

void fixed_workspace(detail::CommandScratchStore& store) {
    MEM_CHECK(store.page_size() == 512);
    MEM_CHECK(store.stats().resident_bytes == store_workspace_bytes());
    // Inspect the actual lazy backing and actual production cache. Both
    // supported physical media are wrapped in CachedScratchBacking.
    const auto* state = store.*access(StoreState{});
    MEM_NEED(state);
    const auto* lazy = dynamic_cast<const DormantBookScratchBacking*>(state->backing_);
    MEM_NEED(lazy);
    const auto& inner = lazy->*access(BackingInner{});
    const auto* cached = dynamic_cast<const detail::CachedScratchBacking*>(inner.get());
    MEM_NEED(cached);
    // CacheDefaults::book_pages specifies 128 lines of 512 bytes. This is a
    // cache policy, NOT a record bound. Count tag/pin bytes as well as payload.
    const std::size_t lines = 128, page = 512;
    const auto line_bytes = detail::ReadThroughCache::line_bytes(page);
    MEM_CHECK(cached->read_cache_stats().allocation_bypass_count == 0);
    MEM_CHECK(cached->cache_slot_count() == lines);
    MEM_CHECK(cached->cache_payload_bytes() == lines * page);
    MEM_CHECK(cached->cache_storage_bytes() == lines * line_bytes);
    MEM_CHECK(cached->cache_metadata_bytes() == lines * (line_bytes - page));
}

struct Observation {
    Rows rows;
    std::size_t bars = 0;
    detail::ScratchStoreStats storage;
};

Observation book(std::size_t population, std::size_t bar_multiplier, bool drain) {
    BookHost host(population);
    MEM_NEED(host.configure_native(spec()).status == NativeSetupStatus::Applied);
    const auto n = ((population + batch - 1) / batch + 8) * bar_multiplier;
    std::vector<Bar> bars;
    for (std::size_t i = 0; i != n; ++i)
        bars.push_back({100.0, 100.0, 100.0, 100.0, 1.0,
                        start + static_cast<std::int64_t>(i) * 60000});
    MEM_NEED(host.stream_begin(bars.data(), static_cast<int>(bars.size()), "1", "1"));
    auto& consumer = host.consumer();
    const auto& core = consumer.request_core();
    const auto spill = core.*access(Spill{});
    MEM_NEED(spill && spill->level_index);
    auto& store = *spill->level_index;
    MEM_CHECK((consumer.*access(BookStore{})) == &store);
    MEM_CHECK(host.callbacks == bars.size());
    MEM_CHECK(host.issued.size() == population && core.live_size() == population);
    MEM_CHECK(host.fills == 0);
    // Insertion evidence is independent of the test's expected count: the real
    // core spilled, the real matcher joined, and the backing crossed its fixed
    // 128*512-byte resident prefix. The full read below checks actual records.
    MEM_CHECK(core.book_spilled() && spill->alive > 0);
    MEM_CHECK(spill->active_spilled == 0 && !spill->poison);
    MEM_CHECK(consumer.dormant_rows() == host.issued.size());
    MEM_CHECK(store.stats().backing_bytes > 128 * 512);
    auto expected = model(host);
    Observation result{observe(store, expected, host.issued.size()),
                       host.callbacks, store.stats()};
    fixed_workspace(store);

    phase += "/cancel-even";
    for (std::size_t i = 0; i < host.issued.size(); i += 2) {
        auto& issued = host.issued[i];
        const auto cancelled = host.cancel(issued.handle);
        MEM_CHECK(cancelled.status == no::CancelStatus::Cancelled);
        // The oracle follows the issued cancellation, not the store's reply.
        issued.pending = false;
    }
    expected = model(host);
    MEM_CHECK(expected.size() == population / 2);
    MEM_CHECK(core.live_size() == expected.size());
    observe(store, expected, host.issued.size());
    fixed_workspace(store);

    if (drain) {
        phase += "/cancel-odd";
        for (std::size_t i = 1; i < host.issued.size(); i += 2) {
            auto& issued = host.issued[i];
            const auto cancelled = host.cancel(issued.handle);
            MEM_CHECK(cancelled.status == no::CancelStatus::Cancelled);
            issued.pending = false;
        }
        expected = model(host);
        MEM_CHECK(expected.empty() && core.live_size() == 0);
        observe(store, expected, host.issued.size());
        fixed_workspace(store);
    }

    // Exercise release with both an empty store and a still-live store.
    // Erasure retires logical rows; it does not promise to shorten the file.
    // Save the exact live digest before end, never dereference store afterward.
    std::array<std::byte, 512> digest_page{};
    Word digest = 0;
    MEM_CHECK(store.state_digest({digest_page.data(), digest_page.size()}, digest)
             == detail::ScratchStatus::ok);
    phase += drain ? "/end-empty" : "/end-live";
    MEM_CHECK(host.stream_end(false));
    MEM_CHECK(spill->level_index == nullptr);
    MEM_CHECK((consumer.*access(BookStore{})) == nullptr);
    const auto released = consumer.released_command_scratch(
        NativeExecutionConsumer::CommandScratchBinding::Book);
    MEM_CHECK(released.has_value());
    if (released) {
        MEM_CHECK(released->status == detail::ScratchStatus::ok);
        MEM_CHECK(released->digest == digest);
    }
    return result;
}
} // namespace memory_spill_book_workspace

int main() {
    using namespace memory_spill_book_workspace;
    std::optional<Observation> small_population;
    for (const std::size_t population : {std::size_t{4352}, std::size_t{17408}}) {
        std::optional<Observation> short_run, long_run;
        trial("pending=" + std::to_string(population) + "/bars-x1", [&] {
            short_run = book(population, 1, true);
        });
        trial("pending=" + std::to_string(population) + "/bars-x4", [&] {
            long_run = book(population, 4, false);
        });
        trial("pending=" + std::to_string(population) + "/compare-bars", [&] {
            MEM_NEED(short_run && long_run);
            MEM_CHECK(long_run->bars == 4 * short_run->bars);
            MEM_CHECK(short_run->rows == long_run->rows);
            MEM_CHECK(short_run->rows.size() == population);
            MEM_CHECK(short_run->storage.backing_bytes == long_run->storage.backing_bytes);
            MEM_CHECK(short_run->storage.resident_bytes == long_run->storage.resident_bytes);
            if (small_population) {
                MEM_CHECK(short_run->rows.size() == 4 * small_population->rows.size());
                MEM_CHECK(short_run->storage.backing_bytes > small_population->storage.backing_bytes);
                MEM_CHECK(short_run->storage.resident_bytes == small_population->storage.resident_bytes);
            } else small_population = short_run;
        });
    }
    std::printf("memory proof spill_book_workspace: %zu checks, %zu failures\n", checks, failures);
    return failures ? 1 : 0;
}
