#pragma once

// Independent real CSV inputs for selected-capacity tests. This fixture only
// formats supplied OHLCV records; it does not count, admit, execute, or
// compute expected failures.

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace selected_capacity_fixture {

inline constexpr char kCsvHeader[] = "timestamp,open,high,low,close,volume\n";

struct CsvRecord {
    std::int64_t timestamp = 0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
};

inline CsvRecord make_record(std::int64_t timestamp, double open, double high, double low,
                             double close, double volume) noexcept {
    return CsvRecord{timestamp, open, high, low, close, volume};
}

inline std::string make_csv(const std::vector<CsvRecord>& rows) {
    std::ostringstream output;
    output << kCsvHeader << std::setprecision(17);
    for (const CsvRecord& row : rows) {
        output << row.timestamp << ',' << row.open << ',' << row.high << ',' << row.low
               << ',' << row.close << ',' << row.volume << '\n';
    }
    return output.str();
}

}  // namespace selected_capacity_fixture
