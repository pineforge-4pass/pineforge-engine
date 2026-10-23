// D-07: cleanup must not replace the failed append's typed arena diagnostic.
// The edit lane does not execute this row; the supervisor runs base/fix/mutant
// controls on the lab box.
#include <pineforge/detail/journal_codec.hpp>
#include <pineforge/detail/scratch_arena.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {
using namespace pineforge::detail;
namespace codec = pineforge::detail::journal_codec;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

ScratchKeyOrder word_order() noexcept {
    return {0x4430374a4f555201ULL, nullptr,
        [](void*, const ScratchByteSource& left, const ScratchByteSource& right,
           int& order) noexcept {
            std::array<std::byte, 8> a{}, b{};
            if (left.size() != a.size() || right.size() != b.size())
                return ScratchStatus::programming_error;
            auto status = left.read(0, {a.data(), a.size()});
            if (status != ScratchStatus::ok) return status;
            status = right.read(0, {b.data(), b.size()});
            if (status != ScratchStatus::ok) return status;
            order = std::memcmp(a.data(), b.data(), a.size());
            return ScratchStatus::ok;
        }};
}

// Use the ScratchBacking forwarding/fault seam from test_scratch_store.cpp,
// with carried typed errors as in test_state_hash_carried_scratch_failure.cpp.
// Construct the errors before entering any noexcept backing operation.
class FaultBacking final : public ScratchBacking {
public:
    explicit FaultBacking(FixedMemoryScratchBacking& inner)
        : ScratchBacking(inner.kind(), inner.page_size()), inner_(inner),
          append_failure_(EFBIG, "journal append exhausted",
              {{"reason", "arena_exhausted"}, {"store", "command"}}),
          rollback_failure_(EIO, "journal rollback write",
              {{"reason", "arena_io"}}) {}

    std::uint64_t page_count() const noexcept override { return inner_.page_count(); }
    const ReportScratchIoError* carried_failure() const noexcept override {
        return carried_;
    }
    void arm() noexcept { armed_ = true; }
    std::uint64_t write_faults() const noexcept { return write_faults_; }
    std::uint64_t successful_writes() const noexcept { return successful_writes_; }

protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer out) const noexcept override {
        return inner_.read_page(id, out);
    }
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes source) noexcept override {
        if (armed_ && write_faults_ < 2) {
            carried_ = write_faults_ == 0 ? &append_failure_ : &rollback_failure_;
            ++write_faults_;
            return ScratchStatus::io_error;
        }
        const auto status = inner_.write_page(id, source);
        if (status == ScratchStatus::ok) ++successful_writes_;
        return status;
    }
    ScratchStatus do_sync(ScratchDurability durability) noexcept override {
        return inner_.sync(durability);
    }

private:
    FixedMemoryScratchBacking& inner_;
    const ReportScratchIoError append_failure_;
    const ReportScratchIoError rollback_failure_;
    const ReportScratchIoError* carried_ = nullptr;
    bool armed_ = false;
    std::uint64_t write_faults_ = 0;
    std::uint64_t successful_writes_ = 0;
};

void journal_codec_finish_preserves_first_failure() {
    constexpr std::size_t page_size = scratch_min_page_size;
    std::array<std::byte, 256 * page_size> storage{};
    FixedMemoryScratchBacking memory({storage.data(), storage.size()}, page_size);
    FaultBacking backing(memory);
    CommandScratchStore store(
        {scratch_min_page_count, page_size, 1, 1, word_order(),
         ScratchOpenMode::create_empty}, &backing);
    require(store.status() == ScratchStatus::ok, "open journal rollback fixture");

    const codec::WordSource committed_key(1), append_key(2);
    std::array<std::byte, 8> original{}, replacement{};
    original.fill(std::byte{0x11});
    replacement.fill(std::byte{0x22});
    const ScratchSpanSource original_source({original.data(), original.size()});
    const ScratchSpanSource replacement_source({replacement.data(), replacement.size()});
    ScratchHandle handle = scratch_no_handle;
    require(store.begin() == ScratchStatus::ok, "begin committed journal seed");
    require(store.put(committed_key, original_source, handle) == ScratchStatus::ok,
            "put committed journal seed");
    require(store.commit(ScratchDurability::buffered) == ScratchStatus::ok,
            "commit journal seed");
    require(store.record_count() == 1, "fixture has one committed record");

    require(store.begin() == ScratchStatus::ok, "begin failing journal transaction");
    const auto writes_before_edit = backing.successful_writes();
    require(store.put(committed_key, replacement_source, handle) == ScratchStatus::ok,
            "stage an edit of the committed record before arming faults");
    require(backing.successful_writes() > writes_before_edit,
            "editing committed bytes writes an undo image before fault injection");

    // This append exceeds the entire resident cache, so it must attempt a
    // backing write during put. The preceding edit already created undo work,
    // so finish's rollback must write even though this append itself fails.
    std::array<std::byte, 4 * page_size> payload{};
    payload.fill(std::byte{0x33});
    const ScratchSpanSource value({payload.data(), payload.size()});
    backing.arm();
    const auto first_status = store.put(append_key, value, handle);
    require(first_status == ScratchStatus::io_error, "append reaches the first I/O fault");
    require(backing.write_faults() == 1, "exactly the append write has failed before finish");
    require(store.transaction_active(), "failed append leaves rollback work active");
    require(store.carried_failure() != nullptr, "append carries its original typed failure");
    require(store.carried_failure()->code() == std::error_code(EFBIG, std::generic_category()),
            "the first failure is the injected arena exhaustion");

    std::optional<ReportScratchIoError> surfaced;
    try {
        const auto status = codec::finish(store, first_status);
        require(status == first_status, "finish returns the first failing status");
        // The owner raises the carried diagnostic after the status boundary.
        if (status == ScratchStatus::io_error)
            rethrow_carried_scratch_failure(store.carried_failure());
    } catch (const ReportScratchIoError& error) {
        surfaced = error;
    }
    require(backing.write_faults() == 2, "rollback reaches its distinct injected write fault");
    require(store.transaction_active(), "failed rollback leaves the transaction active");
    require(backing.carried_failure() != nullptr
            && backing.carried_failure()->code() == std::error_code(EIO, std::generic_category()),
            "rollback really replaced the backing's borrowed diagnostic with EIO");
    require(surfaced.has_value(), "journal owner surfaces a typed failure");
    require(surfaced->run_failure_code() == pineforge::RunFailureCode::report_sink_failed,
            "journal owner retains the report_sink_failed code");
    require(surfaced->run_failure().args != nullptr, "journal owner retains typed arguments");
    const auto& args = *surfaced->run_failure().args;
    require(args.find("\"reason\":\"arena_exhausted\"") != std::string::npos,
            "D-07: finish must preserve the append's arena_exhausted reason");
    require(args.find("\"store\":\"command\"") != std::string::npos,
            "journal owner retains the append's command store identity");
    require(surfaced->code() == std::error_code(EFBIG, std::generic_category()),
            "journal owner retains the append's errno");
    require(std::string(surfaced->what()).find("journal append exhausted") != std::string::npos,
            "journal owner retains the append's operation text");

    // Both faults are one-shot. A later rollback can finish the same undo
    // work, and the committed record must still contain its original bytes.
    require(store.rollback() == ScratchStatus::ok, "retry rollback after both faults are spent");
    require(!store.transaction_active() && store.record_count() == 1,
            "rollback restores the committed record count");
    ScratchRecordInfo info{};
    require(codec::find(store, 1, info) == ScratchStatus::ok, "find the committed seed after rollback");
    std::array<std::byte, 8> actual{};
    require(store.read(info.handle, ScratchField::value, 0, {actual.data(), actual.size()})
                == ScratchStatus::ok,
            "read the restored committed payload");
    require(actual == original, "failed append and rollback preserve committed bytes");
    require(codec::find(store, 2, info) == ScratchStatus::not_found,
            "failed append publishes no record");
}
} // namespace

int main() {
    try {
        journal_codec_finish_preserves_first_failure();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_journal_codec_finish_preserves_first_failure: %s\n", error.what());
        return 1;
    }
}
