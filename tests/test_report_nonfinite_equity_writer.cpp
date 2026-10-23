// CR-289: native doubles must be refused before an E line changes writer state.
#include <pineforge/run_failure.hpp>

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

// Inspect the opaque counters without adding a production accessor or a copy
// of State. Compile the actual implementation here; the archive supplies its
// dependencies, and its separate writer object is not needed by this target.
#define private public
#include <pineforge/report_sink_writer.hpp>
#undef private
#include "../src/report_sink_writer.cpp"

namespace {
namespace rs = pineforge::report_sink;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Pipe {
    int descriptors[2]{-1, -1};
    Pipe() {
        require(::pipe(descriptors) == 0, "pipe failed");
        const int flags = ::fcntl(descriptors[0], F_GETFL);
        require(flags >= 0 && ::fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) == 0,
                "nonblocking reader setup failed");
    }
    ~Pipe() {
        for (const int fd : descriptors) if (fd >= 0) ::close(fd);
    }
    std::string drain() const {
        std::string bytes;
        std::array<char, 4096> buffer{};
        for (;;) {
            const auto count = ::read(descriptors[0], buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return bytes;
            require(count >= 0, "pipe read failed");
            if (count == 0) return bytes;
            bytes.append(buffer.data(), static_cast<std::size_t>(count));
        }
    }
};

void refusal(double equity, double open_profit, const char* field) {
    // Mutation: remove either corresponding finite_value() call in write_e.
    // That field's three rows then reach the missing-refusal assertion below.
    Pipe pipe;
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, pipe.descriptors[1], "CR-289-writer"};
    rs::LineWriter writer(sink);
    writer.write_header({false, false, false, 0, 0});
    writer.write_e({0, 10000.0, -0.0});
    auto& state = *writer.state_;
    state.flush();
    const auto prefix = pipe.drain();
    require(prefix.find("[\"E\",0,10000.0,-0.0]\n") != std::string::npos,
            "finite prefix was not written");
    const auto counts = state.counts();
    const auto hash = state.hash.hex_digest();
    const auto time = state.equity_time;
    const auto had_time = state.has_equity_time;
    const std::string scratch(state.scratch.view());
    const pf_equity_point_t point{1, equity, open_profit};
    bool refused = false;
    try {
        writer.write_e(point);
    } catch (const std::exception& error) {
        refused = true;
        const auto failure = pineforge::classify_run_failure(error);
        require(failure.code == pineforge::RunFailureCode::report_result_invalid,
                "wrong failure code");
        const std::string expected = std::string("{\"field\":\"") + field
            + "\",\"reason\":\"nonfinite\"}";
        require(failure.args && *failure.args == expected, "wrong field/reason");
    }
    require(refused, "nonfinite E point was accepted");
    require(state.counts() == counts && counts[0] == 1, "refusal changed counters");
    require(state.hash.hex_digest() == hash, "refusal hashed a partial E line");
    require(state.has_equity_time == had_time && state.equity_time == time,
            "refusal advanced the equity time");
    require(state.scratch.view() == scratch, "refusal formatted a partial E line");
    require(state.phase == rs::LineWriter::State::Phase::failed && state.used == 0,
            "refusal did not poison the writer and discard its pending buffer");
    require(pipe.drain().empty(), "refusal appended stream bytes");
    bool trailer_refused = false;
    try {
        (void)writer.write_trailer();
    } catch (const std::exception&) {
        trailer_refused = true;
    }
    require(trailer_refused && state.counts() == counts && pipe.drain().empty(),
            "failed writer published a trailer or changed counters");
}

void finite() {
    Pipe pipe;
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, pipe.descriptors[1], "CR-289-finite"};
    rs::LineWriter writer(sink);
    writer.write_header({false, false, false, 0, 0});
    writer.write_e({0, 10000.0, -0.0});
    writer.write_e({1, std::numeric_limits<double>::max(),
                    -std::numeric_limits<double>::denorm_min()});
    const auto trailer = writer.write_trailer();
    require(trailer.count_e == 2 && trailer.count_t == 0 && trailer.count_o == 0
            && trailer.count_v == 0 && trailer.count_k == 0 && trailer.count_x == 0
            && trailer.count_h == 0, "finite point counters changed");
    const auto bytes = pipe.drain();
    require(bytes.find("[\"E\",0,10000.0,-0.0]\n") != std::string::npos
            && bytes.find("[\"E\",1,1.7976931348623157e+308,-5e-324]\n") != std::string::npos
            && bytes.find("{\"trailer\":") != std::string::npos,
            "finite E points did not complete");
}
} // namespace

int main() {
    int failures = 0;
    const double values[] = {std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity()};
    const char* names[] = {"nan", "posinf", "neginf"};
    for (int field = 0; field < 2; ++field) {
        for (std::size_t i = 0; i < 3; ++i) {
            try {
                refusal(field == 0 ? values[i] : 10000.0,
                        field == 1 ? values[i] : -37.5,
                        field == 0 ? "equity_curve.equity" : "equity_curve.open_profit");
            } catch (const std::exception& error) {
                ++failures;
                std::fprintf(stderr, "writer %s-%s: %s\n",
                             field == 0 ? "equity" : "open_profit", names[i], error.what());
            }
        }
    }
    try {
        // Mutation: reverse the checks; the both-nonfinite precedence row fails.
        refusal(values[0], values[1], "equity_curve.equity");
        finite();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "writer precedence/finite: %s\n", error.what());
    }
    return failures == 0 ? 0 : 1;
}
