#pragma once

#include "custody.hpp"
#include "result.hpp"

namespace pineforge::selected_admit {
inline int emit_result(const Outcome& out, int out_fd, const Seam& seam = Seam{}) {
    if (write_all(out_fd, render_result(out), seam)) return out.exit;
    (void)write_all(STDERR_FILENO, "helper:result_write_failed\n");
    return kIo;
}

inline Outcome execute_stage_selected(const Descriptor& d, const Seam& seam = Seam{}) {
    Outcome out;
    out.descriptor = d;
    out.streams.resize(d.streams.size());
    std::vector<std::unique_ptr<FdStreambuf>> buffers;
    std::vector<std::unique_ptr<std::istream>> streams;
    std::vector<sc::StreamInput> inputs;
    std::size_t active = std::numeric_limits<std::size_t>::max();
    try {
        if (!any_requested(d.bounds)) {
            sc::DriverResult result;
            result.reason = sc::DriverReason::NoRequestedFields;
            native_outcome(out, std::move(result));
            return out;
        }
        std::vector<struct stat> snapshots(d.streams.size());
        for (std::size_t i = 0; i < d.streams.size(); ++i) {
            const auto reason = preflight_file(d.streams[i].fd, d.streams[i], seam, &snapshots[i]);
            if (reason != CustodyReason::None) {
                out.exit = kCustody;
                out.status = "input_error";
                out.reason = custody_reason_name(reason);
                out.input_index = i;
                return out;
            }
            for (std::size_t p = 0; p < i; ++p) {
                if (snapshots[p].st_dev == snapshots[i].st_dev
                    && snapshots[p].st_ino == snapshots[i].st_ino
                    && d.streams[p].role == d.streams[i].role) {
                    out.exit = kDescriptor;
                    out.status = "input_error";
                    out.reason = "helper:descriptor_invalid";
                    out.input_index = i;
                    return out;
                }
            }
        }
        buffers.reserve(d.streams.size());
        streams.reserve(d.streams.size());
        inputs.reserve(d.streams.size());
        for (std::size_t i = 0; i < d.streams.size(); ++i) {
            buffers.push_back(std::make_unique<FdStreambuf>(d.streams[i], d.max_record_bytes,
                                                           snapshots[i], seam, &active, i));
            streams.push_back(std::make_unique<std::istream>(buffers.back().get()));
            inputs.emplace_back(d.streams[i].role, streams.back().get(), d.streams[i].label.c_str());
        }
        struct CountingContext {
            CountingState* count;
            const Seam* seam;
            static void accept(void* context, const Bar& bar) {
                auto& c = *static_cast<CountingContext*>(context);
                if (c.seam->on_bar) c.seam->on_bar(c.seam->ctx, c.count->bars);
                if (c.count->bars == std::numeric_limits<std::uint64_t>::max())
                    throw coded<std::runtime_error>(RunFailureCode::engine_invariant, {},
                                                     "Pine native run counter exhausted");
                if (!c.count->has_timestamp) c.count->first_ts_ms = bar.timestamp;
                c.count->last_ts_ms = bar.timestamp;
                c.count->has_timestamp = true;
                ++c.count->bars;
            }
        } counting{&out.sink, &seam};
        native_outcome(out, sc::materialize_selected_capacity(
            inputs.data(), inputs.size(), d.calendar, d.bounds, &CountingContext::accept, &counting));
    } catch (const std::exception& error) {
        exception_outcome(out, error);
        out.input_index = active;
        if (active < buffers.size()) out.row_index = buffers[active]->tracker().row_index();
    } catch (...) {
        out.exit = kInternal;
        out.status = "internal_error";
        out.reason = "helper:internal_error";
        out.input_index = active;
    }
    bool complete = buffers.size() == d.streams.size();
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        const auto& buffer = *buffers[i];
        out.streams[i] = {buffer.bytes_read(), buffer.custody_complete()};
        complete = complete && buffer.custody_complete();
        try { buffer.rethrow_internal(); }
        catch (const std::exception& error) {
            exception_outcome(out, error);
            out.input_index = i;
            complete = false;
            break;
        }
        if (buffer.reason() != CustodyReason::None || buffer.io_failed()) {
            out.input_index = i;
            if (!out.has_native || buffer.reason() == CustodyReason::RecordTooLong)
                out.row_index = buffer.tracker().row_index();
            out.reason = buffer.io_failed() ? "input_read_failure"
                                            : custody_reason_name(buffer.reason());
            out.exit = buffer.io_failed() ? kIo
                : buffer.reason() == CustodyReason::RecordTooLong ? kInput : kCustody;
            out.status = out.exit == kIo ? "io_error" : "input_error";
        }
    }
    out.custody_complete = complete && out.exit != kInternal
        && !(out.exit >= kSourceRows && out.exit <= kBytes);
    return out;
}

inline int run_stage_selected(const Descriptor& d, int out_fd, const Seam& seam = Seam{}) {
    return emit_result(execute_stage_selected(d, seam), out_fd, seam);
}
}  // namespace pineforge::selected_admit
