// CR-150 RAM foundation. The frozen previous closure lacks both the v1
// descriptor and its C symbol; compile/link this whole body there first and
// record the first diagnostic remotely. No local execution was performed.
// Include C declarations first: engine.hpp suppresses per-strategy declarations.
#include <pineforge/pineforge.h>
#include <pineforge/native_host.hpp>
#include <pineforge/detail/scratch_slab.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {
using namespace pineforge;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

// A real reference/call requires the runtime C symbol at link time. There is
// no dynamic "unsupported" skip and no fallback to a different setter.
int (*const setter)(pf_strategy_t, const pf_scratch_tables_v1*) = &strategy_set_scratch_tables_v1;

pf_strategy_t handle(BacktestEngine& engine) { return static_cast<pf_strategy_t>(&engine); }

pf_scratch_tables_v1 descriptor(std::uint32_t backend = PF_SCRATCH_TABLE_BOUNDED_MEMORY) {
    return {static_cast<std::uint32_t>(sizeof(pf_scratch_tables_v1)), 1, backend, 0,
            2, 7, 3, 11, 4, 13, 5, 17, 6, 19, 7};
}

bool same_manifest(const detail::ScratchManifest& left, const detail::ScratchManifest& right) {
    return left.K_q == right.K_q && left.B_q == right.B_q
        && left.K_e == right.K_e && left.B_e == right.B_e
        && left.K_c == right.K_c && left.B_c == right.B_c
        && left.U == right.U && left.B_U == right.B_U
        && left.V == right.V && left.B_V == right.B_V && left.R == right.R;
}

void same_config(const BacktestEngine& engine, const detail::ScratchTableConfig& expected) {
    require(engine.scratch_table_config().backend == expected.backend, "backend preserved/copied");
    require(same_manifest(engine.scratch_table_config().manifest, expected.manifest),
            "all eleven maxima preserved/copied");
}

NativeRunSpec spec(const char* key) {
    NativeRunSpec out;
    out.identity = {key, 1};
    out.input_tf = "1";
    out.script_tf = "1";
    out.ticker = "MOCK";
    out.tickerid = "TEST:MOCK";
    out.type = "crypto";
    out.currency = "USD";
    out.timezone = "UTC";
    out.session = "24x7";
    out.initial_capital = 10000.0;
    out.point_value = 1.0;
    out.account_fx = 1.0;
    out.price_tick = 0.01;
    out.fee_kind = NativeFeeKind::CashPerExecution;
    out.fee_value = 0.0;
    out.report_policy = NativeReportPolicy::KernelRecorded;
    return out;
}

const std::array<Bar, 2> bars{{
    {100.0, 100.0, 100.0, 100.0, 1.0, 0},
    {100.0, 100.0, 100.0, 100.0, 1.0, 60000},
}};

class ActiveHost final : public NativeStrategyHost {
public:
    int prepare_calls = 0;
    int begin_calls = 0;
    int bar_calls = 0;
    bool is_stream = false;

    void prepare_native_begin(const NativeBeginArgs& args) override {
        is_stream = args.is_stream;
        ++prepare_calls;
        probe(); // Before Running: the public-call barrier still refuses.
    }
    void on_native_run_begin() override { ++begin_calls; probe(); }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++bar_calls; probe(); }

private:
    void probe() {
        const auto before = scratch_table_config();
        auto next = descriptor();
        require(setter(handle(*this), &next) == (is_stream ? -2 : -3), "active setter refusal");
        require(setter(handle(*this), nullptr) == (is_stream ? -2 : -3), "active NULL refusal");
        same_config(*this, before);
    }
};

class InspectionHost final : public NativeStrategyHost {
public:
    int prepare_calls = 0;
    int begin_calls = 0;
    int bar_calls = 0;
    std::shared_ptr<detail::ScratchSlab> observed;
    bool has_run_owner() const { return static_cast<bool>(scratch_tables_for_run()); }

    void prepare_native_begin(const NativeBeginArgs& args) override {
        ++prepare_calls;
        observed = scratch_tables_for_run();
        const auto before = scratch_table_config();
        auto replacement = descriptor(PF_SCRATCH_TABLE_DISK_SCRATCH);
        require(setter(handle(*this), &replacement) == (args.is_stream ? -2 : -3),
                "pre-begin bounded owner refuses replacement");
        require(setter(handle(*this), nullptr) == (args.is_stream ? -2 : -3),
                "pre-begin bounded owner refuses NULL");
        same_config(*this, before);
        require(observed == scratch_tables_for_run(), "active setter preserves the same slab owner");
        // Deliberately stop at the provider boundary: this witness exercises
        // the foundation allocation, not the other lanes' backend stubs.
        throw coded<std::runtime_error>(RunFailureCode::engine_invariant, {},
                                        "scratch foundation provider-boundary witness");
    }
    void on_native_run_begin() override { ++begin_calls; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++bar_calls; }
};

// NativeStrategyHost itself is intentionally noncopyable. Exercise the
// inherited engine copy policy through a plain engine subobject instead.
class CopyProbe final : public BacktestEngine {
public:
    CopyProbe() : BacktestEngine(NativeConsumerBindTag{}) {}
    explicit CopyProbe(const BacktestEngine& other) : BacktestEngine(other) {}
    void on_bar(const Bar&) override {}
    bool has_run_owner() const { return static_cast<bool>(scratch_tables_for_run()); }
};

void descriptor_layout_and_idle_setter() {
    static_assert(std::is_standard_layout<pf_scratch_tables_v1>::value, "C POD descriptor");
    // Four uint32 fields followed by eleven uint64 fields: 16+88=104 bytes.
    static_assert(sizeof(pf_scratch_tables_v1) == 4 * sizeof(std::uint32_t) + 11 * sizeof(std::uint64_t),
                  "scratch v1 has no appended/hidden fields");
    static_assert(offsetof(pf_scratch_tables_v1, struct_size) == 0, "size first");
    static_assert(offsetof(pf_scratch_tables_v1, version) == sizeof(std::uint32_t), "version second");
    static_assert(offsetof(pf_scratch_tables_v1, backend) == 2 * sizeof(std::uint32_t), "backend third");
    static_assert(offsetof(pf_scratch_tables_v1, reserved) == 3 * sizeof(std::uint32_t), "reserved fourth");
    const std::array<std::size_t, 11> offsets{{
        offsetof(pf_scratch_tables_v1, K_q), offsetof(pf_scratch_tables_v1, B_q),
        offsetof(pf_scratch_tables_v1, K_e), offsetof(pf_scratch_tables_v1, B_e),
        offsetof(pf_scratch_tables_v1, K_c), offsetof(pf_scratch_tables_v1, B_c),
        offsetof(pf_scratch_tables_v1, U), offsetof(pf_scratch_tables_v1, B_U),
        offsetof(pf_scratch_tables_v1, V), offsetof(pf_scratch_tables_v1, B_V),
        offsetof(pf_scratch_tables_v1, R),
    }};
    for (std::size_t i = 0; i < offsets.size(); ++i)
        require(offsets[i] == 4 * sizeof(std::uint32_t) + i * sizeof(std::uint64_t),
                "manifest fields retain the documented order");
    require(static_cast<std::uint32_t>(detail::ScratchTableBackend::disk_scratch)
                == PF_SCRATCH_TABLE_DISK_SCRATCH, "C/C++ disk discriminant");
    require(static_cast<std::uint32_t>(detail::ScratchTableBackend::bounded_memory)
                == PF_SCRATCH_TABLE_BOUNDED_MEMORY, "C/C++ RAM discriminant");

    ActiveHost host;
    same_config(host, {});
    auto config = descriptor();
    require(setter(nullptr, &config) == -1 && setter(nullptr, nullptr) == -1, "NULL handle refused");
    require(setter(handle(host), &config) == 0, "idle C setter exists and accepts v1");
    const detail::ScratchTableConfig expected{detail::ScratchTableBackend::bounded_memory,
                                              {2, 7, 3, 11, 4, 13, 5, 17, 6, 19, 7}};
    config = {}; // The engine must have copied every field, not borrowed this object.
    same_config(host, expected);

    auto invalid = descriptor();
    invalid.struct_size -= static_cast<std::uint32_t>(sizeof(std::uint64_t));
    require(setter(handle(host), &invalid) == -1, "short descriptor refused");
    same_config(host, expected);
    invalid = descriptor();
    ++invalid.struct_size;
    require(setter(handle(host), &invalid) == -1, "larger v1 descriptor refused");
    same_config(host, expected);
    invalid = descriptor();
    invalid.version = 0;
    require(setter(handle(host), &invalid) == -1, "version zero refused");
    invalid.version = 2;
    require(setter(handle(host), &invalid) == -1, "unknown version refused");
    same_config(host, expected);
    invalid = descriptor();
    invalid.backend = 2;
    require(setter(handle(host), &invalid) == -1, "unknown backend refused");
    invalid.backend = std::numeric_limits<std::uint32_t>::max();
    require(setter(handle(host), &invalid) == -1, "wrapped backend refused");
    same_config(host, expected);
    invalid = descriptor();
    invalid.reserved = 1;
    require(setter(handle(host), &invalid) == -1, "reserved field must be zero");
    invalid = descriptor();
    invalid.V = 28;
    require(setter(handle(host), &invalid) == -1, "rank maximum enforced");
    same_config(host, expected);
    require(setter(handle(host), nullptr) == 0, "idle NULL restores disk");
    same_config(host, {});
}

void active_batch_and_stream_refusals() {
    ActiveHost batch;
    require(batch.configure_native(spec("scratch-abi-batch")).status == NativeSetupStatus::Applied,
            "batch native setup");
    batch.run(bars.data(), static_cast<int>(bars.size()));
    require(batch.prepare_calls == 1 && batch.begin_calls == 1 && batch.bar_calls == 2,
            "all batch refusal sites actually executed");
    require(batch.native_state().kind == NativeLifecycleKind::Completed, "batch completed");
    require(std::strcmp(strategy_get_last_error_code(handle(batch)), "") == 0, "no batch failure");
    auto config = descriptor();
    require(setter(handle(batch), &config) == 0, "setter allowed after batch");
    require(setter(handle(batch), nullptr) == 0, "reset after batch");

    ActiveHost stream;
    require(stream.configure_native(spec("scratch-abi-stream")).status == NativeSetupStatus::Applied,
            "stream native setup");
    require(strategy_stream_begin(handle(stream), reinterpret_cast<const pf_bar_t*>(bars.data()),
                                  static_cast<int>(bars.size()), "1", "1") == 0,
            "C stream begin");
    require(stream.prepare_calls == 1 && stream.begin_calls == 1 && stream.bar_calls == 2,
            "warmup refusal sites actually executed");
    require(setter(handle(stream), &config) == -2, "between-input stream setter refusal");
    require(setter(handle(stream), nullptr) == -2, "between-input stream NULL refusal");
    same_config(stream, {});
    require(strategy_stream_end(handle(stream), 0) == 0, "C stream end");
    require(setter(handle(stream), &config) == 0, "setter allowed after stream");
    require(setter(handle(stream), nullptr) == 0, "NULL allowed after stream");
    same_config(stream, {});
}

void allocation_precedes_provider_and_copies_drop_ownership() {
    InspectionHost host;
    auto config = descriptor();
    require(setter(handle(host), &config) == 0, "select bounded memory");
    const auto expected = host.scratch_table_config();
    host.run(bars.data(), static_cast<int>(bars.size()));
    require(host.prepare_calls == 1 && host.begin_calls == 0 && host.bar_calls == 0,
            "allocation observed at provider boundary before execution");
    require(host.has_run_owner() && host.observed, "sink-OFF common begin owns the slab");
    require(same_manifest(host.observed->manifest(), expected.manifest), "run uses the copied manifest");
    require(std::strcmp(strategy_get_last_error_code(handle(host)), "engine_invariant") == 0,
            "provider-boundary witness stopped where requested");
    for (const auto& region : host.observed->regions()) {
        require(region.logical_extent == 0, "directory starts empty before backend binding");
        host.observed->extend(region.resource, region.capacity);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(region.capacity), 0xff);
        host.observed->read_at(region, 0, bytes.data(), bytes.size());
        require(std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte == 0; }),
                "complete slab is zero before provider mutation");
    }
    CopyProbe copied(static_cast<const BacktestEngine&>(host));
    same_config(copied, expected);
    require(!copied.has_run_owner(), "engine copy discards active slab ownership");
    CopyProbe assigned;
    assigned = copied;
    same_config(assigned, expected);
    require(!assigned.has_run_owner(), "engine assignment discards active ownership");
    require(setter(handle(host), nullptr) == 0, "idle reset after provider refusal");
    same_config(host, {});
    require(host.has_run_owner(), "changing policy preserves old readers until the next begin");
}

void overflow_precedes_provider_mutation() {
    InspectionHost host;
    auto config = descriptor();
    config.K_q = 0;
    config.B_q = std::numeric_limits<std::uint64_t>::max(); // page(max) cannot fit.
    require(setter(handle(host), &config) == 0, "copy representable scalar fields while idle");
    host.run(bars.data(), static_cast<int>(bars.size()));
    require(host.prepare_calls == 0 && host.begin_calls == 0 && host.bar_calls == 0,
            "overflow refused before any provider callback");
    require(!host.has_run_owner(), "overflow publishes no allocation");
    require(std::strcmp(strategy_get_last_error_code(handle(host)), "scratch_capacity_exceeded") == 0,
            "pre-begin typed layout refusal");
    require(std::strcmp(strategy_get_last_error_args(handle(host)), "{\"resource\":\"layout\"}") == 0,
            "pre-begin refusal carries only its closed resource");
}

} // namespace

int main() {
    try {
        descriptor_layout_and_idle_setter();
        active_batch_and_stream_refusals();
        allocation_precedes_provider_and_copies_drop_ownership();
        overflow_precedes_provider_mutation();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_scratch_tables_abi: %s\n", error.what());
        return 1;
    }
}
