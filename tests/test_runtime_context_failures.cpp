// #310 N3/N4 regressions. WRITTEN NOT RUN by the source author.
// Separate TU: ordinary allocations only, never global_allocation_replacement.
// The managed mode requires the production quota host; it supplies no server.
#include <pineforge/detail/run_quota.hpp>
#include <pineforge/runtime_context.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {
using namespace pineforge;
namespace quota = pineforge::detail;

void require(bool condition, const char* expression, int line) {
    if (!condition)
        throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(line)
                                 + ": " + expression);
}
#define REQUIRE(condition) require(static_cast<bool>(condition), #condition, __LINE__)

template<class F>
void require_logic_error(F&& body) {
    try {
        body();
    } catch (const std::logic_error&) {
        return;
    }
    throw std::runtime_error("expected a stale runtime context to throw");
}

// Varied authored inputs; the checks below use their closes directly.
const std::array<Bar, 3> bars{{
    {10, 14, 8, 11, 3, 0},
    {13, 19, 12, 17, 5, 60000},
    {8, 12, 6, 7, 9, 120000},
}};

// The same handwritten PineStrategyHost pattern as test_runtime_context.cpp.
// Resetting the public service revokes its history binding. In a batch/warmup
// the next input must fail in accept_batch/append_live, outside this callback.
// No selected window/observer is registered, so these are untracked entries.
class HandwrittenFailureHost : public source::PineStrategyHost {
public:
    bool reset_after_first = false;
    unsigned callbacks = 0;
    double close_sum = 0;

    void configure_security_evaluators() override {
        runtime_contexts().enable_history();
    }
    void on_source_bar(const Bar& input) override {
        ++callbacks;
        close_sum += input.close;
        if (reset_after_first && callbacks == 1) runtime_contexts().reset();
    }
};

struct Lifetime {
    unsigned sources_destroyed = 0;
    unsigned evaluators_destroyed = 0;
    unsigned reads = 0;
    unsigned evaluations = 0;
};

struct History : RuntimeHistorySource {
    Lifetime& lifetime;
    const Bar input;
    History(Lifetime& counts, Bar value) : lifetime(counts), input(value) {}
    ~History() override { ++lifetime.sources_destroyed; }
    std::uint64_t size() const override { return 1; }
    Bar read(std::uint64_t index) const override {
        if (index != 0) throw std::out_of_range("history test input");
        ++lifetime.reads;
        return input;
    }
};

struct Evaluator : RuntimeContextEvaluator {
    Lifetime& lifetime;
    double close_sum = 0;
    explicit Evaluator(Lifetime& counts) : lifetime(counts) {}
    ~Evaluator() override { ++lifetime.evaluators_destroyed; }
    void consume(RuntimeContextService&, RuntimeContextHandle,
                 const RuntimeContextEvent& event) override {
        REQUIRE(event.index == 0 && event.frontier == 1);
        close_sum += event.bar.close;
        ++lifetime.evaluations;
    }
    std::uint64_t storage_bytes() const noexcept override { return sizeof(*this); }
    void observe_owned_storage(OwnedStorage& out) const noexcept override {
        out.metadata(sizeof(*this)); // Lifetime is borrowed from this test.
    }
};

struct Binding {
    std::weak_ptr<const RuntimeHistorySource> source;
    RuntimeContextHandle context;
};

Binding bind_owned_history(RuntimeContextService& service, Lifetime& counts,
                           const Bar& input) {
    try {
        auto source = std::make_shared<History>(counts, input);
        std::weak_ptr<const RuntimeHistorySource> weak = source;
        service.bind_source(std::move(source));
        service.accept_batch(1);
        const auto context = service.get_or_create({310, "entry cleanup", {}}, [&] {
            return std::make_unique<Evaluator>(counts);
        });
        service.advance(context, 1);
        REQUIRE(dynamic_cast<Evaluator&>(service.published(context)).close_sum == input.close);
        REQUIRE(counts.reads == 1 && counts.evaluations == 1);
        REQUIRE(!weak.expired());
        return {weak, context};
    } catch (...) {
        service.end_run(); // The borrowed Lifetime must outlive every owner.
        throw;
    }
}

// A failed native host stays failed. Bind new public service roots BETWEEN
// entries so the next refused entry's end_run is observable independently of
// the guard that caught the original exception. The old processing_input_ bug
// makes that next guard think it is nested and retain both roots.
template<class Entry>
void require_next_entry_cleanup(HandwrittenFailureHost& host, Entry&& entry) {
    Lifetime counts; // Declared before the owning service binding.
    auto& service = host.runtime_contexts();
    const auto binding = bind_owned_history(service, counts, bars[1]);
    try {
        entry();
        REQUIRE(host.last_run_status() == 1);
        REQUIRE(binding.source.expired());
        REQUIRE(counts.sources_destroyed == 1 && counts.evaluators_destroyed == 1);
        REQUIRE(service.usage().accepted == 0 && service.usage().contexts == 0);
        require_logic_error([&] { (void)service.published(binding.context); });

        // Public reset still releases new ownership and invalidates handles;
        // it does not recover the native host from its terminal failure.
        service.reset();
        Lifetime reset_counts;
        const auto fresh = bind_owned_history(service, reset_counts, bars[2]);
        try {
            REQUIRE(fresh.context != binding.context);
            REQUIRE(fresh.context.generation != binding.context.generation);
            service.reset();
            REQUIRE(fresh.source.expired());
            REQUIRE(reset_counts.sources_destroyed == 1);
            REQUIRE(reset_counts.evaluators_destroyed == 1);
            require_logic_error([&] { (void)service.published(fresh.context); });
        } catch (...) {
            service.end_run(); // Release borrowed counters before unwinding.
            throw;
        }
    } catch (...) {
        service.end_run();
        throw;
    }
}

void require_history_failure(const HandwrittenFailureHost& host, const char* text) {
    REQUIRE(host.callbacks == 1);
    REQUIRE(host.close_sum == bars[0].close);
    REQUIRE(host.last_run_status() == 1);
    REQUIRE(host.last_error() == text);
    REQUIRE(std::string(run_failure_code_of(host)) == "engine_invariant");
    REQUIRE(host.runtime_contexts().usage().accepted == 0);
}

void untracked_batch_history_throw() {
    HandwrittenFailureHost host;
    host.reset_after_first = true;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require_history_failure(host, "runtime batch lease is not live");
    require_next_entry_cleanup(host, [&] { REQUIRE(!host.stream_end()); });
    require_next_entry_cleanup(host, [&] {
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    });
    REQUIRE(host.callbacks == 1); // Neither refused entry dispatched input.
}

void untracked_warmup_history_throw() {
    HandwrittenFailureHost host;
    host.reset_after_first = true;
    REQUIRE(!host.stream_begin(bars.data(), static_cast<int>(bars.size()), "1", "1"));
    require_history_failure(host, "runtime live history is not bound");
    require_next_entry_cleanup(host, [&] { REQUIRE(!host.stream_end()); });
}

void untracked_live_history_throw() {
    HandwrittenFailureHost host;
    REQUIRE(host.stream_begin(bars.data(), 1, "1", "1"));
    REQUIRE(host.runtime_contexts().accepted_frontier() == 1);
    host.runtime_contexts().reset(); // Actual public invalidation between pushes.
    REQUIRE(!host.stream_push_bar(bars[1]));
    require_history_failure(host, "runtime live history is not bound");
    require_next_entry_cleanup(host, [&] { REQUIRE(!host.stream_end()); });
}

void healthy_history_controls() {
    const double expected = bars[0].close + bars[1].close + bars[2].close;
    HandwrittenFailureHost batch;
    batch.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    REQUIRE(batch.last_error().empty() && batch.last_run_status() == 0);
    REQUIRE(batch.callbacks == bars.size() && batch.close_sum == expected);
    REQUIRE(batch.runtime_contexts().usage().accepted == 0);

    HandwrittenFailureHost live;
    REQUIRE(live.stream_begin(bars.data(), 1, "1", "1"));
    REQUIRE(live.stream_push_bar(bars[1]));
    REQUIRE(live.stream_push_bar(bars[2]));
    REQUIRE(live.runtime_contexts().accepted_frontier() == bars.size());
    REQUIRE(live.stream_end());
    REQUIRE(live.last_error().empty() && live.last_run_status() == 0);
    REQUIRE(live.callbacks == bars.size() && live.close_sum == expected);
    REQUIRE(live.runtime_contexts().usage().accepted == 0);
}

OwnedStorageUsage observe_endpoint(const quota::RunQuota& client) {
    OwnedStorage out;
    errno = EDOM;
    client.observe_owned_storage(out);
    REQUIRE(errno == EDOM);
    const auto once = out.usage();
    client.observe_owned_storage(out);
    REQUIRE(errno == EDOM);
    const auto twice = out.usage();
    REQUIRE(once.resident_metadata_bytes == twice.resident_metadata_bytes);
    REQUIRE(once.resident_payload_bytes == twice.resident_payload_bytes);
    REQUIRE(once.backing_bytes == twice.backing_bytes);
    REQUIRE(once.unsupported == twice.unsupported && once.overflow == twice.overflow);
    REQUIRE(twice.complete());
    return twice;
}

void require_configuration_refusal(const std::exception& error) {
    const auto value = classify_run_failure(error);
    REQUIRE(value.code == RunFailureCode::run_quota_configuration_invalid);
    REQUIRE(value.args);
    REQUIRE(*value.args ==
        "{\"reason\":\"counter_overflow\",\"setting\":\"scratch_bytes\",\"stage\":\"execution\"}");
    REQUIRE(std::string(error.what()) == "run quota refused");
}

// This row tests endpoint synchronization, not managed scratch accounting or
// host-limit enforcement. A REAL production-host HELLO must have succeeded.
// Client-side overflow validation creates the actual retained refusal/snapshot
// under the protocol mutex while another execution thread observes its graph.
// Run with a race detector remotely; plain execution alone is not race proof.
void managed_endpoint_observer_race(quota::QuotaMode captured_mode) {
    REQUIRE(captured_mode == quota::QuotaMode::managed);
    auto client = quota::RunQuota::from_environment();
    REQUIRE(!client.refusal());
    const auto before = observe_endpoint(client);
    REQUIRE(before.resident_metadata_bytes != 0);
    std::atomic<bool> start{false};
    std::exception_ptr mutation_error;
    std::exception_ptr observation_error;
    std::thread mutator([copy = client, &start, &mutation_error]() mutable {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        try {
            bool refused = false;
            try {
                (void)copy.reserve({310, 1, 0, 0}, {quota::kRunQuotaMax + 1, 0, 0, 0},
                                   0, 0, quota::QuotaStage::execution, quota::QuotaArtifact::spool);
            } catch (const coded<std::runtime_error>& error) {
                require_configuration_refusal(error);
                refused = true;
            }
            REQUIRE(refused);
        } catch (...) {
            mutation_error = std::current_exception();
        }
    });
    start.store(true, std::memory_order_release);
    try {
        for (unsigned i = 0; i < 1024; ++i) {
            const auto observed = observe_endpoint(client);
            REQUIRE(observed.resident_metadata_bytes >= before.resident_metadata_bytes);
            std::this_thread::yield();
        }
    } catch (...) {
        observation_error = std::current_exception();
    }
    mutator.join();
    if (mutation_error && observation_error) {
        // Preserve both failures when cleanup joins a concurrently failed
        // mutator; the observer's exception is rethrown below.
        try {
            std::rethrow_exception(mutation_error);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "quota mutator also failed: %s\n", error.what());
        } catch (...) {
            std::fprintf(stderr, "quota mutator also failed: non-standard exception\n");
        }
    }
    if (observation_error) std::rethrow_exception(observation_error);
    if (mutation_error) std::rethrow_exception(mutation_error);

    const auto refusal = client.refusal();
    REQUIRE(refusal && refusal->failure.args);
    const auto after = observe_endpoint(client);
    REQUIRE(after.resident_metadata_bytes >= before.resident_metadata_bytes + sizeof(quota::QuotaSnapshot));
    // Factory JSON is an observed ownership domain; a foreign string owner
    // cannot inherit that claim just by copying the same JSON bytes.
    OwnedStorage minted;
    refusal->failure.observe_owned_storage(minted);
    REQUIRE(minted.usage().complete() && minted.usage().resident_bytes() != 0);
    RunFailureValue foreign{refusal->failure.code,
                           std::make_shared<const std::string>(*refusal->failure.args)};
    OwnedStorage unknown;
    foreign.observe_owned_storage(unknown);
    REQUIRE(!unknown.usage().complete());

    bool refused = false;
    try {
        client.check();
    } catch (const coded<std::runtime_error>& error) {
        require_configuration_refusal(error);
        refused = true;
    }
    REQUIRE(refused && client.refusal() == refusal); // First latch retained.

    // The overflow never reached the host. FINISH must still reach it before
    // disconnect, while the client continues to report its original refusal.
    refused = false;
    try {
        (void)client.finish();
    } catch (const coded<std::runtime_error>& error) {
        require_configuration_refusal(error);
        refused = true;
    }
    REQUIRE(refused && client.refusal() == refusal);
}
} // namespace

int main(int argc, char** argv) {
    // Trusted capture precedes host construction or any test/strategy callback.
    const auto captured_mode = quota::mode();
    try {
        if (argc == 2 && std::strcmp(argv[1], "--managed-endpoint-observer-race") == 0) {
            managed_endpoint_observer_race(captured_mode);
        } else {
            REQUIRE(argc == 1);
            REQUIRE(captured_mode == quota::QuotaMode::unmanaged);
            untracked_batch_history_throw();
            untracked_warmup_history_throw();
            untracked_live_history_throw();
            healthy_history_controls();
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "runtime context failure control: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "runtime context failure control: non-standard exception\n");
        return 1;
    }
}
