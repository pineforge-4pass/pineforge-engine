// B2-01: BASE counts terminal lots in total_trades, but gives the sink summary
// a null/zero resident trade array. Its count invariant throws before trailer.
#include "report_terminal_quote_fix_fixture.hpp"

using namespace report_modes_fix;

int main() {
    return test_main("test_report_streamed_open_lot", [] {
        for (bool sealed : {false, true}) {
            const auto bars = tape(sealed ? SealedPrefixHost::closed_tape_bars : 12);
            SealedPrefixHost full, streamed;
            full.hold_lots = streamed.hold_lots = 3;
            full.close_first = streamed.close_first = sealed;
            streamed.set_trade_history_streamable(sealed);
            const auto path = sink_path(sealed ? "open-lot-sealed" : "open-lot-retained");
            install_sink(streamed, path);
            require(streamed.set_report_mode_v1(ReportMode::STREAMED)
                        == PF_REPORT_MODE_ACCEPTED, "STREAMED refused");
            run(full, bars);
            run(streamed, bars);
            require(streamed.open_lots() == 3, "fixture did not leave three open lots");
            const int expected_closed = sealed ? SealedPrefixHost::warmup_closes : 0;
            require(full.open_lots() == 3 && full.closed_rows() == expected_closed
                        && streamed.closed_rows() == expected_closed,
                    "fixture lost its closed rows or FULL terminal lots");
            require(streamed.sealed_base() == streamed.prefix_base(),
                    "ledger and report fold disagree on the sealed base");
            if (sealed) {
                require(streamed.sealed_base() > 0, "closed prefix never sealed");
                require(streamed.base_before_open > 0,
                        "terminal entries preceded the production seal");
            } else {
                require(streamed.sealed_base() == 0, "retained case unexpectedly sealed");
            }

            Report reference;
            full.fill_report(&reference.value);
            require(reference.value.total_trades == expected_closed + 3
                        && reference.value.trades_len == expected_closed + 3,
                    "FULL fixture lost closed or range-end rows");
            require_distinct_closed_prefix(reference.value, expected_closed);
            Envelope result;
            require(streamed.fill_report_envelope_v1(&result.value)
                        == PF_REPORT_MODE_ACCEPTED, "STREAMED envelope refused");
            const auto got = receipt(streamed);
            const auto folded = summary(streamed);
            require(result.value.metadata.complete == 1, "envelope incomplete");
            require(got.complete == 1 && folded.complete == 1, "sink incomplete");
            require(got.count_t == expected_closed + 3, "terminal lots missing from receipt");
            require(folded.count_t == result.value.report.total_trades,
                    "summary lost range-end rows");
            require(folded.count_t == got.count_t, "summary differs from trailer count");
            require(result.value.report.trades == nullptr
                        && result.value.report.trades_len == 0, "STREAMED owns trade rows");
            require(same_bits(result.value.report.net_profit, reference.value.net_profit),
                    "STREAMED net profit differs from FULL");
            require(std::memcmp(&result.value.report.metrics, &reference.value.metrics,
                                sizeof(pf_metrics_t)) == 0, "STREAMED metrics differ from FULL");
            const auto provenance = diagnostics(streamed);
            require(provenance.complete == 1 && reference.value.equity_curve_len > 0,
                    "fixture has no terminal equity");
            const auto& last = reference.value.equity_curve[reference.value.equity_curve_len - 1];
            require(std::memcmp(&provenance.last, &last, sizeof last) == 0,
                    "STREAMED last equity differs from FULL");
            require_trailer(read_sink(path));
        }
    });
}
