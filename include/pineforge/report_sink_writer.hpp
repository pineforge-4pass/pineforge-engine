#pragma once

// CR-150 P00 internal C++ declarations only. DESIGN.md revision 6,
// section 3.7:779-897; C receipt mapping in section 6:1268-1294.
#include <pineforge/report_sink.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace pineforge {
namespace report_sink {

/// Run properties selecting the fixed version-1 descriptors (3.7:792-806,
/// 820-828; 6:1287-1288). E and T are unconditional. outputs selects O, V,
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

/// Trailer values for mapping to the C receipt (3.7:830-834; 6:1268-1275).
/// Only types present in the header are emitted as counts, in header order.
/// sha256_hex is 64 lowercase hex digits followed by NUL. Completion and
/// trades_retained belong to the runtime receipt, not to this wire trailer.
struct Trailer {
    std::int64_t count_e, count_t, count_o, count_v, count_k, count_x, count_h;
    char sha256_hex[65];
};

/// One complete JSON f64 token, exactly Python 3 repr(double), or "null"
/// for non-finite values (3.7:847-850, 855-872). Preserve -0.0 and shortest
/// round-trip digits. First-digit decimal exponent -4 <= X < 16 uses fixed
/// notation with a digit after the point; otherwise use scientific notation
/// with an exponent sign and at least two digits. No locale-dependent text.
/// DESIGN prescribes shortest scientific to_chars followed by re-layout;
/// this declaration supplies no formatter implementation.
std::string f64(double value);

/// One complete quoted JSON string (3.7:875-876). Escape double quote and
/// backslash; encode U+0000..U+001F as lowercase \\u00xx. Other bytes remain
/// raw UTF-8; a byte not starting a valid UTF-8 sequence becomes U+FFFD.
std::string escaped_string(std::string_view utf8);

/// Checked 64 KiB buffered path writer (3.7:783-885), with opaque state.
/// Every line is one compact JSON value plus LF, UTF-8 without BOM, at most
/// 1 MiB including LF. No whitespace outside strings, blank lines or CR.
/// Integers are base 10 without leading zeros/+ or exponents, within
/// +/-(2^53-1) for i64 and 0..2^53-1 for u64; bool uses true/false; hex64
/// uses exactly 16 lowercase hex digits. f64/strings use the helpers above.
///
/// Lifecycle: header at run begin, records in final order, trailer only on
/// successful fill/capture. Failure/refusal/abort leaves no valid trailer.
/// I/O errors fail the run as report_sink_failed. OPEN S2: DESIGN does not
/// choose the internal C++ error transport or repeated-finalization policy;
/// no exception type, retry or idempotence guarantee is declared here.
/// O/V/K/X/H writer operations are the later L6b-3 extension, not guessed here.
class LineWriter {
public:
    /// Descriptor strings are copied, as required by 6:1284. This is a
    /// declaration of configuration; run-begin work belongs to write_header.
    explicit LineWriter(const pf_report_sink_v1& sink);
    ~LineWriter();

    /// Create/truncate the path and emit, in order, schema, version, run_id,
    /// engine, records (3.7:792-806, 878-880). schema="pf-report-sink",
    /// version=1, run_id from the descriptor (NULL => ""), engine is
    /// PINEFORGE_VERSION_STRING. Each descriptor has type,fields,types,units.
    void write_header(const RecordSet& records);

    /// Emit ["E",time_ms,equity,open_profit] (3.7:822), in time order.
    /// time_ms is the script-bar OPEN. Caller supplies the final presented
    /// point after hold-back; selected output is anchor + window points
    /// (3.2:487-506; 3.6:753-770). The writer does not alter the held point.
    void write_e(const pf_equity_point_t& point);

    /// Emit T in exactly this field order (3.7:823): n,side,entry_time,
    /// exit_time,entry_price,exit_price,qty,pnl,pnl_pct,max_runup,max_drawdown,
    /// commission,entry_bar_index,exit_bar_index,open_at_end,entry_incarnation,
    /// close_cause,entry_id,exit_id,exit_comment. n starts at 1; side is
    /// "long"/"short" from trade.is_long, open_at_end is a JSON bool.
    /// Numeric trade fields come from pf_trade_t; extra fields are the
    /// existing runtime accessors' values. Strings are consumed by the call.
    /// Caller supplies sealed rows, then final unsealed and range-end rows
    /// in report order (3.3:544-548; 3.4:636-649).
    void write_t(std::int64_t n, const pf_trade_t& trade,
                 std::uint64_t entry_incarnation, std::int64_t close_cause,
                 std::string_view entry_id, std::string_view exit_id,
                 std::string_view exit_comment);

    /// Emit trailer,version,counts,sha256 in that key order, version=1
    /// (3.7:830-834). SHA-256 is FIPS 180-4 over every byte before the
    /// trailer, including the header and each LF; the trailer excludes
    /// itself. Flush/check writes before returning the trailer values.
    /// SHA-256 runs inside the engine with no new external dependency
    /// (3.7:878-885). No destructor-generated success trailer is promised.
    Trailer write_trailer();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace report_sink
} // namespace pineforge
