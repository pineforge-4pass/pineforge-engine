#pragma once

// Failed runs publish no partial report. These rows use real native hosts.
namespace perf_nopartial {
enum class Fault { read, write, sync, teardown, capacity, typed, callback, sink_record, sink_flush,
                   stream_file };
void failure_case(Fault);
void success_case();
void scratch_read();
void scratch_write();
void scratch_sync();
void scratch_teardown();
void scratch_capacity();
void typed_failure();
void callback_exception();
void sink_record_guard();
void sink_flush_guard();
void native_c_callback();
void stream_file();
void generated_case(int route);
void generated_full();
void native_module_batch();
void native_module_full();
void expect_empty_readers(void* engine_handle, bool with_sink = false);
bool report_value_is_empty(const void* report);
}
