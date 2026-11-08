// Tests independent v2 count/byte bounds with real disk records and native
// admission counters, distinguishing omitted fields from explicit zero.
// The module build exposes the same real source host to run_json's CLI.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/detail/disk_id_table.hpp>
#include <pineforge/detail/id_pair_store.hpp>
#ifndef PINEFORGE_SCRATCH_MANIFEST_MODULE
#include "../src/native_execution_consumer.hpp"
#endif

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
namespace sd = pineforge::detail;

#ifndef PINEFORGE_SCRATCH_MANIFEST_MODULE
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

pf_scratch_tables_v2 policy(std::uint64_t fields = 0) {
    pf_scratch_tables_v2 result{};
    result.struct_size = sizeof(result);
    result.version = 2;
    result.active_fields = fields;
    return result;
}

template<class Operation>
void capacity_error(Operation operation, const char* resource) {
    bool caught = false;
    try { operation(); }
    catch (const sd::ScratchCapacityError& error) {
        const auto failure = classify_run_failure(error);
        require(failure.code == RunFailureCode::scratch_capacity_exceeded, "typed capacity code");
        require(failure.args && *failure.args == std::string("{\"resource\":\"") + resource + "\"}",
                "capacity names the actual store");
        caught = true;
    }
    require(caught, "the actual admission must refuse");
}

std::shared_ptr<const sd::ScratchBounds> bounds(sd::ScratchBound field, std::uint64_t limit) {
    auto result = std::make_shared<sd::ScratchBounds>();
    result->active_fields = static_cast<std::uint64_t>(field);
    switch (field) {
    case sd::ScratchBound::K_e: result->manifest.K_e = limit; break;
    case sd::ScratchBound::B_e: result->manifest.B_e = limit; break;
    case sd::ScratchBound::K_c: result->manifest.K_c = limit; break;
    case sd::ScratchBound::B_c: result->manifest.B_c = limit; break;
    case sd::ScratchBound::U: result->manifest.U = limit; break;
    case sd::ScratchBound::B_U: result->manifest.B_U = limit; break;
    case sd::ScratchBound::V: result->manifest.V = limit; break;
    case sd::ScratchBound::B_V: result->manifest.B_V = limit; break;
    default: throw std::runtime_error("unexpected test field");
    }
    return result;
}

// These tests insert real disk records. They cover independent count/byte
// accounting (including tombstones), not just construction of a descriptor.
void pair_limits() {
    for (const bool consumed : {false, true}) {
        const auto data = consumed ? sd::ScratchResource::consumed_data : sd::ScratchResource::erased_data;
        const auto index = consumed ? sd::ScratchResource::consumed_index : sd::ScratchResource::erased_index;
        const auto count_field = consumed ? sd::ScratchBound::K_c : sd::ScratchBound::K_e;
        const auto byte_field = consumed ? sd::ScratchBound::B_c : sd::ScratchBound::B_e;
        for (const std::uint64_t limit : {0, 1, 3, 7}) {
            sd::IdPairStore<std::int64_t> store;
            store.bind_scratch_tables({}, data, index);
            store.bind_scratch_bounds(bounds(count_field, limit));
            for (std::uint64_t i = 0; i < limit; ++i) {
                const auto key = "key-" + std::to_string(i) + std::string(65, 'x');
                require(store.insert_or_assign(key, i), "new count key inserted");
                require(!store.insert_or_assign(key, i + 1), "update consumes no new key");
                require(store.erase(key), "erase succeeds");
                require(store.insert_or_assign(key, i), "resurrection reuses historical capacity");
            }
            const auto before = store.disk_bytes();
            capacity_error([&] { store.insert_or_assign("beyond", 1); },
                           consumed ? "consumed_index" : "erased_index");
            require(store.size() == limit && store.disk_bytes() == before,
                    "count refusal precedes index growth and append");
        }
        for (const std::uint64_t limit : {0, 1, 3, 7}) {
            sd::IdPairStore<std::int64_t> store;
            store.bind_scratch_tables({}, data, index);
            store.bind_scratch_bounds(bounds(byte_field, limit));
            const std::string key(static_cast<std::size_t>(limit), 'x');
            require(store.insert_or_assign(key, 1), "exact byte equality admitted");
            require(store.insert_or_assign("", "", 2), "omitted count bound admits another zero-byte key domain");
            const auto before = store.disk_bytes();
            capacity_error([&] { store.insert_or_assign("z", 3); },
                           consumed ? "consumed_data" : "erased_data");
            require(store.size() == 2 && store.disk_bytes() == before,
                    "byte refusal counts strings, not headers or index geometry");
        }
    }
    sd::IdPairStore<std::int64_t, true> folded;
    folded.bind_scratch_tables({}, sd::ScratchResource::consumed_data, sd::ScratchResource::consumed_index);
    folded.bind_scratch_bounds(bounds(sd::ScratchBound::B_c, 5));
    folded.preflight_joined_single("ab", '\x1f', "cd");
    folded.insert_or_assign(std::string("ab\x1f" "cd", 5), 9);
    folded.preflight_joined_single("ab", '\x1f', "cd");
    capacity_error([&] { folded.preflight_joined_single("ab", '\x1f', "ce"); }, "consumed_data");
    BrokerStateHashSink sink;
    folded.fold_sorted(sink); // Exercises the real disk fold with optional admission.
    require(folded.lookup(std::string("ab\x1f" "cd", 5)) == 9, "joined key persisted");
}

void tree_limits() {
    for (const bool rank : {false, true}) {
        const auto resource = rank ? sd::ScratchResource::exit_rank : sd::ScratchResource::cohort;
        const auto count_field = rank ? sd::ScratchBound::V : sd::ScratchBound::U;
        const auto byte_field = rank ? sd::ScratchBound::B_V : sd::ScratchBound::B_U;
        const auto* error_resource = rank ? "exit_rank" : "cohort_ids_total";
        for (const std::uint64_t limit : {0, 1, 3, 7}) {
            sd::DiskIdTable<std::uint32_t> store;
            store.bind_scratch({}, resource);
            store.bind_scratch_bounds(bounds(count_field, limit), resource);
            for (std::uint64_t i = 0; i < limit; ++i) {
                const auto key = std::to_string(i) + std::string(65, 'x');
                store.require_insert(key);
                require(store.insert(key, i).second, "tree equality admitted");
                require(!store.insert(key, i).second, "tree duplicate reuses capacity");
            }
            const auto before = store.file_bytes();
            capacity_error([&] { store.require_insert("extra"); }, error_resource);
            capacity_error([&] { store.insert("extra", 0); }, error_resource);
            require(store.size() == limit && store.file_bytes() == before, "tree refusal is read-only");
        }
        for (const std::uint64_t limit : {0, 1, 3, 7}) {
            sd::DiskIdTable<std::uint32_t> store;
            store.bind_scratch({}, resource);
            store.bind_scratch_bounds(bounds(byte_field, limit), resource);
            require(store.insert(std::string(limit, 'x'), 1).second, "tree exact byte equality");
            if (limit != 0) require(store.insert("", 2).second, "omitted tree count remains unbounded");
            const auto before = store.file_bytes();
            capacity_error([&] { store.insert("z", 3); }, error_resource);
            require(store.file_bytes() == before, "tree byte refusal precedes writes");
        }
    }
    sd::DiskIdTable<std::uint32_t> pending;
    pending.bind_scratch({}, sd::ScratchResource::cohort);
    pending.bind_scratch_bounds(bounds(sd::ScratchBound::U, 2), sd::ScratchResource::cohort);
    const std::array<std::string, 3> keys{{"a", "b", "a"}};
    pending.require_inserts([&](auto&& visit) { for (const auto& key : keys) visit(key); });
    require(pending.size() == 0 && pending.file_bytes() == 0, "workset ticket does not publish rows");
    pending.insert("a", 1);
    pending.insert("b", 2);
    capacity_error([&] { pending.require_insert("c"); }, "cohort_ids_total");
}
#endif

// A source host performing real admission and cancellation. No Pine script,
// harvested tape, alternate store, or special limit-dependent branch.
class ManifestHost final : public source::PineStrategyHost {
public:
    std::uint64_t completed = 0;
    std::string workload = "cohorts";
    bool has_run_slab() const noexcept { return static_cast<bool>(scratch_tables_for_run()); }
    ManifestHost() {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000;
        config.default_qty_value = 1;
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        const auto na = std::numeric_limits<double>::quiet_NaN();
        if (workload == "cohorts") {
            // Keep a real priced request alive across a driver boundary.
            // It never fills; retiring it on the next bar records both
            // erased-minima domains and leaves historical cohort IDs intact.
            if (i > 0) strategy_cancel("entry-" + std::to_string(i - 1));
            strategy_entry("entry-" + std::to_string(i), true, 1.0, na, 1.0);
        } else if (workload == "fills") {
            if (i % 3 == 0) strategy_entry("entry-" + std::to_string(i / 3), true, na, na, 2.0);
            if (i % 3 == 1) strategy_close_all();
        } else if (workload == "cycles") {
            const auto entry = "entry-" + std::to_string(i / 4);
            if (i % 4 == 0) strategy_entry(entry, true, na, na, 2.0);
            if (i % 4 == 1)
                strategy_exit("exit-" + std::to_string(i / 4), entry, 100.0, na, na, na, na, 50.0);
            if (i % 4 == 2) strategy_close_all();
        } else if (workload == "roots") {
            // A priced same-ID reissue replaces a real pending request.
            // Both prices remain below the chart; each new bar roots one
            // successor after the initial rootless entry.
            strategy_entry("replace", true, 1.0 + (i % 2), na, 1.0);
        } else throw std::runtime_error("unknown scratch fixture workload");
        ++completed;
    }
};

#ifndef PINEFORGE_SCRATCH_MANIFEST_MODULE
void require_source_completion(ManifestHost& host, std::uint64_t expected, const char* phase,
                               bool expected_optional) {
    const auto state = host.native_state().kind;
    const auto& current = NativeExecutionConsumer::bound(host).scratch_bounds_for_run();
    if (host.completed == expected && host.last_run_status() == 0
        && host.last_error().empty() && state == NativeLifecycleKind::Completed
        && static_cast<bool>(current) == expected_optional && !host.has_run_slab()) return;
    throw std::runtime_error(std::string(phase)
        + ": completed=" + std::to_string(host.completed)
        + ", expected=" + std::to_string(expected)
        + ", status=" + std::to_string(host.last_run_status())
        + ", state=" + std::to_string(static_cast<unsigned>(state))
        + ", consumed=" + std::to_string(host.native_consumed_high_water())
        + ", current_mask=" + std::to_string(current ? current->active_fields : 0)
        + ", expected_optional=" + std::to_string(expected_optional)
        + ", slab=" + std::to_string(host.has_run_slab())
        + ", code=" + strategy_get_last_error_code(&host)
        + ", error=" + host.last_error());
}

std::vector<Bar> chart(int count) {
    std::vector<Bar> rows(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        rows[static_cast<std::size_t>(i)] = {100, 101, 99, 100, 1, 1736121600000LL + 60000LL * i};
    return rows;
}

void source_bridge() {
    const auto rows = chart(5);
    for (const std::uint64_t limit : {0, 1, 3, 5}) {
        ManifestHost host;
        auto config = policy(PF_SCRATCH_BOUND_U);
        config.U = limit;
        require(strategy_set_scratch_tables_v2(&host, &config) == 0, "real C setter accepts U");
        host.run(rows.data(), 5);
        require(host.completed == limit, "U enforced at actual source/native admissions");
        if (limit < rows.size()) {
            require(std::strcmp(strategy_get_last_error_code(&host), "scratch_capacity_exceeded") == 0,
                    "real engine keeps typed U failure");
            require(std::strcmp(strategy_get_last_error_args(&host), "{\"resource\":\"cohort_ids_total\"}") == 0,
                    "U refusal names real cohort counter");
        } else require(host.last_run_status() == 0 && host.last_error().empty(), "U equality completes");
    }
    ManifestHost bytes;
    auto config = policy(PF_SCRATCH_BOUND_B_U);
    config.B_U = 14; // entry-0 and entry-1 are seven bytes each.
    require(strategy_set_scratch_tables_v2(&bytes, &config) == 0, "byte-only policy accepted");
    bytes.run(rows.data(), 5);
    require(bytes.completed == 2 && bytes.last_run_status() != 0, "source aggregate byte admission");
    require(std::strcmp(strategy_get_last_error_args(&bytes), "{\"resource\":\"cohort_ids_total\"}") == 0,
            "source byte failure remains typed");

    // A setting replacement does not revive this terminal failed run.
    require(strategy_set_scratch_tables_v1(&bytes, nullptr) == 0, "v1 NULL restores legacy disk default");
    require(bytes.native_state().kind == NativeLifecycleKind::Failed,
            "scratch reset does not revive a terminal failed host");

    // Use a completed run for reuse: entry-0 and entry-1 exactly fill B_U=14.
    // PineStrategyHost projects the next run number when a completed host begins.
    ManifestHost reset;
    require(strategy_set_scratch_tables_v2(&reset, &config) == 0, "install bounds on reusable source host");
    reset.run(rows.data(), 2);
    require_source_completion(reset, 2, "bounded source prefix", true);
    const auto preceding = NativeExecutionConsumer::bound(reset).scratch_bounds_for_run();
    require(preceding && preceding->active(sd::ScratchBound::B_U) && preceding->manifest.B_U == config.B_U,
            "completed prefix retains the requested byte policy");
    const auto consumed = reset.native_consumed_high_water();
    require(strategy_set_scratch_tables_v1(&reset, nullptr) == 0, "v1 NULL replaces completed v2 policy");
    require(NativeExecutionConsumer::bound(reset).scratch_bounds_for_run() == preceding,
            "idle replacement leaves the preceding run owner intact");
    reset.completed = 0;
    reset.run(rows.data(), 5);
    require_source_completion(reset, 5, "v1 reset source run", false);
    require(reset.completed == 5 && reset.last_run_status() == 0, "v1 reset removes optional bounds");
    require(reset.native_consumed_high_water() > consumed
                && !NativeExecutionConsumer::bound(reset).scratch_bounds_for_run()
                && !reset.has_run_slab() && preceding->manifest.B_U == config.B_U,
            "next legitimate source run uses disk defaults and preserves the old immutable owner");

    // Exercise every remaining active field through the actual native C
    // setter, source begin binders, and real order/settlement counters.
    struct Case {
        std::uint64_t mask;
        const char* workload;
        const char* resource;
    };
    const std::array<Case, 6> cases{{
        {PF_SCRATCH_BOUND_K_E, "cohorts", "erased_index"},
        {PF_SCRATCH_BOUND_B_E, "cohorts", "erased_data"},
        {PF_SCRATCH_BOUND_K_C, "cycles", "consumed_index"},
        {PF_SCRATCH_BOUND_B_C, "cycles", "consumed_data"},
        {PF_SCRATCH_BOUND_V, "fills", "exit_rank"},
        {PF_SCRATCH_BOUND_B_V, "fills", "exit_rank"},
    }};
    const auto settlements = chart(12);
    for (const auto& test : cases) {
        ManifestHost host;
        host.workload = test.workload;
        config = policy(test.mask); // One explicit finite zero, all others omitted.
        require(strategy_set_scratch_tables_v2(&host, &config) == 0, "native zero bound accepted");
        host.run(settlements.data(), static_cast<int>(settlements.size()));
        require(host.last_run_status() != 0, "actual source execution reaches finite-zero refusal");
        require(std::strcmp(strategy_get_last_error_code(&host), "scratch_capacity_exceeded") == 0,
                "native zero bound keeps typed capacity code");
        require(std::string(strategy_get_last_error_args(&host))
                    == std::string("{\"resource\":\"") + test.resource + "\"}",
                "source/native bridge enforces the requested counter");
        require(host.completed < settlements.size(), "refusal stops the source workload");
    }
}
#endif
} // namespace

#ifdef PINEFORGE_SCRATCH_MANIFEST_MODULE
extern "C" {
pf_strategy_t strategy_create(const char*) { return static_cast<pineforge::BacktestEngine*>(new ManifestHost); }
void strategy_free(pf_strategy_t state) { delete static_cast<pineforge::BacktestEngine*>(state); }
void strategy_set_input(pf_strategy_t state, const char* key, const char* value) {
    auto& host = *static_cast<ManifestHost*>(static_cast<pineforge::BacktestEngine*>(state));
    if (key && value && std::strcmp(key, "workload") == 0
        && (std::strcmp(value, "cohorts") == 0 || std::strcmp(value, "fills") == 0
            || std::strcmp(value, "cycles") == 0 || std::strcmp(value, "roots") == 0)) {
        host.workload = value;
        return;
    }
    pineforge::note_run_failure(host, "unsupported scratch fixture input",
                               pineforge::RunFailureCode::setting_unsupported);
}
void strategy_set_override(pf_strategy_t state, const char*, const char*) {
    pineforge::note_run_failure(*static_cast<pineforge::BacktestEngine*>(state),
                               "fixture has no overrides", pineforge::RunFailureCode::setting_unsupported);
}
void run_backtest_full(pf_strategy_t state, pf_bar_t* rows, int count,
                       const char* input_tf, const char* script_tf, int magnifier,
                       int samples, pf_magnifier_distribution_t distribution, pf_report_t* out) {
    auto& host = *static_cast<ManifestHost*>(static_cast<pineforge::BacktestEngine*>(state));
    *out = {};
    try {
        std::vector<pineforge::Bar> values;
        values.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i)
            values.push_back({rows[i].open, rows[i].high, rows[i].low, rows[i].close,
                              rows[i].volume, rows[i].timestamp});
        host.run(values.data(), count, input_tf, script_tf, magnifier != 0, samples,
                 static_cast<pineforge::MagnifierDistribution>(distribution));
        if (strategy_last_run_status(state) != 0) return;
        host.fill_report(reinterpret_cast<pineforge::ReportC*>(out));
    } catch (const std::exception& error) { pineforge::note_run_failure(host, error.what(), error); }
}
void run_backtest(pf_strategy_t state, pf_bar_t* rows, int count, pf_report_t* out) {
    run_backtest_full(state, rows, count, "1", "1", 0, 4, PF_MAGNIFIER_ENDPOINTS, out);
}
void report_free(pf_report_t* out) {
    pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(out));
}
}
#else
int main() {
    try {
        pair_limits();
        tree_limits();
        source_bridge();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "scratch manifest options: %s\n", error.what());
        return 1;
    }
}
#endif
