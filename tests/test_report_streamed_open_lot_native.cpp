// B2-01: the native STREAMED route must fold its retained close followed by
// all three range-end rows before completing the sink. Keep this TU source-free
// so the kernel profile runs it beside the source scheduler's sealed cases.
#include "report_modes_fix_fixture.hpp"

using namespace report_modes_fix;

int main() {
    return test_main("test_report_streamed_open_lot_native", [] {
        constexpr int expected_closed = 1;
        constexpr int expected_open = 3;
        constexpr int expected_total = expected_closed + expected_open;
        const auto bars = tape();
        Host full, streamed;
        full.hold_lots = streamed.hold_lots = expected_open;
        full.close_first = streamed.close_first = true;
        configure(full);
        configure(streamed);
        full.set_trade_history_streamable(false);
        streamed.set_trade_history_streamable(false);
        require(full.set_report_mode_v1(ReportMode::FULL) == PF_REPORT_MODE_ACCEPTED,
                "FULL refused");
        const auto path = sink_path("open-lot-native-retained");
        install_sink(streamed, path);
        require(streamed.set_report_mode_v1(ReportMode::STREAMED)
                    == PF_REPORT_MODE_ACCEPTED, "STREAMED refused");
        run(full, bars);
        run(streamed, bars);
        require(full.open_lots() == expected_open && streamed.open_lots() == expected_open,
                "native fixture did not leave three open lots");
        require(full.closed_trade_count() == expected_closed
                    && streamed.closed_trade_count() == expected_closed,
                "native fixture lost its retained close");
        require(full.sealed_base() == 0 && streamed.sealed_base() == 0,
                "native retained ledger unexpectedly sealed");

        Report reference;
        full.fill_report(&reference.value);
        require(reference.value.total_trades == expected_total
                    && reference.value.trades_len == expected_total,
                "FULL fixture lost its retained close or range-end rows");
        require_distinct_closed_prefix(reference.value, expected_closed);
        for (int i = expected_closed; i < expected_total; ++i)
            require(reference.value.trades[i].open_at_end
                        && reference.value.trades[i].pnl > 0.0,
                    "FULL fixture lacks three nonzero terminal rows");
        require(reference.value.equity_curve_len > 0, "FULL fixture has no terminal equity");
        const auto& last = reference.value.equity_curve[reference.value.equity_curve_len - 1];

        Envelope result;
        require(streamed.fill_report_envelope_v1(&result.value) == PF_REPORT_MODE_ACCEPTED,
                "native STREAMED envelope refused");
        const auto got = receipt(streamed);
        const auto folded = summary(streamed);
        require(result.value.metadata.complete == 1
                    && result.value.metadata.mode == PF_REPORT_MODE_STREAMED,
                "native STREAMED envelope incomplete or wrong mode");
        require(got.complete == 1 && folded.complete == 1, "native sink incomplete");
        require(got.trades_retained == 1 && streamed.sealed_base() == 0
                    && streamed.closed_trade_count() == expected_closed,
                "native sink did not retain its closed ledger");
        require(got.count_t == expected_closed + 3, "native receipt lost terminal lots");
        require(result.value.report.total_trades == expected_total
                    && result.value.metadata.trades.logical_count == expected_total,
                "native envelope lost closed or range-end rows");
        require(folded.count_t == result.value.report.total_trades,
                "native summary lost range-end rows");
        require(folded.count_t == got.count_t, "native summary differs from trailer count");
        require(result.value.report.trades == nullptr && result.value.report.trades_len == 0,
                "native STREAMED owns trade rows");
        require(result.value.report.equity_curve == nullptr
                    && result.value.report.equity_curve_len == 0,
                "native STREAMED owns equity rows");
        require(same_bits(result.value.report.net_profit, reference.value.net_profit),
                "native STREAMED net-profit bits differ from FULL");
        require(std::memcmp(&result.value.report.metrics, &reference.value.metrics,
                            sizeof(pf_metrics_t)) == 0,
                "native STREAMED metrics differ from FULL");
        const auto provenance = diagnostics(streamed);
        require(provenance.complete == 1 && got.count_e == reference.value.equity_curve_len
                    && provenance.count_e == got.count_e
                    && result.value.metadata.equity_curve.logical_count == got.count_e,
                "native sink lost equity provenance");
        require(std::memcmp(&provenance.last, &last, sizeof last) == 0,
                "native STREAMED terminal equity differs from FULL");
        require_trailer(read_sink(path));
    });
}
