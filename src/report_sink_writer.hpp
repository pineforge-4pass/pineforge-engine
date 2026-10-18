#pragma once

#include <pineforge/report_sink_writer.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace pineforge {
namespace report_sink {

// Internal error transport for L6b-1 to map at the run boundary. No C ABI
// declaration is changed. S2's runtime mapping remains an integration item.
enum class WriterFailure { io, invalid_input, invalid_state };

class WriterError final : public std::runtime_error {
public:
    WriterError(WriterFailure reason, std::string message, int error_number = 0)
        : std::runtime_error(std::move(message)), reason_(reason), error_number_(error_number) {}

    WriterFailure reason() const noexcept { return reason_; }
    int error_number() const noexcept { return error_number_; }

private:
    WriterFailure reason_;
    int error_number_;
};

} // namespace report_sink
} // namespace pineforge
