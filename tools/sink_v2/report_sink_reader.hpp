#pragma once

// Internal reference reader, docs/design/sink-v2.md, sections 3-9 and 11.
// This header is deliberately not installed and introduces no C ABI.
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include "report_sink_v2_writer.hpp"

namespace pineforge {
namespace report_sink {
class PhaseLog;
namespace v2 {

// All wire-layout constants live here so an APP-TOP layout correction has
// one home. Tests intentionally derive their encoder separately from the spec.
namespace layout {
inline constexpr std::string_view tags = "ETOVKXHI";
inline constexpr unsigned version = 2;
inline constexpr std::string_view block_magic = "BLK2", index_magic = "IDX2";
inline constexpr std::string_view root_magic = "FTR2", summary_magic = "SUM2";
inline constexpr std::uint32_t min_block_bytes = 4096;
inline constexpr std::uint64_t u32_position_count = UINT64_C(4294967296);
enum Encoding : unsigned { plain64=0, delta=1, xorshuf=2, decimal=3, bytes=4, dict=5, plainstr=6 };
inline constexpr std::uint64_t safe = UINT64_C(9007199254740991);
inline constexpr std::int64_t delta_limit = INT64_C(4503599627370496);
inline constexpr std::int64_t decimal_limit = INT64_C(1125899906842623);
inline constexpr std::size_t max_head = 2097152, max_line = 1048576;
inline constexpr std::size_t max_string = 1048575, zstd_header = 18;
inline constexpr std::size_t max_column = 16777216, max_block = 33554432;
// Section 3.7's S_MAX bounds decoded SUMMARY JSON and its window.
inline constexpr std::size_t max_summary = 1048576;
inline constexpr std::uint32_t max_slots = 1048576;
inline constexpr std::uint32_t max_records = 65536, max_interleave = 1048576;
inline constexpr std::size_t max_depth = 64, page_cache = 2;
inline constexpr std::size_t table_count = 19, stream_count = 8, rank_table_end = 18;
inline constexpr std::uint32_t figure_table = 18, figure_buckets = 1200, present = 1;
inline constexpr std::size_t digest = 32, head_prefix = 12, head_overhead = 16;
inline constexpr std::size_t bhead = 32, directory = 16, descriptor = 512;
inline constexpr std::size_t column_framing = directory + digest, figure_entry = 256;
// Section 3.7's effective ceiling (699,050), already implied by the block budget.
inline constexpr std::uint32_t max_columns = static_cast<std::uint32_t>((max_block - bhead) / column_framing);
inline constexpr std::size_t page = 65536, page_header = 4096, page_payload = 61440;
inline constexpr std::size_t root = 4096, tail = 160, summary_header = 52;
// Section 3.7 includes SUM2 plus the largest raw fallback at J=S_MAX.
inline constexpr std::size_t max_summary_length = 1048661;
inline constexpr std::size_t max_summary_stored = max_summary_length - summary_header;
inline constexpr std::size_t compact_root = 320, compact_descriptor = 96, compact_blocks = 128;
inline constexpr std::uint64_t compact_e_limit = 10980, compact_t_limit = 3314;
inline constexpr std::size_t compact_table_limit = compact_descriptor * compact_blocks;
inline constexpr std::size_t descriptors_per_page = page_payload / descriptor;
inline constexpr std::array<std::uint8_t, 8> magic{{0x89, 0x50, 0x46, 0x53, 0x32, 0x0d, 0x0a, 0x1a}};
inline constexpr std::string_view tail_magic = "PFS2END\n";
inline constexpr std::string_view col_domain{"PFS2-COL\0", 9};
inline constexpr std::string_view block_domain{"PFS2-BLOCK\0", 11};
inline constexpr std::string_view stream_domain{"PFS2-STREAM\0", 12};
inline constexpr std::string_view index_domain{"PFS2-INDEX\0", 11};
inline constexpr std::string_view node_domain{"PFS2-NODE\0", 10};
inline constexpr std::string_view logical_domain{"PFS2-LOGICAL\0", 13};
inline constexpr std::string_view compact_index_domain{"PFS2-CINDEX\0", 12};
inline constexpr std::array<double, 13> pow10{{
    1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0, 1000000.0,
    10000000.0, 100000000.0, 1000000000.0, 10000000000.0,
    100000000000.0, 1000000000000.0}};
struct Block { enum : std::size_t { tag=4, flags=5, reserved=6, ncols=8, n=12, rec0=16, body=24, crc=28 }; };
struct Dir { enum : std::size_t { codec=0, enc=1, flags=2, reserved=3, raw=4, stored=8, crc=12 }; };
struct Entry { enum : std::size_t {
    offset=0, length=8, tag=12, flags=13, reserved=14, n=16, decoded=20, rec0=24,
    amin=32, amax=40, bmin=48, bmax=56, sha=64, mtm=96, book=288, finite_prefix=480, padding=488 }; };
struct Series { enum : std::size_t { count=0, flags=4, witnesses=8, witness_size=40, padding=168, size=192 }; };
struct Witness { enum : std::size_t { ordinal=0, time=8, mtm=16, book=24, mask=32, reserved=36 }; };
struct Figure { enum : std::size_t { count=0, reserved=1, selectors=8, witnesses=16, slots=6, absent=255 }; };
struct Page { enum : std::size_t {
    id=4, flags=5, depth=6, reserved=7, number=8, used=12, width=16, pages=20,
    leaf=24, siblings=56, max_depth=32, padding=1080, crc=4092 }; };
struct Root { static constexpr std::size_t
    version=4, flags=6, data_end=8, summary_offset=16, summary_length=24, blocks=28,
    counts=32, streams=96, summary_sha=352, v1_sha=384, finite_count=416, figure_buckets=424, padding1=428,
    tables=512, table_size=64, padding2=1728, crc=4092; };
struct CompactRoot { static constexpr std::size_t
    summary_length=16, blocks=20, counts=24, finite_count=88, summary_sha=96, v1_sha=128,
    descriptors_offset=160, descriptors_length=168, descriptors_sha=176, block_counts=208,
    padding=240, crc=316; };
struct Table { enum : std::size_t { offset=0, pages=8, width=12, count=16, last_bytes=24, flags=28, sha=32 }; };
struct Tail { enum : std::size_t {
    root_offset=0, root_length=8, flags=12, file_bytes=16, root_sha=24,
    logical_sha=56, stored_sha=88, reserved=120, magic=152 }; };
struct Summary { enum : std::size_t { stored=4, length=8, crc=12, flags=16, sha=20 }; };
struct FieldSpec { const char* name; const char* type; const char* unit; };
inline constexpr FieldSpec equity[] = {
    {"time_ms","i64","unix_ms"}, {"equity","f64?","account_ccy"}, {"open_profit","f64?","account_ccy"}};
inline constexpr FieldSpec trade[] = {
    {"n","i64",""}, {"side","str",""}, {"entry_time","i64","unix_ms"}, {"exit_time","i64","unix_ms"},
    {"entry_price","f64?","quote_ccy"}, {"exit_price","f64?","quote_ccy"}, {"qty","f64?","qty"},
    {"pnl","f64?","account_ccy"}, {"pnl_pct","f64?","percent"}, {"max_runup","f64?","account_ccy"},
    {"max_drawdown","f64?","account_ccy"}, {"commission","f64?","account_ccy"},
    {"entry_bar_index","i64","script_bar"}, {"exit_bar_index","i64","script_bar"},
    {"open_at_end","bool",""}, {"entry_incarnation","u64",""}, {"close_cause","i64",""},
    {"entry_id","str",""}, {"exit_id","str",""}, {"exit_comment","str",""}};
inline constexpr FieldSpec output[] = {
    {"row","i64",""}, {"open_ms","i64?","unix_ms"}, {"close_ms","i64?","unix_ms"}};
inline constexpr FieldSpec event[] = {
    {"sequence","u64",""}, {"output_index","i64",""}, {"bar_index","i64",""},
    {"bar_open_ms","i64?","unix_ms"}, {"bar_close_ms","i64?","unix_ms"}, {"ordinal_in_bar","i64",""},
    {"phase","i64",""}, {"confirmed","i64",""}, {"value","f64?",""}, {"message_hash64","hex64",""},
    {"message","str?",""}};
inline constexpr FieldSpec trace[] = {
    {"timestamp","i64","unix_ms"}, {"bar_index","i64",""}, {"name","str",""}, {"value","f64?",""}};
inline constexpr FieldSpec hash[] = {{"hash","hex64",""}};
inline constexpr std::array<int, 8> a_column{{0,2,1,3,-1,0,-1,-1}};
inline constexpr std::array<int, 8> b_column{{-1,3,2,4,-1,-1,-1,-1}};
inline constexpr std::array<unsigned, 5> rank_columns{{3,7,8,9,10}};
inline constexpr std::array<std::string_view, 5> rank_keys{{"exit","pnl","pct","ru","dd"}};
} // namespace layout

using Bytes = std::vector<std::uint8_t>;
using Digest = std::array<std::uint8_t, layout::digest>;

enum class VerificationScope { columns, block, whole_file, cancelled, unverified, structure };
// File-level state at the time of the access, separate from fetched-column
// integrity. No implicit conversion discards the state. Reference values keep
// the same lifetime as the reader (index pages retain their cache lifetime).
template<class T> struct ReadResult {
    T value;
    VerificationScope scope = VerificationScope::unverified;
};

enum class Failure { malformed, integrity, unsupported_codec, source_changed, io, cancelled, report_result_invalid, unverified, summary_limit };
class ReaderError final : public std::runtime_error {
public:
    ReaderError(Failure failure, const std::string& message);
    Failure failure() const noexcept { return failure_; }
    std::thread::id origin_thread() const noexcept { return origin_thread_; }
private:
    Failure failure_;
    std::thread::id origin_thread_ = std::this_thread::get_id(); // diagnostic, never serialized
};

// Implementations MUST bind every range to this immutable generation (e.g.
// HTTP If-Match), return exact stored bytes, and propagate I/O failure.
// Calls are synchronous: completion is the backpressure/await boundary.
class RandomAccessSource {
public:
    virtual ~RandomAccessSource() = default;
    virtual std::uint64_t size() const = 0;
    virtual std::string generation() const = 0;
    // Explicit opt-in for trusted adapters with immutable concurrent ranges.
    // Generic/remote adapters and cancellation callbacks stay serial.
    virtual bool concurrent_reads() const noexcept { return false; }
    virtual Bytes readRange(std::uint64_t offset, std::size_t length) = 0;
    // Remote adapters override this overload to abort an in-flight request.
    // The default brackets a local synchronous read with cancellation checks.
    virtual Bytes readRange(std::uint64_t offset, std::size_t length,
                            const std::function<bool()>& cancelled);
};

// Private completed staging inode, retained by the publication owner. Borrows
// fd for this source's lifetime; pread never changes its shared file offset.
class FdSource final : public RandomAccessSource {
    int fd_;
    std::uint64_t size_;
public:
    explicit FdSource(int fd);
    std::uint64_t size() const override { return size_; }
    std::string generation() const override { return "private-transcoder-staging"; }
    bool concurrent_reads() const noexcept override { return true; }
    Bytes readRange(std::uint64_t offset, std::size_t length) override;
    void read_into(std::uint64_t offset, std::size_t length, Bytes&);
};

enum class Kind { i64, u64, f64, boolean, string, hex64, order };
struct Field {
    std::string name, type, unit;
    Kind kind = Kind::i64;
    bool nullable = false;
};
struct Schema {
    char tag = 0;
    bool present = false;
    std::vector<Field> fields;
    std::uint32_t block_records = 0;
};
struct Head {
    std::string schema, run_id, engine;
    unsigned version = layout::version;
    int level = 3;
    std::uint32_t block_bytes = 0;
    std::array<Schema, layout::stream_count> records; // slot 7 is internal I, never a v1 descriptor
    std::string json, v1_header, summary; // decoded canonical summary bytes, no synthesized descriptor
};

// Decoded scalar words preserve signed-zero/full hex64 bits. Integer values
// use two's complement words; strings are owned only when a row is requested.
struct Value {
    Kind kind = Kind::i64;
    bool null = true;
    std::uint64_t word = 0;
    std::string text;
    std::int64_t as_i64() const;
    double as_f64() const;
};
struct Record {
    char tag = 0;
    std::uint64_t ordinal = 0;
    std::vector<Value> values;
    VerificationScope file_scope = VerificationScope::unverified;
};
std::string record_json(const Record& record); // canonical v1 line including LF

struct EquityWitness {
    std::uint64_t ordinal = 0;
    std::int64_t time_ms = 0;
    std::uint64_t mtm_bits = 0, book_bits = 0;
    std::uint32_t valid_mask = 0;
};
struct SeriesSummary {
    std::uint64_t finite_count = 0;
    // ID18 does not store the number of finite book values. In that case
    // this is false and finite_count is zero; flags/witnesses remain exact.
    bool finite_count_known = true;
    std::uint32_t flags = 0;
    std::array<EquityWitness, 4> witnesses{}; // first, earliest min, earliest max, last
};
struct BlockEntry {
    std::uint8_t table = 0, flags = 0;
    char tag = 0;
    std::uint64_t ordinal = 0, offset = 0, rec0 = 0, finite_prefix = 0;
    std::uint32_t length = 0, n = 0, decoded_bytes = 0;
    std::int64_t a_min = 0, a_max = 0, b_min = 0, b_max = 0;
    Digest sha{};
    SeriesSummary mtm, book;
    VerificationScope file_scope = VerificationScope::unverified;
};
struct TableLocator {
    std::uint64_t offset = 0, entry_count = 0;
    std::uint32_t page_count = 0, entry_width = 0, last_page_bytes = 0, flags = 0;
    std::uint64_t byte_length() const;
    Digest merkle_root{};
};
struct IndexPage {
    std::uint8_t table = 0;
    std::uint32_t number = 0, used_entries = 0, entry_width = 0;
    Bytes bytes;
};

struct StringRef { std::uint32_t offset = 0, length = 0; };
struct Column {
    std::uint32_t id = 0;
    Kind kind = Kind::i64;
    bool nullable = false;
    Bytes nulls;
    std::vector<std::uint64_t> words;
    std::vector<StringRef> strings;
    std::string arena; // dictionary strings are stored once, never expanded per row
    bool is_null(std::size_t row) const;
    Value value(std::size_t row) const;
};
enum class Profile { full, compact };
struct SummaryFrameInfo {
    // Published only after open checks 52+C, J, the exact object range and
    // the frame's content size/window (5.2), then decoded CRC/SHA/schema.
    std::array<std::uint8_t, 4> magic{};
    std::uint32_t stored_len = 0, json_len = 0, json_crc = 0, flags = 0;
    Digest json_sha256{}; // decoded SUMMARY JSON, never the whole-file logical identity
};
struct MemoryStats {
    std::size_t live_index_pages = 0, peak_index_pages = 0;
    std::size_t live_decoded_blocks = 0, peak_decoded_blocks = 0;
    std::size_t live_decoded_bytes = 0, peak_decoded_bytes = 0;
    std::size_t peak_column_scratch_bytes = 0;
    std::size_t peak_rank_scratch_bytes = 0; // FULL <=64 KiB; COMPACT <=25*T bytes
    std::size_t compact_descriptor_bytes = 0; // replaces the FULL page cache, <=12,288
};
struct BufferLease;
struct DecodedBlock {
    BlockEntry entry;
    VerificationScope scope = VerificationScope::columns;
    VerificationScope file_scope = VerificationScope::unverified;
    std::vector<std::uint32_t> verified_columns; // ids whose decoded CRC AND SHA passed
    std::vector<Column> columns; // requested ids only, in requested order
    Record row(std::uint32_t local) const; // full blocks use schema order; subsets use requested order
    DecodedBlock();
    ~DecodedBlock();
    DecodedBlock(DecodedBlock&&) noexcept;
    DecodedBlock& operator=(DecodedBlock&&) noexcept;
    DecodedBlock(const DecodedBlock&) = delete;
    DecodedBlock& operator=(const DecodedBlock&) = delete;
private:
    friend struct ReaderState;
    std::unique_ptr<BufferLease> lease_;
};

struct ReaderOptions {
    std::string locator; // used only in the synthesized report_sink.path
    std::function<bool()> cancelled; // polled at range, column, row and page boundaries
    bool trusted_compact = false; // publication hint permits one 480-byte suffix request
    std::uint32_t scratch_flags = 0; // caller's local receipt diagnostic, never hashed
};
struct Verification {
    VerificationScope scope = VerificationScope::cancelled;
    Digest logical_sha{};
    std::uint64_t blocks = 0;
};
struct Conversion {
    VerificationScope scope = VerificationScope::cancelled;
    std::string v1_sha256; // excludes the v1 trailer
    std::uint64_t bytes = 0; // complete emitted bytes including trailer
};
struct ReportConversion {
    VerificationScope scope = VerificationScope::cancelled;
    std::string sha256; // complete report-object JSON INCLUDING its terminal LF
    std::uint64_t bytes = 0;
};
struct TradeRankPage {
    std::uint64_t total = 0, page_count = 1, effective_page = 1, from = 0, to = 0;
    std::vector<Record> rows; // at most caller-requested P rows
    // Root-derived totals/bounds are provisional until whole-file verification.
    VerificationScope scope = VerificationScope::unverified;
};
struct EquityBucket {
    std::uint64_t begin = 0, end = 0; // finite-equity ordinals, [begin,end); witnesses keep RAW ordinals
    SeriesSummary mtm, book;
    std::vector<EquityWitness> points; // sorted union, deduplicated by ordinal
};
struct EquityEnvelope {
    std::uint64_t population = 0;
    std::vector<EquityBucket> buckets; // at most requested B, at most 6 points per bucket
    VerificationScope scope = VerificationScope::unverified;
};
struct SinkFolds {
    std::uint64_t closed_count = 0, open_at_end_count = 0;
    double open_at_end_pnl = 0.0;
};

// Private, in-process evidence from decoded, input-compared blocks. Never
// deserialize this from a receipt or accept it from a publication client.
struct IncrementalProof {
    std::array<std::uint64_t, layout::stream_count> counts{}, blocks{}, multiplicities{};
    std::array<Digest, layout::stream_count> streams{}, descriptors{};
    Digest full_e_descriptors{}, stored_data{};
    std::uint64_t data_start = 0, data_end = 0, finite_e_count = 0;
    SinkFolds folds;
    bool complete = false;
};

class IncrementalValidator final : public BlockObserver {
public:
    explicit IncrementalValidator(const BlockPolicy&, std::string_view input_header);
    ~IncrementalValidator() override;
    void input_line(std::string_view); // borrowed until the next synchronous write returns
    void admitted(char tag) override;
    void block_written(int fd, std::uint64_t offset, std::uint32_t length,
                       const std::array<std::string_view, 4>& encoded) override;
    IncrementalProof finish(int fd, const std::array<std::uint64_t, 7>& input_counts);
private:
    struct State;
    std::unique_ptr<State> state_;
};
struct ReaderState;

class Reader {
public:
    // Reader and its cursors share a serial cache. Multiple live cursors are
    // supported; callers serialize their calls and budget one block apiece.
    class Cursor {
    public:
        Cursor();
        ~Cursor();
        Cursor(Cursor&&) noexcept;
        Cursor& operator=(Cursor&&) noexcept;
        Cursor(const Cursor&) = delete;
        Cursor& operator=(const Cursor&) = delete;
        VerificationScope file_scope() const; // also available for an empty cursor
        bool next(Record& record); // record.file_scope accompanies fetched-block integrity
    private:
        friend class Reader;
        struct Impl;
        explicit Cursor(std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
    };
    class MergedCursor {
    public:
        MergedCursor();
        ~MergedCursor();
        MergedCursor(MergedCursor&&) noexcept;
        MergedCursor& operator=(MergedCursor&&) noexcept;
        MergedCursor(const MergedCursor&) = delete;
        MergedCursor& operator=(const MergedCursor&) = delete;
        VerificationScope file_scope() const;
        bool next(Record& record);
    private:
        friend class Reader;
        struct Impl;
        explicit MergedCursor(std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
    };
    class BlockCursor {
    public:
        VerificationScope file_scope() const;
        bool next(BlockEntry& entry);
    private:
        friend class Reader;
        std::shared_ptr<ReaderState> state_;
        std::uint8_t table_ = 0;
        char key_ = 'A';
        std::uint64_t next_ = 0, end_ = 0;
        std::int64_t t0_ = 0, t1_ = 0;
    };

    static Reader open(std::shared_ptr<RandomAccessSource>, ReaderOptions = {});
    // SUMMARY always requires the decoder, even when all columns are stored.
    static bool has_zstd() noexcept;
    // Sections 9.6/11.2: every access carries file-level UNVERIFIED until
    // verify() completes for this generation, in BOTH profiles. FULL open
    // checks only tail/root/HEAD/SUMMARY consistency; its root stream digests
    // are not authenticated against records until verify(). HEAD format is
    // CRC-only in mandatory checks (9.5). SHA integrity still needs a trusted
    // publication. Failure/cancellation clears whole-file state, also on retry.
    // ReadResult.scope, entry/block/record.file_scope and cursor.file_scope()
    // carry this state; DecodedBlock.scope/verified_columns retain the separate
    // per-read block/column integrity of 9.6 and never promote the file.
    ReadResult<const Head&> head() const;
    ReadResult<std::string> summary() const; // metadata plus synthesized report_sink, section 14.5
    ReadResult<const std::array<std::uint64_t, layout::stream_count>&> counts() const;
    // FULL physical locators. COMPACT slots 0..7 expose only normalized
    // descriptor offset/count/width; page/PRESENT/digest fields and 8..18
    // remain zero. descriptor() is the common authenticated access path.
    ReadResult<const std::array<TableLocator, layout::table_count>&> tables() const;
    ReadResult<Profile> profile() const;
    ReadResult<std::uint64_t> finite_equity_count() const;
    ReadResult<const SinkFolds&> sink_folds() const;
    ReadResult<const std::string&> generation() const;
    ReadResult<Digest> logical_sha256() const;
    ReadResult<bool> verified() const;
    ReadResult<MemoryStats> memory_stats() const;
    // Section 9.1's decoded/validated SUM2 header, including J and stored C.
    // Available after open in both profiles without verify() or further reads;
    // this authenticates no decoded E/T/other record stream or logical identity.
    ReadResult<SummaryFrameInfo> summary_frame_info() const;
    // FULL physical pages only; COMPACT uses descriptor(), never synthetic pages.
    // Borrowed until a later page miss; copy descriptors/positions, not pages.
    ReadResult<const IndexPage&> index_page(std::uint8_t table, std::uint32_t page);
    BlockEntry descriptor(std::uint8_t table, std::uint64_t ordinal);
    DecodedBlock read_columns(const BlockEntry&, const std::vector<std::uint32_t>& ids);
    DecodedBlock read_block(const BlockEntry&);
    Cursor iter_type(char tag);
    MergedCursor iter_merged();
    BlockCursor blocks_for_time_range(char tag, char key, std::int64_t t0, std::int64_t t1);
    Cursor trade_slice(std::uint64_t first, std::uint64_t last); // inclusive, 1-based
    // Half-open finite interval; raw rows by default. The optional APP helper
    // changes only the global final finite row's effective open_profit.
    Cursor equity_slice(std::uint64_t begin, std::uint64_t end, bool adjust_final = false);
    // dd compares -abs(wire value); null-last in both directions, ties ascending.
    TradeRankPage trade_rank_page(std::string_view key, bool descending,
                                 std::int64_t page, std::uint64_t size);
    // FULL: binary search the stored null-last boundary; COMPACT: one key scan.
    // The population carries file-level state even when its key column passed.
    ReadResult<std::uint64_t> trade_non_null_count(std::string_view key, bool descending = false);
    // Default full-run B=1200 returns the adjusted figure (FULL ID18, zero E
    // decode; COMPACT one E scan). Other queries use the optional APP helper.
    // FULL composes base witnesses; COMPACT keeps one active bucket/block.
    EquityEnvelope equity_summary(std::uint64_t buckets,
        std::optional<std::array<std::int64_t, 2>> time_range = std::nullopt,
        bool adjust_final = false);
    // Callback records are in physical block order and provisional until a
    // whole_file result. false cancels; iter_merged supplies the v1 order.
    Verification verify(const std::function<bool(const Record&)>& callback = {});
    // Trusted transcoder only. Does not set verified()/whole_file state.
    Verification verify_structure(const IncrementalProof&, unsigned threads = 1);
    // Fully verifies before emitting; trailer only after successful same-generation conversion.
    Conversion to_v1(const std::function<bool(std::string_view)>& output,
                     pineforge::report_sink::PhaseLog* phase_log = nullptr);
    // Section 21.6 report object, distinct from sink JSONL. Fully verify and
    // count/hash before emitting, then repeat under the same generation.
    ReportConversion to_report_json(const std::function<bool(std::string_view)>& output);
    std::string content_sha256();
private:
    Verification verify_impl(const std::function<bool(const Record&)>&, const IncrementalProof*,
                             unsigned threads = 1);
    explicit Reader(std::shared_ptr<ReaderState> state);
    std::shared_ptr<ReaderState> state_;
};

} // namespace v2
} // namespace report_sink
} // namespace pineforge
