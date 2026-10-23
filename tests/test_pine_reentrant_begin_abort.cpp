// A-11: reach PineStrategyHost::prepare_native_begin while actually Running.
// Nested run()/stream_begin() is rejected earlier by native admission and
// would not test this provider hook's clear-before-return defect.
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <atomic>
#include <cstdio>

namespace {
using namespace pineforge;
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
} } while (false)

const std::array<Bar, 4> bars{{
    {100.0, 101.0, 99.0, 100.0, 1.0, 1704067200000LL},
    {100.0, 101.0, 99.0, 100.0, 1.0, 1704067260000LL},
    {100.0, 101.0, 99.0, 100.0, 1.0, 1704067320000LL},
    {100.0, 101.0, 99.0, 100.0, 1.0, 1704067380000LL},
}};

struct Host final : source::PineStrategyHost {
    bool inject = true;
    bool attempted = false;
    bool running_before = false;
    bool running_after = false;
    bool abort_survived = false;
    bool callback_threw = false;
    int callbacks = 0;

    bool abort_pending() const {
        return abort_requested_.load(std::memory_order_relaxed);
    }
    void on_source_bar(const Bar& bar) override {
        ++callbacks;
        if (!inject || attempted) return;
        attempted = true;
        running_before = native_state().kind == NativeLifecycleKind::Running;
        request_abort();
        NativeBeginArgs nested{};
        nested.bars = &bar;
        nested.n = 1;
        try {
            prepare_native_begin(nested);
        } catch (...) {
            callback_threw = true;
        }
        abort_survived = abort_pending();
        running_after = native_state().kind == NativeLifecycleKind::Running;
    }
};

void running_abort_and_reuse() {
    Host host;
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.attempted && host.running_before && host.running_after);
    CHECK(!host.callback_threw);
    CHECK(host.abort_survived);
    CHECK(host.callbacks == 1);
    CHECK(host.last_run_status() == 1);
    CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(host.native_state().failure.code == NativeFailureCode::Aborted);

    // The aborted handle remains reusable, and the prior request is cleared.
    host.inject = false;
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_run_status() == 0);
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.callbacks == 1 + static_cast<int>(bars.size()));
    CHECK(!host.abort_pending());
}

void idle_abort_controls() {
    Host host;
    host.inject = false;
    host.request_abort();
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_run_status() == 0);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.callbacks == static_cast<int>(bars.size()));
    CHECK(!host.abort_pending());

    // Existing idle behavior also consumes a request when the borrowed
    // input check refuses before admission.
    Host invalid;
    invalid.request_abort();
    NativeBeginArgs args{};
    args.n = -1;
    bool rejected = false;
    try {
        invalid.prepare_native_begin(args);
    } catch (const RunFailureInfo& error) {
        rejected = error.run_failure_code() == RunFailureCode::engine_invariant;
    }
    CHECK(rejected);
    CHECK(!invalid.abort_pending());
}
} // namespace

int main() {
    running_abort_and_reuse();
    idle_abort_controls();
    std::printf("%s test_pine_reentrant_begin_abort (%d failures)\n",
                failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
