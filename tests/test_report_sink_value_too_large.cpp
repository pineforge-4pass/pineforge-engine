// B2-17: compile on the base too; compare the catalog name so the red is
// the writer's classification, not a missing new enum declaration.
#include "../src/report_sink_writer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace {
namespace rs = pineforge::report_sink;
constexpr std::size_t line_bytes = 1024 * 1024; // v1 contract, including LF.
constexpr rs::RecordSet records{true, true, false, 0, 0};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct SocketPair {
    std::array<int, 2> descriptors{{-1, -1}};
    SocketPair() {
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) == 0,
                "create writer-level stream fixture");
    }
    ~SocketPair() {
        for (const int descriptor : descriptors)
            if (descriptor >= 0) ::close(descriptor);
    }
    pf_report_sink_fd_v1 sink(const char* run_id = "B2-17") const {
        return {sizeof(pf_report_sink_fd_v1), 1, descriptors[0], run_id};
    }
};

std::string args(const pineforge::RunFailureValue& failure) {
    return failure.args ? *failure.args : "{}";
}

template<class Operation>
void expect_failure(Operation operation, const char* code,
                    const char* expected_args, const char* diagnostic) {
    try {
        operation();
    } catch (const rs::WriterError& error) {
        const auto& failure = error.run_failure();
        const std::string actual = pineforge::run_failure_code_name(failure.code);
        if (actual != code)
            throw std::runtime_error(std::string("expected ") + code + ", got " + actual);
        require(args(failure) == expected_args, "writer carries exact typed limit arguments");
        require(error.error_number() == 0, "data/invariant failure has no syscall errno");
        require(std::string(error.what()) == diagnostic, "original writer diagnostic is retained");
        const auto classified = pineforge::classify_run_failure(error);
        require(classified.code == failure.code && args(classified) == expected_args,
                "boundary classifier preserves the writer's code and arguments");
        require(!pineforge::run_failure_code_retryable(failure.code), "failure is terminal");
        const auto expected_class = std::string_view(code) == "engine_invariant"
            ? pineforge::RunFailureClass::engine_fault : pineforge::RunFailureClass::input;
        require(pineforge::run_failure_code_class(failure.code) == expected_class,
                "data limits are input failures and invariants are engine faults");
        if (std::string_view(code) == "report_sink_value_too_large")
            require(static_cast<std::uint16_t>(failure.code) == 67, "new 1.7.0 code is 67");
        return;
    }
    throw std::runtime_error("writer operation unexpectedly succeeded");
}

template<class Operation>
void expect_limit(Operation operation, bool raw_string) {
    expect_failure(operation, "report_sink_value_too_large",
        raw_string ? "{\"field\":\"report_sink.string\",\"max\":1048576}"
                   : "{\"field\":\"report_sink.line\",\"max\":1048576}",
        raw_string ? "report sink string exceeds the line bound"
                   : "report sink line exceeds 1 MiB including LF");
}

void event_limit(std::string message, bool raw_string) {
    SocketPair sockets;
    rs::LineWriter writer(sockets.sink());
    writer.write_header(records);
    pf_output_event_v1_t event{};
    event.struct_version = 1;
    event.size = sizeof event;
    event.sequence = 1;
    event.message = message.c_str();
    expect_limit([&] { writer.write_v(event, message); }, raw_string);
    require(writer.diagnostics().complete == 0, "rejected V has no completed receipt");
}

void other_string_limits() {
    const std::string large(line_bytes + 1, 'x');
    {
        SocketPair sockets;
        rs::LineWriter writer(sockets.sink(large.c_str()));
        expect_limit([&] { writer.write_header(records); }, true);
    }
    {
        SocketPair sockets;
        rs::LineWriter writer(sockets.sink());
        writer.write_header(records);
        const pf_trace_entry_t entry{};
        expect_limit([&] { writer.write_x(entry, large); }, true);
    }
    {
        SocketPair sockets;
        rs::LineWriter writer(sockets.sink());
        writer.write_header(records);
        const pf_trade_t trade{};
        expect_limit([&] { writer.write_t(1, trade, 0, 0, "", "", large); }, true);
    }
    {
        // Each string fits alone; their combined T row cannot fit including LF.
        const std::string half(line_bytes / 2, 't');
        SocketPair sockets;
        rs::LineWriter writer(sockets.sink());
        writer.write_header(records);
        const pf_trade_t trade{};
        expect_limit([&] { writer.write_t(1, trade, 0, 0, half, half, ""); }, false);
    }
}

void invariants_stay_engine_faults() {
    {
        SocketPair sockets;
        rs::LineWriter writer(sockets.sink());
        writer.write_header(records);
        expect_failure([&] { writer.write_o(1, 0, 0, nullptr, 0); },
            "engine_invariant", "{}",
            "report sink output rows must start at zero and be contiguous");
    }
    {
        SocketPair sockets;
        rs::LineWriter writer(sockets.sink());
        const pf_output_event_v1_t event{};
        expect_failure([&] { writer.write_v(event, ""); }, "engine_invariant", "{}",
                       "report sink is not accepting records");
    }
    {
        SocketPair sockets;
        rs::LineWriter writer(sockets.sink());
        expect_failure([&] { writer.write_header({true, false, false, -1, 0}); },
            "engine_invariant", "{}", "report sink descriptor count exceeds the line bound");
    }
    {
        SocketPair sockets;
        rs::LineWriter writer(sockets.sink());
        writer.write_header(records);
        pf_output_event_v1_t event{};
        event.sequence = UINT64_C(9007199254740992); // One beyond the v1 exact-integer domain.
        expect_failure([&] { writer.write_v(event, ""); }, "engine_invariant", "{}",
                       "report sink u64 exceeds the exact JSON integer bound");
    }
}

void ordinary_event_still_completes() {
    SocketPair sockets;
    rs::LineWriter writer(sockets.sink());
    writer.write_header(records);
    pf_output_event_v1_t event{};
    event.sequence = 1;
    event.message = "small";
    writer.write_v(event, "small");
    require(writer.write_trailer().count_v == 1, "ordinary V still completes");
    require(writer.diagnostics().complete == 1, "ordinary sink has a completed receipt");
}
} // namespace

int main() {
    try {
        event_limit(std::string(line_bytes + 1, 'v'), true);
        event_limit(std::string(line_bytes, 'v'), true);
        // A control byte needs six JSON bytes; invalid UTF-8 needs three.
        // These inputs fit the raw bound but cannot fit the serialized line.
        event_limit(std::string(line_bytes / 6 + 1, '\x01'), false);
        event_limit(std::string(line_bytes / 3 + 1, static_cast<char>(0xff)), false);
        other_string_limits();
        invariants_stay_engine_faults();
        ordinary_event_still_completes();
        std::puts("B2-17: raw/escaped/combined string limits and genuine invariants checked");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_report_sink_value_too_large: %s\n", error.what());
        return 1;
    }
}
