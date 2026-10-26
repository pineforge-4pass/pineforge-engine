#pragma once

#include "sink_v2_test_decoder.hpp"
#include "report_sink_v2_writer.hpp"
#include "../tools/sink_v2/report_sink_v2_codec.hpp"
#include "../tools/sink_v2/report_sink_v2_support.hpp"

#include <filesystem>
#include <cstdlib>
#include <memory>
#include <unistd.h>

namespace sink2_test {
namespace sink = pineforge::report_sink;

inline constexpr std::string_view metadata =
    "{\"engine\":\"pineforge\",\"input\":{},\"applied_inputs\":{},\"applied_overrides\":{},"
    "\"applied_runtime\":{},\"elapsed_seconds\":0.0,\"summary\":{},\"metrics\":{},"
    "\"diagnostics\":{},\"fingerprint\":null,"
    "\"sink_folds\":{\"closed_count\":0,\"open_at_end_count\":0,\"open_at_end_pnl\":0.0}}";

inline std::string metadata_for(const pf_report_sink_receipt_v2& r) {
    std::string out(metadata.substr(0, metadata.find("\"sink_folds\"")));
    out += "\"sink_folds\":{\"closed_count\":" + std::to_string(r.closed_count)
        + ",\"open_at_end_count\":" + std::to_string(r.open_at_end_count)
        + ",\"open_at_end_pnl\":" + sink::f64(r.open_at_end_pnl) + "}}";
    return out;
}

struct Directory {
    std::string path;
    Directory() {
        auto name = (std::filesystem::temp_directory_path() / "pf-sink2-test-XXXXXX").string();
        std::vector<char> buffer(name.begin(), name.end());
        buffer.push_back('\0');
        const auto result = ::mkdtemp(buffer.data());
        require(result != nullptr, "mkdtemp failed");
        path = result;
    }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    std::string file(const char* name) const { return path + "/" + name; }
};

// A compressor declining a frame exercises stored columns on every build,
// including WITH_ZSTD=OFF. SUMMARY uses the codec's standard raw-frame
// fallback; column bytes must never reach inspect_frame.
class StoredCodec : public sink::ColumnCodec {
public:
    void compress_column(std::string_view raw, int, Bytes& frame) override {
        frame.assign(raw.begin(), raw.end());
    }
    sink::ColumnFrame inspect_frame(std::string_view frame) const override {
        const auto info = inspect_raw_frame(frame, max_summary_json_bytes);
        return {info.content, info.window, frame.size(), false};
    }
    std::size_t workspace_bytes() const noexcept override { return 0; }
};
inline pf_report_sink_v2 descriptor(const std::string& path, const std::string& directory,
                                   bool threaded = false, unsigned depth = 1) {
    return {sizeof(pf_report_sink_v2), 2, path.c_str(), "writer-input-reference",
            directory.c_str(), 3, threaded ? 0u : 1u, depth, 0};
}
template<class Writer> inline std::vector<Row> feed_all(Writer& writer, unsigned count, bool stage_a = false) {
    std::vector<Row> reference;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const double variants[] = {0.0, -0.0, nan, inf, -inf, std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::min(), std::numeric_limits<double>::max(),
        -std::numeric_limits<double>::max(), 3.141592653589793, 1.08342, 0x1p-20};
    for (unsigned j = 0; j < count; ++j) {
        const auto clock = INT64_C(1700000000000) + (j ? j - 1 : 0) * INT64_C(60000);
        const double a = variants[j % 12], b = variants[(j + 3) % 12];
        // Stage A on the 1.7.0 line rejects nonfinite E. Standalone v2
        // reader/writer tests still exercise the original nullable population.
        const pf_equity_point_t point{clock, stage_a && !std::isfinite(a) ? 0.0 : a,
            stage_a && !std::isfinite(b) ? 0.0 : b};
        reference.push_back({'E', {i(point.time_ms), f(point.equity), f(point.open_profit)}});
        writer.write_e(point);
        pf_trade_t trade{};
        trade.is_long = (j & 1) == 0;
        trade.entry_time = j % 7 ? clock : static_cast<std::int64_t>(safe);
        trade.exit_time = static_cast<std::int64_t>(count - j) * 1000;
        trade.entry_price = a;
        trade.exit_price = b;
        trade.qty = 1.25;
        trade.pnl = j % 11 == 0 ? nan : static_cast<double>(static_cast<int>(j % 13) - 6) / 4.0;
        trade.pnl_pct = j % 17 == 0 ? -0.0 : trade.pnl / 10.0;
        trade.max_runup = j % 19 == 0 ? inf : static_cast<double>(j % 7);
        trade.max_drawdown = j % 23 == 0 ? -inf : -static_cast<double>(j % 5);
        trade.commission = 0.125;
        trade.entry_bar_index = static_cast<std::int32_t>(j);
        trade.exit_bar_index = static_cast<std::int32_t>(j + 1);
        trade.open_at_end = j + 1 == count;
        const std::uint64_t incarnation = j % 5 ? j + 1 : safe;
        const auto cause = j % 7 ? INT64_C(1) : -static_cast<std::int64_t>(safe);
        const std::string entry = j % 3 ? u8"L\"\\\n台灣" : u8"短";
        const std::string exit = "exit-" + std::to_string(j);
        const std::string comment("nul\0tab\t", 8);
        reference.push_back({'T', {i(j + 1), s(trade.is_long ? "long" : "short"),
            i(trade.entry_time), i(trade.exit_time), f(trade.entry_price), f(trade.exit_price),
            f(trade.qty), f(trade.pnl), f(trade.pnl_pct), f(trade.max_runup), f(trade.max_drawdown),
            f(trade.commission), i(trade.entry_bar_index), i(trade.exit_bar_index),
            u(trade.open_at_end ? 1 : 0), u(incarnation), i(cause), s(entry), s(exit), s(comment)}});
        writer.write_t(j + 1, trade, incarnation, cause, entry, exit, comment);
        const auto open = j % 3 ? clock : INT64_MIN;
        const auto close = j % 5 ? clock + 60000 : INT64_MIN;
        const double output[] = {a, b, 10.25};
        reference.push_back({'O', {i(j), time(open), time(close), f(output[0]), f(output[1]), f(output[2])}});
        writer.write_o(j, open, close, output, 3);
        pf_output_event_v1_t event{};
        event.sequence = j + 1;
        event.output_index = j % 3;
        event.bar_index = static_cast<std::int32_t>(j);
        event.bar_open_ms = open;
        event.bar_close_ms = close;
        event.ordinal_in_bar = 0;
        event.phase = j % 4;
        event.confirmed = j % 2;
        event.value = b;
        event.message_hash64 = UINT64_C(0x8000000000000000) + j;
        const std::string message = j % 3 == 2 ? std::string(u8"price \"€\"\n\0", 13) : "";
        event.message = j % 3 ? message.c_str() : nullptr;
        reference.push_back({'V', {u(event.sequence), i(event.output_index), i(event.bar_index),
            time(open), time(close), i(event.ordinal_in_bar), i(event.phase), i(event.confirmed),
            f(event.value), u(event.message_hash64), event.message ? s(message) : null()}});
        writer.write_v(event, message);
        const std::string name = j % 2 ? "trace-" + std::to_string(j) : u8"重複";
        const pf_trace_entry_t trace{clock, static_cast<std::int32_t>(j), 0, a};
        reference.push_back({'X', {i(clock), i(j), s(name), f(a)}});
        writer.write_x(trace, name);
        const auto hash = UINT64_C(0xfedcba9876543210) ^ (UINT64_C(0x9e3779b97f4a7c15) * j);
        reference.push_back({'H', {u(hash)}});
        writer.write_h(hash);
    }
    const double constants[] = {-0.0, std::numeric_limits<double>::infinity()};
    reference.push_back({'K', {f(constants[0]), f(constants[1])}});
    writer.write_k(constants, 2);
    return reference;
}
inline void feed_equity(sink::SinkWriter& writer, std::vector<Row>& reference, unsigned n) {
    for (unsigned j = 0; j < n; ++j) {
        const pf_equity_point_t point{static_cast<std::int64_t>(j) * 60000,
            10000.0 + static_cast<double>(j % 64) / 4.0, j % 3 ? 0.0 : -0.0};
        reference.push_back({'E', {i(point.time_ms), f(point.equity), f(point.open_profit)}});
        writer.write_e(point);
    }
}
template<class F> inline void refuses(F function, const char* required_text = nullptr) {
    bool failed = false;
    try { function(); }
    catch (const std::exception& error) {
        failed = true;
        if (required_text) require(std::string(error.what()).find(required_text) != std::string::npos,
                                   "wrong refusal reason");
    }
    require(failed, "operation unexpectedly accepted");
}
inline void sealed(sink::BlockWriter& writer) {
    writer.write_trailer();
    const auto before = writer.receipt();
    require(before.data_complete == 1 && before.complete == 0 && before.sha256_hex[0] == 0
            && before.file_bytes == 0, "data completion was published as a sealed receipt");
    writer.finalize(metadata_for(before));
    const auto after = writer.receipt();
    require(after.complete == 1 && after.data_complete == 1 && after.file_bytes > 0
            && std::strlen(after.sha256_hex) == 64, "metadata did not seal v2");
}
} // namespace sink2_test
