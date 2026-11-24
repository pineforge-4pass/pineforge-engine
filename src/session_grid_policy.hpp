#pragma once

#include <string_view>

namespace pineforge::session_grid_policy {

inline constexpr int metals_open_minutes = 18 * 60;
inline constexpr int metals_close_minutes = 17 * 60;

// Stamp lead relative to the first trading open. The legacy timeframe clock
// passes its parsed first window; the native chart clock uses the recorded
// single-window session text.
inline constexpr int stamp_lead_minutes(int open, int close) {
    return open == metals_open_minutes && close == metals_close_minutes ? 60 : 0;
}

inline bool has_windows(std::string_view session, std::string_view windows) {
    return session.compare(0, windows.size(), windows) == 0 &&
        (session.size() == windows.size() || session[windows.size()] == ':');
}

inline int native_stamp_lead_minutes(std::string_view session) {
    return has_windows(session, "1800-1700")
        ? stamp_lead_minutes(metals_open_minutes, metals_close_minutes) : 0;
}

// Grid lead relative to the stamp. HKEX has a grid lead, but no stamp lead.
inline int grid_lead_minutes(std::string_view session) {
    return has_windows(session, "0930-1200,1300-1600") ? 30 : 0;
}

}  // namespace pineforge::session_grid_policy
