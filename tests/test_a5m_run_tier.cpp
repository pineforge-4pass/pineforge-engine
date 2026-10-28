// The run's resident tier on the production path: a script laid out as
// generated code (no matrix Runtime installed), run by the engine, whose public
// calls make the run's Runtime current and end with its flush
// (src/engine_consumer.cpp). The script has the corpus regime mask's shape: a
// var 24x7 boolean mask with at most one set a bar, mT := transpose(mask),
// mTT := transpose(mT), 169 gets of mTT, and a 2x2 covariance with four sets
// and two eigenvalue reads. Rows:
//  - call boundary: after a batch run, and after stream_begin, every
//    stream_push_bar and stream_end, the run's tier is closed and empty and
//    the run's Store holds every script matrix's exact bytes; inside the calls
//    the tier was open and the transposes were made in it.
//  - flush failure: a backing write that fails at a push's flush fails that
//    push with the typed scratch_io_failed failure; the push's matrix changes
//    are not in the Store, which holds the bytes of the call before; the
//    failure is recorded as a failed cleanup, and it latches the run failed as
//    a throw out of the push would: the next push is refused as every input of
//    a failed run is, runs nothing and leaves the Store as it was.
//  - flush failure, batch run: a batch run whose flush fails after its last
//    execution is left failed, with the same typed failure.
//  - bit identity: the run under its tier, under an explicit native choice and
//    under an explicit Runtime (a Store with no tier) reads the same cells and
//    spectra every bar and makes the same trades.
//  - large bypass: a 23x23 numeric matrix (4,264 bytes, more than a slot) is
//    never resident and its cells are exact.
//
// FAIL-BEFORE. Against the previous header closure this unit does not compile:
// Runtime has no tier_open() (the first diagnostic) or tier_stats(), and
// RunRuntime has no open_over().
#include "a5m_run_script.hpp"
#include "../src/native_execution_consumer.hpp"

#include <pineforge/run_failure.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace ms = pineforge::detail::matrix_store;
namespace o1 = pineforge::detail::o1;
using a5m_test::require;
using a5m_growth::value_at;

using MaskModel = std::array<char, 24 * 7>;

// The bars whose execution marks its cell: index % 3 != 2, at (index % 24,
// index / 24 % 7).
bool marks(int index) { return index % 3 != 2; }

// The mask after the executions of `indices`, built here from the rule above.
MaskModel mask_model(const std::vector<int>& indices) {
    MaskModel model{};
    for (const int index : indices)
        if (marks(index)) model[static_cast<std::size_t>((index % 24) * 7 + (index / 24) % 7)] = 1;
    return model;
}

std::vector<int> first(int count) {
    std::vector<int> indices;
    for (int i = 0; i < count; ++i) indices.push_back(i);
    return indices;
}

std::vector<char> transposed(const MaskModel& model) {
    std::vector<char> out(model.size());
    for (int h = 0; h < 24; ++h)
        for (int d = 0; d < 7; ++d)
            out[static_cast<std::size_t>(d * 24 + h)] = model[static_cast<std::size_t>(h * 7 + d)];
    return out;
}

template <class Record>
std::vector<unsigned char> image(std::uint64_t rows, std::uint64_t columns,
                                 const std::vector<Record>& records) {
    const std::uint64_t descriptor[4] = {rows, columns, sizeof(Record), 0};
    std::vector<unsigned char> bytes(sizeof descriptor + records.size() * sizeof(Record));
    std::memcpy(bytes.data(), descriptor, sizeof descriptor);
    if (!records.empty())
        std::memcpy(bytes.data() + sizeof descriptor, records.data(),
                    records.size() * sizeof(Record));
    return bytes;
}

std::vector<unsigned char> stored(const o1::Store& store, o1::Id id) {
    std::vector<unsigned char> bytes(static_cast<std::size_t>(store.size(id)));
    require(store.read(id, 0, bytes.data(), bytes.size()) == bytes.size(),
            "short Store read of a matrix object");
    return bytes;
}

void fold(std::uint64_t& hash, const void* bytes, std::size_t size) {
    const auto* p = static_cast<const unsigned char*>(bytes);
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * UINT64_C(1099511628211);
}

// keep_transposes: mT and mTT are var matrices the transposes are assigned
// to, as in the corpus (the previous bar's transposes are released in the
// next call). Otherwise the transposes are the execution's own temporaries,
// so a call changes the Store only at its flush.
class MaskScript final : public pineforge::source::PineStrategyHost {
public:
    const bool keep_transposes;
    pineforge::PineGenericMatrix<bool> mask, mT, mTT;
    pineforge::PineMatrix cov;
    bool initialized = false;
    // One per execution: every get of the second transpose and both spectra.
    std::vector<std::uint64_t> digests;
    // The run's Runtime as the first execution found it (a run-owned one).
    std::weak_ptr<ms::Runtime> runtime;
    // An execution found the run's tier closed.
    bool tier_closed_inside = false;

    explicit MaskScript(bool keep = true) : keep_transposes(keep) {
        pineforge::source::PineStrategyConfig config{};
        configure_pine_strategy(config);
    }

    void prepare_script_run(const pineforge::Bar*, int, bool) override {
        mask = decltype(mask){};
        mT = decltype(mT){};
        mTT = decltype(mTT){};
        cov = decltype(cov){};
        initialized = false;
        digests.clear();
        runtime.reset();
    }

    void on_source_bar(const pineforge::Bar&) override {
        const int index = pine_bar_index();
        if (!initialized) {
            mask = pineforge::PineGenericMatrix<bool>::new_(24, 7, false);
            if (keep_transposes) {
                mT = pineforge::PineGenericMatrix<bool>::new_(7, 24, false);
                mTT = pineforge::PineGenericMatrix<bool>::new_(24, 7, false);
            }
            cov = pineforge::PineMatrix::new_(2, 2, 0.0);
            initialized = true;
            if (ms::Runtime* current = ms::current_runtime()) runtime = current->weak_from_this();
        }
        if (const auto shared = runtime.lock())
            if (!shared->tier_open()) tier_closed_inside = true;
        const int h = index % 24, d = (index / 24) % 7;
        if (marks(index)) mask.set(h, d, true);
        pineforge::PineGenericMatrix<bool> flipped, twice;
        if (keep_transposes) {
            mT = mask.transpose();
            mTT = mT.transpose();
        } else {
            flipped = mask.transpose();
            twice = flipped.transpose();
        }
        const auto& back = keep_transposes ? mTT : twice;
        std::uint64_t digest = UINT64_C(1469598103934665603);
        const bool sample = back.get(h, d);
        fold(digest, &sample, sizeof sample);
        int marked = 0;
        for (int i = 0; i < 24; ++i)
            for (int j = 0; j < 7; ++j) {
                const bool cell = back.get(i, j);
                marked += cell ? 1 : 0;
                fold(digest, &cell, sizeof cell);
            }
        const double spread = value_at(index, 1) - value_at(index, 0);
        cov.set(0, 0, 1.0 + spread);
        cov.set(0, 1, spread);
        cov.set(1, 0, spread);
        cov.set(1, 1, 1.0 + spread);
        for (const auto& spectrum : {cov.eigenvalues(), cov.eigenvalues()})
            for (const double value : spectrum) fold(digest, &value, sizeof value);
        digests.push_back(digest);
        // The decision reads the matrices, so a changed cell changes trades.
        if (marked % 2 == 1 && live_position_size() == 0) strategy_entry("L", true);
        else if (marked % 5 == 0 && live_position_size() > 0) strategy_close("L");
    }

    std::string fingerprint() const {
        std::uint64_t hash = UINT64_C(1469598103934665603);
        for (const pineforge::Trade& t : trades_) {
            fold(hash, &t.entry_time, sizeof t.entry_time);
            fold(hash, &t.exit_time, sizeof t.exit_time);
            fold(hash, &t.entry_price, sizeof t.entry_price);
            fold(hash, &t.exit_price, sizeof t.exit_price);
            fold(hash, &t.qty, sizeof t.qty);
            fold(hash, &t.pnl, sizeof t.pnl);
        }
        char text[96];
        std::snprintf(text, sizeof text, "trades=%zu fnv=%016llx", trades_.size(),
                      static_cast<unsigned long long>(hash));
        return text;
    }
};

// At a call boundary: the run's tier closed and empty, and the run's Store
// holding the script's matrices' exact bytes after the executions of
// `indices` (the covariance is the last one's), and `objects` live objects.
void at_boundary(ms::Runtime& runtime, const MaskScript& script, const std::vector<int>& indices,
                 const char* call, std::uint64_t objects) {
    const auto tier = runtime.tier_stats();
    if (runtime.tier_open() || tier.objects != 0 || tier.bytes != 0) {
        std::fprintf(stderr, "A5M run tier: after %s the tier is %s with %llu objects\n", call,
                     runtime.tier_open() ? "open" : "closed",
                     static_cast<unsigned long long>(tier.objects));
        require(false, "a public call left the run's tier open or holding objects");
    }
    const o1::Store& store = runtime.store();
    const auto model = mask_model(indices);
    const std::vector<char> rows(model.begin(), model.end());
    const int last = indices.back();
    const double spread = value_at(last, 1) - value_at(last, 0);
    bool exact =
        stored(store, script.mask.external_identity()) == image(24, 7, rows) &&
        stored(store, script.cov.external_identity()) ==
            image(2, 2, std::vector<double>{1.0 + spread, spread, spread, 1.0 + spread});
    if (script.keep_transposes)
        exact = exact && stored(store, script.mTT.external_identity()) == image(24, 7, rows) &&
                stored(store, script.mT.external_identity()) == image(7, 24, transposed(model));
    if (!exact) {
        std::fprintf(stderr, "A5M run tier: after %s the Store's matrix bytes differ\n", call);
        require(false, "a public call returned with the run's Store not holding its matrices");
    }
    require(store.stats().objects_live == objects,
            "the run's Store does not hold exactly the script's matrices");
}

void call_boundary() {
    constexpr int kBars = 240;
    const auto bars = a5m_run::tape(kBars);
    {
        MaskScript script;
        script.run(bars.data(), kBars, "1", "1", false);
        require(script.last_error().empty(), script.last_error().c_str());
        const auto runtime = script.runtime.lock();
        a5m_run::require_or_exit(runtime != nullptr,
                                 "the run's Runtime closed while its matrices were alive");
        require(!script.tier_closed_inside, "an execution found the run's tier closed");
        require(runtime->tier_stats().made >= 2u * kBars,
                "the run's transposes were not made in its tier");
        at_boundary(*runtime, script, first(kBars), "a batch run", 4);
    }
    {
        constexpr int kWarmup = 8;
        MaskScript script;
        require(script.stream_begin(bars.data(), kWarmup, "1", "1"), script.last_error().c_str());
        const auto runtime = script.runtime.lock();
        a5m_run::require_or_exit(runtime != nullptr,
                                 "the stream's Runtime closed while its matrices were alive");
        at_boundary(*runtime, script, first(kWarmup), "stream_begin", 4);
        for (int i = kWarmup; i < kBars; ++i) {
            require(script.stream_push_bar(bars[static_cast<std::size_t>(i)]),
                    script.last_error().c_str());
            at_boundary(*runtime, script, first(i + 1), "stream_push_bar", 4);
        }
        require(!script.tier_closed_inside, "an execution found the stream's tier closed");
        (void)script.stream_end(false);
        at_boundary(*runtime, script, first(kBars), "stream_end", 4);
    }
}

// The run's backing, whose next writes can be made to fail.
class FailingBacking final : public pineforge::detail::ScratchBacking {
public:
    FailingBacking() : ScratchBacking(pineforge::detail::ScratchBackingKind::disk, 4096) {}
    std::uint64_t page_count() const noexcept override { return inner_.page_count(); }
    void fail_next(int writes) noexcept { failing_ = writes; }
    // Arms one failing write once `executions` holds `count` entries (a
    // script's per-execution digests): the first write after that execution.
    void fail_after(const std::vector<std::uint64_t>& executions, std::size_t count) noexcept {
        watched_ = &executions;
        watched_count_ = count;
    }
    // Drops the watch before the watched script can be destroyed.
    void stop_watching() noexcept { watched_ = nullptr; }
    std::uint64_t written() const noexcept { return written_; }
    std::uint64_t failed() const noexcept { return failed_; }

protected:
    pineforge::detail::ScratchStatus do_read_page(
        pineforge::detail::ScratchPageId page,
        pineforge::detail::ScratchBuffer out) const noexcept override {
        return inner_.read_page(page, out);
    }
    pineforge::detail::ScratchStatus do_write_page(
        pineforge::detail::ScratchPageId page,
        pineforge::detail::ScratchBytes in) noexcept override {
        if (watched_ != nullptr && watched_->size() >= watched_count_) {
            watched_ = nullptr;
            failing_ = 1;
        }
        if (failing_ > 0) {
            --failing_;
            ++failed_;
            return pineforge::detail::ScratchStatus::io_error;
        }
        const auto status = inner_.write_page(page, in);
        if (status == pineforge::detail::ScratchStatus::ok) ++written_;
        return status;
    }
    pineforge::detail::ScratchStatus do_sync(
        pineforge::detail::ScratchDurability durability) noexcept override {
        return inner_.sync(durability);
    }

private:
    a5m_test::FileBacking inner_;
    int failing_ = 0;
    std::uint64_t written_ = 0, failed_ = 0;
    const std::vector<std::uint64_t>* watched_ = nullptr;
    std::size_t watched_count_ = 0;
};

// The run is latched failed as a throw out of the entry latches it: the
// consumer's lifecycle is Failed, with an Unexpected failure at `operation`.
void require_latched(const pineforge::BacktestEngine& engine,
                     pineforge::NativeFailureOperation operation, const char* call) {
    const pineforge::NativeStateView state =
        pineforge::NativeExecutionConsumer::bound(engine).view();
    if (state.kind == pineforge::NativeLifecycleKind::Failed &&
        state.failure.code == pineforge::NativeFailureCode::Unexpected &&
        state.failure.operation == operation)
        return;
    std::fprintf(stderr, "A5M run tier: after %s the run's state is %d, failure %d at %d\n", call,
                 static_cast<int>(state.kind), static_cast<int>(state.failure.code),
                 static_cast<int>(state.failure.operation));
    require(false, "a failed matrix flush did not latch the run failed");
}

// The script's transposes are its own temporaries here, so a push changes the
// run's Store only at its flush: the armed write is the flush's.
void flush_failure() {
    constexpr int kWarmup = 8, kBars = 32, kFailing = 24;
    const auto bars = a5m_run::tape(kBars);
    MaskScript script(false);
    auto backing = std::make_unique<FailingBacking>();
    FailingBacking* failing = backing.get();
    pineforge::NativeExecutionConsumer::bound(script).matrix_run().open_over(std::move(backing));
    require(script.stream_begin(bars.data(), kWarmup, "1", "1"), script.last_error().c_str());
    const auto runtime = script.runtime.lock();
    a5m_run::require_or_exit(runtime != nullptr,
                             "the stream's Runtime closed while its matrices were alive");
    for (int i = kWarmup; i < kFailing; ++i)
        require(script.stream_push_bar(bars[static_cast<std::size_t>(i)]),
                script.last_error().c_str());
    require(failing->written() > 0, "the run's Store is not over the installed backing");
    at_boundary(*runtime, script, first(kFailing), "the pushes before the failure", 2);
    const auto before = stored(runtime->store(), script.mask.external_identity());

    failing->fail_next(1);
    require(!script.stream_push_bar(bars[static_cast<std::size_t>(kFailing)]),
            "the push whose matrix flush failed did not fail");
    require(failing->failed() == 1, "the push made no backing write to fail");
    const std::string code = pineforge::run_failure_code_of(script);
    std::printf("A5M run tier flush failure: code=%s error='%s'\n", code.c_str(),
                script.last_error().c_str());
    require(code == "scratch_io_failed",
            "the flush failure is not the failing call's typed scratch failure");
    require(script.last_error().find("matrix store flush") != std::string::npos,
            "the failing call's error does not name the matrix flush");
    const auto tier = runtime->tier_stats();
    require(!runtime->tier_open() && tier.objects == 0 && tier.bytes == 0,
            "the failed flush left the tier open or holding objects");
    require(stored(runtime->store(), script.mask.external_identity()) == before,
            "the failed call's matrix changes reached the Store");
    const auto stats = runtime->store().stats();
    require(stats.cleanup_failed == 1 && stats.cleanup_skipped_rollback_only == 0,
            "the failed flush was not recorded as one failed cleanup");
    require_latched(script, pineforge::NativeFailureOperation::Input,
                    "the push whose matrix flush failed");
    require(script.last_error().rfind("stream_push_bar: matrix store flush: ", 0) == 0,
            "the latch changed the failing call's error");

    // The latch holds: the next push is refused as every input of a failed run
    // is (the consumer's refusal, with its own text and code), runs nothing and
    // leaves the Store as the failed push left it.
    const std::size_t executions = script.digests.size();
    require(!script.stream_push_bar(bars[static_cast<std::size_t>(kFailing + 1)]),
            "the push after a failed flush was not refused");
    const std::string refused = pineforge::run_failure_code_of(script);
    std::printf("A5M run tier flush failure, next push: code=%s error='%s'\n", refused.c_str(),
                script.last_error().c_str());
    require(refused == "engine_invariant" && script.last_error() == "native host already failed",
            "the push after a failed flush was not refused as an input of a failed run");
    require(script.digests.size() == executions && script.last_run_status() == 1,
            "the refused push ran the script or reported success");
    require_latched(script, pineforge::NativeFailureOperation::Input, "the refused push");
    at_boundary(*runtime, script, first(kFailing), "the refused push", 2);
    (void)script.stream_end(false);
}

// A batch run is one call: its flush, armed to fail after the run's last
// execution, fails the run, which is left failed with the typed failure.
void flush_failure_batch() {
    constexpr int kBars = 32;
    const auto bars = a5m_run::tape(kBars);
    MaskScript script(false);
    auto backing = std::make_unique<FailingBacking>();
    FailingBacking* failing = backing.get();
    failing->fail_after(script.digests, static_cast<std::size_t>(kBars));
    pineforge::NativeExecutionConsumer::bound(script).matrix_run().open_over(std::move(backing));
    script.run(bars.data(), kBars, "1", "1", false);
    failing->stop_watching();
    const auto runtime = script.runtime.lock();
    a5m_run::require_or_exit(runtime != nullptr,
                             "the run's Runtime closed while its matrices were alive");
    const std::string code = pineforge::run_failure_code_of(script);
    std::printf("A5M run tier flush failure, batch run: code=%s error='%s'\n", code.c_str(),
                script.last_error().c_str());
    require(failing->failed() == 1 && script.digests.size() == static_cast<std::size_t>(kBars),
            "the batch run's armed write was not one after its last execution");
    require(code == "scratch_io_failed" &&
                script.last_error().rfind("run: matrix store flush: ", 0) == 0,
            "the batch run's flush failure is not its typed scratch failure");
    require(script.last_run_status() == 1, "the batch run whose flush failed reported success");
    require_latched(script, pineforge::NativeFailureOperation::Input, "the batch run");
}

void bit_identity() {
    constexpr int kBars = 480;
    const auto bars = a5m_run::tape(kBars);
    std::vector<std::uint64_t> tier_digests, heap_digests, store_digests;
    std::string tier_trades, heap_trades, store_trades;
    {
        MaskScript script;
        script.run(bars.data(), kBars, "1", "1", false);
        require(script.last_error().empty(), script.last_error().c_str());
        require(script.mask.is_external_storage() && script.mTT.is_external_storage(),
                "the run's matrices are not in its Store");
        require(script.trade_count() > 0, "the matrix-driven orders never traded");
        tier_digests = script.digests;
        tier_trades = script.fingerprint();
    }
    {
        const ms::ScopedRuntime native(nullptr);
        MaskScript script;
        script.run(bars.data(), kBars, "1", "1", false);
        require(script.last_error().empty(), script.last_error().c_str());
        require(!script.mask.is_external_storage(), "an explicit native choice was overridden");
        heap_digests = script.digests;
        heap_trades = script.fingerprint();
    }
    {
        a5m_test::StoreFixture fixture;
        ms::Runtime chosen(*fixture.store);
        const ms::ScopedRuntime scope(chosen);
        {
            MaskScript script;
            script.run(bars.data(), kBars, "1", "1", false);
            require(script.last_error().empty(), script.last_error().c_str());
            require(script.mask.is_external_storage(), "an explicit Runtime was overridden");
            store_digests = script.digests;
            store_trades = script.fingerprint();
        }
        const auto tier = chosen.tier_stats();
        require(!chosen.tier_open() && tier.loaded == 0 && tier.made == 0,
                "a run under an explicit Runtime used a tier");
    }
    std::printf("A5M run tier bit identity: tier %s, heap %s, store %s\n", tier_trades.c_str(),
                heap_trades.c_str(), store_trades.c_str());
    require(tier_digests.size() == static_cast<std::size_t>(kBars) &&
                tier_digests == heap_digests && tier_digests == store_digests,
            "a bar read different cells or spectra under the tier");
    require(tier_trades == heap_trades && tier_trades == store_trades,
            "the run under the tier traded differently");
}

class LargeScript final : public pineforge::source::PineStrategyHost {
public:
    pineforge::PineMatrix big;
    bool initialized = false;
    bool exact = true;
    std::weak_ptr<ms::Runtime> runtime;
    bool resident_seen = false;

    LargeScript() {
        pineforge::source::PineStrategyConfig config{};
        configure_pine_strategy(config);
    }
    void prepare_script_run(const pineforge::Bar*, int, bool) override {
        big = decltype(big){};
        initialized = false;
        runtime.reset();
    }
    static int row_of(int index) { return index % 23; }
    static int column_of(int index) { return (index * 7) % 23; }
    void on_source_bar(const pineforge::Bar&) override {
        const int index = pine_bar_index();
        if (!initialized) {
            big = pineforge::PineMatrix::new_(23, 23, 0.0);
            initialized = true;
            if (ms::Runtime* current = ms::current_runtime()) runtime = current->weak_from_this();
        }
        big.set(row_of(index), column_of(index), value_at(index, 0));
        const auto flipped = big.transpose();
        exact = exact && flipped.get(column_of(index), row_of(index)) == value_at(index, 0) &&
                big.get(row_of(index), column_of(index)) == value_at(index, 0);
        if (const auto shared = runtime.lock()) {
            const auto tier = shared->tier_stats();
            if (tier.loaded || tier.made || tier.objects) resident_seen = true;
        }
    }
};

void large_bypass() {
    constexpr int kBars = 64;
    const auto bars = a5m_run::tape(kBars);
    LargeScript script;
    script.run(bars.data(), kBars, "1", "1", false);
    require(script.last_error().empty(), script.last_error().c_str());
    const auto runtime = script.runtime.lock();
    a5m_run::require_or_exit(runtime != nullptr,
                             "the run's Runtime closed while its matrices were alive");
    require(script.exact, "a matrix larger than a slot read a wrong cell");
    require(!script.resident_seen, "a matrix larger than a slot was held in the tier");
    std::vector<double> cells(23 * 23, 0.0);
    for (int i = 0; i < kBars; ++i)
        cells[static_cast<std::size_t>(LargeScript::row_of(i) * 23 + LargeScript::column_of(i))] =
            value_at(i, 0);
    require(stored(runtime->store(), script.big.external_identity()) == image(23, 23, cells),
            "the run's Store does not hold the large matrix's exact bytes");
}

} // namespace

int main() {
    bool ok = true;
    const auto row = [&ok](const char* name, void (*body)()) {
        try {
            body();
        } catch (const std::exception& error) {
            ok = false;
            std::fprintf(stderr, "A5M run tier %s: %s\n", name, error.what());
        }
    };
    row("call boundary", call_boundary);
    row("flush failure", flush_failure);
    row("flush failure, batch run", flush_failure_batch);
    row("bit identity", bit_identity);
    row("large bypass", large_bypass);
    if (ok) std::puts("A5M run tier: every row reached");
    return ok ? 0 : 1;
}
