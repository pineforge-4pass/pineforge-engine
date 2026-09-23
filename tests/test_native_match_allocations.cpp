// R5 lane PERF-L5: a warm matcher allocates nothing per driver point.
//
// match_path used to allocate as it matched: the pointer snapshot of a book
// of more than eight live requests (once per scan), the provenance of its
// rows, the set of (incarnation, kind) keys it skipped, and observe_trails'
// copy of the book's handles whenever a trail was tracking. Each now lives
// in the consumer's scratch and keeps its capacity from call to call.
//
// The witness is a count of every operator new between two script bars of a
// steady book: requests placed early and resting after, so no command, fill
// or event falls inside the window. The same window of an idle run of the
// same tape counts what the kernel allocates without a book (none per point
// today; the calendar's day boundaries lie outside the window anyway), and
// each book must allocate exactly that. The books cover the matcher's shapes:
//   * entries that await their allowance at every point (Evaluate rows, each
//     refreshed and rescanned alone), 12 of them, over the snapshot's eight;
//   * reduce-only exits bound to the book (rows refreshed in bulk and priced
//     at every point), 12 of them;
//   * host-sized exits bound to a cohort (the side read through the cohort
//     target), 12 of them;
//   * exits trailing the price (a tracked best at every point, advanced by
//     observe_trails), 3 of them next to 9 resting exits;
// each on the four modelled points of a bar and on a synthesized path. The
// window skips the pump's doubling checkpoints, where the command history may
// be sized ahead of its growth.
//
// This row FAILS on f71cd820, the tree before the lane: there each scan of a
// book of more than eight requests allocated its snapshot, and every point
// with a tracking trail allocated observe_trails' handles.
//
// Source-free: it also runs in the kernel-only profile.
#include <pineforge/native_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <vector>

namespace {
bool count_allocations = false;
std::size_t allocations = 0;
}  // namespace

void* operator new(std::size_t size) {
    if (count_allocations) ++allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

enum class Book { Idle, Entries, Exits, CohortExits, Trails };

const char* book_name(Book book) {
    switch (book) {
    case Book::Idle: return "idle";
    case Book::Entries: return "entries";
    case Book::Exits: return "exits";
    case Book::CohortExits: return "cohort exits";
    case Book::Trails: return "trails";
    }
    return "?";
}

constexpr int kBars = 200;
// Counted from the script bar opening the window's first input to the one
// before its last; the pump's doubling checkpoints (64, 128) lie outside.
constexpr int kFirstCounted = 70;
constexpr int kLastCounted = 125;

// A slow random walk around 100 on a quarter-tick grid, 5-minute UTC bars
// from midnight: the window holds no day boundary.
std::vector<Bar> make_bars() {
    std::vector<Bar> bars;
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    double price = 100.0;
    for (int i = 0; i < kBars; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        const double step = static_cast<double>(static_cast<int>(state % 9) - 4) * 0.25;
        Bar bar{};
        bar.open = price;
        bar.close = price + step;
        bar.high = (bar.open > bar.close ? bar.open : bar.close) + 0.25;
        bar.low = (bar.open < bar.close ? bar.open : bar.close) - 0.25;
        bar.volume = 1.0;
        bar.timestamp = 1736121600000LL + static_cast<std::int64_t>(i) * 300000;
        bars.push_back(bar);
        price = bar.close;
    }
    return bars;
}

class SteadyBook final : public NativeStrategyHost {
public:
    explicit SteadyBook(Book book) : book_(book) {}

    std::size_t counted = 0;
    std::size_t working_in_window = 0;

    void on_native_run_begin() override { bar_ = 0; }

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ++bar_;
        if (bar_ == kFirstCounted) {
            working_in_window = native_working_requests().size();
            allocations = 0;
            count_allocations = true;
            return;
        }
        if (bar_ == kLastCounted) {
            count_allocations = false;
            counted = allocations;
            return;
        }
        if (bar_ == 1) open(bar);
        if (bar_ == 3) rest(bar);
    }

private:
    void open(const Bar&) {
        if (book_ == Book::Idle || book_ == Book::Entries) return;
        no::Request entry{no::Transact{12.0}, "open", ""};
        const auto result = submit(entry);
        // The roster takes the opening while it is still working.
        if (result.handle && book_ == Book::CohortExits) {
            cohort_ = cohort_open();
            cohort_add(*cohort_, *result.handle);
        }
    }

    void rest(const Bar& bar) {
        for (int i = 0; i < 12; ++i) {
            const double far = 20.0 + 0.25 * i;
            switch (book_) {
            case Book::Idle:
                return;
            case Book::Entries: {
                no::Request request{no::Transact{1.0}, "rest", ""};
                request.trigger = no::Limit{bar.close - far};
                submit(request);
                break;
            }
            case Book::Exits: {
                no::Request request{no::Reduce{no::ExplicitUnits{1.0}}, "exit", ""};
                if (i % 2 == 0) request.trigger = no::Limit{bar.close + far};
                else request.trigger = no::Stop{bar.close - far};
                submit(request);
                break;
            }
            case Book::CohortExits: {
                no::Request request{no::HostSized{no::HostSizedKind::Close, std::nullopt},
                                    "cohort", ""};
                if (cohort_) request.owner = no::BindCohort{*cohort_};
                if (i % 2 == 0) request.trigger = no::Limit{bar.close + far};
                else request.trigger = no::Stop{bar.close - far};
                submit(request);
                break;
            }
            case Book::Trails: {
                no::Request request{no::Reduce{no::ExplicitUnits{1.0}}, "trail", ""};
                if (i < 3) {
                    no::Trail trail;
                    trail.offset = far;
                    request.trigger = trail;
                } else {
                    request.trigger = no::Limit{bar.close + far};
                }
                submit(request);
                break;
            }
            }
        }
    }

    Book book_;
    int bar_ = 0;
    std::optional<no::CohortHandle> cohort_;
};

NativeRunSpec make_spec(bool synthesized) {
    NativeRunSpec spec;
    spec.identity = {"l5-allocations", 1};
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.tickerid = "TEST:L5ALLOC";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    spec.initial_capital = 1.0e9;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.25;
    spec.fee_kind = NativeFeeKind::Percent;
    spec.fee_value = 0.05;
    if (synthesized) {
        IntrabarPath::synthesized path;
        path.samples = 4;
        spec.intrabar.value = path;
    }
    return spec;
}

struct Counted {
    std::size_t allocations = 0;
    std::size_t working = 0;
    bool completed = false;
};

Counted run(Book book, bool synthesized, const std::vector<Bar>& bars) {
    SteadyBook host(book);
    Counted out;
    if (host.configure_native(make_spec(synthesized)).status != NativeSetupStatus::Applied) {
        return out;
    }
    host.run(bars.data(), static_cast<int>(bars.size()));
    count_allocations = false;
    out.completed = host.native_state().kind == NativeLifecycleKind::Completed;
    out.allocations = host.counted;
    out.working = host.working_in_window;
    return out;
}

void steady_books_allocate_what_an_idle_run_does() {
    const auto bars = make_bars();
    for (const bool synthesized : {false, true}) {
        const Counted idle = run(Book::Idle, synthesized, bars);
        CHECK(idle.completed);
        for (const Book book : {Book::Entries, Book::Exits, Book::CohortExits, Book::Trails}) {
            const Counted counted = run(book, synthesized, bars);
            CHECK(counted.completed);
            // The book must actually be resting through the window.
            CHECK(counted.working == 12);
            const int windows = kLastCounted - kFirstCounted;
            std::printf("  %-12s path=%s: %zu allocations over %d bars (idle %zu)\n",
                        book_name(book), synthesized ? "synth" : "none ", counted.allocations,
                        windows, idle.allocations);
            CHECK(counted.allocations == idle.allocations);
        }
    }
}

}  // namespace

int main() {
    steady_books_allocate_what_an_idle_run_does();
    std::printf("%d checks\n", checks);
    if (failures == 0) std::printf("test_native_match_allocations: ok\n");
    return failures == 0 ? 0 : 1;
}
