// The placement sequence index keeps the default resident cache while its
// external extent grows; nothing of the index outlives its host.
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/command_scratch_cache.hpp"
#include "../src/source/pine_sequence_cache_observer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
namespace pd = pineforge::detail;
using Adapter = source::PineExecutionAdapter;
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Placements { friend auto access(Placements); };
struct Index { friend auto access(Index); };
struct Live { friend auto access(Live); };
template struct Access<Placements, &Adapter::placement_>;
template struct Access<Index, &source::PlacementTable::sequence_index_>;
template struct Access<Live, &Adapter::live_handles_>;

std::size_t checks = 0, failures = 0;
int trial_bars = 0;
void check(bool value, const char* expression, int line) {
    ++checks;
    if (value) return;
    ++failures;
    std::fprintf(stderr, "FAIL [placement_sequence_index] %s:%d: %s (bars=%d)\n",
                 __FILE__, line, expression, trial_bars);
}
#define ROW_CHECK(expression) check((expression), #expression, __LINE__)

constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t epoch = INT64_C(1736121600000);
// Default source policy in include/pineforge/cache_policy.hpp assigns 128
// 512-byte lines. Thus payload=128*512=65536; tags are additional, one per
// complete line, as defined by the shared cache's actual line geometry.
constexpr std::size_t page_bytes = 512;
constexpr std::size_t default_lines = 128;
constexpr std::size_t default_payload = default_lines * page_bytes;
static_assert(default_payload == pd::CachedScratchBacking::kCacheBytes,
              "default resident payload matches the visible product constant");

struct Watch {
    const pd::CachedScratchBacking* backing = nullptr;
    source::detail::SequenceCacheObserver previous;
    Watch() : previous(source::detail::set_sequence_cache_observer({this, created})) {}
    ~Watch() { source::detail::set_sequence_cache_observer(previous); }
    static void created(void* context, const pd::ScratchBacking& backing) noexcept {
        auto& self = *static_cast<Watch*>(context);
        self.backing = dynamic_cast<const pd::CachedScratchBacking*>(&backing);
        if (self.previous.created) self.previous.created(self.previous.context, backing);
    }
};

class Host final : public source::PineStrategyHost {
public:
    Host() {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        config.margin_long = config.margin_short = 0.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(1.0);
        set_broker_state_hash_recording(false);
    }
    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        if (bar == 0) strategy_entry("position", true, na, na, 1.0);
        if (bar == 2)
            strategy_exit("take", "position", 102.0, na, na, na, na, 100.0, {}, 1.0);
        // The flat tape never reaches a buy limit at 1. The 512 source bytes
        // alone occur in TWO real index keys per admitted order, so even the
        // 96-bar case has at least 96*2*512=98304 external key bytes, more
        // than the 65536-byte resident prefix. No cache is fed by the test.
        strategy_order(std::string(512, 'q') + "-" + std::to_string(bar), true, 1.0, 1.0);
    }
    const source::PlacementTable& placements() const {
        return adapter_.*access(Placements{});
    }
    std::size_t live() const { return (adapter_.*access(Live{})).size(); }
};

struct Sample {
    std::size_t slots = 0, payload = 0, metadata = 0, storage = 0;
    std::uint64_t extent = 0, workspace = 0;
};

Sample run(int bars) {
    trial_bars = bars;
    Watch watch;
    Sample result;
    // Retain only a weak identity, so destruction of the actual owning
    // adapter must release the lazy index, not just its placement rows.
    auto released = [&] {
        Host host;
        std::vector<Bar> tape;
        tape.reserve(static_cast<std::size_t>(bars));
        for (int i = 0; i < bars; ++i)
            tape.push_back({100.0, i == 3 ? 102.0 : 101.0, 99.0, 100.0, 1.0,
                            epoch + i * INT64_C(60000)});
        host.run(tape.data(), bars, "1", "1", false);
        ROW_CHECK(host.last_error().empty());
        ROW_CHECK(host.live() == static_cast<std::size_t>(bars));
        ROW_CHECK(host.live_position_size() == 0.0 && host.closed_trade_count() == 1);
        if (host.closed_trade_count() == 1) {
            const auto trade = host.closed_trade(0);
            // One unit bought at 100 and sold at 102 earns exactly 2.
            ROW_CHECK(trade.entry_id == "position" && trade.exit_id == "take"
                      && trade.qty == 1.0 && trade.entry_price == 100.0
                      && trade.exit_price == 102.0 && trade.pnl == 2.0);
        }
        const auto& table = host.placements();
        const auto& index = table.*access(Index{});
        ROW_CHECK(static_cast<bool>(index));
        ROW_CHECK(watch.backing != nullptr);
        if (watch.backing) {
            const auto& backing = *watch.backing;
            const auto line_bytes = pd::ReadThroughCache::line_bytes(page_bytes);
            ROW_CHECK(line_bytes >= page_bytes);
            const auto expected_metadata = default_lines * (line_bytes - page_bytes);
            result = {backing.cache_slot_count(), backing.cache_payload_bytes(),
                      backing.cache_metadata_bytes(), backing.cache_storage_bytes(),
                      backing.page_count(), table.sequence_work()[4]};
            // These are the real backing's container capacities, not a
            // configuration echo or the size of an unrelated control type.
            ROW_CHECK(backing.page_size() == page_bytes);
            ROW_CHECK(result.slots == default_lines && result.payload == default_payload);
            ROW_CHECK(result.metadata == expected_metadata
                      && result.storage == default_payload + expected_metadata);
            ROW_CHECK(backing.read_cache_stats().allocation_bypass_count == 0);
            // Setup witness only: the keys really exceed the resident prefix.
            ROW_CHECK(result.extent > default_lines);
        }
        return std::weak_ptr{index};
    }();
    ROW_CHECK(released.expired());
    // The observer's raw borrow is no longer used after the owning host ends.
    watch.backing = nullptr;
    return result;
}
} // namespace

int main() {
    // A fresh owner/observer in each trial.
    {
        try {
            const auto short_run = run(96);
            const auto long_run = run(4 * 96);
            ROW_CHECK(short_run.slots == long_run.slots
                      && short_run.payload == long_run.payload
                      && short_run.metadata == long_run.metadata
                      && short_run.storage == long_run.storage);
            ROW_CHECK(short_run.workspace != 0 && short_run.workspace == long_run.workspace);
            // External storage is allowed to track pending orders; this is
            // not an assertion that all external keys have a constant count.
            ROW_CHECK(long_run.extent > short_run.extent);
        } catch (const std::exception& error) {
            check(false, error.what(), __LINE__);
        } catch (...) {
            check(false, "unknown exception", __LINE__);
        }
    }
    std::printf("memory proof placement_sequence_index: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
