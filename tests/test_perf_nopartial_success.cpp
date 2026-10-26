// GUARD: real disk/stream execution versus caller-owned memory/batch execution.
#include "perf_nopartial_rows.hpp"

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        if (argc > 2) throw std::runtime_error("one row name expected");
        const std::string row = argc == 1 ? "success" : argv[1];
        if (row == "success") perf_nopartial::success_case();
        else if (row == "scratch-read") perf_nopartial::scratch_read();
        else if (row == "scratch-write") perf_nopartial::scratch_write();
        else if (row == "scratch-sync") perf_nopartial::scratch_sync();
        else if (row == "scratch-teardown") perf_nopartial::scratch_teardown();
        else if (row == "scratch-capacity") perf_nopartial::scratch_capacity();
        else if (row == "typed-failure") perf_nopartial::typed_failure();
        else if (row == "callback-exception") perf_nopartial::callback_exception();
        else if (row == "sink-record") perf_nopartial::sink_record_guard();
        else if (row == "sink-flush") perf_nopartial::sink_flush_guard();
        else if (row == "native-c-callback") perf_nopartial::native_c_callback();
        else if (row == "stream-file") perf_nopartial::stream_file();
        else if (row == "generated") perf_nopartial::generated_case(0);
        else if (row == "generated-full") perf_nopartial::generated_full();
        else if (row == "native-module") perf_nopartial::native_module_batch();
        else if (row == "native-module-full") perf_nopartial::native_module_full();
        else throw std::runtime_error("unknown perf-nopartial row");
        std::printf("perf-nopartial %s PASS\n", row.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "perf-nopartial: %s\n", error.what());
        return 1;
    }
}
