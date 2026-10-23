// B2-18: production passes false to both SelectedReportStream operations.
// Validation counts final observations; the capture caller owns all E rows,
// including the anchor and the decision to hide a presented suffix.
#include "../src/selected_report_view.hpp"

#include <pineforge/report_sink_writer.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

using pineforge::report::SelectedReportStream;
using pineforge::report_sink::LineWriter;
constexpr std::int64_t kStart = 100;
constexpr std::int64_t kEnd = 200;
constexpr double kCapital = 1000.0;
int checks = 0;
int failures = 0;

#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

// Small borrowed-fd streams avoid filesystem fixtures. Each fixture writes
// only a header, at most two E rows, and a trailer before draining the pipe.
struct Output {
    int descriptors[2]{-1, -1};

    Output() {
        if (::pipe(descriptors) != 0) throw std::runtime_error("pipe failed");
    }
    ~Output() {
        for (const int descriptor : descriptors)
            if (descriptor >= 0) ::close(descriptor);
    }
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    pf_report_sink_fd_v1 sink() const {
        return {sizeof(pf_report_sink_fd_v1), 1, descriptors[1], "B2-18"};
    }
    std::string read_all() {
        const int writer = descriptors[1];
        descriptors[1] = -1;
        if (::close(writer) != 0) throw std::runtime_error("close failed");
        std::string bytes;
        char buffer[1024];
        for (;;) {
            const auto count = ::read(descriptors[0], buffer, sizeof buffer);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) throw std::runtime_error("read failed");
            if (count == 0) return bytes;
            bytes.append(buffer, static_cast<std::size_t>(count));
        }
    }
};

std::string records(const std::string& bytes) {
    const auto header_end = bytes.find('\n');
    const auto trailer = bytes.find("{\"trailer\":");
    if (header_end == std::string::npos || trailer == std::string::npos
        || trailer <= header_end)
        throw std::runtime_error("missing sink header or trailer");
    return bytes.substr(header_end + 1, trailer - header_end - 1);
}

void nonempty_validation_writes_no_rows() {
    Output output;
    {
        LineWriter writer(output.sink());
        writer.write_header({false, false, false, 0, 0});
        SelectedReportStream stream(kStart, kEnd, kCapital);
        stream.note_trade(kStart);
        stream.note_open_lot(kStart);
        stream.write_point(writer, {kStart - 1, kCapital, -0.0}, false);
        stream.write_point(writer, {kStart, 1010.0, -0.0}, false);
        stream.write_point(writer, {kEnd - 1, 1004.5, 4.5}, false);
        stream.write_point(writer, {kEnd, 2000.0, 1000.0}, false);
        stream.finish(writer, 2, false);
        CHECK(stream.observations() == 2);
        CHECK(writer.write_trailer().count_e == 0);
    }
    CHECK(records(output.read_all()).empty());
}

void empty_validation_leaves_anchor_to_caller() {
    for (int caller_anchor = 0; caller_anchor != 2; ++caller_anchor) {
        Output output;
        {
            LineWriter writer(output.sink());
            writer.write_header({false, false, false, 0, 0});
            SelectedReportStream stream(kStart, kEnd, kCapital);
            stream.write_point(writer, {kStart - 1, kCapital, -0.0}, false);
            stream.write_point(writer, {kEnd, 2000.0, 1000.0}, false);
            stream.finish(writer, 0, false);
            if (caller_anchor != 0) writer.write_e({kStart, kCapital, 0.0});
            stream.finish(writer, 0, false);
            CHECK(stream.observations() == 0);
            CHECK(writer.write_trailer().count_e == caller_anchor);
        }
        CHECK(records(output.read_all()) == (caller_anchor != 0
            ? "[\"E\",100,1000.0,0.0]\n" : ""));
    }
}

void copied_validation_preserves_prefix_and_publication() {
    Output output;
    {
        LineWriter writer(output.sink());
        writer.write_header({false, false, false, 0, 0});
        SelectedReportStream prefix(kStart, kEnd, kCapital);
        prefix.write_point(writer, {kStart - 1, kCapital, 0.0}, false);
        prefix.write_point(writer, {kStart, 1010.0, -0.0}, false);
        // Capture judges the held tail on a copy. Repeating capture cannot
        // consume the recorder's prefix or publish that tail prematurely.
        for (int capture = 0; capture != 2; ++capture) {
            auto validation = prefix;
            validation.write_point(writer, {150, 1004.5, 4.5}, false);
            validation.write_point(writer, {kEnd - 1, 999.0, -1.0}, false);
            validation.finish(writer, 3, false);
            CHECK(validation.observations() == 3);
            CHECK(prefix.observations() == 1);
        }
        // Publication is a separate operation over the presented prefix.
        // The two hidden observations above still belonged to validation.
        writer.write_e({kStart, kCapital, 0.0});
        writer.write_e({kStart, 1010.0, -0.0});
        CHECK(writer.write_trailer().count_e == 2);
    }
    CHECK(records(output.read_all()) ==
        "[\"E\",100,1000.0,0.0]\n"
        "[\"E\",100,1010.0,-0.0]\n");
}

} // namespace

int main() {
    try {
        nonempty_validation_writes_no_rows();
        empty_validation_leaves_anchor_to_caller();
        copied_validation_preserves_prefix_and_publication();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "B2-18 exception: %s\n", error.what());
    }
    std::printf("test_selected_report_validation_only: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
