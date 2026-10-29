#pragma once

#include "json.hpp"
#include <string>

namespace pineforge::live {

// Replace the four ratios using the batch routine and the
// cumulative presented curve. All other report fields remain untouched.
void refresh_report_ratios(Json& document, const std::string& chart_timezone);

}
