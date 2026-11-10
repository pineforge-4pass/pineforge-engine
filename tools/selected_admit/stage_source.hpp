#pragma once

#include "stage_selected.hpp"
#include "../../src/selected_capacity_source_stage.hpp"

#include <array>

namespace pineforge::selected_admit {
inline sc::DriverReason source_driver_reason(sc::SourceStageReason reason) {
    switch (reason) {
    case sc::SourceStageReason::None: return sc::DriverReason::None;
    case sc::SourceStageReason::NoStageFields: return sc::DriverReason::NoRequestedFields;
    case sc::SourceStageReason::MissingInput: return sc::DriverReason::MissingInput;
    case sc::SourceStageReason::RowsUnordered: return sc::DriverReason::RowsUnordered;
    case sc::SourceStageReason::CsvColumns: return sc::DriverReason::CsvColumns;
    case sc::SourceStageReason::CsvValue: return sc::DriverReason::CsvValue;
    case sc::SourceStageReason::CsvUtf8: return sc::DriverReason::CsvUtf8;
    case sc::SourceStageReason::CsvSyntax: return sc::DriverReason::CsvSyntax;
    case sc::SourceStageReason::InputReadFailure:
    case sc::SourceStageReason::RecordTooLong:
    case sc::SourceStageReason::ForwardFailure: return sc::DriverReason::InputReadFailure;
    case sc::SourceStageReason::InternalError: return sc::DriverReason::InternalError;
    }
    return sc::DriverReason::InternalError;
}

inline Outcome execute_stage_source(const Descriptor& d, const Seam& seam = Seam{}) {
    Outcome out;
    out.descriptor = d;
    out.descriptor.stage = Stage::Source;
    out.streams.resize(d.streams.size());
    std::array<std::uint64_t, kMaxStreams> consumed{};
    std::array<bool, kMaxStreams> complete{};
    sc::SourceStageProgress progress;
    progress.bytes_read = consumed.data();
    progress.complete = complete.data();
    progress.slots = d.streams.size();
    struct ReadContext {
        int fd;
        const Seam* seam;
        static std::ptrdiff_t pull(void* ctx, char* out, std::size_t cap) {
            auto& c = *static_cast<ReadContext*>(ctx);
            for (;;) {
                const auto count = call_read(*c.seam, c.fd, out, cap);
                if (count >= 0 && static_cast<std::size_t>(count) <= cap) return count;
                if (count < 0 && errno == EINTR) continue;
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)
                    && wait_ready(c.fd, POLLIN)) continue;
                return -1;
            }
        }
    };
    struct ForwardContext {
        const Descriptor* descriptor;
        const Seam* seam;
        sc::SourceStageProgress* progress;
        std::uint64_t bytes = 0;
        report_sink::Sha256 hash;
        static bool send(void* ctx, const char* data, std::size_t size) {
            auto& c = *static_cast<ForwardContext*>(ctx);
            if (c.progress->input_index >= c.descriptor->streams.size())
                throw std::logic_error("Source forwarding input index is invalid");
            if (size > std::numeric_limits<std::uint64_t>::max() - c.bytes)
                throw coded<std::runtime_error>(RunFailureCode::engine_invariant, {},
                                                 "Pine native run counter exhausted");
            const int fd = c.descriptor->streams[c.progress->input_index].forward_fd;
            if (!write_all(fd, std::string_view(data, size), *c.seam)) return false;
            c.hash.update(std::string_view(data, size));
            c.bytes += size;
            return true;
        }
    } forwarding{&d, &seam, &progress, 0, {}};
    out.forward_sha256 = forwarding.hash.hex_digest();
    try {
        if (d.streams.size() > kMaxStreams)
            throw std::logic_error("Source input count exceeds descriptor cap");
        if (!d.bounds.source_rows.present && !d.bounds.bytes.present) {
            sc::DriverResult inactive;
            inactive.reason = sc::DriverReason::NoRequestedFields;
            native_outcome(out, inactive);
            out.reason = "helper:no_stage_fields";
            return out;
        }
        std::vector<std::array<struct stat, 2>> snapshots(d.streams.size());
        // stdout carries the result document and stderr the human text: a forward_fd that
        // is the same file as either would put the forwarded octets into that stream.
        std::array<struct stat, 2> standard{};
        std::array<bool, 2> standard_known{};
        for (unsigned s = 0; s < 2; ++s)
            standard_known[s] = call_fstat(seam, static_cast<int>(s) + 1, &standard[s]) == 0;
        for (std::size_t i = 0; i < d.streams.size(); ++i) {
            const int fds[] = {d.streams[i].fd, d.streams[i].forward_fd};
            for (unsigned part = 0; part < 2; ++part) {
                auto reason = preflight_pipe(fds[part], part != 0, seam);
                if (reason == CustodyReason::None
                    && call_fstat(seam, fds[part], &snapshots[i][part]) != 0)
                    reason = CustodyReason::FdInvalid;
                if (reason != CustodyReason::None) {
                    out.exit = kCustody;
                    out.status = "input_error";
                    out.reason = custody_reason_name(reason);
                    out.input_index = i;
                    return out;
                }
                for (unsigned s = 0; part == 1 && s < 2; ++s) {
                    if (standard_known[s] && standard[s].st_dev == snapshots[i][part].st_dev
                        && standard[s].st_ino == snapshots[i][part].st_ino) {
                        out.exit = kDescriptor;
                        out.status = "input_error";
                        out.reason = "helper:descriptor_invalid";
                        out.input_index = i;
                        return out;
                    }
                }
                for (std::size_t p = 0; p < i; ++p) {
                    for (unsigned earlier = 0; earlier < 2; ++earlier) {
                        if (snapshots[p][earlier].st_dev == snapshots[i][part].st_dev
                            && snapshots[p][earlier].st_ino == snapshots[i][part].st_ino) {
                            out.exit = kDescriptor;
                            out.status = "input_error";
                            out.reason = "helper:descriptor_invalid";
                            out.input_index = i;
                            return out;
                        }
                    }
                }
            }
        }
        std::vector<ReadContext> readers;
        std::vector<sc::SourceStageInput> inputs;
        readers.reserve(d.streams.size());
        inputs.reserve(d.streams.size());
        for (const auto& spec : d.streams) {
            readers.push_back({spec.fd, &seam});
            inputs.push_back({&ReadContext::pull, &readers.back(), spec.label.c_str()});
        }
        const auto source = sc::admit_source_stage(inputs.data(), inputs.size(), d.bounds,
            {static_cast<std::size_t>(d.max_record_bytes), kStreamChunkBytes, &progress},
            &ForwardContext::send, &forwarding);
        sc::DriverResult result;
        result.status = source.status;
        result.reason = source_driver_reason(source.reason);
        result.input_index = source.input_index;
        result.row_index = source.row_index;
        result.state = source.state;
        result.coverage = source.coverage;
        result.message = source.message;
        native_outcome(out, std::move(result));
        if (source.reason == sc::SourceStageReason::NoStageFields)
            out.reason = "helper:no_stage_fields";
        if (source.reason == sc::SourceStageReason::RecordTooLong)
            out.reason = "helper:record_too_long";
        if (source.reason == sc::SourceStageReason::ForwardFailure)
            out.reason = "helper:forward_write_failed";
        if (source.forwarded_bytes != forwarding.bytes)
            throw std::logic_error("Source forwarded-byte counters disagree");
    } catch (const std::exception& error) {
        exception_outcome(out, error);
        out.input_index = progress.input_index;
        out.row_index = progress.row_index;
    } catch (...) {
        out.exit = kInternal;
        out.status = "internal_error";
        out.reason = "helper:internal_error";
        out.input_index = progress.input_index;
        out.row_index = progress.row_index;
    }
    out.forwarded_bytes = forwarding.bytes;
    out.forward_sha256 = forwarding.hash.hex_digest();
    out.custody_complete = !d.streams.empty() && out.exit == kAdmitted;
    for (std::size_t i = 0; i < d.streams.size() && i < kMaxStreams; ++i) {
        out.streams[i] = {consumed[i], complete[i]};
        out.custody_complete = out.custody_complete && complete[i];
    }
    return out;
}

inline int run_stage_source(const Descriptor& d, int out_fd, const Seam& seam = Seam{}) {
    return emit_result(execute_stage_source(d, seam), out_fd, seam);
}
}  // namespace pineforge::selected_admit
