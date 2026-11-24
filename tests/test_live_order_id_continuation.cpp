#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {
using namespace pineforge;

class RestingOrder final : public source::PineStrategyHost {
public:
    RestingOrder() {
        configure_pine_strategy(source::PineStrategyConfig{});
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }

    void on_source_bar(const Bar&) override {
        ++bars;
        strategy_order("resting", true, 1.0, 1.0);
        // Read the acceptance before the end of this bar retires the journal window.
        const auto events = native_events(0);
        const auto book = native_working_requests();
        if (events.size() != 1 || !events.front().command
            || book.size() != 1 || !book.front().definition) return;
        const auto* accepted = std::get_if<native_order::AcceptedEvent>(&*events.front().command);
        if (accepted && accepted->definition && accepted->request().label == "resting"
            && accepted->handle() == book.front().definition->handle)
            acceptance = accepted->ordinal;
    }

    int bars = 0;
    std::uint64_t acceptance = 0;
};

struct Sample {
    std::uint64_t hash, acceptance;
    bool kept_completed_hash;
};

Sample sample(std::uint32_t backend, std::optional<std::uint64_t> cap) {
    RestingOrder host;
    auto* handle = static_cast<BacktestEngine*>(&host);
    pf_scratch_tables_v1 config{};
    config.struct_size = sizeof config;
    config.version = 1;
    config.backend = backend;
    config.K_e = config.K_c = config.U = config.V = config.R = 4;
    config.B_e = config.B_c = config.B_U = config.B_V = 64;
    if (strategy_set_scratch_tables_v1(handle, &config) != 0
        || (cap && strategy_set_scratch_live_order_ids_limit_v1(handle, *cap) != 0))
        throw std::runtime_error("scratch setup refused");

    const Bar bar{100.0, 101.0, 99.0, 100.0, 1.0, 1736121600000LL};
    if (!host.stream_begin(&bar, 1, "1", "1") || !host.stream_is_realtime())
        throw std::runtime_error("stream did not reach realtime: " + host.last_error());
    pf_pending_order_v1_t row{};
    if (host.bars != 1 || !host.acceptance || host.pending_order_count() != 1
        || strategy_pending_order_get(handle, 0, &row, sizeof row) != 0
        || std::string_view(row.id) != "resting" || row.is_long != 1
        || row.qty != 1.0 || row.limit_price != 1.0)
        throw std::runtime_error("expected one accepted and still-resting buy limit");
    const auto hash = host.native_continuation_hash();
    if (!host.stream_end() || host.native_state().kind != NativeLifecycleKind::Completed
        || !host.last_error().empty())
        throw std::runtime_error("stream did not complete: " + host.last_error());
    const auto completed = host.native_continuation_hash();
    if (strategy_set_scratch_live_order_ids_limit_v1(handle, 2) != 0)
        throw std::runtime_error("idle cap setter refused");
    return {hash, host.acceptance, host.native_continuation_hash() == completed};
}
} // namespace

int main() {
    try {
        constexpr auto ram = PF_SCRATCH_TABLE_BOUNDED_MEMORY, disk = PF_SCRATCH_TABLE_DISK_SCRATCH;
        constexpr auto larger = (std::uint64_t{1} << 32) + 1;
        const auto one = sample(ram, 1), other = sample(ram, larger), again = sample(ram, 1);
        const auto unlimited = sample(ram, std::nullopt), zero = sample(ram, 0);
        const auto plain = sample(disk, std::nullopt), disk_zero = sample(disk, 0), ignored = sample(disk, 1);
        for (const auto& s : {one, other, again, unlimited, zero, plain, disk_zero, ignored}) {
            if (s.acceptance != one.acceptance) throw std::runtime_error("acceptance ordinals differ");
            if (!s.kept_completed_hash) {
                std::fputs("FAIL: staging the next cap changed the completed continuation\n", stderr);
                return 1;
            }
        }

        if (one.hash != again.hash) std::fputs("FAIL: identical bounded caps differ\n", stderr);
        else if (zero.hash != unlimited.hash || zero.hash != plain.hash)
            std::fputs("FAIL: unlimited bounded scratch changed continuation identity\n", stderr);
        else if (ignored.hash != plain.hash || disk_zero.hash != plain.hash)
            std::fputs("FAIL: disk scratch folded the live-ID setting\n", stderr);
        else if (one.hash == other.hash)
            std::fputs("FAIL: different positive bounded caps share a continuation hash\n", stderr);
        else if (one.hash == unlimited.hash || other.hash == unlimited.hash)
            std::fputs("FAIL: a positive bounded cap aliases unlimited scratch\n", stderr);
        else {
            std::puts("PASS: continuation identifies the effective live-ID cap");
            return 0;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "NOT COMPARED: %s\n", error.what());
    }
    return 1;
}
