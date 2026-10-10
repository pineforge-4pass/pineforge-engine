#pragma once

#include <exception>

namespace pineforge {

/// The refusal of a read-only state query, or of a report-only write, that may
/// no longer be answered.
///
/// BacktestEngine::broker_state_hash() and BacktestEngine::stream_state_hash()
/// throw it, before they project or fold anything, when the engine carries
/// selected-window provenance and its selected run has sealed: a post-seal read
/// is refused by name instead of answering from state the window no longer
/// owns. An engine without that provenance never throws it, and an engine has
/// it only when its run was admitted as a selected window. The check reads
/// only the engine's own execution slot and its consumer's open flag, so it
/// allocates nothing, runs no host code and is no input of any hash.
///
/// A caller that needs a selected run's terminal witness asks for it before the
/// run: enable set_broker_state_hash_recording(true), run, and read the last
/// recorded broker-state row (pf_report_t::broker_state_hash). A broker-state
/// row is not a stream-token value: stream_state_hash() has no recorded
/// counterpart, and no row means no witness.
///
/// Report-only writes. The same condition, the same predicate and this same
/// exception also fence the writers of the report-only terminal quote
/// (docs/report-terminal-quote.md), whose values the sealed capture can no
/// longer use: once the selected run has sealed (completed, failed or aborted),
/// source::PineStrategyHost::set_syminfo_metadata for the two reserved keys
/// report_terminal_quote_time_ms and report_terminal_quote_close, and
/// source::PineStrategyHost::clear_report_terminal_quote, throw it before any
/// store, whatever the value; the bool
/// source::PineStrategyHost::set_report_terminal_quote returns false instead,
/// also with no store. A preset before the first admitted begin, and every
/// handle without selected-window provenance, keep their original behaviour. The
/// refusal latches no run failure, leaves the captured report as it was and
/// changes nothing a read of the run's error, status or first cause would show.
///
/// The C exports never throw it. strategy_state_query_status_v1() reports
/// whether both queries would be refused, and strategy_broker_state_hash() and
/// strategy_stream_state_hash() return 0 for a refused read without entering
/// the C++ path. The C export strategy_set_syminfo_metadata() is void and
/// swallows every exception, so its refusal of a quote key is observed through
/// strategy_state_query_status_v1(): SELECTED_WINDOW_AFTER_SEAL means the write
/// stored nothing. Neither route changes a run's error, status or first cause.
/// Proof: tests/test_selected_quote_seal.cpp forces every writer after a
/// selected seal and compares the host object's bytes across the rejected call.
///
/// what() is the fixed literal "selected_window_query_after_seal"; the class
/// holds no string and allocates nothing.
class SelectedWindowQueryAfterSeal : public std::exception {
public:
    const char* what() const noexcept override {
        return "selected_window_query_after_seal";
    }
};

}  // namespace pineforge
