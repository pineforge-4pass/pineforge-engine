#pragma once

#include <pineforge/report_sink_writer.hpp>
#include <pineforge/run_failure.hpp>

#include <cerrno>
#include <stdexcept>
#include <string>
#include <utility>

namespace pineforge {
namespace report_sink {

// Carry the validated cause from the syscall to every run-boundary wrapper.
// Only errno distinguishes a closed peer; human operation text is diagnostic.
enum class WriterFailure { io, invalid_input, invalid_state };

class WriterError final : public std::runtime_error, public RunFailureInfo {
public:
    WriterError(WriterFailure reason, std::string message, int error_number = 0)
        : std::runtime_error(std::move(message)),
          RunFailureInfo(reason == WriterFailure::io ? RunFailureCode::report_sink_failed
                                                     : RunFailureCode::engine_invariant,
              reason == WriterFailure::io
                  ? RunFailureArgs{{"reason", error_number == EPIPE || error_number == ECONNRESET
                                                 ? "sink_peer_closed" : "sink_io"}}
                  : RunFailureArgs{}),
          reason_(reason), error_number_(error_number) {}

    WriterFailure reason() const noexcept { return reason_; }
    int error_number() const noexcept { return error_number_; }

private:
    WriterFailure reason_;
    int error_number_;
};

} // namespace report_sink
} // namespace pineforge
