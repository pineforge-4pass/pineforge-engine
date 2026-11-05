#pragma once

// A test seam for the file that holds the runtime history once it no longer
// fits in memory. The seam is defined in builds without NDEBUG only (see
// runtime_context.cpp); no production code installs a replacement, and a test
// that installs one restores the previous value.
namespace pineforge::detail {

// Opens an unnamed read-write file in `directory`: returns its descriptor, or
// -1 with errno set as open(2) sets it. The system call is the default.
using RuntimeHistoryUnnamedOpen = int (*)(const char* directory);

// Installs `replacement` (nullptr restores the system call) and returns the
// replacement that was installed before.
RuntimeHistoryUnnamedOpen exchange_runtime_history_unnamed_open(
    RuntimeHistoryUnnamedOpen replacement) noexcept;

}  // namespace pineforge::detail
