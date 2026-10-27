#pragma once
#include <pineforge/report_sink_json.h>

namespace pineforge {
namespace report_sink {
int json_v1(const pf_report_sink_json_request_v1*,
            pf_report_sink_json_result_v1*) noexcept;
}
}
