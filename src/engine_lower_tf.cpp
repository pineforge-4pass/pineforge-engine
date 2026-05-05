/*
 * engine_lower_tf.cpp — lower-timeframe emulation helpers (request.security on a finer TF)
 */

#include "engine_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace pineforge {
namespace internal {


bool is_fixed_intraday_minute_tf(const std::string& tf) {
    if (tf.empty()) {
        return false;
    }
    return std::all_of(tf.begin(), tf.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
}


bool supports_lower_tf_emulation(const std::string& input_tf,
                                        const std::string& requested_tf,
                                        int* out_ratio,
                                        int* out_requested_seconds) {
    if (!is_fixed_intraday_minute_tf(input_tf) || !is_fixed_intraday_minute_tf(requested_tf)) {
        return false;
    }

    int input_seconds = tf_to_seconds(input_tf);
    int requested_seconds = tf_to_seconds(requested_tf);
    if (input_seconds <= 0 || requested_seconds <= 0 || requested_seconds >= input_seconds) {
        return false;
    }
    if (input_seconds % requested_seconds != 0) {
        return false;
    }

    int ratio = input_seconds / requested_seconds;
    if (ratio <= 1) {
        return false;
    }

    if (out_ratio != nullptr) {
        *out_ratio = ratio;
    }
    if (out_requested_seconds != nullptr) {
        *out_requested_seconds = requested_seconds;
    }
    return true;
}


void ensure_supported_lower_tf_emulation_flags(bool lookahead_on, bool gaps_on) {
    if (lookahead_on || gaps_on) {
        throw std::runtime_error(
            "request.security lower TF emulation only supports lookahead=barmerge.lookahead_off and gaps=barmerge.gaps_off"
        );
    }
}


std::vector<Bar> synthesize_lower_tf_bars(const Bar& input_bar,
                                                 int ratio,
                                                 int requested_seconds) {
    std::vector<Bar> out;
    if (ratio <= 1 || requested_seconds <= 0) {
        return out;
    }

    std::vector<double> path_prices =
        sample_price_path(input_bar, ratio + 1, MagnifierDistribution::ENDPOINTS);
    if (static_cast<int>(path_prices.size()) != ratio + 1) {
        return out;
    }

    out.reserve(static_cast<std::size_t>(ratio));
    int64_t sub_bar_ms = static_cast<int64_t>(requested_seconds) * 1000;
    double slice_volume = input_bar.volume / static_cast<double>(ratio);
    for (int i = 0; i < ratio; ++i) {
        double open = path_prices[static_cast<std::size_t>(i)];
        double close = path_prices[static_cast<std::size_t>(i + 1)];
        double volume = (i == ratio - 1)
            ? (input_bar.volume - slice_volume * static_cast<double>(ratio - 1))
            : slice_volume;
        out.push_back({
            open,
            std::max(open, close),
            std::min(open, close),
            close,
            volume,
            input_bar.timestamp + static_cast<int64_t>(i) * sub_bar_ms,
        });
    }
    return out;
}


}  // namespace internal
}  // namespace pineforge
