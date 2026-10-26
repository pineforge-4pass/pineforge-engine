#pragma once

#include <pineforge/report_sink_writer.hpp>
#include "report_sink_v2_types.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <optional>
#include <string_view>

namespace pineforge { namespace report_sink {

class PhaseLog;

// Canonical v1 header without LF; keep the descriptor table in one place.
std::string v1_header_json(const std::string& run_id, const RecordSet& records);

/// Storage interface only: engine report ordering remains at the call sites.
class SinkWriter {
public:
    virtual ~SinkWriter() = default;
    virtual void write_header(const RecordSet&) = 0;
    virtual void write_e(const pf_equity_point_t&) = 0;
    virtual void write_t(std::int64_t, const pf_trade_t&, std::uint64_t, std::int64_t,
                         std::string_view, std::string_view, std::string_view) = 0;
    virtual void write_o(std::int64_t, std::int64_t, std::int64_t, const double*, std::int64_t) = 0;
    virtual void write_v(const pf_output_event_v1_t&, std::string_view) = 0;
    virtual void write_k(const double*, std::int64_t) = 0;
    virtual void write_x(const pf_trace_entry_t&, std::string_view) = 0;
    virtual void write_h(std::uint64_t) = 0;
    virtual Trailer write_trailer() = 0;
    virtual unsigned format() const noexcept { return 1; }
    virtual bool accepts_records() const noexcept { return true; }
    // Only v2 needs configuration between runs. Copies contain no live run.
    virtual std::unique_ptr<SinkWriter> fresh_configuration() const { return {}; }
    virtual void abort() noexcept {}
};

std::unique_ptr<SinkWriter> copy_sink_configuration(const SinkWriter*);
// Runtime availability; codec implementation remains in its own source file.
bool sink_v2_available() noexcept;

/// Preserve LineWriter's published one-pointer C++ layout. Introducing a
/// virtual base on that existing class would break old callers that allocate
/// it by value. Only this new adapter has a vtable.
class LineSinkWriter final : public SinkWriter {
    LineWriter writer_;
public:
    explicit LineSinkWriter(const pf_report_sink_v1& sink) : writer_(sink) {}
    void write_header(const RecordSet& records) override { writer_.write_header(records); }
    void write_e(const pf_equity_point_t& point) override { writer_.write_e(point); }
    void write_t(std::int64_t n, const pf_trade_t& trade, std::uint64_t incarnation,
                 std::int64_t cause, std::string_view entry, std::string_view exit,
                 std::string_view comment) override {
        writer_.write_t(n, trade, incarnation, cause, entry, exit, comment);
    }
    void write_o(std::int64_t row, std::int64_t open, std::int64_t close,
                 const double* values, std::int64_t count) override {
        writer_.write_o(row, open, close, values, count);
    }
    void write_v(const pf_output_event_v1_t& event, std::string_view message) override {
        writer_.write_v(event, message);
    }
    void write_k(const double* values, std::int64_t count) override { writer_.write_k(values, count); }
    void write_x(const pf_trace_entry_t& entry, std::string_view name) override { writer_.write_x(entry, name); }
    void write_h(std::uint64_t hash) override { writer_.write_h(hash); }
    Trailer write_trailer() override { return writer_.write_trailer(); }
};


// Trusted transcoder observer. Input snapshots belong to the consumer, never
// to the encoder. Inline only: callbacks complete before buffers are reused.
class BlockObserver {
public:
    virtual ~BlockObserver() = default;
    virtual void admitted(char tag) = 0;
    virtual void block_written(int fd, std::uint64_t offset, std::uint32_t length,
                              const std::array<std::string_view, 4>& encoded) = 0;
};

struct BlockPolicy {
    // Physical output high-water cap, including buffered bytes, SUMMARY,
    // footer reservation and tail. A breach aborts; it never clips a write.
    std::optional<std::uint64_t> max_output_bytes;
    int staging_fd = -1; // trusted tool's still-open mkstemp inode; borrowed
    int zstd_level = 3;
    bool threaded = true;
    unsigned queue_depth = 4;
    unsigned late_threads = 1; // trusted C++/tool only, 1..4; caller included
    unsigned block_records[8] = {4096, 1024, 4096, 4096, 1, 4096, 8192, 65536};
    std::uint32_t block_bytes = 1u << 20;
    unsigned min_zstd_raw = 64;
    // C++ conformance fixtures may exercise the FULL layout below thresholds.
    // The C ABI always uses the canonical final-count decision.
    bool force_full = false;
    static constexpr std::size_t max_block_bytes = 32u << 20;
    static constexpr std::size_t max_column_bytes = 16u << 20;
    static constexpr unsigned max_queue_depth = 16;
    PhaseLog* phase_log = nullptr; // trusted-tool finish phases only; borrowed
    BlockObserver* observer = nullptr; // borrowed; no engine/C ABI configuration
};

// Diagnostic counters are independent of record count; no retained history.
struct BlockMemory {
    std::size_t peak_blocks = 0;
    std::size_t block_limit = 0;
    std::size_t peak_builder_budget = 0;
    std::size_t codec_workspace = 0;
    std::uint64_t queue_waits = 0;
    std::size_t finalization_bytes = 0; // counted fixed state, including stack allowance
    std::size_t work_file_bytes = 0;    // this implementation uses no work files
    std::size_t decoder_bytes = 0;      // aggregate scratch; bounded two-Umax per decode lane
    static constexpr std::size_t finalization_allowance = 1836032;
};

// Successful finalization sizes for reports, without reopening the file.
// These mirror SUM2's C/J fields; they do not extend the C receipt or wire.
struct SummarySize {
    std::uint32_t stored_bytes = 0; // C: one frame, excluding the 52-byte header
    std::uint32_t json_bytes = 0;   // J: exact decoded JSON, without LF
};

class ColumnCodec;

class BlockWriter final : public SinkWriter {
public:
    explicit BlockWriter(const pf_report_sink_v2&, const BlockPolicy& = {});
    // Codec injection is internal C++ test/tool support, never a C ABI.
    BlockWriter(const pf_report_sink_v2&, const BlockPolicy&, std::unique_ptr<ColumnCodec>);
    ~BlockWriter() override;
    void write_header(const RecordSet&) override;
    void write_e(const pf_equity_point_t&) override;
    void write_t(std::int64_t, const pf_trade_t&, std::uint64_t, std::int64_t,
                 std::string_view, std::string_view, std::string_view) override;
    void write_o(std::int64_t, std::int64_t, std::int64_t, const double*, std::int64_t) override;
    void write_v(const pf_output_event_v1_t&, std::string_view) override;
    void write_k(const double*, std::int64_t) override;
    void write_x(const pf_trace_entry_t&, std::string_view) override;
    void write_h(std::uint64_t) override;
    Trailer write_trailer() override; // data completion only; no tail
    unsigned format() const noexcept override { return 2; }
    bool accepts_records() const noexcept override;
    std::unique_ptr<SinkWriter> fresh_configuration() const override;
    void abort() noexcept override;
    // Pair the result with receipt() for file bytes and RAW E/T/O/V/K/X/H.
    // Actual run bars B come from the caller, never from the raw E count.
    SummarySize finalize(std::string_view summary_json);
    pf_report_sink_receipt_v2 receipt() const noexcept;
    BlockMemory memory() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}} // namespace pineforge::report_sink
