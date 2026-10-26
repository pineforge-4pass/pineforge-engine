#include "report_sink_v2_codec.hpp"
#include "report_sink_v2_support.hpp"

#include <algorithm>
#include <stdexcept>

#if defined(PINEFORGE_WITH_ZSTD) && PINEFORGE_WITH_ZSTD
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zstd_errors.h>
#endif

namespace pineforge { namespace report_sink {

namespace v2 {
namespace {
[[noreturn]] void summary_limit() {
    throw BlockError(BlockFailure::summary_limit,
        "finalization failure leaves an invalid file with no valid tail and receipt complete=0; "
        "it is not a partial-file result");
}
}
void check_summary_json_length(std::size_t length) {
    if (length > ColumnCodec::max_summary_json_bytes) summary_limit();
    if (!length) throw BlockError(BlockFailure::invalid_input, "empty summary JSON");
}
void check_summary_stored_length(std::size_t length) {
    // Subtraction avoids overflow in the header-inclusive 52+C comparison.
    if (length > ColumnCodec::max_summary_stored_bytes - 52) summary_limit();
}
std::size_t raw_summary_frame_bytes(std::size_t length) {
    check_summary_json_length(length);
    const unsigned size_bytes = length < 256 ? 1 : length < 65792 ? 2 : 4;
    return 5 + size_bytes + 3 * ((length + 131071) / 131072) + length;
}
} // namespace v2

#if defined(PINEFORGE_WITH_ZSTD) && PINEFORGE_WITH_ZSTD
namespace {
void checked(std::size_t result) {
    if (ZSTD_isError(result))
        throw BlockError(BlockFailure::io, ZSTD_getErrorName(result));
}

class ZstdColumnCodec final : public ColumnCodec {
    std::unique_ptr<std::uint64_t[]> encoder_storage_;
    std::unique_ptr<std::uint64_t[]> decoder_storage_;
    std::size_t encoder_bytes_ = 0, decoder_bytes_ = 0;
    ZSTD_CCtx* context_ = nullptr;
    ZSTD_DCtx* decoder_ = nullptr;
    void configure(std::size_t length, int level) {
        checked(ZSTD_CCtx_reset(context_, ZSTD_reset_session_and_parameters));
        checked(ZSTD_CCtx_setParameter(context_, ZSTD_c_compressionLevel, level));
        unsigned window = 10;
        while (window < 24 && (std::size_t{1} << window) < length) ++window;
        checked(ZSTD_CCtx_setParameter(context_, ZSTD_c_windowLog, static_cast<int>(window)));
        checked(ZSTD_CCtx_setParameter(context_, ZSTD_c_contentSizeFlag, 1));
        checked(ZSTD_CCtx_setParameter(context_, ZSTD_c_checksumFlag, 0));
        checked(ZSTD_CCtx_setParameter(context_, ZSTD_c_dictIDFlag, 0));
        checked(ZSTD_CCtx_setParameter(context_, ZSTD_c_nbWorkers, 0));
        checked(ZSTD_CCtx_setPledgedSrcSize(context_, length));
    }
public:
    explicit ZstdColumnCodec(int level, bool decoder_only = false) {
        if (decoder_only) return;
        const auto bound = codec_workspace_bound(level);
        encoder_bytes_ = bound.encoder;
        encoder_storage_ = std::make_unique<std::uint64_t[]>(bound.encoder / 8);
        context_ = ZSTD_initStaticCCtx(encoder_storage_.get(), bound.encoder);
        if (!context_) throw std::bad_alloc();
    }
    void compress_column(std::string_view raw, int level,
                         std::vector<std::uint8_t>& frame) override {
        if (!context_) throw BlockError(BlockFailure::invalid_state, "decoder-only codec cannot encode");
        configure(raw.size(), level);
        // Only a shorter frame is useful. Cap capacity at one raw column,
        // including incompressible inputs, inside the two-Umax scratch bound.
        if (frame.capacity() < raw.size()) std::vector<std::uint8_t>(raw.size()).swap(frame);
        else frame.resize(raw.size());
        const auto size = ZSTD_compress2(context_, frame.data(), frame.size(), raw.data(), raw.size());
        if (ZSTD_isError(size) && ZSTD_getErrorCode(size) == ZSTD_error_dstSize_tooSmall) {
            frame.assign(raw.begin(), raw.end()); // equal length declines compression
            return;
        }
        checked(size);
        frame.resize(size);
    }
    void compress_summary(std::string_view json, int level,
                          std::vector<std::uint8_t>& frame) override {
        if (!context_) throw BlockError(BlockFailure::invalid_state, "decoder-only codec cannot encode");
        v2::check_summary_json_length(json.size());
        configure(json.size(), level);
        const auto capacity = std::min(ZSTD_compressBound(json.size()), max_summary_stored_bytes - 52);
        if (frame.capacity() < capacity) std::vector<std::uint8_t>(capacity).swap(frame);
        else frame.resize(capacity);
        const auto size = ZSTD_compress2(context_, frame.data(), frame.size(), json.data(), json.size());
        if (ZSTD_isError(size) && ZSTD_getErrorCode(size) == ZSTD_error_dstSize_tooSmall) {
            // Sections 9.1/12.1: the library frame would exceed the stored
            // SUMMARY bound. Raw blocks fit for every admitted J.
            ColumnCodec::compress_summary(json, level, frame);
            return;
        }
        checked(size);
        frame.resize(size);
    }
    ColumnFrame inspect_frame(std::string_view frame) const override {
        // Parse the public RFC frame header before asking any decoder to
        // allocate. No static/experimental libzstd interface is required.
        const auto byte = [&](std::size_t i) -> std::uint64_t {
            if (i >= frame.size()) throw BlockError(BlockFailure::invalid_input, "short zstd header");
            return static_cast<unsigned char>(frame[i]);
        };
        if (frame.size() < 6 || byte(0) != 0x28 || byte(1) != 0xb5
                || byte(2) != 0x2f || byte(3) != 0xfd)
            throw BlockError(BlockFailure::invalid_input, "invalid zstd magic");
        const auto flags = byte(4);
        if (flags & 0x18) throw BlockError(BlockFailure::invalid_input, "reserved zstd flags");
        const bool single = (flags & 0x20) != 0;
        std::size_t pos = 5;
        std::uint64_t window = 0;
        if (!single) {
            const auto wd = byte(pos++);
            const auto base = UINT64_C(1) << (10 + (wd >> 3));
            window = base + (base >> 3) * (wd & 7);
        }
        const unsigned dict_size[] = {0, 1, 2, 4};
        const auto dict = dict_size[flags & 3];
        pos += dict;
        unsigned size_bytes = 1u << (flags >> 6);
        if ((flags >> 6) == 0) size_bytes = single ? 1 : 0;
        if (!size_bytes) throw BlockError(BlockFailure::invalid_input, "zstd content size absent");
        std::uint64_t size = 0;
        for (unsigned i = 0; i < size_bytes; ++i) size |= byte(pos++) << (8 * i);
        if (size_bytes == 2) size += 256;
        if (single) window = size;
        const auto frame_size = ZSTD_findFrameCompressedSize(frame.data(), frame.size());
        checked(frame_size);
        return {size, window, frame_size, dict != 0};
    }
    void decompress_column(std::string_view frame, std::size_t length,
                           std::vector<std::uint8_t>& raw) override {
        const auto info = inspect_frame(frame);
        if (length > (16u << 20) || info.content_bytes != length
                || info.window_bytes > (16u << 20) || info.dictionary || info.frame_bytes != frame.size())
            throw BlockError(BlockFailure::io, "invalid finalization column frame");
        if (!decoder_) {
            const auto bytes = codec_workspace_bound(1).decoder;
            decoder_bytes_ = bytes;
            decoder_storage_ = std::make_unique<std::uint64_t[]>(bytes / 8);
            decoder_ = ZSTD_initStaticDCtx(decoder_storage_.get(), bytes);
            if (!decoder_) throw std::bad_alloc();
        }
        if (raw.capacity() < length) std::vector<std::uint8_t>(length).swap(raw);
        else raw.resize(length);
        const auto size = ZSTD_decompressDCtx(decoder_, raw.data(), raw.size(), frame.data(), frame.size());
        checked(size);
        if (size != length) throw BlockError(BlockFailure::io, "short finalization column decode");
    }
    std::size_t workspace_bytes() const noexcept override {
        return encoder_bytes_ + decoder_bytes_;
    }
};
} // namespace
#endif

void ColumnCodec::compress_summary(std::string_view json, int,
                                  std::vector<std::uint8_t>& frame) {
    // Check before allocation or touching the input. Single-segment FCS
    // encodings use J, J-256, or J in one, two, or four bytes respectively.
    const auto length = v2::raw_summary_frame_bytes(json.size());
    v2::check_summary_stored_length(length);
    constexpr std::size_t block_bytes = 131072;
    const unsigned size_bytes = json.size() < 256 ? 1 : json.size() < 65792 ? 2 : 4;
    if (frame.capacity() < length) std::vector<std::uint8_t>(length).swap(frame);
    else frame.resize(length);
    std::size_t at = 0;
    const auto put = [&](std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) frame[at++] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    put(UINT32_C(0xfd2fb528), 4);
    put(size_bytes == 1 ? 0x20 : size_bytes == 2 ? 0x60 : 0xa0, 1);
    put(size_bytes == 2 ? json.size() - 256 : json.size(), size_bytes);
    for (std::size_t offset = 0; offset < json.size();) {
        const auto n = std::min(block_bytes, json.size() - offset);
        put((n << 3) | (offset + n == json.size() ? 1u : 0u), 3);
        std::copy_n(json.data() + offset, n, frame.data() + at);
        at += n;
        offset += n;
    }
}

void ColumnCodec::decompress_column(std::string_view, std::size_t, std::vector<std::uint8_t>&) {
    throw BlockError(BlockFailure::io, "column codec has no finalization decoder");
}

bool sink_v2_available() noexcept {
#if defined(PINEFORGE_WITH_ZSTD) && PINEFORGE_WITH_ZSTD
    return true;
#else
    return false;
#endif
}

CodecWorkspace codec_workspace_bound(int level) {
    if (level < 1 || level > 19)
        throw BlockError(BlockFailure::invalid_input, "zstd level outside 1..19");
#if defined(PINEFORGE_WITH_ZSTD) && PINEFORGE_WITH_ZSTD
    // estimateCCtxSize is an upper bound for one-shot compress2 through this
    // level. Include our explicit windowLog=24 policy as well. Static contexts
    // can never grow: an insufficient estimate becomes a typed codec failure.
    auto encoder = ZSTD_estimateCCtxSize(level);
    checked(encoder);
    for (const auto length : {0ULL, 16384ULL, 131072ULL, 262144ULL, 16777216ULL}) {
        auto params = ZSTD_getCParams(level, length, 0);
        params.windowLog = 24;
        const auto bytes = ZSTD_estimateCCtxSize_usingCParams(params);
        checked(bytes);
        encoder = std::max(encoder, bytes);
    }
    const auto decoder = ZSTD_estimateDCtxSize();
    checked(decoder);
    return {(encoder + 7) / 8 * 8, (decoder + 7) / 8 * 8};
#else
    throw BlockError(BlockFailure::invalid_state, "zstd disabled");
#endif
}

const char* sink_v2_codec_build() noexcept {
#ifdef PINEFORGE_SINK_ZSTD_BUILD
    return PINEFORGE_SINK_ZSTD_BUILD;
#else
    return "unavailable";
#endif
}

std::unique_ptr<ColumnCodec> make_column_codec(int level) {
#if defined(PINEFORGE_WITH_ZSTD) && PINEFORGE_WITH_ZSTD
    return std::make_unique<ZstdColumnCodec>(level);
#else
    (void)level;
    throw BlockError(BlockFailure::invalid_state, "report sink format 2 unavailable (zstd disabled)");
#endif
}

std::unique_ptr<ColumnCodec> make_column_decoder() {
#if defined(PINEFORGE_WITH_ZSTD) && PINEFORGE_WITH_ZSTD
    return std::make_unique<ZstdColumnCodec>(1, true);
#else
    throw BlockError(BlockFailure::invalid_state, "report sink format 2 unavailable (zstd disabled)");
#endif
}

}} // namespace pineforge::report_sink
