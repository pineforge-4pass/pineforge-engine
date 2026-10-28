// A live trade-scratch digest preserves the backing's typed failure.
// Static read in the edit lane; execution belongs to the supervisor's lab.
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/detail/scratch_store.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {
using namespace pineforge;
using namespace pineforge::detail;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class FaultBacking final : public ScratchBacking {
public:
    explicit FaultBacking(FixedMemoryScratchBacking& inner) noexcept
        : ScratchBacking(inner.kind(), inner.page_size()), inner_(inner) {}

    std::uint64_t page_count() const noexcept override { return inner_.page_count(); }
    const ReportScratchIoError* carried_failure() const noexcept override {
        return carried ? &*carried : nullptr;
    }
    void fail_reads(bool on) noexcept { fail_reads_ = on; }
    std::uint64_t read_faults() const noexcept { return read_faults_; }

    std::optional<ReportScratchIoError> carried;

protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer destination) const noexcept override {
        if (fail_reads_) {
            ++read_faults_;
            return ScratchStatus::io_error;
        }
        return inner_.read_page(id, destination);
    }
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes source) noexcept override {
        return inner_.write_page(id, source);
    }
    ScratchStatus do_sync(ScratchDurability durability) noexcept override {
        return inner_.sync(durability);
    }

private:
    FixedMemoryScratchBacking& inner_;
    bool fail_reads_ = false;
    mutable std::uint64_t read_faults_ = 0;
};

struct Host final : source::PineStrategyHost {
    void on_source_bar(const Bar&) override {}
};

void live_store_hash_failure() {
    constexpr std::size_t page_size = 512;
    constexpr std::uint64_t records = 4;
    std::vector<std::byte> storage(256 * page_size);
    FixedMemoryScratchBacking inner({storage.data(), storage.size()}, page_size);
    FaultBacking fault(inner);
    CommandScratchStore store{
        {2, page_size, 1, 1, Host::trade_scratch_order(), ScratchOpenMode::create_empty},
        &fault};
    require(store.status() == ScratchStatus::ok, "open live hash scratch fixture");

    Host host;
    host.set_trade_scratch(store); // The public setter requires an empty store.
    require(store.begin() == ScratchStatus::ok, "begin live hash records");
    // Each value alone exceeds the two-page cache; a full digest cannot be
    // served from resident pages even immediately after a successful hash.
    std::array<std::byte, 4 * page_size> value{};
    value.fill(std::byte{0x5a});
    ScratchSpanSource value_source({value.data(), value.size()});
    for (std::uint64_t index = 0; index < records; ++index) {
        const std::array<std::uint64_t, 4> key{0, index, 0, 0};
        ScratchSpanSource key_source(
            {reinterpret_cast<const std::byte*>(key.data()), sizeof key});
        ScratchHandle handle = scratch_no_handle;
        require(store.put(key_source, value_source, handle) == ScratchStatus::ok,
                "put live hash record");
        require(handle != scratch_no_handle, "put returns a live handle");
    }
    require(store.commit(ScratchDurability::buffered) == ScratchStatus::ok,
            "commit live hash records");
    require(store.record_count() == records && inner.page_count() > 2,
            "committed hash population exceeds the resident cache");
    const auto reads_before = store.stats().page_reads;
    const auto h0 = host.broker_state_hash();
    require(store.stats().page_reads > reads_before,
            "pre-arm host hash actually reads backing pages");

    // A: the hash must throw the precise carried type/code/reason, not its
    // status-only fallback. Count faults around this hash alone.
    fault.carried = ReportScratchIoError(
        EFBIG, "scratch arena exhausted", {{"reason", "arena_exhausted"}, {"store", "command"}});
    fault.fail_reads(true);
    const auto typed_faults_before = fault.read_faults();
    bool caught_carried = false;
    try {
        (void)host.broker_state_hash();
    } catch (const ReportScratchIoError& error) {
        caught_carried = true;
        require(error.run_failure_code() == RunFailureCode::report_sink_failed,
                "A: hash preserves report_sink_failed");
        require(error.run_failure().args != nullptr, "A: hash preserves typed arguments");
        const auto& args = *error.run_failure().args;
        require(args.find("\"reason\":\"arena_exhausted\"") != std::string::npos,
                "A: hash preserves arena_exhausted");
        require(args.find("\"store\":\"command\"") != std::string::npos,
                "A: hash preserves the command store identity");
        require(error.code() == std::error_code(EFBIG, std::generic_category()),
                "A: hash preserves the original errno");
        require(std::string(error.what()).find("scratch arena exhausted") != std::string::npos,
                "A: hash preserves the original operation text");
    }
    require(caught_carried, "A: host hash must throw ReportScratchIoError");
    require(fault.read_faults() > typed_faults_before, "A: host hash reached a faulted backing read");

    // C, carried half: run before B replaces the store's retained diagnostic,
    // so both the backing and the store really carry it during this hash.
    fault.fail_reads(false);
    require(store.carried_failure() != nullptr, "C: store retained A's carried failure");
    const auto with_carrier = host.broker_state_hash();
    require(with_carrier == h0, "C: a carried failure is transport, not hash state");
    require(store.carried_failure() != nullptr, "C: successful reads retain the store diagnostic");

    // B: a later status-only I/O fault replaces that diagnostic and must
    // still raise the exact coded fallback rather than return a hash.
    fault.carried.reset();
    fault.fail_reads(true);
    const auto plain_faults_before = fault.read_faults();
    bool caught_plain = false;
    try {
        (void)host.broker_state_hash();
    } catch (const coded<std::logic_error>& error) {
        caught_plain = true;
        require(error.run_failure_code() == RunFailureCode::scratch_io_failed,
                "B: status-only hash failure retains scratch_io_failed");
        require(std::string(error.what()) == "scratch state digest failed",
                "B: status-only hash failure retains its exact text");
    }
    require(caught_plain, "B: host hash must throw coded<std::logic_error>");
    require(fault.read_faults() > plain_faults_before, "B: host hash reached a faulted backing read");
    require(store.carried_failure() == nullptr, "B: status-only fault replaced the carried failure");

    // C, no-carrier half: the same host and committed rows recover to h0.
    fault.fail_reads(false);
    const auto without_carrier = host.broker_state_hash();
    require(without_carrier == h0 && without_carrier == with_carrier,
            "C: the same host hashes equally with and without a carrier");
}
} // namespace

int main() {
    try {
        live_store_hash_failure();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_state_hash_carried_scratch_failure: %s\n", error.what());
        return 1;
    }
}
