// Trusted finalizer delegate; never run inside the strategy child's file jail.
#include "sink_v2/report_sink_transcode_v2.hpp"
#include <pineforge/report_sink_writer.hpp>
#include "sink_v2/report_sink_v2_codec.hpp"
#include "sink_v2/report_sink_v2_bounds.hpp"
#include "sink_v2/report_sink_v2_support.hpp"
#include "sink_v2/report_sink_phase_log.hpp"
#include <pineforge/version.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace {
namespace sink = pineforge::report_sink;
constexpr const char* usage =
    "pf_sink_transcode_v2 [--in-fd N] --out PATH (--summary PATH|fd:K | --summary-fd K)\n"
    "  [--run-id ID] [--spool-dir DIR] [--max-bytes B] [--level 1..19] [--receipt-fd M]\n"
    "  [--spool-max-bytes B] [--spool-max-files N] [--block-records E=N,T=N,...]\n"
    "  [--expect-v1-sha256 HEX --expect-v1-bytes N --expect-v1-counts E,T,O,V,K,X,H]\n"
    "  [--phase-log FD]  CLOCK_MONOTONIC JSONL; fd differs from input/receipt/stderr\n"
    "  [--late-verify full|structure]  default full; structure requires incremental proof\n"
    "  [--late-threads 1..4]  default 4; 1 runs independent late work serially\n"
    "  The tee driver defaults to structure/4; --tee-late-verify full opts into full verify/inverse.\n"
    "  One-strategy 1M-bar probe, four clean builds, wall/v1-STDOUT: structure/4 1.073x,\n"
    "  structure/1 1.118x, full 1.337x (about 1.34x). The 1.10 limit held at default 4;\n"
    "  a 1-core host misses it. Probe numbers only; replace with the gate box's measurements.\n"
    "  [--staging-fd N --publish none]  owner-provided inode; ready, never publish\n"
    "  Expectations: all three or none, no repeats; lowercase SHA-256, safe integers.\n"
    "Admission calculator: --bounds --input-bytes B --process-reserve B\n"
    "  --count-e N --count-t N --count-o N --count-v N --count-k N --count-x N --count-h N\n"
    "  [--series-slots N] [--constant-count N] [--outputs 0|1] [--trace 0|1] [--hashes 0|1]\n"
    "Sequential canonical v1 NDJSON -> verified v2; inherited input defaults to fd 0.\n"
    "SUMMARY is the same run's compact stdout JSON (optional terminal LF).\n"
    "Spool defaults to PATH's parent, must share its filesystem; existing PATH refuses.\n"
    "Receipt: one JSON line on stdout, or --receipt-fd; failures have complete=false\n"
    "and input_bytes read (including read-ahead); diagnostics are on stderr.\n"
    "Exit: 0 complete; 2 usage/codec unavailable; 3 malformed stream/metadata/run id;\n"
    "      4 verification/summary_limit; 5 I/O; 6 no trailer; 7 internal;\n"
    "      8 input cap; 9 spool bytes/per-file fsize; 10 spool file-count cap.\n";

std::uint64_t number(std::string_view text) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || text.empty())
        throw sink::TranscodeError(2, "expected a nonnegative decimal integer");
    return value;
}
// Discover only the opt-in diagnostic fd at main entry, before opening SUMMARY
// or consuming input. Ordinary parsing below still validates every option.
int phase_fd(int argc, char** argv) noexcept {
    int result = -1, input = STDIN_FILENO, summary = -1, receipt = STDOUT_FILENO, staging = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (flag == "--bounds" || flag == "--help" || flag == "-h" || flag == "--version") continue;
        if (++i == argc) break;
        auto value = std::string_view(argv[i]);
        if (flag == "--summary" && value.substr(0, 3) == "fd:") value.remove_prefix(3);
        else if (flag != "--phase-log" && flag != "--in-fd" && flag != "--summary-fd"
                 && flag != "--receipt-fd" && flag != "--staging-fd") continue;
        int fd = -1;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), fd);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || fd < 0) {
            if (flag == "--phase-log") return -1;
            continue; // the ordinary parser supplies the usage diagnostic
        }
        if (flag == "--phase-log") {
            if (result >= 0) return -1;
            result = fd;
        } else if (flag == "--in-fd") input = fd;
        else if (flag == "--staging-fd") staging = fd;
        else if (flag == "--receipt-fd") receipt = fd;
        else summary = fd;
    }
    // Do not contaminate an input, receipt or stderr before usage validation.
    return result == input || result == summary || result == staging
        || result == receipt || result == STDERR_FILENO ? -1 : result;
}
int fd_number(std::string_view text) {
    const auto value = number(text);
    if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        throw sink::TranscodeError(2, "fd is outside int range");
    return static_cast<int>(value);
}
std::uint64_t expectation_number(std::string_view text) {
    const auto value = number(text);
    if (value > UINT64_C(9007199254740991))
        throw sink::TranscodeError(2, "v1 expectation exceeds safe integer range");
    return value;
}
std::array<std::uint64_t, 7> expectation_counts(std::string_view text) {
    std::array<std::uint64_t, 7> counts{};
    for (std::size_t i = 0; i < counts.size(); ++i) {
        const auto end = text.find(',');
        if ((i + 1 == counts.size()) != (end == std::string_view::npos))
            throw sink::TranscodeError(2, "v1 expectations require seven ETOVKXH counts");
        counts[i] = expectation_number(text.substr(0, end));
        if (end != std::string_view::npos) text.remove_prefix(end + 1);
    }
    return counts;
}
void block_caps(std::string_view text, sink::BlockPolicy& policy) {
    unsigned seen = 0;
    while (!text.empty()) {
        const auto end = text.find(',');
        const auto item = text.substr(0, end);
        const auto tag = item.empty() ? std::string_view::npos : std::string_view("ETOVKXHI").find(item[0]);
        if (tag == std::string_view::npos || item.size() < 3 || item[1] != '=' || (seen & (1u << tag)))
            throw sink::TranscodeError(2, "invalid/repeated block cap");
        const auto count = number(item.substr(2));
        if (!count || count > (tag == 7 ? 1048576u : 65536u) || (tag == 4 && count != 1))
            throw sink::TranscodeError(2, "block cap outside admitted range");
        policy.block_records[tag] = static_cast<unsigned>(count);
        seen |= 1u << tag;
        if (end == std::string_view::npos) return;
        text.remove_prefix(end + 1);
        if (text.empty()) throw sink::TranscodeError(2, "empty block cap");
    }
    if (!seen) throw sink::TranscodeError(2, "empty block caps");
}
class OwnedFd {
public:
    int fd = -1;
    ~OwnedFd() { if (fd >= 0) (void)::close(fd); }
};
void write_all(int fd, std::string_view text) {
    while (!text.empty()) {
        const auto n = ::write(fd, text.data(), text.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw sink::TranscodeError(errno == EFBIG ? 9 : 5, "receipt write failed");
        text.remove_prefix(static_cast<std::size_t>(n));
    }
}
std::string receipt(const sink::TranscodeResult& result) {
    const auto& r = result.receipt;
    return "{\"complete\":true,\"count_e\":" + std::to_string(r.count_e)
        + ",\"count_t\":" + std::to_string(r.count_t) + ",\"count_o\":" + std::to_string(r.count_o)
        + ",\"count_v\":" + std::to_string(r.count_v) + ",\"count_k\":" + std::to_string(r.count_k)
        + ",\"count_x\":" + std::to_string(r.count_x) + ",\"count_h\":" + std::to_string(r.count_h)
        + ",\"sink_bytes\":" + std::to_string(r.file_bytes) + ",\"sha256\":\"" + r.sha256_hex
        + "\",\"format\":2,\"transcoder_version\":2,\"input_bytes\":" + std::to_string(result.input_bytes)
        + ",\"closed_count\":" + std::to_string(r.closed_count)
        + ",\"open_at_end_count\":" + std::to_string(r.open_at_end_count)
        + ",\"open_at_end_pnl\":" + sink::f64(r.open_at_end_pnl)
        + ",\"block_count\":" + std::to_string(r.block_count)
        + ",\"scratch_flags\":" + std::to_string(r.scratch_flags)
        + ",\"zstd_build\":" + sink::escaped_string(sink::sink_v2_codec_build())
        + (result.late_verify == sink::TranscodeOptions::LateVerify::structure
            ? ",\"late_verify\":\"structure\"}\n" : "}\n");
}
} // namespace

int main(int argc, char** argv) {
    (void)std::signal(SIGPIPE, SIG_IGN); // a lost receipt reader is a typed I/O failure
    const auto xfsz_handler = std::signal(SIGXFSZ, SIG_IGN);
    sink::PhaseLog phases(phase_fd(argc, argv));
    sink::TranscodeOptions options;
    options.policy.late_threads = 4; // library policies stay serial; CLI opts into bounded late work
    OwnedFd summary;
    int receipt_fd = STDOUT_FILENO;
    bool published = false;
    try {
        if (xfsz_handler == SIG_ERR)
            throw sink::TranscodeError(5, "cannot ignore SIGXFSZ");
        bool has_summary = false;
        bool bounds_mode = false;
        unsigned count_proofs = 0;
        unsigned expectation_fields = 0;
        bool has_phase_log = false;
        bool has_staging = false, has_publish = false;
        bool has_late_verify = false;
        bool has_late_threads = false;
        int requested_phase_fd = -1;
        sink::StageABinding binding;
        std::uint64_t process_reserve = 0;
        sink::TranscodeShape shape;
        for (int i = 1; i < argc; ++i) {
            const std::string_view flag(argv[i]);
            if (flag == "--help" || flag == "-h") { std::cout << usage; phases.status(0); return 0; }
            if (flag == "--bounds") { bounds_mode = true; continue; }
            if (flag == "--version") {
                std::cout << "{\"engine\":" << sink::escaped_string(PINEFORGE_VERSION_STRING)
                    << ",\"transcoder_version\":2,\"zstd_build\":"
                    << sink::escaped_string(sink::sink_v2_codec_build()) << "}\n";
                phases.status(0);
                return 0;
            }
            if (i + 1 == argc) throw sink::TranscodeError(2, "missing option value");
            const std::string_view value(argv[++i]);
            if (flag == "--in-fd") options.in_fd = fd_number(value);
            else if (flag == "--staging-fd") {
                if (has_staging) throw sink::TranscodeError(2, "repeated --staging-fd");
                has_staging = true;
                options.staging_fd = fd_number(value);
            }
            else if (flag == "--publish") {
                if (has_publish || value != "none") throw sink::TranscodeError(2, "--publish requires none, once");
                has_publish = true;
                options.publish_none = true;
            }
            else if (flag == "--phase-log") {
                if (has_phase_log) throw sink::TranscodeError(2, "repeated --phase-log");
                has_phase_log = true;
                requested_phase_fd = fd_number(value);
                options.phase_log = &phases;
            }
            else if (flag == "--late-verify") {
                if (has_late_verify || (value != "full" && value != "structure"))
                    throw sink::TranscodeError(2, "--late-verify requires full or structure, once");
                has_late_verify = true;
                options.late_verify = value == "full" ? sink::TranscodeOptions::LateVerify::full
                                                       : sink::TranscodeOptions::LateVerify::structure;
            }
            else if (flag == "--late-threads") {
                const auto n = number(value);
                if (has_late_threads || n < 1 || n > 4)
                    throw sink::TranscodeError(2, "--late-threads requires 1..4, once");
                has_late_threads = true;
                options.policy.late_threads = static_cast<unsigned>(n); // bounds uses the same configuration
            }
            else if (flag == "--out") options.out = std::string(value);
            else if (flag == "--spool-dir") options.spool_dir = std::string(value);
            else if (flag == "--run-id") options.run_id = std::string(value);
            else if (flag == "--max-bytes") options.max_bytes = number(value);
            else if (flag == "--spool-max-bytes") options.spool_max_bytes = number(value);
            else if (flag == "--spool-max-files") options.spool_max_files = number(value);
            else if (flag == "--block-records") block_caps(value, options.policy);
            else if (flag == "--expect-v1-sha256" || flag == "--expect-v1-bytes"
                     || flag == "--expect-v1-counts") {
                const unsigned field = flag == "--expect-v1-sha256" ? 1u
                    : flag == "--expect-v1-bytes" ? 2u : 4u;
                if (expectation_fields & field)
                    throw sink::TranscodeError(2, "repeated v1 expectation option");
                expectation_fields |= field;
                if (field == 1) {
                    if (value.size() != 64 || value.find_first_not_of("0123456789abcdef") != std::string_view::npos)
                        throw sink::TranscodeError(2, "v1 expectation requires lowercase SHA-256 hex");
                    binding.sha256 = std::string(value);
                } else if (field == 2) binding.sink_bytes = expectation_number(value);
                else binding.counts = expectation_counts(value);
            }
            else if (flag == "--input-bytes") shape.input_bytes = number(value);
            else if (flag == "--process-reserve") process_reserve = number(value);
            else if (flag == "--series-slots" || flag == "--constant-count") {
                const auto n = number(value);
                if (n > 65536) throw sink::TranscodeError(2, "too many descriptor slots");
                (flag == "--series-slots" ? shape.records.series_slots : shape.records.constant_count)
                    = static_cast<std::int64_t>(n);
            } else if (flag == "--outputs" || flag == "--trace" || flag == "--hashes") {
                const auto n = number(value);
                if (n > 1) throw sink::TranscodeError(2, "boolean option requires 0 or 1");
                (flag == "--outputs" ? shape.records.outputs
                    : flag == "--trace" ? shape.records.trace : shape.records.hashes) = n != 0;
            } else if (flag.size() == 9 && flag.substr(0, 8) == "--count-") {
                const auto tag = std::string_view("etovkxh").find(flag.back());
                if (tag == std::string_view::npos || (count_proofs & (1u << tag)))
                    throw sink::TranscodeError(2, "unknown or repeated count certificate");
                shape.counts[tag] = number(value);
                count_proofs |= 1u << tag;
            }
            else if (flag == "--receipt-fd") receipt_fd = fd_number(value);
            else if (flag == "--level") {
                const auto level = number(value);
                if (level < 1 || level > 19) throw sink::TranscodeError(2, "--level must be in 1..19");
                options.level = static_cast<int>(level);
            } else if (flag == "--summary" || flag == "--summary-fd") {
                if (has_summary) throw sink::TranscodeError(2, "only one SUMMARY source is allowed");
                has_summary = true;
                if (flag == "--summary-fd" || value.substr(0, 3) == "fd:") {
                    options.summary_fd = fd_number(flag == "--summary-fd" ? value : value.substr(3));
                } else {
                    const std::string path(value);
                    summary.fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
                    if (summary.fd < 0) throw sink::TranscodeError(5, "cannot open SUMMARY path");
                    options.summary_fd = summary.fd;
                }
            } else throw sink::TranscodeError(2, "unknown option: " + std::string(flag));
        }
        if (expectation_fields && expectation_fields != 7)
            throw sink::TranscodeError(2, "all three v1 expectation options are required together");
        if (expectation_fields) options.stage_a_binding = std::move(binding);
        if (has_staging != has_publish)
            throw sink::TranscodeError(2, "--staging-fd and --publish none are required together");
        if (has_staging && (options.staging_fd == options.in_fd || options.staging_fd == options.summary_fd
            || options.staging_fd == receipt_fd || options.staging_fd == STDERR_FILENO))
            throw sink::TranscodeError(2, "staging fd must differ from input, summary, receipt and stderr");
        if (has_phase_log && (requested_phase_fd == options.in_fd || requested_phase_fd == options.summary_fd
            || requested_phase_fd == options.staging_fd || requested_phase_fd == receipt_fd || requested_phase_fd == STDERR_FILENO))
            throw sink::TranscodeError(2, "phase log fd must differ from input, receipt and stderr fds");
        if (bounds_mode) {
            if (expectation_fields || has_staging || has_late_verify)
                throw sink::TranscodeError(2, "v1 expectations/staging require conversion, not --bounds");
            if (count_proofs != 127 || !shape.input_bytes || !process_reserve)
                throw sink::TranscodeError(2, "bounds need all seven counts, input bytes and a certified process reserve");
            sink::TranscodeBounds b;
            try {
                b = sink::transcode_bounds(shape, options.level, process_reserve, options.policy);
            } catch (const sink::BlockError& error) {
                throw sink::TranscodeError(2, error.what());
            }
            write_all(receipt_fd, "{\"scope\":\"conditional-address-space-admission\",\"E_as\":"
                + std::to_string(b.E_as) + ",\"buffer_bytes\":" + std::to_string(b.buffer_bytes)
                + ",\"codec_bytes\":" + std::to_string(b.codec_bytes)
                + ",\"spool_bytes\":" + std::to_string(b.spool_bytes)
                + ",\"spool_files\":" + std::to_string(b.spool_files)
                + ",\"block_count_bound\":" + std::to_string(b.block_count)
                + ",\"zstd_build\":" + sink::escaped_string(sink::sink_v2_codec_build()) + "}\n");
            phases.status(0);
            return 0;
        }
        if (count_proofs || shape.input_bytes || process_reserve)
            throw sink::TranscodeError(2, "admission certificates require --bounds");
        if (!has_summary || (!options.publish_none && options.out.empty()))
            throw sink::TranscodeError(2, "--out (or staging fd) and --summary are required");
        if (receipt_fd == options.in_fd || receipt_fd == options.summary_fd)
            throw sink::TranscodeError(2, "receipt fd must differ from input fds");
        const auto result = sink::transcode_v2(options);
        if (options.publish_none) {
            // The library borrows N; this CLI owns its inherited copy. No ready
            // line is emitted until that last C-side staging close is checked.
            const int fd = options.staging_fd;
            options.staging_fd = -1;
            if (::close(fd) < 0) throw sink::TranscodeError(5, "close inherited staging fd", result.input_bytes);
        }
        published = !options.publish_none;
        auto document = receipt(result);
        if (options.publish_none) {
            document.resize(document.size() - 2); // same receipt, plus private readiness binding
            document += ",\"ready\":true,\"input_sha256\":" + sink::escaped_string(result.input_sha256) + "}\n";
        }
        write_all(receipt_fd, document);
        phases.status(0);
        return 0;
    } catch (const sink::TranscodeError& error) {
        phases.status(error.status());
        if (published) (void)::unlink(options.out.c_str());
        if (!published && error.status() != 2 && receipt_fd != options.in_fd && receipt_fd != options.summary_fd) {
            try {
                write_all(receipt_fd, "{\"complete\":false,\"input_bytes\":" + std::to_string(error.input_bytes())
                    + ",\"sink_bytes\":0,\"format\":2,\"transcoder_version\":2,\"status\":"
                    + std::to_string(error.status()) + ",\"code\":\"report_sink_failed\",\"reason\":"
                    + sink::escaped_string(sink::transcode_failure_reason(error.status()))
                    + ",\"diagnostic\":" + sink::escaped_string(error.what())
                    + (options.late_verify == sink::TranscodeOptions::LateVerify::structure
                        ? ",\"late_verify\":\"structure\"}\n" : "}\n"));
            } catch (...) {} // preserve the primary failure if its receipt also cannot be written
        }
        std::cerr << "pf_sink_transcode_v2: " << error.what() << '\n';
        if (error.status() == 2) std::cerr << usage;
        return error.status();
    } catch (const std::exception& error) {
        if (published) (void)::unlink(options.out.c_str());
        std::cerr << "pf_sink_transcode_v2: " << error.what() << '\n';
        return 7;
    }
}
