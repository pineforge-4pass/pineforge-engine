#pragma once

#include "json.hpp"
#include <string>

namespace pineforge::live {

class SpooledReportDocument;

// Replace the four ratios using the batch routine and the
// cumulative presented curve. All other report fields remain untouched.
void refresh_report_ratios(Json& document, const std::string& chart_timezone);

// The same refresh for a document whose curve is spooled: the batch routine
// reads the curve through one fixed window, never as a resident array.
void refresh_report_ratios(SpooledReportDocument& document, const std::string& chart_timezone);

}
