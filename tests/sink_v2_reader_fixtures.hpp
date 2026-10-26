#pragma once

// Shared independent stored-column fixtures, derived from sink-v2.md.
// Reader::verify and IncrementalValidator consume the SAME encoder.
#include "../src/sha256.hpp"
#include <pineforge/report_sink_writer.hpp>
#include <pineforge/version.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sink_v2_reader_fixture {
namespace sink = pineforge::report_sink;
inline void require(bool ok, const std::string& why) { if (!ok) throw std::runtime_error(why); }
inline std::uint64_t word(double d) { std::uint64_t w; std::memcpy(&w, &d, 8); return w; }

// Only generic SHA-256 and the existing v1 scalar formatter are reused.
// No layout, schema, encoding, checksum-domain, or query helper is shared.
using Cell = std::variant<std::monostate, std::int64_t, std::uint64_t, double, bool, std::string>;
struct Row { char tag; std::vector<Cell> values; };
struct Definition { char tag; std::vector<std::string> names, types, units; };
constexpr std::string_view tags = "ETOVKXHI";
using Hash = std::array<std::uint8_t, 32>;
inline std::string raw_hash(const Hash& h) { return {reinterpret_cast<const char*>(h.data()), h.size()}; }
inline Hash hash(std::string_view s) { sink::Sha256 h; h.update(s); return h.digest(); }
inline std::uint32_t checksum(std::string_view s) {
    std::uint32_t v = ~UINT32_C(0);
    for (unsigned char c : s) {
        v ^= c;
        for (unsigned b = 0; b < 8; ++b) {
            const auto low = v & 1;
            v >>= 1;
            if (low) v ^= UINT32_C(0xedb88320);
        }
    }
    return ~v;
}
inline void put(std::string& s, std::size_t at, std::uint64_t v, unsigned width) {
    require(at + width <= s.size(), "test writer fixed store");
    for (unsigned i = 0; i < width; ++i) s[at + i] = static_cast<char>(v >> (8 * i));
}
inline std::uint64_t get(std::string_view s, std::size_t at, unsigned width) {
    require(at + width <= s.size(), "test writer fixed load");
    std::uint64_t v = 0;
    for (unsigned i = 0; i < width; ++i) v |= std::uint64_t(static_cast<unsigned char>(s[at + i])) << (8 * i);
    return v;
}
inline std::string little(std::uint64_t v, unsigned width) { std::string s(width, '\0'); put(s, 0, v, width); return s; }
inline std::string raw_frame(std::string_view bytes, std::size_t block_size = 131072) {
    // RFC 8878 single-segment frame, pledged size, no dictionary/checksum.
    // This is a decode fixture, never purported level-3 compressor output.
    require(block_size >= 1 && block_size <= 131072, "fixture zstd block cap");
    std::string frame("\x28\xb5\x2f\xfd",4);
    if (bytes.size() < 256) frame += little(0x20,1) + little(bytes.size(),1);
    else if (bytes.size() < 65792) frame += little(0x60,1) + little(bytes.size()-256,2);
    else frame += little(0xa0,1) + little(bytes.size(),4);
    std::size_t at = 0;
    do {
        const auto n = std::min(block_size,bytes.size()-at);
        frame += little((n<<3) | (at+n==bytes.size() ? 1u : 0u),3);
        frame.append(bytes.substr(at,n)); at += n;
    } while (at < bytes.size());
    return frame;
}
inline std::string uv(std::uint64_t v) {
    std::string s;
    do { const auto b = v & 127; v >>= 7; s += static_cast<char>(b | (v ? 128 : 0)); } while (v);
    return s;
}
inline bool null(const Cell& c) {
    return std::holds_alternative<std::monostate>(c)
        || (std::holds_alternative<double>(c) && !std::isfinite(std::get<double>(c)));
}
inline std::uint64_t numeric(const Cell& c) {
    if (const auto p = std::get_if<std::int64_t>(&c)) return static_cast<std::uint64_t>(*p);
    if (const auto p = std::get_if<std::uint64_t>(&c)) return *p;
    if (const auto p = std::get_if<double>(&c)) return word(*p);
    return std::get<bool>(c) ? 1 : 0;
}
inline std::vector<Definition> definitions(bool all, unsigned slots = 2, unsigned constants = 2) {
    std::vector<Definition> d{
        {'E',{"time_ms","equity","open_profit"},{"i64","f64?","f64?"},{"unix_ms","account_ccy","account_ccy"}},
        {'T',{"n","side","entry_time","exit_time","entry_price","exit_price","qty","pnl","pnl_pct","max_runup",
            "max_drawdown","commission","entry_bar_index","exit_bar_index","open_at_end","entry_incarnation",
            "close_cause","entry_id","exit_id","exit_comment"},
            {"i64","str","i64","i64","f64?","f64?","f64?","f64?","f64?","f64?","f64?","f64?",
             "i64","i64","bool","u64","i64","str","str","str"},
            {"","","unix_ms","unix_ms","quote_ccy","quote_ccy","qty","account_ccy","percent","account_ccy",
             "account_ccy","account_ccy","script_bar","script_bar","","","","","",""}}};
    if (all) {
        Definition o{'O',{"row","open_ms","close_ms"},{"i64","i64?","i64?"},{"","unix_ms","unix_ms"}};
        Definition k{'K',{},{},{}};
        for (unsigned i = 0; i < slots; ++i) {
            o.names.push_back("v" + std::to_string(i)); o.types.push_back("f64?"); o.units.push_back("");
        }
        for (unsigned i = 0; i < constants; ++i) {
            k.names.push_back("c" + std::to_string(i)); k.types.push_back("f64?"); k.units.push_back("");
        }
        d.push_back(std::move(o));
        d.push_back({'V',{"sequence","output_index","bar_index","bar_open_ms","bar_close_ms","ordinal_in_bar",
            "phase","confirmed","value","message_hash64","message"},
            {"u64","i64","i64","i64?","i64?","i64","i64","i64","f64?","hex64","str?"},
            {"","","","unix_ms","unix_ms","","","","","",""}});
        d.push_back(std::move(k));
        d.push_back({'X',{"timestamp","bar_index","name","value"},{"i64","i64","str","f64?"},{"unix_ms","","",""}});
        d.push_back({'H',{"hash"},{"hex64"},{""}});
    }
    return d;
}
inline std::string canonical(const Cell& c, const std::string& type) {
    std::string out;
    const bool optional = type.back() == '?';
    if (optional) out += null(c) ? '\0' : '\1';
    if (null(c)) { require(optional, "test input nonoptional null"); return out; }
    if (type.compare(0, 3, "str") == 0) {
        const auto& s = std::get<std::string>(c); out += little(s.size(), 4) + s;
    } else out += little(numeric(c), type == "bool" || type == "order" ? 1 : 8);
    return out;
}
inline std::string canonical_row(const Row& row, const Definition& d) {
    require(row.values.size() == d.types.size(), "test input width");
    std::string out;
    for (std::size_t c = 0; c < d.types.size(); ++c) out += canonical(row.values[c], d.types[c]);
    return out;
}
inline std::string input_line(const Row& row, const Definition& d) {
    // INPUT projection uses fixture cells, never decoded reader records.
    std::string out = "[\"" + std::string(1, row.tag) + "\"";
    for (std::size_t c = 0; c < row.values.size(); ++c) {
        out += ',';
        const auto& cell = row.values[c];
        if (null(cell)) out += "null";
        else if (d.types[c].compare(0, 3, "str") == 0) out += sink::escaped_string(std::get<std::string>(cell));
        else if (d.types[c] == "hex64") {
            out += '"';
            const auto value = numeric(cell);
            for (int shift = 60; shift >= 0; shift -= 4) out += "0123456789abcdef"[(value >> shift) & 15];
            out += '"';
        } else if (const auto p = std::get_if<double>(&cell)) out += sink::f64(*p);
        else if (const auto p = std::get_if<std::int64_t>(&cell)) out += std::to_string(*p);
        else if (const auto p = std::get_if<std::uint64_t>(&cell)) out += std::to_string(*p);
        else out += std::get<bool>(cell) ? "true" : "false";
    }
    return out + "]\n";
}
struct Encoding { unsigned mode = 0, flags = 0, codec = 0; std::string bytes; };
inline Encoding column(const std::vector<Row>& rows, const Definition& d, std::size_t c) {
    Encoding e;
    std::vector<const Cell*> present;
    std::string bitmap((rows.size() + 7) / 8, '\0');
    for (std::size_t r = 0; r < rows.size(); ++r) {
        if (null(rows[r].values[c])) {
            e.flags = 1; bitmap[r / 8] = static_cast<char>(static_cast<unsigned char>(bitmap[r / 8]) | (1u << (r % 8)));
        } else present.push_back(&rows[r].values[c]);
    }
    if (e.flags) e.bytes = bitmap;
    const auto& type = d.types[c];
    if (type.compare(0, 3, "str") == 0) {
        e.mode = 6;
        for (auto cell : present) e.bytes += uv(std::get<std::string>(*cell).size());
        for (auto cell : present) e.bytes += std::get<std::string>(*cell);
    } else if (type.compare(0, 3, "f64") == 0 || type == "hex64") {
        e.mode = 2;
        std::vector<std::uint64_t> x;
        std::uint64_t previous = 0;
        for (auto cell : present) { const auto w = numeric(*cell); x.push_back(w ^ previous); previous = w; }
        for (unsigned p = 0; p < 8; ++p) for (auto w : x) e.bytes += static_cast<char>(w >> (8 * p));
    } else {
        e.mode = type == "bool" || type == "order" ? 4 : 0;
        for (auto cell : present) e.bytes += little(numeric(*cell), e.mode == 4 ? 1 : 8);
    }
    return e;
}
inline std::vector<std::string> figures(const std::vector<Row>& rows, std::uint64_t open_count, double open_pnl) {
    // Independent brute-force input oracle: sort sample ordinals per series,
    // then write the exact union/selectors from the spec's field ledger.
    std::vector<std::string> witnesses;
    std::array<std::vector<std::optional<double>>,2> values;
    const auto last = std::find_if(rows.rbegin(), rows.rend(), [](const auto& row) { return !null(row.values[1]); });
    for (std::size_t raw = 0; raw < rows.size(); ++raw) {
        const auto& cells = rows[raw].values;
        if (null(cells[1])) continue;
        const double mtm = std::get<double>(cells[1]);
        double profit = null(cells[2]) ? 0.0 : std::get<double>(cells[2]);
        if (last != rows.rend() && raw == rows.size()-1-static_cast<std::size_t>(last-rows.rbegin())
            && open_count && std::abs(profit) <= 1e-9) profit = open_pnl;
        const double book = mtm - profit;
        std::string w(40, '\0');
        put(w,0,raw,8); put(w,8,numeric(cells[0]),8); put(w,16,word(mtm),8);
        if (std::isfinite(book)) put(w,24,word(book),8);
        put(w,32,std::isfinite(book) ? 3 : 1,4);
        witnesses.push_back(std::move(w)); values[0].push_back(mtm);
        values[1].push_back(std::isfinite(book) ? std::optional<double>(book) : std::nullopt);
    }
    std::vector<std::string> result;
    const auto n = witnesses.size(), buckets = std::min<std::size_t>(1200,n);
    for (std::size_t j = 0; j < buckets; ++j) {
        const auto begin = j*n/buckets, end = (j+1)*n/buckets;
        std::array<std::size_t,8> selected{};
        selected.fill(n);
        for (unsigned which = 0; which < 2; ++which) {
            selected[4*which] = begin; selected[4*which+3] = end-1;
            std::vector<std::size_t> finite;
            for (auto at = begin; at < end; ++at) if (values[which][at]) finite.push_back(at);
            std::stable_sort(finite.begin(), finite.end(), [&](auto a,auto b) { return *values[which][a] < *values[which][b]; });
            if (!finite.empty()) {
                selected[4*which+1] = finite.front();
                selected[4*which+2] = *std::find_if(finite.begin(), finite.end(),
                    [&](auto at) { return *values[which][at] == *values[which][finite.back()]; });
            }
        }
        auto union_set = std::vector<std::size_t>(selected.begin(),selected.end());
        std::sort(union_set.begin(),union_set.end());
        union_set.erase(std::unique(union_set.begin(),union_set.end()),union_set.end());
        if (union_set.back() == n) union_set.pop_back();
        require(union_set.size() <= 6, "independent figure union bound");
        std::string entry(256,'\0'); put(entry,0,union_set.size(),1);
        for (std::size_t k = 0; k < selected.size(); ++k)
            put(entry,8+k,selected[k] == n ? 255 : static_cast<std::size_t>(
                std::find(union_set.begin(),union_set.end(),selected[k])-union_set.begin()),1);
        for (std::size_t k = 0; k < union_set.size(); ++k) entry.replace(16+40*k,40,witnesses[union_set[k]]);
        result.push_back(std::move(entry));
    }
    return result;
}
struct ReplayEvent {
    enum class Kind { admission, block };
    Kind kind;
    char tag;
    std::size_t input = 0;
    std::uint64_t offset = 0;
    std::uint32_t length = 0;
    unsigned admissions = 1;
};
struct Built {
    std::string bytes, v1_header, summary;
    std::array<std::vector<std::string>, 8> descriptors;
    std::array<std::vector<Row>, 8> rows;
    std::vector<std::string> input_lines;
    std::vector<ReplayEvent> replay;
    std::uint64_t data_end = 0;
};
struct Writer {
    bool all = true, stored_digest = false, bad_merkle_padding = false, compact = false, regroup_blocks = false;
    bool export_replay = false;
    // The oversized decoded-budget fixture declares 65,536 O rows in its
    // header. Replay must admit that many INPUT rows to reach the budget
    // check, before the deliberately short body is ever decoded.
    unsigned replay_admissions = 1;
    std::string run_id = "reader-test", engine = PINEFORGE_VERSION_STRING;
    int level = 3;
    unsigned slots = 2, constants = 2;
    std::array<unsigned, 8> caps{{2,2,2,2,1,2,2,4}};
    unsigned soft_bytes = 4096;
    std::function<void(char,std::size_t,Encoding&)> change_column;
    std::function<void(char,std::string&)> change_column_shas;
    std::function<void(char,std::string&)> change_directory, change_bhead;
    std::function<void(char,std::uint64_t,std::string&)> change_descriptor;
    std::function<void(unsigned,std::vector<std::uint64_t>&)> change_rank;
    std::function<void(std::string&)> change_figure;
    std::function<void(unsigned,unsigned,std::string&)> change_page;
    std::function<char(char)> change_order;
    std::function<void(std::string&)> change_head, change_summary, change_root, change_tail;
    std::function<std::string(std::string_view)> summary_frame;
    std::function<void(std::string&)> change_summary_header;
    std::string summary = "{\"engine\":\"" PINEFORGE_VERSION_STRING "\",\"input\":{},\"applied_inputs\":{},"
        "\"applied_overrides\":{},\"applied_runtime\":{},\"elapsed_seconds\":-0.0,\"summary\":{},"
        "\"metrics\":{},\"diagnostics\":{},\"fingerprint\":{},"
        "\"report_window\":{\"startMs\":100,\"endMs\":1000,\"additive\":{\"kept\":true}},\"report_shape\":{}}";

    Built encode(const std::vector<Row>& input) const {
        if (export_replay) require(!regroup_blocks, "replay requires original block retirement order");
        Built b;
        auto defs = definitions(all, slots, constants);
        std::array<Definition, 8> schema;
        std::array<bool, 8> enabled{};
        std::string records = "[";
        for (const auto& d : defs) {
            const auto t = tags.find(d.tag); schema[t] = d; enabled[t] = true;
            if (records.size() > 1) records += ',';
            records += "{\"type\":\"" + std::string(1,d.tag) + "\"";
            for (const auto part : {0u,1u,2u}) {
                records += part == 0 ? ",\"fields\":[" : part == 1 ? ",\"types\":[" : ",\"units\":[";
                const auto& list = part == 0 ? d.names : part == 1 ? d.types : d.units;
                for (std::size_t i = 0; i < list.size(); ++i) {
                    if (i) records += ',';
                    records += sink::escaped_string(list[i]);
                }
                records += ']';
            }
            records += '}';
        }
        records += ']';
        schema[7] = {'I',{"tag"},{"order"},{""}}; enabled[7] = true;
        const std::string prefix = "{\"schema\":\"pf-report-sink\",\"version\":";
        const std::string rest = ",\"run_id\":" + sink::escaped_string(run_id) + ",\"engine\":" + sink::escaped_string(engine)
            + ",\"records\":" + records;
        b.v1_header = prefix + "1" + rest + "}\n";
        std::string head = prefix + "2" + rest + ",\"format\":{\"byte_order\":\"le\",\"codec\":\"zstd\",\"level\":"
            + std::to_string(level) + ",\"block_records\":{";
        bool comma = false;
        for (std::size_t t = 0; t < enabled.size(); ++t) if (enabled[t]) {
            if (comma) head += ',';
            comma = true; head += "\"" + std::string(1,tags[t]) + "\":" + std::to_string(caps[t]);
        }
        head += "},\"block_bytes\":" + std::to_string(soft_bytes) + "}}";
        if (change_head) change_head(head);
        b.bytes = std::string("\x89PFS2\r\n\x1a", 8) + little(head.size(), 4) + head;
        b.bytes += little(checksum(b.bytes), 4);
        std::array<std::vector<Row>, 8> open;
        std::array<std::uint64_t, 8> rec0{}, budgets{};
        std::array<sink::Sha256, 8> streams;
        std::uint64_t finite_e = 0;
        for (std::size_t t = 0; t < streams.size(); ++t)
            streams[t].update(std::string("PFS2-STREAM\0", 12) + std::string(1, tags[t]));
        auto close = [&](std::size_t t) {
            auto& rows = open[t]; if (rows.empty()) return;
            const auto& d = schema[t];
            std::string dir, shas, payload;
            std::uint64_t decoded = 32 + 48 * d.types.size();
            for (std::size_t c = 0; c < d.types.size(); ++c) {
                auto e = column(rows, d, c);
                const auto raw_len = e.bytes.size();
                if (change_column) change_column(d.tag, c, e);
                // A test codec frame's decoded length is the original raw length.
                const auto declared_raw = e.codec ? raw_len : e.bytes.size();
                std::string canonical_values = std::string("PFS2-COL\0", 9) + d.tag
                    + little(c, 4) + little(rec0[t], 8) + little(rows.size(), 4);
                for (const auto& row : rows) canonical_values += canonical(row.values[c], d.types[c]);
                dir += little(e.codec,1) + little(e.mode,1) + little(e.flags,1) + little(0,1)
                    + little(declared_raw,4) + little(e.bytes.size(),4) + little(checksum(canonical_values),4);
                shas += raw_hash(hash(canonical_values));
                payload += e.bytes; decoded += declared_raw;
            }
            if (change_directory) change_directory(d.tag, dir);
            if (change_column_shas) change_column_shas(d.tag, shas);
            std::string header(32, '\0'); header.replace(0, 4, "BLK2"); header[4] = d.tag;
            put(header,8,d.types.size(),4); put(header,12,rows.size(),4); put(header,16,rec0[t],8);
            put(header,24,dir.size()+shas.size()+payload.size(),4);
            if (change_bhead) change_bhead(d.tag, header);
            put(header,28,checksum(header.substr(0,28)+dir+shas),4);
            // Authenticate the declared header, including deliberate count mutations.
            const auto block_input = std::string("PFS2-BLOCK\0",11) + d.tag
                + header.substr(16,8) + header.substr(12,4) + shas;
            for (const auto& row : rows) {
                streams[t].update(canonical_row(row, d));
            }
            std::string descriptor(512, '\0');
            put(descriptor,0,b.bytes.size(),8); put(descriptor,8,header.size()+dir.size()+shas.size()+payload.size(),4);
            descriptor[12] = d.tag; put(descriptor,16,rows.size(),4); put(descriptor,20,decoded,4); put(descriptor,24,rec0[t],8);
            descriptor.replace(64,32,raw_hash(hash(block_input)));
            const int a[] = {0,2,1,3,-1,0,-1,-1}, clock_b[] = {-1,3,2,4,-1,-1,-1,-1};
            for (unsigned clock = 0; clock < 2; ++clock) {
                const int column_id = clock ? clock_b[t] : a[t];
                std::optional<std::int64_t> low, high;
                if (column_id >= 0) for (const auto& row : rows) {
                    const auto& cell = row.values[static_cast<std::size_t>(column_id)];
                    if (null(cell)) continue;
                    const auto value = std::get<std::int64_t>(cell);
                    if (!low || value < *low) low = value;
                    if (!high || value > *high) high = value;
                }
                if (low) {
                    descriptor[13] = static_cast<char>(static_cast<unsigned char>(descriptor[13]) | (1u << clock));
                    put(descriptor,32+16*clock,static_cast<std::uint64_t>(*low),8);
                    put(descriptor,40+16*clock,static_cast<std::uint64_t>(*high),8);
                }
            }
            if (d.tag == 'E') {
                put(descriptor,480,finite_e,8);
                descriptor[13] = static_cast<char>(static_cast<unsigned char>(descriptor[13]) | 4);
                std::vector<std::string> witnesses;
                std::array<std::vector<std::optional<double>>,2> values;
                for (std::size_t r = 0; r < rows.size(); ++r) {
                    std::string w(40, '\0'); unsigned mask = 0;
                    put(w,0,rec0[t]+r,8); put(w,8,numeric(rows[r].values[0]),8);
                    std::optional<double> mtm, book;
                    if (!null(rows[r].values[1])) {
                        mtm = std::get<double>(rows[r].values[1]); mask |= 1; put(w,16,word(*mtm),8);
                        const double v = *mtm - (null(rows[r].values[2]) ? 0.0 : std::get<double>(rows[r].values[2]));
                        if (std::isfinite(v)) { book = v; mask |= 2; put(w,24,word(v),8); }
                    }
                    put(w,32,mask,4); witnesses.push_back(w); values[0].push_back(mtm); values[1].push_back(book);
                }
                for (unsigned which = 0; which < 2; ++which) {
                    std::string s(192, '\0');
                    std::vector<std::size_t> finite;
                    for (std::size_t r = 0; r < rows.size(); ++r) if (values[which][r]) finite.push_back(r);
                    const auto first = std::find_if(values[0].begin(), values[0].end(), [](const auto& v) { return v.has_value(); });
                    const auto last = std::find_if(values[0].rbegin(), values[0].rend(), [](const auto& v) { return v.has_value(); });
                    put(s,0,finite.size(),4); put(s,4,finite.empty() ? first == values[0].end() ? 0 : 9 : 15,4);
                    if (first != values[0].end()) {
                        s.replace(8,40,witnesses[static_cast<std::size_t>(first-values[0].begin())]);
                        s.replace(128,40,witnesses[rows.size()-1-static_cast<std::size_t>(last-values[0].rbegin())]);
                    }
                    if (!finite.empty()) {
                        const auto low = *std::min_element(finite.begin(),finite.end(),[&](auto x,auto y) {
                            return *values[which][x] < *values[which][y]; });
                        const auto high = *std::max_element(finite.begin(),finite.end(),[&](auto x,auto y) {
                            return *values[which][x] < *values[which][y]; });
                        s.replace(48,40,witnesses[low]); s.replace(88,40,witnesses[high]);
                    }
                    descriptor.replace(96+192*which,192,s);
                }
                finite_e += static_cast<std::uint64_t>(std::count_if(values[0].begin(), values[0].end(),
                    [](const auto& v) { return v.has_value(); }));
            }
            if (change_descriptor) change_descriptor(d.tag, rec0[t], descriptor);
            b.descriptors[t].push_back(descriptor);
            if (export_replay) b.replay.push_back({ReplayEvent::Kind::block, d.tag, 0,
                static_cast<std::uint64_t>(b.bytes.size()),
                static_cast<std::uint32_t>(header.size()+dir.size()+shas.size()+payload.size())});
            b.bytes += header + dir + shas + payload;
            rec0[t] += rows.size(); budgets[t] = 0; rows.clear();
        };
        for (const auto& row : input) {
            const auto t = tags.find(row.tag);
            require(t < 7 && enabled[t], "test input absent tag");
            std::uint64_t cost = 0;
            for (std::size_t c = 0; c < row.values.size(); ++c) {
                if (schema[t].types[c].compare(0,3,"str") == 0) {
                    if (!null(row.values[c])) cost += std::get<std::string>(row.values[c]).size();
                } else cost += 8;
            }
            if (!open[t].empty() && budgets[t] + cost > soft_bytes) close(t);
            if (export_replay) {
                b.replay.push_back({ReplayEvent::Kind::admission, row.tag, b.input_lines.size(), 0, 0,
                                    replay_admissions});
                b.input_lines.push_back(input_line(row, schema[t]));
            }
            open[t].push_back(row); b.rows[t].push_back(row); budgets[t] += cost;
            const char order_tag = change_order ? change_order(row.tag) : row.tag;
            Row order{'I',{std::uint64_t(static_cast<unsigned char>(order_tag))}};
            open[7].push_back(order); b.rows[7].push_back(order);
            if (open[t].size() == caps[t]) close(t);
            if (open[7].size() == caps[7]) close(7);
        }
        for (std::size_t t = 0; t < open.size(); ++t) close(t);
        if (regroup_blocks) {
            std::string reordered=b.bytes.substr(0,16+static_cast<std::size_t>(get(b.bytes,8,4)));
            for (auto& type:b.descriptors) for (auto& descriptor:type) {
                const auto original=static_cast<std::size_t>(get(descriptor,0,8));
                const auto length=static_cast<std::size_t>(get(descriptor,8,4));
                put(descriptor,0,reordered.size(),8);
                reordered+=b.bytes.substr(original,length);
            }
            b.bytes=std::move(reordered);
        }
        const auto data_end = b.bytes.size();
        b.data_end = data_end;
        std::uint64_t closed_count = 0, open_count = 0;
        double open_pnl = 0.0;
        for (const auto& row : b.rows[1]) {
            if (std::get<bool>(row.values[14])) {
                ++open_count;
                if (!null(row.values[7])) open_pnl = open_pnl + std::get<double>(row.values[7]);
            } else ++closed_count;
        }
        auto metadata = summary;
        require(!metadata.empty() && metadata.back() == '}', "fixture metadata object");
        metadata.pop_back();
        metadata += ",\"sink_folds\":{\"closed_count\":" + std::to_string(closed_count)
            + ",\"open_at_end_count\":" + std::to_string(open_count) + ",\"open_at_end_pnl\":" + sink::f64(open_pnl) + "}}";
        if (change_summary) change_summary(metadata);
        b.summary = metadata;
        const auto metadata_hash = hash(metadata);
        const auto frame = summary_frame ? summary_frame(metadata) : raw_frame(metadata);
        auto sum_header = "SUM2" + little(frame.size(),4) + little(metadata.size(),4)
            + little(checksum(metadata),4) + little(0,4) + raw_hash(metadata_hash);
        if (change_summary_header) change_summary_header(sum_header);
        b.bytes += sum_header + frame;
        std::string root(compact ? 320 : 4096,'\0'); root.replace(0,4,"FTR2"); put(root,4,2,2); put(root,6,compact,2);
        put(root,8,data_end,8);
        if (!compact) put(root,16,data_end,8);
        put(root,compact ? 16 : 24,sum_header.size()+frame.size(),4);
        std::uint64_t nblocks = 0;
        for (std::size_t t = 0; t < streams.size(); ++t) {
            put(root,(compact ? 24 : 32)+8*t,b.rows[t].size(),8);
            if (!compact) root.replace(96+32*t,32,raw_hash(streams[t].digest()));
            nblocks += b.descriptors[t].size();
        }
        put(root,compact ? 20 : 28,nblocks,4);
        root.replace(compact ? 96 : 352,32,raw_hash(metadata_hash));
        root.replace(compact ? 128 : 384,32,raw_hash(hash(b.v1_header)));
        put(root,compact ? 88 : 416,finite_e,8);
        if (compact) {
            std::string descriptors;
            for (unsigned t = 0; t < 8; ++t) {
                put(root,208+4*t,b.descriptors[t].size(),4);
                for (const auto& d : b.descriptors[t]) {
                    auto entry = d.substr(0,96); entry[13] = static_cast<char>(static_cast<unsigned char>(entry[13]) & ~4u);
                    descriptors += entry;
                }
            }
            put(root,160,b.bytes.size(),8); put(root,168,descriptors.size(),8);
            root.replace(176,32,raw_hash(hash(std::string("PFS2-CINDEX\0",12)+descriptors)));
            b.bytes += descriptors;
        } else put(root,424,1200,4);
        const auto figure_rows = compact ? std::vector<std::string>{} : figures(b.rows[0],open_count,open_pnl);
        for (unsigned table = 0; !compact && table < 19; ++table) {
            std::vector<std::string> entries;
            const unsigned width = table < 8 ? 512 : table == 18 ? 256 : 4;
            if (table < 8) entries = b.descriptors[table];
            else if (table == 18) {
                entries = figure_rows;
                if (change_figure) for(auto& e:entries)change_figure(e);
            }
            else {
                const unsigned fields[] = {3,7,8,9,10};
                const auto c = fields[(table-8)/2];
                std::vector<std::uint64_t> positions(b.rows[1].size()); std::iota(positions.begin(),positions.end(),UINT64_C(0));
                std::stable_sort(positions.begin(),positions.end(),[&](auto x,auto y) {
                    const auto& a = b.rows[1][x].values[c]; const auto& z = b.rows[1][y].values[c];
                    if (null(a) != null(z)) return !null(a);
                    if (null(a)) return false;
                    double av = c == 3 ? static_cast<double>(std::get<std::int64_t>(a)) : std::get<double>(a);
                    double zv = c == 3 ? static_cast<double>(std::get<std::int64_t>(z)) : std::get<double>(z);
                    if (c == 10) { av = -std::abs(av); zv = -std::abs(zv); }
                    return (table & 1) ? av > zv : av < zv;
                });
                if (change_rank) change_rank(table, positions);
                for (auto p : positions) entries.push_back(little(p,4));
            }
            const auto capacity = 61440 / width;
            const auto pages_count = (entries.size()+capacity-1)/capacity;
            const auto locator = 512 + table*64;
            put(root,locator+12,width,4); put(root,locator+16,entries.size(),8);
            if (!pages_count) continue;
            put(root,locator,b.bytes.size(),8); put(root,locator+8,pages_count,4);
            put(root,locator+24,4096+width*(entries.size()-(pages_count-1)*capacity),4);
            put(root,locator+28,1,4);
            std::vector<std::string> pages(pages_count);
            unsigned depth = 0; std::size_t padded = 1; while (padded < pages_count) { padded *= 2; ++depth; }
            std::vector<std::vector<Hash>> levels(1);
            for (std::size_t p = 0; p < pages_count; ++p) {
                auto& page = pages[p]; page.assign(4096+width*std::min<std::size_t>(capacity,entries.size()-p*capacity),'\0');
                page.replace(0,4,"IDX2"); page[4] = static_cast<char>(table); page[6] = static_cast<char>(depth);
                put(page,8,p,4); put(page,12,std::min<std::size_t>(capacity,entries.size()-p*capacity),4);
                put(page,16,width,4); put(page,20,pages_count,4);
                for (std::size_t i = p*capacity; i < std::min(entries.size(),(p+1)*capacity); ++i)
                    page.replace(4096+(i-p*capacity)*width,width,entries[i]);
                levels[0].push_back(hash(std::string("PFS2-INDEX\0",11)+page.substr(0,24)+page.substr(4096)));
                page.replace(24,32,raw_hash(levels[0].back()));
            }
            while (levels[0].size() < padded) levels[0].push_back(levels[0].back());
            if (bad_merkle_padding && padded > pages_count) levels[0].back()[0] ^= 1;
            for (unsigned l = 0; l < depth; ++l) {
                std::vector<Hash> parents;
                for (std::size_t i = 0; i < levels[l].size(); i += 2)
                    parents.push_back(hash(std::string("PFS2-NODE\0",10)+raw_hash(levels[l][i])+raw_hash(levels[l][i+1])));
                levels.push_back(std::move(parents));
            }
            root.replace(locator+32,32,raw_hash(levels.back()[0]));
            for (std::size_t p = 0; p < pages_count; ++p) {
                auto& page = pages[p];
                for (unsigned l = 0; l < depth; ++l) page.replace(56+32*l,32,raw_hash(levels[l][(p>>l)^1]));
                if (change_page) change_page(table,static_cast<unsigned>(p),page);
                put(page,4092,checksum(page.substr(0,4092)+page.substr(4096)),4); b.bytes += page;
            }
        }
        if (change_root) change_root(root);
        put(root,root.size()-4,checksum(root.substr(0,root.size()-4)),4);
        const auto root_at = b.bytes.size(); b.bytes += root;
        std::string tail(160,'\0'); put(tail,0,root_at,8); put(tail,8,root.size(),4); put(tail,16,b.bytes.size()+160,8);
        tail.replace(24,32,raw_hash(hash(root)));
        std::string identity = std::string("PFS2-LOGICAL\0",13)+root.substr(compact ? 128 : 384,32)+root.substr(compact ? 96 : 352,32);
        for (unsigned t = 0; t < 8; ++t) identity += tags[t]+root.substr((compact ? 24 : 32)+8*t,8)
            +(compact ? raw_hash(streams[t].digest()) : root.substr(96+32*t,32));
        tail.replace(56,32,raw_hash(hash(identity)));
        if (stored_digest) { put(tail,12,1,4); tail.replace(88,32,raw_hash(hash(b.bytes))); }
        tail.replace(152,8,"PFS2END\n");
        if (change_tail) change_tail(tail);
        b.bytes += tail;
        return b;
    }
};

inline Cell i(std::int64_t n) { return n; }
inline Cell u(std::uint64_t n) { return n; }
inline Cell s(std::string text) { return text; }
inline Row equity(std::int64_t time, double value, double profit) { return {'E',{i(time),value,profit}}; }
inline Row trade(std::int64_t n, double pnl) {
    return {'T',{i(n),s(n%2 ? "long" : "short"),i(300-n*11),i(600-(n%3)*17),10.0,12.0,1.0,pnl,
        pnl/10.0,n%2 ? -0.0 : 0.0,3.0,0.25,i(n-1),i(n+1),n%2 == 0,u(static_cast<std::uint64_t>(n)),
        i(1),s("entry"),s("exit"),s("")}};
}
inline std::vector<Row> population() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<Row> rows{
        equity(100,10.0,1.0),
        trade(1,4.0),
        {'O',{i(0),std::monostate{},i(160),-0.0,nan}},
        {'V',{u(0),i(1),i(0),std::monostate{},i(160),i(0),i(2),i(1),nan,u(UINT64_MAX),std::monostate{}}},
        {'X',{i(100),i(-9007199254740991LL),s(std::string("q\"\\\n\0",5)+u8"雪🌲"),std::numeric_limits<double>::denorm_min()}},
        equity(100,10.0,0.0),
        {'H',{u(UINT64_C(0x8000000000000000))}},
        trade(2,-0.0),
        {'V',{u(9007199254740991ULL),i(-9007199254740991LL),i(1),i(100),std::monostate{},i(1),i(3),i(1),inf,u(0),s("")}},
        {'O',{i(1),i(100),std::monostate{},inf,-inf}},
        equity(200,-0.0,-0.0),
        trade(3,4.0),
        equity(300,0.0,0.0),
        {'H',{u(UINT64_MAX)}},
        equity(400,nan,0.0),
        trade(4,nan),
        {'O',{i(2),i(200),i(260),1e16,1e-5}},
        {'X',{i(-9007199254740991LL),i(9007199254740991LL),s(""),-0.0}},
        equity(500,20.0,10.0),
        trade(5,-4.0),
        equity(600,20.0,0.0),
        {'K',{nan,-0.0}},
        equity(700,std::numeric_limits<double>::max(),-std::numeric_limits<double>::max())};
    rows[1].values[4] = nan; rows[1].values[15] = u(9007199254740991ULL);
    rows[1].values[17] = s(std::string("id\0\t",4)+u8"é");
    rows[7].values[18] = s("exit\"\\\r");
    return rows;
}
} // namespace sink_v2_reader_fixture

