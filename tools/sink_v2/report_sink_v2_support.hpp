#pragma once

#include <pineforge/report_sink_writer.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace pineforge {
namespace report_sink {

// Tool-local error transport. The engine's v1 writer and ABI stay unchanged.
enum class BlockFailure { io, invalid_input, invalid_state, summary_limit, spool_bytes };

class BlockError final : public std::runtime_error {
public:
    BlockError(BlockFailure reason, std::string message, int error_number = 0)
        : std::runtime_error(std::move(message)), reason_(reason), error_number_(error_number) {}

    BlockFailure reason() const noexcept { return reason_; }
    int error_number() const noexcept { return error_number_; }

private:
    BlockFailure reason_;
    int error_number_;
};

std::string v1_header_json(const std::string&, const RecordSet&);

} // namespace report_sink
} // namespace pineforge
