#pragma once

// Shared checks for the invalid-length rows. An indicator built with a length
// (or a bar count) that describes no window answers na on every call and keeps
// nothing: no source enters its window and nothing is read from it.
//
// Every line is flushed as it is written, so the lines of the calls made
// before a crash or a sanitizer report stay in the log, and the last CALL line
// names the call that faulted.

#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace ta_invalid_length {

inline int failures = 0;

inline void calling(const char* indicator, const char* length, const char* call) {
    std::printf("CALL %s %s %s\n", indicator, length, call);
    std::fflush(stdout);
}

inline void expect_na(const char* indicator, const char* length, const char* call, double got) {
    const bool ok = pineforge::is_na(got);
    std::printf("%s %s %s %s: got %.17g, want na\n",
                ok ? "PASS" : "FAIL", indicator, length, call, got);
    std::fflush(stdout);
    if (!ok) ++failures;
}

// `held` is what the indicator's window holds after the call. A window that
// holds something it has no room for is past its storage: neither a later call
// on the object nor its destructor is safe to run, so the process ends here,
// without unwinding, with a failing status.
inline void expect_nothing_held(const char* indicator, const char* length, const char* call,
                                std::size_t held) {
    if (held == 0) {
        std::printf("PASS %s %s %s: window holds 0\n", indicator, length, call);
        std::fflush(stdout);
        return;
    }
    std::printf("FAIL %s %s %s: window holds %zu, want 0; stopping, the object is past its storage\n",
                indicator, length, call, held);
    std::fflush(stdout);
    std::_Exit(1);
}

inline int result() {
    std::printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
    std::fflush(stdout);
    return failures != 0;
}

}  // namespace ta_invalid_length
