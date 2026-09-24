// R5 lane L3: what a warm command and a warm fill take from the allocator.
//
// Before the lane, every request-core mutation staged a MutationPlan in a
// token drawn from a thread-local pool, a fill's dependency drain built a
// std::function closure (four captured pointers, past the small-object
// buffer) and three fresh handle vectors, a bound leg that ended pushed its
// seed into a fresh vector, and the host's precommit view took a fresh
// closed-row vector at every closing fill. The pool hid the tokens from the
// allocator; the rest reached it at every fill. After the lane the order core
// writes in place and the consumer keeps those vectors and that view as
// scratch, so on a warm book:
//
//   * an accepted command allocates its definition and nothing else (its
//     label and comment fit the string's own buffer here) -- 1 per submit and
//     per replace, before the lane as after it;
//   * what the kernel allocates on its own -- fills, their dependency drains,
//     the matcher's mutations, the journal's retirement -- is the calendar
//     extending its session-day spans, a few times a day, whether the book
//     fills or not (at most one per 40 bars here); before the lane the fills
//     added about five allocations each;
//   * a Full read of the whole record (native_events(0)) allocates its one
//     result vector (no event of these books holds a vector of its own).
//
// Each book is a bare host (no Pine); the windows start after a warm-up and
// end at the last bar's callback, so a run's setup and its end are outside
// them. The counts are printed with the bounds.
//
// This row FAILS on 10f20197 (the tree before the lane), where the bracket and
// market books' kernel allocations are several per fill.
//
// Source-free: it also runs in the kernel-only profile.
#include <pineforge/native_host.hpp>

#include "global_allocation_replacement.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

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

enum class Mode { Bracket, Market, Reissue };

const char* mode_name(Mode mode) {
    switch (mode) {
    case Mode::Bracket: return "bracket";
    case Mode::Market: return "market";
    case Mode::Reissue: return "re-issue";
    }
    return "?";
}

double snap(double price) { return static_cast<double>(static_cast<long>(price * 100.0 + 0.5)) / 100.0; }

// Deterministic five-minute bars: a random walk in whole cents.
std::vector<Bar> make_bars(int count) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    auto uniform = [&state] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<double>(state >> 11) * (1.0 / 9007199254740992.0);
    };
    double price = 100.0;
    const std::int64_t t0 = 1700000100000LL - (1700000100000LL % 300000);
    for (int index = 0; index < count; ++index) {
        const double open = snap(price);
        double close = snap(open * (1.0 + (uniform() - 0.5) * 0.004));
        if (close < 1.0) close = 1.0;
        Bar bar{};
        bar.open = open;
        bar.close = close;
        bar.high = snap((open > close ? open : close) * (1.0 + uniform() * 0.001));
        bar.low = snap((open < close ? open : close) * (1.0 - uniform() * 0.001));
        bar.volume = 10.0;
        bar.timestamp = t0 + static_cast<std::int64_t>(index) * 300000;
        bars.push_back(bar);
        price = close;
    }
    return bars;
}

class Book final : public NativeStrategyHost {
public:
    Book(Mode mode, int warmup, int bars) : mode_(mode), warmup_(warmup), bars_(bars) {}

    // The window's counts.
    std::size_t window_allocations = 0;
    std::size_t command_allocations = 0;
    long commands = 0;
    long fills = 0;
    long bars_seen = 0;

private:
    // One command, its allocations counted inside the window.
    template <class Call>
    auto command(Call call) {
        const std::size_t before = global_allocation::allocations;
        auto result = call();
        if (counting_) {
            command_allocations += global_allocation::allocations - before;
            ++commands;
        }
        return result;
    }

public:
    void on_native_run_begin() override {
        bar_ = 0;
        entry_.reset();
        take_.reset();
        stop_.reset();
    }

    void on_native_applied(const no::ExecutionAppliedEvent&, const NativeDecisionContext&) override {
        if (counting_) ++fills;
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ++bar_;
        if (bar_ == warmup_) {
            counting_ = true;
            window_start_ = global_allocation::allocations;
        }
        if (counting_) ++bars_seen;
        const double position = physical_position().signed_units;
        if (mode_ == Mode::Bracket) {
            if (position == 0.0 && bar_ % 10 == 0) {
                no::Request entry{no::Transact{1.0}, "E", ""};
                const auto accepted = command([&] { return submit(entry); });
                if (accepted.handle) {
                    no::WaitForApplied wait;
                    wait.parent = *accepted.handle;
                    no::Member member;
                    member.group = static_cast<std::uint64_t>(bar_);
                    no::Request take{no::Reduce{no::ExplicitUnits{1.0}}, "x", ""};
                    take.trigger = no::Limit{snap(bar.close * 1.004)};
                    take.owner = wait;
                    take.group = member;
                    no::Request stop{no::Reduce{no::ExplicitUnits{1.0}}, "x", ""};
                    stop.trigger = no::Stop{snap(bar.close * 0.996)};
                    stop.owner = wait;
                    stop.group = member;
                    command([&] { return submit(take); });
                    command([&] { return submit(stop); });
                }
            }
        } else if (mode_ == Mode::Market) {
            if (bar_ % 10 == 0) {
                no::Request request{no::Transact{1.0}, "m", ""};
                if (position != 0.0) request.intent = no::Flatten{};
                command([&] { return submit_market(request); });
            }
        } else {
            if (bar_ == 1) {
                no::Request entry{no::Transact{1.0}, "L", ""};
                command([&] { return submit_market(entry); });
            }
            if (position > 0.0) {
                reissue(take_, no::Limit{snap(bar.close * 1.6)});
                reissue(stop_, no::Stop{snap(bar.close * 0.4)});
            }
        }
        if (counting_ && bar_ == bars_) {
            window_allocations = global_allocation::allocations - window_start_;
            counting_ = false;
        }
    }

private:
    void reissue(std::optional<no::RequestHandle>& handle, const no::Trigger& trigger) {
        no::Request request{no::Reduce{no::ExplicitUnits{1.0}}, "x", ""};
        request.trigger = trigger;
        no::Member member;
        member.group = 1;
        request.group = member;
        if (!handle) {
            const auto accepted = command([&] { return submit(request); });
            if (accepted.handle) handle = *accepted.handle;
        } else {
            const auto replaced = command([&] { return replace(*handle, request); });
            if (replaced.successor) handle = *replaced.successor; else handle.reset();
        }
    }

    Mode mode_;
    int warmup_;
    int bars_;
    int bar_ = 0;
    bool counting_ = false;
    std::size_t window_start_ = 0;
    std::optional<no::RequestHandle> entry_, take_, stop_;
};

NativeRunSpec make_spec(NativeEventRetention retention) {
    NativeRunSpec spec;
    spec.identity = {"l3-alloc", 1};
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    spec.initial_capital = 1000000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::Percent;
    spec.fee_value = 0.05;
    spec.event_retention = retention;
    return spec;
}

void witness(Mode mode) {
    const int bars = 4000;
    const int warmup = 1000;
    const std::vector<Bar> tape = make_bars(bars);
    Book book(mode, warmup, bars);
    CHECK(book.configure_native(make_spec(NativeEventRetention::Window)).status
          == NativeSetupStatus::Applied);
    book.run(tape.data(), static_cast<int>(tape.size()));
    CHECK(book.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(book.commands > 0);
    const std::size_t kernel = book.window_allocations - book.command_allocations;
    const double per_command = book.commands
        ? static_cast<double>(book.command_allocations) / static_cast<double>(book.commands) : 0.0;
    std::printf("%s book: %ld bars, %ld commands (%zu allocations, %.2f per command), %ld fills; "
                "the kernel's own %zu allocations, %.3f per fill, %.3f per bar\n",
                mode_name(mode), book.bars_seen, book.commands, book.command_allocations,
                per_command, book.fills, kernel,
                book.fills ? static_cast<double>(kernel) / static_cast<double>(book.fills) : 0.0,
                static_cast<double>(kernel) / static_cast<double>(book.bars_seen));
    // A command allocates its definition and nothing else, besides the
    // amortized doubling of what grows with the run (the chain index keeps a
    // row per replace, V19-B), a handful of times over the window.
    CHECK(book.command_allocations <= static_cast<std::size_t>(book.commands) + 8);
    // The kernel's own work -- fills, their drains, the matcher's mutations,
    // the journal's retirement -- takes nothing but the calendar's session-day
    // spans (a few per day, 288 bars here), fills or none.
    CHECK(kernel * 40 <= static_cast<std::size_t>(book.bars_seen));
    if (mode != Mode::Reissue) CHECK(book.fills > 100);
}

// The whole record of a Full run, read once: one result vector, and what the
// read costs in time (printed; the lane measures it against 10f20197 on spark).
void full_read() {
    const int bars = 4000;
    const std::vector<Bar> tape = make_bars(bars);
    Book book(Mode::Bracket, bars + 1, bars);
    CHECK(book.configure_native(make_spec(NativeEventRetention::Full)).status
          == NativeSetupStatus::Applied);
    book.run(tape.data(), static_cast<int>(tape.size()));
    CHECK(book.native_state().kind == NativeLifecycleKind::Completed);
    const std::size_t before = global_allocation::allocations;
    const auto start = std::chrono::steady_clock::now();
    const auto events = book.native_events(0);
    const double micros = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
    const std::size_t allocations = global_allocation::allocations - before;
    std::size_t commands = 0;
    for (const auto& row : events) commands += row.command ? 1u : 0u;
    std::printf("Full read: %zu rows (%zu commands) in %.0f us, %zu allocations\n",
                events.size(), commands, micros, allocations);
    CHECK(commands > 1000);
    CHECK(allocations == 1);
}

}  // namespace

int main() {
    witness(Mode::Bracket);
    witness(Mode::Market);
    witness(Mode::Reissue);
    full_read();
    std::printf("%d checks\n", checks);
    if (failures == 0) {
        std::printf("test_native_direct_mutation_allocations: ok\n");
        return 0;
    }
    std::printf("test_native_direct_mutation_allocations: %d of %d checks failed\n", failures,
                checks);
    return 1;
}
