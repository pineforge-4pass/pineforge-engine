#pragma once
#include <iostream>
#include <string>
#include <stdexcept>

namespace pineforge {

inline void pine_log_info(const std::string& msg) { std::cerr << "[INFO] " << msg << "\n"; }
inline void pine_log_warning(const std::string& msg) { std::cerr << "[WARN] " << msg << "\n"; }
inline void pine_log_error(const std::string& msg) { std::cerr << "[ERROR] " << msg << "\n"; }
inline void pine_runtime_error(const std::string& msg) { throw std::runtime_error(msg); }

} // namespace pineforge
