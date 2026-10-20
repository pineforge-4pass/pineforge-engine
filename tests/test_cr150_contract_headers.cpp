// CR-150 P00 compile witness. Include every contract in an ordinary test
// target so the lab build must parse them. All checks are unevaluated or
// static_asserts: no constructor, destructor or API definition is required.
#include <pineforge/report_sink.h>
#include <pineforge/report_sink_writer.hpp>
#include <pineforge/trade_ledger.hpp>
#include <pineforge/metrics_stream.hpp>
#include <pineforge/native_bar_views.hpp>
#include "../src/native_execution_consumer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// DESIGN revision 6, section 6:1283-1306: fixed prefix, three native
// pointers, and the explicitly padded 144-byte receipt. The descriptor's
// pointer-dependent size is stated without assuming an LP64-only caller.
static_assert(std::is_standard_layout_v<pf_report_sink_v1>);
static_assert(sizeof(pf_report_sink_v1) == 8 + 3 * sizeof(const char*));
static_assert(offsetof(pf_report_sink_v1, struct_size) == 0);
static_assert(offsetof(pf_report_sink_v1, version) == 4);
static_assert(offsetof(pf_report_sink_v1, path) == 8);
static_assert(offsetof(pf_report_sink_v1, run_id) == 8 + sizeof(const char*));
static_assert(offsetof(pf_report_sink_v1, scratch_dir) == 8 + 2 * sizeof(const char*));

static_assert(std::is_standard_layout_v<pf_report_sink_fd_v1>);
static_assert(sizeof(int) == 4);
static_assert(offsetof(pf_report_sink_fd_v1, struct_size) == 0);
static_assert(offsetof(pf_report_sink_fd_v1, version) == 4);
static_assert(offsetof(pf_report_sink_fd_v1, fd) == 8);
static_assert(offsetof(pf_report_sink_fd_v1, run_id)
    == (12 + alignof(const char*) - 1) / alignof(const char*) * alignof(const char*));
static_assert(sizeof(pf_report_sink_fd_v1)
    == offsetof(pf_report_sink_fd_v1, run_id) + sizeof(const char*));
static_assert(std::is_same_v<decltype(&strategy_set_report_sink_fd_v1),
    int (*)(pf_strategy_t, const pf_report_sink_fd_v1*)>);

static_assert(std::is_standard_layout_v<pf_report_sink_receipt_v1>);
static_assert(sizeof(pf_report_sink_receipt_v1) == 144);
static_assert(offsetof(pf_report_sink_receipt_v1, struct_size) == 0);
static_assert(offsetof(pf_report_sink_receipt_v1, version) == 4);
static_assert(offsetof(pf_report_sink_receipt_v1, count_e) == 8);
static_assert(offsetof(pf_report_sink_receipt_v1, count_t) == 16);
static_assert(offsetof(pf_report_sink_receipt_v1, count_o) == 24);
static_assert(offsetof(pf_report_sink_receipt_v1, count_v) == 32);
static_assert(offsetof(pf_report_sink_receipt_v1, count_k) == 40);
static_assert(offsetof(pf_report_sink_receipt_v1, count_x) == 48);
static_assert(offsetof(pf_report_sink_receipt_v1, count_h) == 56);
static_assert(offsetof(pf_report_sink_receipt_v1, sha256_hex) == 64);
static_assert(sizeof(decltype(pf_report_sink_receipt_v1::sha256_hex)) == 65);
static_assert(offsetof(pf_report_sink_receipt_v1, reserved) == 129);
static_assert(sizeof(decltype(pf_report_sink_receipt_v1::reserved)) == 7);
static_assert(offsetof(pf_report_sink_receipt_v1, complete) == 136);
static_assert(offsetof(pf_report_sink_receipt_v1, trades_retained) == 140);
static_assert(PF_ABI_VERSION == 4);

// DESIGN 3.7:825,833-837,858-879: formatting and writer declarations are
// type-checked without taking an evaluated address of an undefined symbol.
namespace sink = pineforge::report_sink;
static_assert(std::is_same_v<decltype(&sink::f64), std::string (*)(double)>);
static_assert(std::is_same_v<decltype(&sink::escaped_string),
                             std::string (*)(std::string_view)>);
static_assert(std::is_same_v<decltype(&sink::LineWriter::write_e),
                             void (sink::LineWriter::*)(const pf_equity_point_t&)>);
static_assert(std::is_same_v<decltype(&sink::LineWriter::write_trailer),
                             sink::Trailer (sink::LineWriter::*)()>);
static_assert(std::is_constructible_v<sink::LineWriter, const pf_report_sink_fd_v1&>);
static_assert(sizeof(decltype(sink::Trailer::sha256_hex)) == 65);

// DESIGN 3.4:613-616: global counts and indices use the declared size_type;
// these expressions do not instantiate a ledger or its opaque State.
using Ledger = pineforge::TradeLedger;
static_assert(std::is_same_v<Ledger::size_type, std::size_t>);
static_assert(std::is_same_v<Ledger::value_type, pineforge::Trade>);
static_assert(std::is_same_v<decltype(std::declval<const Ledger&>().size()),
                             Ledger::size_type>);
static_assert(std::is_same_v<decltype(std::declval<const Ledger&>().base()),
                             Ledger::size_type>);
static_assert(std::is_same_v<decltype(std::declval<Ledger&>()[Ledger::size_type{}]),
                             pineforge::Trade&>);
static_assert(std::is_same_v<decltype(std::declval<const Ledger&>()[Ledger::size_type{}]),
                             const pineforge::Trade&>);

// CORE-TOP rev 6 amendment (10-18 16:04), T1: new frozen signatures only.
static_assert(std::is_same_v<decltype(&Ledger::clear), void (Ledger::*)()>);
static_assert(std::is_same_v<decltype(&Ledger::reserve),
                             void (Ledger::*)(Ledger::size_type)>);
static_assert(std::is_same_v<decltype(&Ledger::empty), bool (Ledger::*)() const>);
static_assert(std::is_same_v<decltype(std::declval<Ledger&>().at(Ledger::size_type{})),
                             pineforge::Trade&>);
static_assert(std::is_same_v<decltype(std::declval<const Ledger&>().at(Ledger::size_type{})),
                             const pineforge::Trade&>);

// DESIGN 3.3:529,535-551 and 3.8:906-907: the accumulator accepts TradeC,
// finish is a const read, and both sources expose exact 24-byte points.
using Accumulator = pineforge::metrics::TradeStatsAccumulator;
using ArrayCurve = pineforge::metrics::ArrayCurve;
using SpillCurve = pineforge::metrics::SpillCurve;
static_assert(std::is_same_v<decltype(&Accumulator::fold),
                             void (Accumulator::*)(const pineforge::TradeC&)>);
static_assert(std::is_same_v<decltype(&Accumulator::finish),
                             pf_trade_stats_t (Accumulator::*)() const>);
static_assert(sizeof(pf_equity_point_t) == 24);
static_assert(offsetof(pf_equity_point_t, time_ms) == 0);
static_assert(offsetof(pf_equity_point_t, equity) == 8);
static_assert(offsetof(pf_equity_point_t, open_profit) == 16);
static_assert(std::is_same_v<decltype(std::declval<const ArrayCurve&>().size()),
                             std::int64_t>);
static_assert(std::is_same_v<decltype(std::declval<const ArrayCurve&>()[std::int64_t{}]),
                             pf_equity_point_t>);
static_assert(std::is_same_v<decltype(std::declval<const SpillCurve&>().size()),
                             std::int64_t>);
static_assert(std::is_same_v<decltype(std::declval<const SpillCurve&>()[std::int64_t{}]),
                             pf_equity_point_t>);

// DESIGN 2.2:347-349: exact borrowed bar/close fields in their own namespace.
using BarView = pineforge::native_bar_views_v1::NativeBarView;
using FeedView = pineforge::native_bar_views_v1::NativeInstrumentFeedView;
static_assert(std::is_standard_layout_v<BarView>);
static_assert(std::is_same_v<decltype(BarView::data), const pineforge::Bar*>);
static_assert(std::is_same_v<decltype(BarView::size), std::size_t>);
static_assert(std::is_standard_layout_v<FeedView>);
static_assert(std::is_same_v<decltype(FeedView::bars), BarView>);
static_assert(std::is_same_v<decltype(FeedView::close_ms), const std::int64_t*>);

// CORE-TOP rev 6 amendment (10-18 19:24), V1/V2 as amended by changes 1-9.
namespace bv = pineforge::native_bar_views_v1;
using ReadView = bv::NativeInstrumentFeedReadView;
using Views = bv::NativeBarViews;
using Source = bv::NativeLowerPathSource;
using Consumer = pineforge::NativeExecutionConsumer;
static_assert(std::is_standard_layout_v<ReadView>);
static_assert(std::is_same_v<decltype(ReadView::bars), BarView>);
static_assert(std::is_same_v<decltype(ReadView::close_ms), const std::int64_t*>);
static_assert(std::is_same_v<decltype(ReadView::close_count), std::size_t>);
static_assert(std::is_standard_layout_v<Views>);
static_assert(std::is_same_v<decltype(Views::instrument_feeds), const FeedView* const*>);
static_assert(std::is_same_v<decltype(Views::instrument_feed_count), std::size_t>);
static_assert(std::is_same_v<decltype(Views::lower_path), const Source*>);
static_assert(std::is_same_v<decltype(Views::lower_segment), BarView>);
static_assert(Views{}.instrument_feeds == nullptr);
static_assert(Views{}.instrument_feed_count == 0);
static_assert(Views{}.lower_path == nullptr);
static_assert(Views{}.lower_segment.data == nullptr);
static_assert(Views{}.lower_segment.size == 0);

static_assert(std::is_same_v<decltype(&bv::native_instrument_feed_view),
    ReadView (*)(const pineforge::NativeInstrumentFeed&, const FeedView*) noexcept>);
static_assert(std::is_same_v<decltype(&bv::normalize_native_run_spec),
    pineforge::NativeRunSpecValidation (*)(pineforge::NativeRunSpec&, const Views&) noexcept>);
static_assert(std::is_same_v<decltype(&bv::validate_native_instrument_feeds),
    pineforge::NativeRunSpecValidation (*)(
        const std::vector<pineforge::NativeInstrumentFeed>&, const Views&) noexcept>);
static_assert(std::is_same_v<decltype(&bv::validate_native_timeframe_subscriptions),
    pineforge::NativeRunSpecValidation (*)(
        const std::vector<pineforge::NativeTimeframeSubscription>&, const std::string&, bool,
        const std::optional<pineforge::NativeAuxiliaryFeed>&,
        const std::vector<pineforge::NativeInstrumentFeed>&, const Views&) noexcept>);
static_assert(std::is_same_v<decltype(&bv::native_instrument_feeds_digest),
    std::uint64_t (*)(const std::vector<pineforge::NativeInstrumentFeed>&,
                     const Views&) noexcept>);
static_assert(std::is_same_v<decltype(&bv::native_run_spec_digest),
    std::uint64_t (*)(const pineforge::NativeRunSpec&, const Views&)>);
static_assert(std::is_same_v<decltype(&bv::native_intrabar_path_digest),
    std::uint64_t (*)(const pineforge::IntrabarPath&, const Source*)>);

static_assert(std::is_abstract_v<Source>);
static_assert(std::has_virtual_destructor_v<Source>);
static_assert(std::is_nothrow_destructible_v<Source>);
static_assert(std::is_same_v<Source::Visitor,
    bool (*)(void*, const pineforge::Bar&) noexcept>);
static_assert(std::is_same_v<decltype(&Source::count), std::size_t (Source::*)() const>);
static_assert(std::is_same_v<decltype(&Source::for_each),
    bool (Source::*)(Source::Visitor, void*) const>);
static_assert(std::is_same_v<decltype(&Source::segment),
    BarView (Source::*)(std::int64_t, std::int64_t)>);
static_assert(std::is_same_v<decltype(&Source::next_after_segment),
    const pineforge::Bar* (Source::*)() const noexcept>);

static_assert(std::is_same_v<decltype(&Consumer::stage_instrument_feed_views),
    void (Consumer::*)(const FeedView* const*, std::size_t)>);
static_assert(std::is_same_v<decltype(&Consumer::stage_lower_path_source),
    void (Consumer::*)(std::unique_ptr<Source>) noexcept>);
static_assert(std::is_same_v<decltype(&Consumer::bar_views),
    Views (Consumer::*)() const noexcept>);

int main() {
    // CTest reports the build-time witness; it does not exercise future APIs.
    std::puts("test_cr150_contract_headers: C++ and C contract assertions compiled");
    return 0;
}
