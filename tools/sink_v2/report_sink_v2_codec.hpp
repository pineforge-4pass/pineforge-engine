#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace pineforge { namespace report_sink {

struct ColumnFrame {
    std::uint64_t content_bytes = 0;
    std::uint64_t window_bytes = 0;
    std::size_t frame_bytes = 0;
    bool dictionary = false;
};

// This is the only production boundary that knows libzstd. Buffers belong to
// the caller and are reused one column (or the bounded SUMMARY) at a time.
class ColumnCodec {
public:
    // Section 3.7's S_MAX and stored bound (including the SUM2 header).
    static constexpr std::size_t max_summary_json_bytes = 1048576;
    static constexpr std::size_t max_summary_stored_bytes = 1048661;
    virtual ~ColumnCodec() = default;
    virtual void compress_column(std::string_view raw, int level,
                                 std::vector<std::uint8_t>& frame) = 0;
    // Unlike columns, SUMMARY always retains a standard frame. The default
    // is the bounded raw-block fallback, also usable by injected test codecs.
    virtual void compress_summary(std::string_view json, int level,
                                  std::vector<std::uint8_t>& frame);
    virtual ColumnFrame inspect_frame(std::string_view frame) const = 0;
    // Footer reconstruction, E figures and T-key rereads reuse this decoder.
    // Stored-only injected codecs refuse an unexpected compressed frame.
    virtual void decompress_column(std::string_view, std::size_t,
                                   std::vector<std::uint8_t>&);
    virtual std::size_t workspace_bytes() const noexcept = 0;
};

namespace v2 {
// Both guards use the single production SUMMARY bound pair above.
void check_summary_json_length(std::size_t);
void check_summary_stored_length(std::size_t);
std::size_t raw_summary_frame_bytes(std::size_t json_bytes);
}

bool sink_v2_available() noexcept;
struct CodecWorkspace {
    std::size_t encoder = 0;
    std::size_t decoder = 0;
};
// Reserved static contexts, obtained from the shipped codec's own estimator.
CodecWorkspace codec_workspace_bound(int level);
const char* sink_v2_codec_build() noexcept;
std::unique_ptr<ColumnCodec> make_column_codec(int level = 3);
// Footer workers need a private decoder, never another encoder allocation.
std::unique_ptr<ColumnCodec> make_column_decoder();

}} // namespace pineforge::report_sink
