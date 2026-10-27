// CR150 ID-pair integration. Predicted red on d91c96d47d: the fixture
// consumed/erased observation members are absent; the real nonempty ID-pair
// binder also throws "bounded IdPair scratch binding is not installed".
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/detail/scratch_slab.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using detail::ScratchManifest;
using detail::ScratchResource;
using detail::ScratchSlab;
constexpr auto kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

struct Leaves {
    std::uint64_t consumed = 0, erased = 0, erased_rows = 0;
    std::size_t consumed_keys = 0;
    bool operator==(const Leaves& other) const {
        return consumed == other.consumed && erased == other.erased
            && erased_rows == other.erased_rows && consumed_keys == other.consumed_keys;
    }
};
Leaves leaves(const source::PineExecutionAdapter& adapter) {
    const auto& cycles = adapter.fixture_consumed_partial_exit_cycles();
    std::vector<std::string> keys;
    for (const auto& entry : cycles) {
        require(!entry.from_entry, "consumed cycles remain in the single-string domain");
        keys.push_back(entry.first);
    }
    std::sort(keys.begin(), keys.end());
    // Exactly the existing leaf at pine_state_hash.cpp: count, then sorted
    // strings and signed at(key). There are no harvested digest literals.
    BrokerStateHashSink fold;
    fold.u(keys.size());
    for (const auto& key : keys) { fold.s(key); fold.i(cycles.at(key)); }
    return {fold.h, adapter.fixture_erased_digest(), adapter.fixture_erased_rows(), keys.size()};
}

struct CycleImage {
    std::array<std::vector<std::uint8_t>, 2> bytes;
    std::array<std::uint64_t, 2> extent{};
    explicit CycleImage(const ScratchSlab& slab) {
        const std::array<ScratchResource, 2> resources{{ScratchResource::consumed_data,
                                                       ScratchResource::consumed_index}};
        for (std::size_t i = 0; i < resources.size(); ++i) {
            extent[i] = slab.region(resources[i]).logical_extent;
            bytes[i].resize(static_cast<std::size_t>(extent[i]));
            slab.read_at(resources[i], 0, bytes[i].data(), bytes[i].size());
        }
    }
    bool operator==(const CycleImage& other) const { return bytes == other.bytes && extent == other.extent; }
};

ScratchManifest envelope() {
    ScratchManifest result;
    result.K_e = 1024;
    result.B_e = 65536; // finite generous erasure envelope, not an observed bound
    // Six consumed keys: half-A0/B0/A1/B1/A2/B2 + separator + A0/B0/...
    // Each is 7+1+2=10 bytes. Thus K_c=6, B_c=60, D_c=40*6+60=300.
    result.K_c = 6;
    result.B_c = 6 * (7 + 1 + 2);
    return result;
}

class MultiEntryHost final : public source::PineStrategyHost {
public:
    explicit MultiEntryHost(std::shared_ptr<ScratchSlab> slab) : slab_(std::move(slab)) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 4.0;
        config.pyramiding = 2;
        config.process_orders_on_close = true;
        config.close_entries_rule_any = true;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        set_broker_state_hash_recording(true);
    }
    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 0) {
            reads.clear();
            peak_position = 0.0;
            before_partial.reset();
            // Host prepare has already reset every table. Bind the real
            // production ID-pair seam before the first source command. Trees
            // and roots stay on disk so this test needs no other lane's stub.
            if (slab_) detail::bind_id_pair_scratch_tables(adapter_, slab_);
        }
        peak_position = std::max(peak_position, std::abs(live_position_size()));
        const int cycle = index / 10;
        if (cycle < 12) {
            const auto suffix = std::to_string(cycle % 3);
            const std::string a = "A" + suffix, b = "B" + suffix;
            if (index % 10 == 0) strategy_entry(a, true, kNa, kNa, 4.0);
            if (index % 10 == 1) strategy_entry(b, true, kNa, kNa, 4.0);
            if (index % 10 == 2 || index % 10 == 4)
                strategy_exit("half-" + a, a, 100.5, 99.5, kNa, kNa, kNa, 50.0);
            if (index % 10 == 3 || index % 10 == 5)
                strategy_exit("half-" + b, b, 100.5, 99.5, kNa, kNa, kNa, 50.0);
            if (index % 10 == 6) strategy_close_all();
        }
        if (index == 2 && slab_) {
            // This callback precedes the first partial fill. Make all c
            // capacity readable for the failure byte comparison, before its
            // operation boundary; the store still has zero logical records.
            for (const auto resource : {ScratchResource::consumed_data, ScratchResource::consumed_index})
                slab_->extend(resource, slab_->region(resource).capacity);
            before_partial.emplace(*slab_);
            units_before_partial = live_position_size();
            trades_before_partial = trade_count();
        }
        reads.push_back(leaves(adapter_));
    }
    Leaves final_leaves() const { return leaves(adapter_); }
    std::uint64_t auxiliary_disk_bytes() const { return adapter_.fixture_auxiliary_disk_bytes(); }
    const std::vector<std::uint64_t>& hashes() const { return broker_state_hashes_; }
    std::vector<Leaves> reads;
    std::optional<CycleImage> before_partial;
    double units_before_partial = 0.0, peak_position = 0.0;
    int trades_before_partial = 0;
private:
    std::shared_ptr<ScratchSlab> slab_;
};

std::vector<Bar> tape() {
    std::vector<Bar> result;
    for (int i = 0; i < 128; ++i)
        result.push_back({100.0, 101.0, 99.0, 100.0, 1000.0, kStart + i * 60000LL});
    return result;
}
void same_trade(const Trade& a, const Trade& b) {
    require(a.entry_time == b.entry_time && a.exit_time == b.exit_time
            && a.entry_bar_index == b.entry_bar_index && a.exit_bar_index == b.exit_bar_index
            && a.entry_id == b.entry_id && a.exit_id == b.exit_id
            && a.entry_comment == b.entry_comment && a.exit_comment == b.exit_comment
            && a.entry_incarnation == b.entry_incarnation && a.is_long == b.is_long
            && a.open_at_end == b.open_at_end && a.exit_from_bracket == b.exit_from_bracket
            && a.close_cause == b.close_cause, "all trade identity/metadata fields match disk");
    require(same_bits(a.entry_price, b.entry_price) && same_bits(a.exit_price, b.exit_price)
            && same_bits(a.qty, b.qty) && same_bits(a.pnl, b.pnl) && same_bits(a.pnl_pct, b.pnl_pct)
            && same_bits(a.commission, b.commission) && same_bits(a.max_runup, b.max_runup)
            && same_bits(a.max_drawdown, b.max_drawdown), "all trade doubles match disk bitwise");
}

void identity() {
    const auto bars = tape();
    auto slab = std::make_shared<ScratchSlab>(envelope());
    const auto layout = slab->layout();
    MultiEntryHost disk({}), ram(slab);
    for (int run = 0; run < 2; ++run) {
        disk.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        ram.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        require(disk.last_error().empty() && ram.last_error().empty(), "both multi-entry runs complete");
        require(disk.peak_position >= 8.0 && ram.peak_position == disk.peak_position,
                "witness holds two independent four-unit openings before partial exits");
        require(disk.trade_count() > 0 && disk.trade_count() == ram.trade_count(), "same nonempty trade sequence");
        bool partial = false;
        for (int i = 0; i < disk.trade_count(); ++i) {
            const auto& trade = disk.get_trade(i);
            partial = partial || (trade.exit_from_bracket && trade.qty < 4.0);
            same_trade(trade, ram.get_trade(i));
        }
        require(partial, "real partial bracket fills exercised");
        const auto a = disk.final_leaves(), b = ram.final_leaves();
        require(a.consumed_keys == 6 && a.erased_rows > 0 && a == b && disk.reads == ram.reads,
                "consumed-cycle leaf and erasure-point digest match at every observation and final state");
        require(!disk.hashes().empty() && disk.hashes() == ram.hashes()
                && disk.broker_state_hash() == ram.broker_state_hash(), "whole broker hashes remain identical");
        require(disk.auxiliary_disk_bytes() > 0 && ram.auxiliary_disk_bytes() == 0,
                "comparison actually uses disk versus the bounded ID-pair slices");
        for (std::size_t i = 0; i < layout.regions.size(); ++i)
            require(slab->layout().regions[i].capacity == layout.regions[i].capacity
                    && slab->layout().regions[i].offset == layout.regions[i].offset,
                    "repeat runs never replace/grow a scratch region");
    }
}

void capacity_before_financial_commit(bool bytes) {
    auto manifest = envelope();
    manifest.K_c = bytes ? 1 : 0;
    // First consumed string half-A0 + separator + A0 is 10 bytes. For K=1,
    // B=9 yields D=49 rather than its 50-byte record, exactly one byte short.
    manifest.B_c = bytes ? 9 : 0;
    auto slab = std::make_shared<ScratchSlab>(manifest);
    MultiEntryHost host(slab);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    const auto failure = run_failure_value_of(host);
    // Bytes: limit 49=40*1+9, need 50=40+7+1+2; count: limit 0=K_c, need 1 for the first key.
    require(!host.last_error().empty() && failure.code == RunFailureCode::scratch_capacity_exceeded
            && failure.args && *failure.args == (bytes ? "{\"limit\":49,\"need\":50,\"resource\":\"consumed_data\"}"
                                                       : "{\"limit\":0,\"need\":1,\"resource\":\"consumed_index\"}"),
            "run boundary preserves the precommit capacity exception and exact resource");
    require(host.before_partial.has_value() && host.units_before_partial >= 8.0
            && host.trade_count() == host.trades_before_partial
            && same_bits(host.live_position_size(), host.units_before_partial)
            && host.final_leaves().consumed_keys == 0 && CycleImage(*slab) == *host.before_partial,
            "capacity refusal precedes financial close/trade creation and any consumed-cycle byte write");
}
} // namespace

int main() {
    try {
        identity();
        capacity_before_financial_commit(false);
        capacity_before_financial_commit(true);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_adapter_idpair_ram: %s\n", error.what());
        return 1;
    }
}
