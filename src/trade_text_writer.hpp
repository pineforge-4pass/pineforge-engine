#pragma once

#include "sha256.hpp"
#include <pineforge/report_pages.hpp>
#include <pineforge/report_sink_writer.hpp>

#include <algorithm>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pineforge {
namespace report {

// A separate finished-report stream. read_capacity is a transfer quantum,
// supplied explicitly; it never limits a field, record or complete stream.
// Write must consume every supplied byte or throw. No execution state changes.
template<class Write>
void write_trade_text_companion_to(Write&& write, std::string_view run_id,
                                    const TradeSourceV1& source,
                                    std::size_t read_capacity) {
    if (!read_capacity) throw std::invalid_argument("trade text read capacity must be positive");
    std::vector<char> buffer(read_capacity);
    report_sink::Sha256 digest;
    const auto emit = [&](std::string_view bytes) {
        while (!bytes.empty()) {
            const auto part = bytes.substr(0, std::min<std::size_t>(bytes.size(), 65536));
            write(part);
            digest.update(part);
            bytes.remove_prefix(part.size());
        }
    };
    const auto quoted_run = report_sink::escaped_string(run_id);
    emit("{\"schema\":\"pf-trade-text\",\"version\":1,\"run_id\":");
    emit(quoted_run);
    emit(",\"fields\":[\"run_id\",\"row_index\",\"entry_incarnation\",\"entry_id\","
         "\"exit_id\",\"entry_comment\",\"exit_comment\"]}\n");

    const auto text = [&](LogicalIndexV1 index, TradeTextV1 field) {
        const auto length = source.text_size(index, field);
        std::uint64_t offset = 0;
        std::string pending;
        emit("\"");
        while (offset < length) {
            const auto wanted = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), length - offset));
            const auto copied = source.read_text(index, field, offset, buffer.data(), wanted);
            if (!copied || copied > wanted)
                throw std::runtime_error("trade text source made invalid progress");
            offset += copied;
            std::string bytes = std::move(pending);
            bytes.append(buffer.data(), copied);
            auto end = bytes.size();
            // Hold at most three trailing bytes when a UTF-8 scalar may cross
            // the transfer boundary. The sink helper decides actual validity.
            if (offset < length) {
                for (std::size_t back = 1; back <= std::min<std::size_t>(3, bytes.size()); ++back) {
                    const auto byte = static_cast<unsigned char>(bytes[bytes.size() - back]);
                    if (byte >= 0x80 && byte <= 0xbf) continue;
                    const std::size_t width = byte >= 0xc2 && byte <= 0xdf ? 2
                        : byte >= 0xe0 && byte <= 0xef ? 3
                        : byte >= 0xf0 && byte <= 0xf4 ? 4 : 1;
                    if (width > back) end = bytes.size() - back;
                    break;
                }
            }
            pending = bytes.substr(end);
            const auto encoded = report_sink::escaped_string(std::string_view(bytes).substr(0, end));
            emit(std::string_view(encoded).substr(1, encoded.size() - 2));
        }
        emit("\"");
    };

    const auto count = source.size();
    for (LogicalIndexV1 index = 0; index < count; ++index) {
        const auto row = source.at(index);
        emit("[\"T\",");
        emit(quoted_run);
        emit(",");
        emit(std::to_string(index));
        emit(",");
        emit(std::to_string(row.entry_incarnation));
        for (const auto field : {TradeTextV1::EntryId, TradeTextV1::ExitId,
                                  TradeTextV1::EntryComment, TradeTextV1::ExitComment}) {
            emit(",");
            text(index, field);
        }
        emit("]\n");
    }
    // SHA-256 covers the complete prefix, including each LF, not the trailer.
    const auto trailer = "{\"trailer\":\"pf-trade-text\",\"version\":1,\"counts\":{\"T\":"
        + std::to_string(count) + "},\"sha256\":\"" + digest.hex_digest() + "\"}\n";
    write(trailer);
}

inline void write_trade_text_companion(std::ostream& output, std::string_view run_id,
                                        const TradeSourceV1& source,
                                        std::size_t read_capacity) {
    write_trade_text_companion_to([&](std::string_view bytes) {
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output) throw std::runtime_error("trade text companion write failed");
    }, run_id, source, read_capacity);
    output.flush();
    if (!output) throw std::runtime_error("trade text companion flush failed");
}

} // namespace report
} // namespace pineforge
