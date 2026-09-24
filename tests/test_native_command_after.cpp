// R5 lane D2-A (PB): NativeExecutionConsumer::has_command_after against the
// two journal searches it replaced, at every host callback of windowed runs.
//
// The Pine adapter's quiet receipt read (PineExecutionAdapter::
// observe_terminal_receipts, up to four times a bar) asks whether the journal
// holds a command event above its receipt cursor. It used to ask by comparing
// first_command_after(cursor) with first_command_after(UINT64_MAX) -- two
// searches -- and asks has_command_after(cursor) now: the test
// first_command_after makes first, which answers the history's size exactly
// when it fails. The answer is a function of the retained journal and the
// cursor alone, so this row takes three answers -- has_command_after, the two
// searches, and a scan of the whole retained journal that shares no code with
// either -- for every kind of cursor a reader can hold (zero, the window's
// start, the event high water, each side of the first, last and a random
// retained command's ordinal, random values up to the high water, and
// UINT64_MAX), inside every callback of K3's randomized books -- the bar open,
// the calculation before and after its commands, and every applied fill --
// and after each run ends. The books run under the V19-B journal window with
// a host that acknowledges as it reads (so the window retires a prefix at
// script-bar boundaries), under the stress switch that retires at every
// driver point, and under Full retention; every intrabar path (a lower one
// labelled canonically, so its sub-bars are walked), with and without
// calculate-on-fills. The three answers must be equal every time, and both
// answers must come up many times, the empty journal among them.
//
// Fail-before: at the lane's base the consumer has no has_command_after, so
// this TU does not compile there (the lane report records the first
// diagnostic).
//
// Source-free: this TU runs in the kernel-only profile.
#include "../src/native_execution_consumer.hpp"
#include "native_match_book_fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <variant>
#include <vector>

using namespace pineforge;

namespace {
using k3_book::BookConfig;
using k3_book::Path;
namespace no = pineforge::native_order;

int failures = 0;
long checks = 0;
long answered_true = 0;
long answered_false = 0;
long empty_journal = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

constexpr std::uint64_t kTop = std::numeric_limits<std::uint64_t>::max();

std::uint64_t ordinal_of(const no::CommandEvent& event) {
    return std::visit([](const auto& payload) { return payload.ordinal; }, event);
}

enum class Window { Full, Acknowledged, EveryPoint };

const char* window_name(Window window) {
    switch (window) {
    case Window::Full: return "full";
    case Window::Acknowledged: return "window";
    case Window::EveryPoint: return "every-point";
    }
    return "?";
}

class ProbingHost final : public k3_book::BookHost {
public:
    ProbingHost(const BookConfig& config, Window window)
        : BookHost(config), window_(window), probe_rng_(config.seed ^ 0xC2B2AE3D27D4EB4Full) {
        if (window == Window::EveryPoint) {
            pineforge::as_native_consumer(execution_consumer()).set_retire_every_point(true);
        }
    }

    void on_native_run_begin() override {
        BookHost::on_native_run_begin();
        // A polling host acknowledges at its begin (V19-B); the window then
        // keeps what it has not read.
        if (window_ != Window::Full) native_acknowledge_events(0);
    }

    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override { probe(); }

    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        probe();
        BookHost::on_native_bar(bar, context);
        probe();
        if (window_ != Window::Full && probe_rng_.percent(70)) {
            // Acknowledge somewhere at or below the high water, so the next
            // script-bar boundary retires part of the journal.
            const auto& consumer = pineforge::as_native_consumer(execution_consumer());
            const std::uint64_t high = consumer.event_high_water();
            const std::uint64_t back = static_cast<std::uint64_t>(probe_rng_.below(40));
            native_acknowledge_events(high > back ? high - back : 0);
        }
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        probe();
        BookHost::on_native_applied(event, context);
        probe();
    }

    void probe() {
        const auto& consumer = pineforge::as_native_consumer(execution_consumer());
        const auto& history = consumer.request_core().history();
        if (history.empty()) ++empty_journal;
        const std::uint64_t high = consumer.event_high_water();
        const std::uint64_t start = consumer.event_window_start();
        cursors_.clear();
        cursors_.push_back(0);
        cursors_.push_back(kTop);
        cursors_.push_back(kTop - 1);
        for (int delta = -2; delta <= 2; ++delta) {
            cursors_.push_back(high + static_cast<std::uint64_t>(delta));
            cursors_.push_back(start + static_cast<std::uint64_t>(delta));
        }
        if (!history.empty()) {
            const std::size_t middle =
                static_cast<std::size_t>(probe_rng_.below(static_cast<int>(history.size())));
            for (const std::size_t index : {std::size_t{0}, history.size() - 1, middle}) {
                const std::uint64_t ordinal = ordinal_of(history[index]);
                cursors_.push_back(ordinal - 1);
                cursors_.push_back(ordinal);
                cursors_.push_back(ordinal + 1);
            }
        }
        for (int draw = 0; draw < 4; ++draw) {
            cursors_.push_back(probe_rng_.next() % (high + 3));
        }
        for (const std::uint64_t cursor : cursors_) {
            const bool fast = consumer.has_command_after(cursor);
            const bool searched =
                consumer.first_command_after(cursor) != consumer.first_command_after(kTop);
            // And a scan of the whole retained journal, which shares no code
            // with either.
            bool scanned = false;
            for (const auto& event : history) {
                if (ordinal_of(event) > cursor) {
                    scanned = true;
                    break;
                }
            }
            CHECK(fast == searched);
            CHECK(fast == scanned);
            ++(fast ? answered_true : answered_false);
        }
    }

private:
    Window window_;
    k3_book::Rng probe_rng_;
    std::vector<std::uint64_t> cursors_;
};

void run_one(const BookConfig& config, Window window) {
    const k3_book::Tape tape = k3_book::make_tape(config);
    NativeRunSpec spec = k3_book::make_spec(config, tape);
    // A lower-timeframe run labelled canonically, so its sub-bars are walked
    // (under the fixture's FeedTolerant labels every window is zero-width).
    if (spec.intrabar.lower()) spec.slot_label_policy = NativeSlotLabelPolicy::Canonical;
    if (window != Window::Full) spec.event_retention = NativeEventRetention::Window;
    ProbingHost host(config, window);
    const long before = failures;
    const auto setup = host.configure_native(spec);
    CHECK(setup.status == NativeSetupStatus::Applied);
    if (setup.status != NativeSetupStatus::Applied) return;
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    // After the run, the journal as it was left.
    host.probe();
    if (failures != before) {
        std::fprintf(stderr, "  seed=%llu live=%d path=%s window=%s calc_on_fills=%d\n",
                     static_cast<unsigned long long>(config.seed), config.live,
                     k3_book::path_name(config.path), window_name(window),
                     config.calc_on_fills ? 1 : 0);
    }
}

}  // namespace

int main() {
    std::uint64_t seed = 44001;
    int runs = 0;
    for (const int live : {1, 5, 20, 60}) {
        for (const Path path : {Path::None, Path::Synthesized, Path::Lower}) {
            for (const Window window : {Window::Full, Window::Acknowledged, Window::EveryPoint}) {
                for (const bool calc_on_fills : {false, true}) {
                    BookConfig config;
                    config.seed = seed++;
                    config.live = live;
                    config.bars = live >= 20 ? 30 : 50;
                    config.path = path;
                    config.calc_on_fills = calc_on_fills;
                    run_one(config, window);
                    ++runs;
                }
            }
        }
    }
    std::printf("test_native_command_after: %d runs, %ld comparisons (%ld above the cursor, "
                "%ld not), %ld probes of an empty journal\n",
                runs, checks, answered_true, answered_false, empty_journal);
    // Vacuity guards: both answers, and the empty journal the window leaves.
    CHECK(answered_true > 10000);
    CHECK(answered_false > 10000);
    CHECK(empty_journal > 0);
    if (failures != 0) {
        std::fprintf(stderr, "test_native_command_after: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_native_command_after: ok\n");
    return 0;
}
