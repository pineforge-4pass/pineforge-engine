// CR-150 P00 compile witness. Include every contract in an ordinary test
// target so the lab build must parse them. All checks are unevaluated or
// static_asserts: no constructor, destructor or API definition is required.
#include <pineforge/report_sink.h>
#include <pineforge/report_sink_writer.hpp>
#include <pineforge/trade_ledger.hpp>
#include <pineforge/metrics_stream.hpp>
#include <pineforge/native_bar_views.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

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

int main() {
    // CTest reports the build-time witness; it does not exercise future APIs.
    std::puts("test_cr150_contract_headers: C++ and C contract assertions compiled");
    return 0;
}
