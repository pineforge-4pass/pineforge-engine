#pragma once

// Internal trusted-tool interface. No C ABI or engine execution dependency.
#include "report_sink_v2_writer.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace pineforge { namespace report_sink {

// Matches pineforge-sink-finalize: 0 success, 2 usage, 3 malformed,
// 4 verification, 5 I/O, 6 missing trailer, 7 internal,
// 8 input cap, 9 spool bytes/per-file fsize, 10 spool file count.
class TranscodeError final : public std::runtime_error {
public:
    TranscodeError(int status, std::string message, std::uint64_t input_bytes = 0)
        : std::runtime_error(std::move(message)), status_(status), input_bytes_(input_bytes) {}
    int status() const noexcept { return status_; }
    std::uint64_t input_bytes() const noexcept { return input_bytes_; }
private:
    int status_;
    std::uint64_t input_bytes_;
};

struct StageABinding {
    std::string sha256;        // stage A's pre-trailer v1 digest, including header/LFs
    std::uint64_t sink_bytes = 0; // complete v1 byte length, including trailer/LF
    std::array<std::uint64_t, 7> counts{}; // ETOVKXH, including zero absent types
};

struct TranscodeOptions {
    enum class LateVerify { full, structure };
    int in_fd = 0;             // borrowed, sequential read only; pipes are supported
    int summary_fd = -1;       // borrowed, same run's compact stdout JSON, optional LF
    int staging_fd = -1;       // borrowed regular inode; requires publish_none
    bool publish_none = false; // owner publishes only after every child succeeds
    std::string out;
    std::string spool_dir;     // empty = parent of out
    std::optional<std::string> run_id; // when set, must equal the stream header
    std::optional<StageABinding> stage_a_binding; // compare with consumed bytes before sealing
    std::optional<std::uint64_t> max_bytes; // input-stream bytes, including trailer
    std::optional<std::uint64_t> spool_max_bytes; // sole output object; no sort spool
    std::optional<std::uint64_t> spool_max_files; // implementation creates exactly one
    BlockPolicy policy{}; // default policy; explicit block caps for conformance/size probes
    int level = 3;
    PhaseLog* phase_log = nullptr; // borrowed opt-in diagnostic observer
    LateVerify late_verify = LateVerify::full;
};

// Closed report_sink_failed.reason vocabulary. Diagnostics stay separate.
const char* transcode_failure_reason(int status) noexcept;

struct TranscodeResult {
    pf_report_sink_receipt_v2 receipt{};
    std::uint64_t input_bytes = 0;
    std::string input_sha256; // independently folded pre-trailer v1 identity
    BlockMemory memory{};
    TranscodeOptions::LateVerify late_verify = TranscodeOptions::LateVerify::full;
};

// Produces an owner-only, fully verified v2 file through no-replace publication.
// Refuses an existing out path; never removes or overwrites a caller's file.
// With staging_fd/publish_none, only verifies/fsyncs/closes its duplicate and
// returns readiness; creates/unlinks no name. FDs remain caller-owned.
// On serial failure removes only its private staging file.
TranscodeResult transcode_v2(const TranscodeOptions&);
// Same codec-injection seam as BlockWriter. Corrupting implementations live
// only in a separate test executable; no production CLI fault switch.
TranscodeResult transcode_v2(const TranscodeOptions&, std::unique_ptr<ColumnCodec>);

}} // namespace pineforge::report_sink
