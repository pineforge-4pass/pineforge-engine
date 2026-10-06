#pragma once
#include <pineforge/run_failure.hpp>
#include <iostream>
#include <string>
#include <stdexcept>

namespace pineforge {

inline void pine_log_info(const std::string& msg) { std::cerr << "[INFO] " << msg << "\n"; }
inline void pine_log_warning(const std::string& msg) { std::cerr << "[WARN] " << msg << "\n"; }
inline void pine_log_error(const std::string& msg) { std::cerr << "[ERROR] " << msg << "\n"; }
// runtime.error(): still a std::runtime_error whose what() is the message, now
// coded strategy_runtime_error -- the only code a script's own text can read.
[[noreturn]] inline void pine_runtime_error(const std::string& msg) {
    throw coded<std::runtime_error>(RunFailureCode::strategy_runtime_error, {}, msg);
}

} // namespace pineforge
