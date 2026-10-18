// CR-150 L15a: execute the frozen V1/V2 contract over synthetic data.
// Red on ee3083d4a: the accessor, context overloads and staging members are
// declared but undefined. No compiler or runtime result is claimed locally.
#include "native_instrument_feed_fixture.hpp"
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_bar_views.hpp>
#include <pineforge/run_failure.hpp>

#include <cassert>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
using namespace instrument_fixture;
namespace views = pineforge::native_bar_views_v1;

NativeInstrumentFeed tape() {
    FeedBuilder builder("SYN:VIEW", "1", 101.0);
    for (int i = 0; i < 6; ++i)
        builder.add(monday() + i * kMinute, monday() + (i + 1) * kMinute);
    builder.feed.columns.push_back({"x", {1, 2, 3, 4, 5, 6}});
    return builder.feed;
}

NativeInstrumentFeed metadata(const NativeInstrumentFeed& feed) {
    NativeInstrumentFeed result = feed;
    result.bars.clear();
    result.close_ms.clear();
    return result;
}

NativeInstrumentFeedView borrowed(const NativeInstrumentFeed& feed) {
    return {{feed.bars.data(), feed.bars.size()}, feed.close_ms.data()};
}

NativeRunSpec spec_for(const NativeInstrumentFeed& feed) {
    auto spec = continuous_spec("l15a-views", "1");
    spec.instrument_feeds.push_back(feed);
    spec.subscriptions.push_back(instrument_series(feed));
    return spec;
}

void effective_data_and_refusals() {
    const auto owned = tape();
    const auto lent = metadata(owned);
    const auto descriptor = borrowed(owned);
    const NativeInstrumentFeedView* slots[] = {&descriptor};
    const NativeBarViews context{slots, 1};
    const auto read = native_instrument_feed_view(lent, &descriptor);
    assert(read.bars.data == owned.bars.data());
    assert(read.close_ms == owned.close_ms.data());
    assert(read.bars.size == 6 && read.close_count == 6);
    const auto fallback = native_instrument_feed_view(owned, nullptr);
    assert(fallback.bars.data == owned.bars.data());

    const NativeInstrumentFeedView empty{{nullptr, 0}, nullptr};
    assert(native_instrument_feed_view(owned, &empty).bars.size == 0);
    auto missing_close = owned;
    missing_close.close_ms.pop_back();
    assert(native_instrument_feed_view(missing_close, nullptr).close_count == 5);
    assert(views::native_instrument_feeds_digest({missing_close}, {})
           == pineforge::native_run_spec_v3::native_instrument_feeds_digest({missing_close}));

    auto logical = spec_for(lent);
    const auto reference = spec_for(owned);
    assert(views::normalize_native_run_spec(logical, context));
    assert(views::native_instrument_feeds_digest(logical.instrument_feeds, context)
           == pineforge::native_run_spec_v3::native_instrument_feeds_digest(reference.instrument_feeds));
    assert(views::native_run_spec_digest(logical, context)
           == pineforge::native_run_spec_v3::native_run_spec_digest(reference));
    assert(views::validate_native_timeframe_subscriptions(
        logical.subscriptions, logical.input_tf, false, logical.auxiliary_feed,
        logical.instrument_feeds, context));

    auto bad_column = logical;
    bad_column.instrument_feeds[0].columns[0].values.pop_back();
    const auto column_error = views::normalize_native_run_spec(bad_column, context);
    assert(column_error.error == NativeRunSpecError::InvalidInstrumentFeedColumn);
    assert(column_error.field == NativeRunSpecField::InstrumentFeedColumns);

    auto bad_close = owned.close_ms;
    bad_close[0] = owned.bars[0].timestamp;
    const NativeInstrumentFeedView invalid{{owned.bars.data(), owned.bars.size()}, bad_close.data()};
    const NativeInstrumentFeedView* bad_slots[] = {&invalid};
    const auto close_error = views::validate_native_instrument_feeds({lent}, {bad_slots, 1});
    assert(close_error.error == NativeRunSpecError::InvalidInstrumentFeedClose);
    assert(close_error.field == NativeRunSpecField::InstrumentFeedClose);
    const auto conflict = views::validate_native_instrument_feeds({owned}, context);
    assert(conflict.error == NativeRunSpecError::InvalidInstrumentFeedBar);
    const auto mismatch = views::validate_native_instrument_feeds({}, context);
    assert(mismatch.error == NativeRunSpecError::InvalidInstrumentFeedBar);

    auto empty_meta = lent;
    empty_meta.columns.clear();
    const NativeInstrumentFeedView* empty_slots[] = {&empty};
    assert(views::validate_native_instrument_feeds({empty_meta}, {empty_slots, 1}));
    const NativeInstrumentFeedView* mixed_slots[] = {nullptr, &descriptor};
    auto other = owned;
    other.instrument = "SYN:OWNED";
    assert(views::validate_native_instrument_feeds({other, lent}, {mixed_slots, 2}));

    auto nan_twin = owned;
    std::uint64_t bits = UINT64_C(0x7ff8000000000042);
    std::memcpy(&nan_twin.bars[0].volume, &bits, sizeof(bits));
    const auto nan_descriptor = borrowed(nan_twin);
    const NativeInstrumentFeedView* nan_slots[] = {&nan_descriptor};
    assert(views::native_instrument_feeds_digest({lent}, {nan_slots, 1})
           == pineforge::native_run_spec_v3::native_instrument_feeds_digest({owned}));
}

class HashHost : public InstrumentHost {
public:
    std::vector<std::uint64_t> hashes;
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        InstrumentHost::on_native_bar(bar, context);
        hashes.push_back(native_continuation_hash());
    }
};

void binding_delivery_and_detachment() {
    auto owned = tape();
    const auto reference = spec_for(owned);
    auto logical = spec_for(metadata(owned));
    HashHost copied;
    HashHost lent;
    auto& consumer = NativeExecutionConsumer::bound(lent);
    {
        auto descriptor = borrowed(owned);
        const NativeInstrumentFeedView* slots[] = {&descriptor};
        consumer.stage_instrument_feed_views(slots, 1);
    } // staging must own descriptor copies, not these stack objects.
    assert(consumer.bar_views().instrument_feed_count == 0);
    assert(copied.configure_native(reference).status == NativeSetupStatus::Applied);
    assert(lent.configure_native(logical).status == NativeSetupStatus::Applied);
    const auto active = consumer.bar_views();
    assert(active.instrument_feed_count == 1);
    assert(active.instrument_feeds[0]->bars.data == owned.bars.data());

    // Staging while Ready affects pending only; no Ready restaging.
    const NativeInstrumentFeedView empty{{nullptr, 0}, nullptr};
    const NativeInstrumentFeedView* next[] = {&empty};
    consumer.stage_instrument_feed_views(next, 1);
    assert(consumer.bar_views().instrument_feeds[0]->bars.size == owned.bars.size());
    consumer.stage_instrument_feed_views(nullptr, 0);

    const auto chart = continuous_quarter_chart(monday(), 1);
    // The chart is a valid single 1-minute input regardless of fixture spacing.
    copied.run(chart.data(), 1, "1", "1", false, 4, MagnifierDistribution::ENDPOINTS);
    lent.run(chart.data(), 1, "1", "1", false, 4, MagnifierDistribution::ENDPOINTS);
    assert(copied.last_error().empty() && lent.last_error().empty());
    assert(copied.deliveries.size() == lent.deliveries.size());
    assert(!lent.deliveries.empty());
    for (std::size_t i = 0; i < copied.deliveries.size(); ++i) {
        assert(bars_equal(copied.deliveries[i].bar, lent.deliveries[i].bar));
        assert(copied.deliveries[i].context.interval.last_traded_close_ms
               == lent.deliveries[i].context.interval.last_traded_close_ms);
    }
    assert(copied.hashes == lent.hashes);
    assert(copied.native_continuation_hash() == lent.native_continuation_hash());
    assert(consumer.bar_views().instrument_feed_count == 0);
    const auto after = lent.native_continuation_hash();
    owned.bars.clear();
    owned.bars.shrink_to_fit();
    owned.close_ms.clear();
    owned.close_ms.shrink_to_fit();
    assert(lent.native_continuation_hash() == after);
}

class VectorSource final : public NativeLowerPathSource {
    const std::vector<Bar>& bars_;
    int& destroyed_;
    bool throws_;
public:
    VectorSource(const std::vector<Bar>& bars, int& destroyed, bool throws = false)
        : bars_(bars), destroyed_(destroyed), throws_(throws) {}
    ~VectorSource() noexcept override { ++destroyed_; }
    std::size_t count() const override { return bars_.size(); }
    bool for_each(Visitor visitor, void* context) const override {
        if (throws_) throw std::runtime_error("l15a-walk-fault");
        for (const auto& bar : bars_) if (!visitor(context, bar)) return false;
        return true;
    }
    NativeBarView segment(std::int64_t, std::int64_t) override {
        return {bars_.data(), bars_.size()};
    }
    const Bar* next_after_segment() const noexcept override { return nullptr; }
};

void lower_context_and_exception_atomicity() {
    const auto chart = continuous_quarter_chart(monday(), 2);
    auto spec = continuous_spec("l15a-lower", "15");
    IntrabarPath::lower_tf path;
    path.tf = "1";
    path.bars = chart;
    spec.intrabar.value = path;
    auto lent = spec;
    lent.intrabar = IntrabarPath{};
    path.bars.clear();
    lent.intrabar.value = path;

    int destroyed = 0;
    {
        VectorSource source(chart, destroyed);
        assert(views::native_intrabar_path_digest(lent.intrabar, &source)
               == pineforge::native_run_spec_v3::native_intrabar_path_digest(spec.intrabar));
        const NativeBarViews context{nullptr, 0, &source};
        assert(views::native_run_spec_digest(lent, context)
               == pineforge::native_run_spec_v3::native_run_spec_digest(spec));
    }
    assert(destroyed == 1);
    HashHost host;
    auto& consumer = NativeExecutionConsumer::bound(host);
    consumer.stage_lower_path_source(std::make_unique<VectorSource>(chart, destroyed, true));
    assert(consumer.bar_views().lower_path == nullptr);
    bool threw = false;
    try {
        (void)host.configure_native(lent);
    } catch (const std::runtime_error& error) {
        threw = std::string(error.what()) == "l15a-walk-fault";
    }
    assert(threw);
    assert(host.native_state().kind == NativeLifecycleKind::Unconfigured);
    assert(consumer.bar_views().lower_path == nullptr);
    assert(destroyed == 2);
    assert(host.configure_native(spec).status == NativeSetupStatus::Applied);

    // A null staging table is a programmer invariant; active state is intact.
    bool invalid_table = false;
    try { consumer.stage_instrument_feed_views(nullptr, 1); }
    catch (const std::logic_error&) { invalid_table = true; }
    assert(invalid_table);
    assert(host.native_state().kind == NativeLifecycleKind::Ready);
}

void refused_configure_discards_pending() {
    HashHost host;
    auto& consumer = NativeExecutionConsumer::bound(host);
    const auto owned = tape();
    auto descriptor = borrowed(owned);
    const NativeInstrumentFeedView* slots[] = {&descriptor};
    consumer.stage_instrument_feed_views(slots, 1);
    auto bad = spec_for(metadata(owned));
    bad.instrument_feeds[0].columns[0].values.pop_back();
    const auto refused = host.configure_native(bad);
    assert(refused.status != NativeSetupStatus::Applied);
    assert(consumer.bar_views().instrument_feed_count == 0);

    HashHost retry;
    auto& retry_consumer = NativeExecutionConsumer::bound(retry);
    const auto logical = spec_for(metadata(owned));
    retry_consumer.stage_instrument_feed_views(slots, 1);
    assert(retry.configure_native(logical).status == NativeSetupStatus::Applied);
    auto malformed = make_bar(monday(), 0, 1.0);
    malformed.high = malformed.low - 1.0;
    retry.run(&malformed, 1, "1", "1", false, 4, MagnifierDistribution::ENDPOINTS);
    assert(retry.native_state().kind == NativeLifecycleKind::Ready);
    assert(retry_consumer.bar_views().instrument_feed_count == 0);
    const auto valid = make_bar(monday(), 0, 1.0);
    retry.run(&valid, 1, "1", "1", false, 4, MagnifierDistribution::ENDPOINTS);
    assert(!retry.last_error().empty()); // detached Ready is engine_invariant.
}
} // namespace

int main() {
    effective_data_and_refusals();
    binding_delivery_and_detachment();
    lower_context_and_exception_atomicity();
    refused_configure_discards_pending();
    std::puts("test_native_bar_views_l15a: contract, identity and lifetime assertions passed");
}
