#pragma once

// CR-150 P00 internal C++ declarations only. DESIGN.md revision 6,
// section 3.7:782-901; C receipt mapping in section 6:1291-1317.
#include <pineforge/report_sink.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace pineforge {
namespace report_sink {

/// Run properties selecting the fixed version-1 descriptors (3.7:795-809,
/// 823-831; 6:1310-1311). E and T are unconditional. outputs selects O, V,
/// K together; trace selects X; hashes selects H. series_slots supplies k
/// for O's v0..v<k-1>, constant_count supplies m for K's c0..c<m-1>.
/// Descriptor order is E,T,O,V,K,X,H; fields/types/units are exactly DESIGN's
/// table, excluding the tag. No caller-defined schema is admitted in v1.
struct RecordSet {
    bool outputs;
    bool trace;
    bool hashes;
    std::int64_t series_slots;
    std::int64_t constant_count;
};

/// Trailer values for mapping to the C receipt (3.7:833-837; 6:1291-1298).
/// Only types present in the header are emitted as counts, in header order.
/// sha256_hex is 64 lowercase hex digits followed by NUL. Completion and
/// trades_retained belong to the runtime receipt, not to this wire trailer.
struct Trailer {
    std::int64_t count_e, count_t, count_o, count_v, count_k, count_x, count_h;
    char sha256_hex[65];
};

/// One complete JSON f64 token, exactly Python 3 repr(double), or "null"
/// for non-finite values (3.7:850-853, 858-875). Preserve -0.0 and shortest
/// round-trip digits. First-digit decimal exponent -4 <= X < 16 uses fixed
/// notation with a digit after the point; otherwise use scientific notation
/// with an exponent sign and at least two digits. No locale-dependent text.
/// DESIGN prescribes shortest scientific to_chars followed by re-layout;
/// this declaration supplies no formatter implementation.
std::string f64(double value);

/// One complete quoted JSON string (3.7:878-879). Escape double quote and
/// backslash; encode U+0000..U+001F as lowercase \\u00xx. Other bytes remain
/// raw UTF-8; a byte not starting a valid UTF-8 sequence becomes U+FFFD.
std::string escaped_string(std::string_view utf8);

/// Checked 64 KiB buffered path writer (3.7:786-888), with opaque state.
/// Every line is one compact JSON value plus LF, UTF-8 without BOM, at most
/// 1 MiB including LF. No whitespace outside strings, blank lines or CR.
/// Integers are base 10 without leading zeros/+ or exponents, within
/// +/-(2^53-1) for i64 and 0..2^53-1 for u64; bool uses true/false; hex64
/// uses exactly 16 lowercase hex digits. f64/strings use the helpers above.
///
/// Lifecycle: header at run begin, records in final order, trailer only on
/// successful fill/capture. Failure/refusal/abort leaves no valid trailer.
/// I/O errors fail the run with the catalog code planned in DESIGN 6:1330-1332
/// (docker/run_failure_codes.json; 3.7:881-888). OPEN S2: DESIGN does not
/// choose the internal C++ error transport or repeated-finalization policy;
/// no exception type, retry or idempotence guarantee is declared here.
/// O/V/K/X/H operations below implement the same fixed version-1 descriptors.
class LineWriter {
public:
    /// Descriptor strings are copied, as required by 6:1307. This is a
    /// declaration of configuration; run-begin work belongs to write_header.
    explicit LineWriter(const pf_report_sink_v1& sink);
    ~LineWriter();

    /// Create/truncate the path and emit, in order, schema, version, run_id,
    /// engine, records (3.7:795-809, 881-883). schema="pf-report-sink",
    /// version=1, run_id from the descriptor (NULL => ""), engine is
    /// PINEFORGE_VERSION_STRING. Each descriptor has type,fields,types,units.
    void write_header(const RecordSet& records);

    /// Emit ["E",time_ms,equity,open_profit] (3.7:825), in time order.
    /// time_ms is the script-bar OPEN. Caller supplies the final presented
    /// point after hold-back; selected output is anchor + window points
    /// (3.2:490-509; 3.6:756-773). The writer does not alter the held point.
    void write_e(const pf_equity_point_t& point);

    /// Emit T in exactly this field order (3.7:826): n,side,entry_time,
    /// exit_time,entry_price,exit_price,qty,pnl,pnl_pct,max_runup,max_drawdown,
    /// commission,entry_bar_index,exit_bar_index,open_at_end,entry_incarnation,
    /// close_cause,entry_id,exit_id,exit_comment. n starts at 1; side is
    /// "long"/"short" from trade.is_long, open_at_end is a JSON bool.
    /// Numeric trade fields come from pf_trade_t; extra fields are the
    /// existing runtime accessors' values. Strings are consumed by the call.
    /// Caller supplies sealed rows, then final unsealed and range-end rows
    /// in report order (3.3:547-551; 3.4:639-652).
    void write_t(std::int64_t n, const pf_trade_t& trade,
                 std::uint64_t entry_incarnation, std::int64_t close_cause,
                 std::string_view entry_id, std::string_view exit_id,
                 std::string_view exit_comment);

    /// Emit trailer,version,counts,sha256 in that key order, version=1
    /// (3.7:833-837). SHA-256 is FIPS 180-4 over every byte before the
    /// trailer, including the header and each LF; the trailer excludes
    /// itself. Flush/check writes before returning the trailer values.
    /// SHA-256 runs inside the engine with no new external dependency
    /// (3.7:881-888). No destructor-generated success trailer is promised.
    Trailer write_trailer();

    // L6b-3 additions implementing the existing v1 descriptors. INT64_MIN
    // represents a null output time. Spans are consumed during the call.
    void write_o(std::int64_t row, std::int64_t open_ms, std::int64_t close_ms,
                 const double* values, std::int64_t count);
    // event.message selects null versus present; message supplies the exact
    // bytes of a present message, including embedded NULs.
    void write_v(const pf_output_event_v1_t& event, std::string_view message);
    void write_k(const double* values, std::int64_t count);
    void write_x(const pf_trace_entry_t& entry, std::string_view name);
    void write_h(std::uint64_t hash);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace report_sink
} // namespace pineforge
