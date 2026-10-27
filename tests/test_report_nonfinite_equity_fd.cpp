// Test-only ctypes bridge to the production sink writer.
// Python's existing sink doubles format their own records; they cannot
// exercise LineWriter::write_e. This module formats no JSON and classifies
// only exceptions actually thrown by production code.
#include <pineforge/report_sink_writer.hpp>
#include <pineforge/run_failure.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
thread_local std::string error_text;
thread_local std::string error_code;
thread_local std::string error_args;
} // namespace

extern "C" PF_API const char* cr289_writer_error_text() {
    return error_text.c_str();
}

extern "C" PF_API const char* cr289_writer_error_code() {
    return error_code.c_str();
}

extern "C" PF_API const char* cr289_writer_error_args() {
    return error_args.c_str();
}

extern "C" PF_API int cr289_write_equity(
        const pf_report_sink_v1* path,
        const pf_report_sink_fd_v1* fd,
        const pf_equity_point_t* points, std::int64_t count,
        pf_report_sink_receipt_v1* receipt) {
    error_text.clear();
    error_code.clear();
    error_args.clear();
    if (receipt != nullptr) {
        *receipt = {};
        receipt->struct_size = sizeof(*receipt);
        receipt->version = 1;
    }
    try {
        if ((path == nullptr) == (fd == nullptr)
                || points == nullptr || count < 2 || receipt == nullptr)
            throw std::invalid_argument("invalid writer fixture arguments");
        std::unique_ptr<pineforge::report_sink::LineWriter> writer;
        if (fd != nullptr)
            writer = std::make_unique<pineforge::report_sink::LineWriter>(*fd);
        else
            writer = std::make_unique<pineforge::report_sink::LineWriter>(*path);
        writer->write_header({false, false, false, 0, 0});
        for (std::int64_t i = 0; i < count; ++i)
            writer->write_e(points[i]); // Read the actual native allocation.
        const auto trailer = writer->write_trailer();
        receipt->count_e = trailer.count_e;
        receipt->count_t = trailer.count_t;
        receipt->count_o = trailer.count_o;
        receipt->count_v = trailer.count_v;
        receipt->count_k = trailer.count_k;
        receipt->count_x = trailer.count_x;
        receipt->count_h = trailer.count_h;
        std::memcpy(receipt->sha256_hex, trailer.sha256_hex, sizeof(receipt->sha256_hex));
        receipt->complete = 1;
        return 0;
    } catch (const std::exception& error) {
        const auto failure = pineforge::classify_run_failure(error);
        error_text = error.what();
        error_code = pineforge::run_failure_code_name(failure.code);
        error_args = failure.args ? *failure.args : "{}";
        return -1;
    }
}
