#pragma once

#include <string_view>

namespace pineforge::session_grid_policy {

// A recorded window list may have a day mask, but no additional window.
inline bool has_windows(std::string_view session, std::string_view windows) {
    return session.compare(0, windows.size(), windows) == 0 &&
        (session.size() == windows.size() || session[windows.size()] == ':');
}

// Stamp lead relative to the first trading open.
inline int stamp_lead_minutes(std::string_view session) {
    return has_windows(session, "1800-1700") ? 60 : 0;
}

// Grid lead relative to the stamp. HKEX has a grid lead, but no stamp lead.
inline int grid_lead_minutes(std::string_view session) {
    return has_windows(session, "0930-1200,1300-1600") ? 30 : 0;
}

}  // namespace pineforge::session_grid_policy
