#pragma once

// Deliberately test-local. No production v2 encoder, codec, layout helper or
// reference-reader decoder is included. Only the established SHA primitive is
// reused; framing, CRC, canonicalization and all column decoders live here.
#include "../src/sha256.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <charconv>

#if defined(PINEFORGE_WITH_ZSTD) && PINEFORGE_WITH_ZSTD
#include <zstd.h>
#endif

namespace sink2_test {
using Bytes = std::vector<std::uint8_t>;
using Digest = std::array<std::uint8_t, 32>;
constexpr std::string_view tags = "ETOVKXHI";
constexpr std::uint64_t safe = UINT64_C(9007199254740991);

inline void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
inline std::string_view bytes(const void* p, std::size_t n) { return {static_cast<const char*>(p), n}; }
inline std::string_view bytes(const Bytes& b) { return bytes(b.data(), b.size()); }
inline std::uint64_t load(const std::uint8_t* p, unsigned width) {
    std::uint64_t out = 0;
    for (unsigned i = 0; i < width; ++i) out += std::uint64_t{p[i]} << (8 * i);
    return out;
}
inline void store(Bytes& b, std::uint64_t word, unsigned width) {
    for (unsigned i = 0; i < width; ++i) b.push_back(static_cast<std::uint8_t>(word >> (8 * i)));
}
inline std::uint64_t word(double value) {
    std::uint64_t out;
    std::memcpy(&out, &value, 8);
    return out;
}
inline double real(std::uint64_t bits) {
    double out;
    std::memcpy(&out, &bits, 8);
    return out;
}
inline std::int64_t signed_value(std::uint64_t bits) {
    std::int64_t out;
    std::memcpy(&out, &bits, 8);
    return out;
}
inline std::uint32_t crc(std::string_view input) {
    std::uint32_t out = UINT32_C(0xffffffff);
    for (unsigned char c : input) {
        out ^= c;
        for (unsigned bit = 0; bit < 8; ++bit)
            out = (out & 1) ? (out >> 1) ^ UINT32_C(0xedb88320) : out >> 1;
    }
    return out ^ UINT32_C(0xffffffff);
}
inline Digest sha(std::string_view input) {
    pineforge::report_sink::Sha256 h;
    h.update(input);
    return h.digest();
}
inline bool digest_at(const Digest& digest, const std::uint8_t* at) {
    return std::equal(digest.begin(), digest.end(), at);
}
inline Bytes read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "cannot read writer output");
    return Bytes(std::istreambuf_iterator<char>(input), {});
}

struct Type { char kind; bool nullable = false; };
struct Value {
    std::uint64_t bits = 0;
    std::string text;
    bool null = false;
    bool operator==(const Value& other) const {
        return null == other.null && (null || (bits == other.bits && text == other.text));
    }
};
inline Value i(std::int64_t value) { return {static_cast<std::uint64_t>(value), {}, false}; }
inline Value u(std::uint64_t value) { return {value, {}, false}; }
inline Value f(double value) { return {std::isfinite(value) ? word(value) : 0, {}, !std::isfinite(value)}; }
inline Value s(std::string value) { return {0, std::move(value), false}; }
inline Value null() { return {0, {}, true}; }
inline Value time(std::int64_t value) { return value == INT64_MIN ? null() : i(value); }
using Schema = std::map<char, std::vector<Type>>;
struct Row {
    char tag;
    std::vector<Value> values;
    bool operator==(const Row& other) const { return tag == other.tag && values == other.values; }
};
inline Schema all_types(unsigned slots, unsigned constants) {
    Schema out{
        {'E', {{'i'}, {'f', true}, {'f', true}}},
        {'T', {{'i'}, {'s'}, {'i'}, {'i'}, {'f', true}, {'f', true}, {'f', true}, {'f', true},
               {'f', true}, {'f', true}, {'f', true}, {'f', true}, {'i'}, {'i'}, {'b'}, {'u'},
               {'i'}, {'s'}, {'s'}, {'s'}}},
        {'V', {{'u'}, {'i'}, {'i'}, {'i', true}, {'i', true}, {'i'}, {'i'}, {'i'},
               {'f', true}, {'h'}, {'s', true}}},
        {'X', {{'i'}, {'i'}, {'s'}, {'f', true}}}, {'H', {{'h'}}}, {'I', {{'c'}}}
    };
    out['O'] = {{'i'}, {'i', true}, {'i', true}};
    out['O'].insert(out['O'].end(), slots, {'f', true});
    out['K'] = std::vector<Type>(constants, {'f', true});
    return out;
}
inline Schema equity_types() {
    auto schema = all_types(0, 0);
    for (char t : std::string("OVKXH")) schema.erase(t);
    return schema;
}
inline Bytes canonical(const Value& value, Type type) {
    Bytes out;
    if (type.nullable) {
        out.push_back(value.null ? 0 : 1);
        if (value.null) return out;
    }
    require(!value.null, "nonnullable decoded null");
    if (type.kind == 's') {
        store(out, value.text.size(), 4);
        out.insert(out.end(), value.text.begin(), value.text.end());
    } else store(out, value.bits, type.kind == 'b' || type.kind == 'c' ? 1 : 8);
    return out;
}

class Cursor {
    const Bytes& b_;
public:
    std::size_t at = 0;
    explicit Cursor(const Bytes& b) : b_(b) {}
    std::uint64_t fixed(unsigned width) {
        require(at <= b_.size() && width <= b_.size() - at, "short column word");
        const auto out = load(b_.data() + at, width);
        at += width;
        return out;
    }
    std::string string(std::size_t length) {
        require(at <= b_.size() && length <= b_.size() - at, "short column string");
        std::string out(bytes(b_.data() + at, length));
        at += length;
        return out;
    }
    std::uint64_t varint() {
        std::uint64_t out = 0;
        for (unsigned n = 0; n < 8; ++n) {
            const auto b = fixed(1);
            out |= (b & 127) << (7 * n);
            if (!(b & 128)) {
                require((n == 0 || b != 0) && out <= safe, "bad LEB128");
                return out;
            }
        }
        throw std::runtime_error("long LEB128");
    }
    std::int64_t zz() {
        const auto z = varint();
        return (z & 1) ? -static_cast<std::int64_t>(z / 2) - 1 : static_cast<std::int64_t>(z / 2);
    }
    bool end() const { return at == b_.size(); }
};
inline std::vector<std::int64_t> deltas(Cursor& cursor, std::size_t count) {
    require(count != 0, "empty DELTA");
    std::vector<std::int64_t> out;
    out.push_back(cursor.zz());
    const auto gcd = cursor.varint();
    require(gcd != 0, "zero delta GCD");
    while (out.size() < count) {
        const auto q = cursor.zz();
        require(q > -INT64_C(4503599627370496) && q < INT64_C(4503599627370496), "delta q domain");
        const auto magnitude = static_cast<std::uint64_t>(q < 0 ? -q : q);
        require(!magnitude || gcd <= static_cast<std::uint64_t>(INT64_MAX) / magnitude, "delta product overflow");
        const auto product = static_cast<std::int64_t>(gcd * magnitude) * (q < 0 ? -1 : 1);
        const auto previous = out.back();
        require(product >= 0 ? previous <= INT64_MAX - product : previous >= INT64_MIN - product,
                "delta addition overflow");
        out.push_back(previous + product);
    }
    for (auto value : out)
        require(value > -INT64_C(4503599627370496) && value < INT64_C(4503599627370496), "delta value domain");
    return out;
}
inline std::vector<Value> column(const Bytes& raw, unsigned n, Type type, unsigned enc, unsigned flags) {
    require(flags <= 1 && (!flags || type.nullable), "bad column flags");
    Cursor cursor(raw);
    std::vector<bool> nulls(n, false);
    unsigned count = n;
    if (flags) {
        const auto bitmap = cursor.string((n + 7) / 8);
        for (unsigned row = 0; row < n; ++row) {
            nulls[row] = ((static_cast<unsigned char>(bitmap[row / 8]) >> (row % 8)) & 1) != 0;
            count -= nulls[row] ? 1u : 0u;
        }
        require(n % 8 == 0 || (static_cast<unsigned char>(bitmap.back()) >> (n % 8)) == 0, "bitmap padding");
    }
    std::vector<Value> nonnull(count);
    if (enc == 0) {
        require(type.kind == 'i' || type.kind == 'u', "bad PLAIN64");
        for (auto& v : nonnull) v.bits = cursor.fixed(8);
    } else if (enc == 1 || enc == 3) {
        require(enc == 1 ? type.kind == 'i' || type.kind == 'u' : type.kind == 'f', "bad DELTA/DECIMAL");
        const auto scale = enc == 3 ? cursor.fixed(1) : 0;
        require(scale <= 12, "decimal scale");
        constexpr double power[] = {1., 10., 100., 1000., 10000., 100000., 1000000.,
            10000000., 100000000., 1000000000., 10000000000., 100000000000., 1000000000000.};
        const auto values = deltas(cursor, count);
        for (unsigned j = 0; j < count; ++j) {
            if (enc == 3) {
                require(std::abs(values[j]) <= INT64_C(1125899906842623), "decimal integer domain");
                volatile double value = static_cast<double>(values[j]) / power[scale];
                nonnull[j].bits = word(value);
            } else nonnull[j].bits = static_cast<std::uint64_t>(values[j]);
        }
    } else if (enc == 2) {
        require(type.kind == 'f' || type.kind == 'h', "bad XORSHUF");
        std::vector<std::uint64_t> x(count, 0);
        for (unsigned plane = 0; plane < 8; ++plane)
            for (auto& value : x) value |= cursor.fixed(1) << (8 * plane);
        std::uint64_t previous = 0;
        for (unsigned j = 0; j < count; ++j) {
            previous ^= x[j];
            nonnull[j].bits = previous;
        }
    } else if (enc == 4) {
        require(type.kind == 'b' || type.kind == 'c', "bad BYTES");
        for (auto& value : nonnull) {
            value.bits = cursor.fixed(1);
            require(type.kind != 'b' || value.bits <= 1, "bad bool");
        }
    } else if (enc == 5 || enc == 6) {
        require(type.kind == 's', "bad string encoding");
        const auto distinct = enc == 5 ? cursor.varint() : count;
        require(distinct <= count, "dictionary count");
        std::vector<std::uint64_t> lengths;
        for (std::uint64_t j = 0; j < distinct; ++j) lengths.push_back(cursor.varint());
        std::vector<std::string> dictionary;
        for (auto length : lengths) dictionary.push_back(cursor.string(static_cast<std::size_t>(length)));
        for (unsigned j = 0; j < count; ++j) {
            const auto index = enc == 5 ? cursor.varint() : j;
            require(index < dictionary.size(), "dictionary index");
            nonnull[j].text = dictionary[static_cast<std::size_t>(index)];
        }
    } else throw std::runtime_error("unknown encoding");
    require(cursor.end(), "column trailing bytes");
    for (const auto& value : nonnull) {
        if (type.kind == 'i') require(signed_value(value.bits) >= -static_cast<std::int64_t>(safe)
            && signed_value(value.bits) <= static_cast<std::int64_t>(safe), "i64 domain");
        if (type.kind == 'u') require(value.bits <= safe, "u64 domain");
        if (type.kind == 'f') require(std::isfinite(real(value.bits)), "stored nonfinite");
    }
    std::vector<Value> out(n);
    unsigned next = 0;
    for (unsigned row = 0; row < n; ++row) out[row] = nulls[row] ? null() : nonnull[next++];
    return out;
}

struct Block {
    std::uint64_t offset, rec0;
    unsigned n, length, decoded;
    char tag;
    Digest digest;
    std::vector<Row> rows;
};
struct Decoded {
    std::vector<Row> merged;
    std::map<char, std::vector<Row>> by_type;
    std::vector<Block> blocks;
    std::array<bool, 7> encodings{};
    bool stored = false, compressed = false;
    std::string logical_hex, summary_json;
};

struct FrameHeader {
    std::uint64_t content, window;
    std::size_t bytes;
    bool checksum;
};
inline FrameHeader frame_header(std::string_view data, std::uint64_t window_limit) {
    std::size_t pos = 0;
    const auto take = [&](unsigned n) {
        require(n <= data.size() - pos, "short frame header");
        const auto value = load(reinterpret_cast<const std::uint8_t*>(data.data()) + pos, n);
        pos += n;
        return value;
    };
    require(take(4) == UINT32_C(0xfd2fb528), "frame magic");
    const auto flags = take(1);
    require((flags & 0x18) == 0, "frame reserved flags");
    require((flags & 3) == 0, "frame dictionary");
    const bool single = (flags & 0x20) != 0;
    std::uint64_t window = 0;
    if (!single) {
        const auto descriptor = take(1);
        const auto base = UINT64_C(1) << (10 + (descriptor >> 3));
        window = base + (base >> 3) * (descriptor & 7);
    }
    const unsigned width = (flags >> 6) ? 1u << (flags >> 6) : single ? 1u : 0u;
    require(width != 0, "frame content size absent");
    const auto content = take(width) + (width == 2 ? 256u : 0u);
    if (single) window = content;
    require(window <= window_limit, "frame window");
    return {content, window, pos, (flags & 4) != 0};
}
inline FrameHeader inspect_raw_frame(std::string_view data, std::uint64_t window_limit) {
    const auto header = frame_header(data, window_limit);
    std::size_t at = header.bytes;
    std::uint64_t decoded = 0;
    bool last = false;
    while (!last) {
        require(data.size() - at >= 3, "short raw frame block");
        const auto block = load(reinterpret_cast<const std::uint8_t*>(data.data()) + at, 3);
        at += 3;
        last = (block & 1) != 0;
        const auto kind = (block >> 1) & 3, length = block >> 3;
        require(kind <= 1 && length <= 131072, "test frame requires raw/RLE blocks");
        const auto stored = kind ? 1 : length;
        require(stored <= data.size() - at && decoded <= header.content
                && length <= header.content - decoded, "raw frame block length");
        at += static_cast<std::size_t>(stored);
        decoded += length;
    }
    if (header.checksum) {
        require(data.size() - at >= 4, "short frame checksum");
        at += 4;
    }
    require(at == data.size() && decoded == header.content, "frame trailing bytes/decoded length");
    return header;
}
inline Bytes inflate(std::string_view data, unsigned length, std::uint64_t window_limit = 16u << 20) {
    const auto header = frame_header(data, window_limit);
    require(header.content == length, "frame content size");
#if defined(PINEFORGE_WITH_ZSTD) && PINEFORGE_WITH_ZSTD
    require(ZSTD_getFrameContentSize(data.data(), data.size()) == length, "frame content size");
    require(ZSTD_getDictID_fromFrame(data.data(), data.size()) == 0, "frame dictionary");
    require(ZSTD_findFrameCompressedSize(data.data(), data.size()) == data.size(), "frame trailing bytes");
    Bytes raw(length);
    const auto actual = ZSTD_decompress(raw.data(), raw.size(), data.data(), data.size());
    require(!ZSTD_isError(actual) && actual == raw.size(), "zstd decode");
    return raw;
#else
    (void)inspect_raw_frame(data, window_limit);
    require(!header.checksum, "raw test decoder needs zstd for content checksum");
    Bytes raw(length);
    std::size_t at = header.bytes, out = 0;
    bool last = false;
    while (!last) {
        const auto block = load(reinterpret_cast<const std::uint8_t*>(data.data()) + at, 3);
        at += 3;
        last = (block & 1) != 0;
        const auto n = static_cast<std::size_t>(block >> 3);
        if ((block >> 1) & 3) {
            std::fill_n(raw.data() + out, n, static_cast<std::uint8_t>(data[at++]));
        } else {
            std::copy_n(data.data() + at, n, raw.data() + out);
            at += n;
        }
        out += n;
    }
    return raw;
#endif
}
inline void check_series(const std::uint8_t* summary, const Block& block, bool book) {
    std::array<Bytes, 4> witnesses;
    std::uint32_t finite = 0;
    double lo = 0, hi = 0;
    for (unsigned row = 0; row < block.n; ++row) {
        const auto& values = block.rows[row].values;
        const bool mtm_ok = !values[1].null;
        if (!mtm_ok) continue;
        volatile double value = real(values[1].bits) - (values[2].null ? 0.0 : real(values[2].bits));
        const bool book_ok = std::isfinite(value);
        Bytes witness;
        store(witness, block.rec0 + row, 8);
        store(witness, values[0].bits, 8);
        store(witness, mtm_ok ? values[1].bits : 0, 8);
        store(witness, book_ok ? word(value) : 0, 8);
        store(witness, (mtm_ok ? 1u : 0u) + (book_ok ? 2u : 0u), 4);
        store(witness, 0, 4);
        if (witnesses[0].empty()) witnesses[0] = witness;
        witnesses[3] = witness;
        if (!(book ? book_ok : mtm_ok)) continue;
        const double sample = book ? value : real(values[1].bits);
        if (!finite || sample < lo) { lo = sample; witnesses[1] = witness; }
        if (!finite || sample > hi) { hi = sample; witnesses[2] = witness; }
        ++finite;
    }
    require(load(summary, 4) == finite
            && load(summary + 4, 4) == (finite ? 15u : witnesses[0].empty() ? 0u : 9u), "E series flags/count");
    for (unsigned j = 0; j < 4; ++j) {
        if (witnesses[j].empty()) witnesses[j].resize(40, 0);
        require(std::equal(witnesses[j].begin(), witnesses[j].end(), summary + 8 + 40 * j), "E witness mismatch");
    }
    for (unsigned j = 168; j < 192; ++j) require(summary[j] == 0, "E summary padding");
}

struct Folds {
    std::uint64_t closed = 0, open = 0;
    double pnl = 0.0;
};
inline Folds reference_folds(const std::vector<Row>& trades) {
    Folds folds;
    for (const auto& row : trades) {
        if (row.values[14].bits) {
            ++folds.open;
            if (!row.values[7].null) {
                volatile double next = folds.pnl + real(row.values[7].bits);
                folds.pnl = next;
            }
        } else ++folds.closed;
    }
    return folds;
}
inline void check_folds(std::string_view summary, const Folds& folds) {
    const auto start = summary.find("\"sink_folds\":{");
    require(start != std::string_view::npos, "summary sink_folds absent");
    auto pos = start + std::string_view("\"sink_folds\":{").size();
    const std::string_view names[] = {"\"closed_count\":", ",\"open_at_end_count\":", ",\"open_at_end_pnl\":"};
    for (unsigned i = 0; i < 3; ++i) {
        require(summary.substr(pos, names[i].size()) == names[i], "summary fold member order");
        pos += names[i].size();
        if (i < 2) {
            std::uint64_t n = 0;
            const auto parsed = std::from_chars(summary.data() + pos, summary.data() + summary.size(), n);
            require(parsed.ec == std::errc{} && n == (i ? folds.open : folds.closed), "summary count fold");
            pos = static_cast<std::size_t>(parsed.ptr - summary.data());
        } else {
            double value = 0;
            const auto parsed = std::from_chars(summary.data() + pos, summary.data() + summary.size(), value);
            require(parsed.ec == std::errc{} && word(value) == word(folds.pnl), "summary open pnl fold");
            pos = static_cast<std::size_t>(parsed.ptr - summary.data());
        }
    }
    require(summary.substr(pos, 1) == "}", "summary fold closure");
}
// Brute-force bucket oracle deliberately retains the finite input population.
// It shares no writer summary/figure code and rebuilds all 256 entry bytes.
inline Bytes reference_figures(const std::vector<Row>& equity, const Folds& folds) {
    std::vector<std::size_t> finite;
    for (std::size_t i = 0; i < equity.size(); ++i) if (!equity[i].values[1].null) finite.push_back(i);
    const auto buckets = std::min<std::size_t>(1200, finite.size());
    Bytes out;
    for (std::size_t b = 0; b < buckets; ++b) {
        const auto begin = b * finite.size() / buckets, end = (b + 1) * finite.size() / buckets;
        std::vector<Bytes> samples;
        for (auto i = begin; i < end; ++i) {
            const auto ordinal = finite[i];
            const auto& row = equity[ordinal].values;
            double profit = row[2].null ? 0.0 : real(row[2].bits);
            if (i + 1 == finite.size() && folds.open && std::fabs(profit) <= 1e-9) profit = folds.pnl;
            volatile double book = real(row[1].bits) - profit;
            Bytes witness;
            store(witness, ordinal, 8); store(witness, row[0].bits, 8);
            store(witness, row[1].bits, 8); store(witness, std::isfinite(book) ? word(book) : 0, 8);
            store(witness, std::isfinite(book) ? 3 : 1, 4); store(witness, 0, 4);
            samples.push_back(std::move(witness));
        }
        std::array<std::size_t, 8> chosen{};
        std::array<bool, 8> exists{};
        for (unsigned series = 0; series < 2; ++series) {
            const auto base = 4 * series;
            chosen[base] = 0; chosen[base + 3] = samples.size() - 1;
            exists[base] = exists[base + 3] = true;
            for (std::size_t i = 0; i < samples.size(); ++i) {
                if (!(load(samples[i].data() + 32, 4) & (1u << series))) continue;
                for (unsigned extreme = 1; extreme <= 2; ++extreme) {
                    const auto field = base + extreme;
                    const double value = real(load(samples[i].data() + 16 + 8 * series, 8));
                    const double previous = real(load(samples[chosen[field]].data() + 16 + 8 * series, 8));
                    if (!exists[field] || (extreme == 1 ? value < previous : value > previous)) chosen[field] = i;
                    exists[field] = true;
                }
            }
        }
        std::vector<std::size_t> selected;
        for (unsigned i = 0; i < 8; ++i) if (exists[i]) selected.push_back(chosen[i]);
        std::sort(selected.begin(), selected.end());
        selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
        require(selected.size() <= 6, "oracle figure witness count");
        Bytes entry(256, 0);
        entry[0] = static_cast<std::uint8_t>(selected.size());
        for (unsigned i = 0; i < 8; ++i)
            entry[8 + i] = exists[i] ? static_cast<std::uint8_t>(
                std::find(selected.begin(), selected.end(), chosen[i]) - selected.begin()) : 0xff;
        for (std::size_t i = 0; i < selected.size(); ++i)
            std::copy(samples[selected[i]].begin(), samples[selected[i]].end(), entry.begin() + 16 + 40 * i);
        out.insert(out.end(), entry.begin(), entry.end());
    }
    return out;
}

inline Decoded decode(const Bytes& file, const Schema& schema, bool require_stored_prefix = true) {
    require(file.size() >= 160 + 320 + 16, "missing tail");
    const auto* tail = file.data() + file.size() - 160;
    require(std::memcmp(tail + 152, "PFS2END\n", 8) == 0, "missing tail magic");
    const auto root_at = load(tail, 8);
    const auto root_length = load(tail + 8, 4);
    require((root_length == 320 || root_length == 4096) && root_at <= file.size()
            && root_at + root_length + 160 == file.size()
            && load(tail + 16, 8) == file.size(), "tail lengths");
    const auto* root = file.data() + root_at;
    const bool compact = root_length == 320;
    require(std::memcmp(root, "FTR2", 4) == 0 && load(root + 4, 2) == 2
            && load(root + 6, 2) == (compact ? 1u : 0u), "root version/profile");
    require(crc(bytes(root, root_length - 4)) == load(root + root_length - 4, 4), "root CRC");
    require(digest_at(sha(bytes(root, root_length)), tail + 24), "root SHA");
    const auto* counts = root + (compact ? 24 : 32);
    const auto nblocks = load(root + (compact ? 20 : 28), 4);
    const auto finite_e_count = load(root + (compact ? 88 : 416), 8);
    if (compact) {
        require(load(counts, 8) < 10980 && load(counts + 8, 8) < 3314 && nblocks <= 128,
                "compact thresholds");
        require(std::all_of(root + 240, root + 316, [](auto b) { return b == 0; }), "compact root reserve");
    } else {
        require(load(root + 424, 4) == 1200, "figure buckets");
        require(std::all_of(root + 428, root + 512, [](auto b) { return b == 0; }), "full root reserve");
        require(std::all_of(root + 1728, root + 4092, [](auto b) { return b == 0; }), "full root padding");
    }
    const unsigned head_length = static_cast<unsigned>(load(file.data() + 8, 4));
    require(head_length && head_length <= (2u << 20) && head_length + 16 <= file.size(), "head length");
    const std::array<std::uint8_t, 8> magic{{0x89, 'P', 'F', 'S', '2', 13, 10, 26}};
    require(std::equal(magic.begin(), magic.end(), file.begin()), "head magic");
    require(crc(bytes(file.data(), 12 + head_length)) == load(file.data() + 12 + head_length, 4), "head CRC");
    const auto end = load(root + 8, 8);
    require((compact || end == load(root + 16, 8)) && end <= root_at, "data end");
    std::string head(bytes(file.data() + 12, head_length));
    const auto format = head.find(",\"format\":");
    require(format != std::string::npos, "format header absent");
    std::string v1 = head.substr(0, format) + "}\n";
    const auto version = v1.find("\"version\":2");
    require(version != std::string::npos, "version header absent");
    v1[version + 10] = '1';
    const auto* header_digest = root + (compact ? 128 : 384);
    require(digest_at(sha(v1), header_digest), "v1 header digest");
    require(end >= 16 + head_length && root_at - end >= 52, "summary absent");
    const auto* summary = file.data() + end;
    const auto stored_length = load(summary + 4, 4), json_length = load(summary + 8, 4);
    require(std::memcmp(summary, "SUM2", 4) == 0 && stored_length && stored_length <= 1048661 - 52
            && json_length && json_length <= (1u << 20) && load(summary + 16, 4) == 0
            && 52 + stored_length == load(root + (compact ? 16 : 24), 4)
            && 52 + stored_length <= root_at - end, "summary framing");
    const auto decoded_summary = inflate(bytes(summary + 52, static_cast<std::size_t>(stored_length)),
                                         static_cast<unsigned>(json_length), 1u << 20);
    const auto summary_bytes = bytes(decoded_summary);
    require(crc(summary_bytes) == load(summary + 12, 4), "summary CRC");
    const auto summary_digest = sha(summary_bytes);
    require(digest_at(summary_digest, summary + 20)
            && digest_at(summary_digest, root + (compact ? 96 : 352)), "summary SHA");
    Decoded out;
    out.summary_json.assign(summary_bytes);
    std::array<pineforge::report_sink::Sha256, 8> streams;
    for (unsigned j = 0; j < 8; ++j) {
        streams[j].update(std::string_view("PFS2-STREAM\0", 12));
        streams[j].update(tags.substr(j, 1));
    }
    for (std::uint64_t at = 16 + head_length; at < end;) {
        require(end - at >= 32, "short block");
        const auto* b = file.data() + at;
        require(std::memcmp(b, "BLK2", 4) == 0 && load(b + 5, 3) == 0, "block header");
        const char tag = static_cast<char>(b[4]);
        const auto types = schema.find(tag);
        require(types != schema.end(), "unexpected tag");
        const auto columns = static_cast<unsigned>(load(b + 8, 4));
        const auto n = static_cast<unsigned>(load(b + 12, 4));
        const auto rec0 = load(b + 16, 8);
        const auto length = static_cast<unsigned>(32 + load(b + 24, 4));
        require(n && columns == types->second.size() && length >= 32 + 48 * columns
                && length <= (32u << 20) && length <= end - at, "block length/count");
        require(rec0 == out.by_type[tag].size(), "block rec0");
        std::string framing(bytes(b, 28));
        framing.append(bytes(b + 32, 48 * columns));
        require(crc(framing) == load(b + 28, 4), "block header CRC");
        Block block{at, rec0, n, length, 32 + 48 * columns, tag, {}, {}};
        block.rows.resize(n, Row{tag, std::vector<Value>(columns)});
        std::size_t pos = 32 + 48 * columns;
        for (unsigned col = 0; col < columns; ++col) {
            const auto* dir = b + 32 + 16 * col;
            const auto raw_len = static_cast<unsigned>(load(dir + 4, 4));
            const auto stored_len = static_cast<unsigned>(load(dir + 8, 4));
            require(raw_len <= (16u << 20) && pos <= length && stored_len <= length - pos
                    && dir[0] <= 1 && dir[1] <= 6 && dir[3] == 0, "column directory");
            Bytes raw;
            if (dir[0] == 0) {
                require(raw_len == stored_len, "stored lengths");
                raw.assign(b + pos, b + pos + stored_len);
                out.stored = true;
            } else {
                raw = inflate(bytes(b + pos, stored_len), raw_len);
                out.compressed = true;
            }
            out.encodings[dir[1]] = true;
            const auto values = column(raw, n, types->second[col], dir[1], dir[2]);
            std::string canonical_bytes("PFS2-COL\0", 9);
            Bytes identity;
            identity.push_back(static_cast<std::uint8_t>(tag));
            store(identity, col, 4); store(identity, rec0, 8); store(identity, n, 4);
            canonical_bytes.append(bytes(identity));
            for (unsigned row = 0; row < n; ++row) {
                block.rows[row].values[col] = values[row];
                canonical_bytes.append(bytes(canonical(values[row], types->second[col])));
            }
            require(crc(canonical_bytes) == load(dir + 12, 4), "column CRC mismatch");
            require(digest_at(sha(canonical_bytes), b + 32 + 16 * columns + 32 * col), "column SHA mismatch");
            block.decoded += raw_len;
            pos += stored_len;
        }
        require(pos == length && block.decoded <= (32u << 20), "block body length");
        pineforge::report_sink::Sha256 block_sha;
        block_sha.update(std::string_view("PFS2-BLOCK\0", 11));
        Bytes identity{static_cast<std::uint8_t>(tag)};
        store(identity, rec0, 8); store(identity, n, 4);
        block_sha.update(bytes(identity));
        block_sha.update(bytes(b + 32 + 16 * columns, 32 * columns));
        const auto stream = tags.find(tag);
        require(stream != std::string_view::npos, "bad stream tag");
        for (const auto& row : block.rows)
            for (unsigned col = 0; col < columns; ++col) {
                const auto value = canonical(row.values[col], types->second[col]);
                streams[stream].update(bytes(value));
            }
        block.digest = block_sha.digest();
        out.by_type[tag].insert(out.by_type[tag].end(), block.rows.begin(), block.rows.end());
        out.blocks.push_back(std::move(block));
        at += length;
    }
    require(out.blocks.size() == nblocks, "root block count");
    for (unsigned j = 0; j < 8; ++j) {
        require(load(counts + 8 * j, 8) == out.by_type[tags[j]].size(), "root record count");
        if (!compact) require(digest_at(streams[j].digest(), root + 96 + 32 * j), "stream SHA");
    }
    std::map<char, std::size_t> next;
    for (const auto& row : out.by_type['I']) {
        const char tag = static_cast<char>(row.values[0].bits);
        require(tag != 'I' && next[tag] < out.by_type[tag].size(), "interleave multiplicity");
        out.merged.push_back(out.by_type[tag][next[tag]++]);
    }
    for (char tag : tags.substr(0, 7)) require(next[tag] == out.by_type[tag].size(), "missing I positions");
    const auto finite = static_cast<std::uint64_t>(std::count_if(out.by_type['E'].begin(), out.by_type['E'].end(),
        [](const Row& row) { return !row.values[1].null; }));
    require(finite == finite_e_count, "finite E total");
    const auto folds = reference_folds(out.by_type['T']);
    check_folds(summary_bytes, folds);
    const auto figures = compact ? Bytes{} : reference_figures(out.by_type['E'], folds);
    std::uint64_t table_end = end + 52 + stored_length;
    if (compact) {
        require(load(root + 160, 8) == table_end && load(root + 168, 8) == 96 * nblocks
                && table_end + 96 * nblocks == root_at, "compact descriptor tiling");
        std::string input("PFS2-CINDEX\0", 12);
        input.append(bytes(file.data() + table_end, static_cast<std::size_t>(96 * nblocks)));
        require(digest_at(sha(input), root + 176), "compact descriptor SHA");
    }
    for (unsigned id = 0; id < (compact ? 8u : 19u); ++id) {
        const auto* locator = compact ? nullptr : root + 512 + 64 * id;
        const auto count = compact ? load(root + 208 + 4 * id, 4) : load(locator + 16, 8);
        const auto offset = compact ? (count ? table_end : 0) : load(locator, 8);
        const auto pages = compact ? (count ? 1u : 0u) : load(locator + 8, 4);
        const auto width = compact ? 96u : load(locator + 12, 4);
        require(width == (compact ? 96u : id < 8 ? 512u : id == 18 ? 256u
                : out.by_type['T'].size() <= (UINT64_C(1) << 32) ? 4u : 8u),
                "index width");
        const auto capacity = compact ? 128u : 61440 / width;
        require(pages == (count + capacity - 1) / capacity, "index pages");
        const auto last_length = compact ? count * width : load(locator + 24, 4);
        const auto length = pages ? (compact ? count * width : (pages - 1) * 65536 + last_length) : 0;
        if (!compact) {
            require(load(locator + 28, 4) == (count ? 1u : 0u), "table PRESENT");
            require(last_length == (count ? 4096 + width * (count - (pages - 1) * capacity) : 0),
                    "last page length");
            if (id >= 8 && id < 18) require(count == out.by_type['T'].size(), "required FULL rank");
            if (id == 18) require(count == std::min<std::uint64_t>(1200, finite), "required FULL figure");
        }
        std::vector<std::uint64_t> ranks;
        std::size_t descriptor = 0;
        std::uint64_t finite_prefix = 0;
        for (std::uint64_t page_number = 0; page_number < pages; ++page_number) {
            require(offset == table_end && offset + length <= root_at, "table tiling");
            const auto* page = file.data() + offset + page_number * 65536;
            const auto used = std::min<std::uint64_t>(capacity, count - page_number * capacity);
            const auto page_length = compact ? length : page_number + 1 == pages ? last_length : 65536;
            unsigned depth = 0;
            if (!compact) {
            require(std::memcmp(page, "IDX2", 4) == 0 && page[4] == id
                    && page[5] == 0 && page[7] == 0
                    && load(page + 8, 4) == page_number && load(page + 16, 4) == width
                    && load(page + 20, 4) == pages, "index header");
            std::string page_crc(bytes(page, 4092));
            page_crc.append(bytes(page + 4096, page_length - 4096));
            require(crc(page_crc) == load(page + 4092, 4), "index CRC");
            std::string leaf("PFS2-INDEX\0", 11);
            leaf.append(bytes(page, 24)); leaf.append(bytes(page + 4096, page_length - 4096));
            auto digest = sha(leaf);
            require(digest_at(digest, page + 24), "index leaf");
            for (std::uint64_t n = 1; n < pages; n *= 2) ++depth;
            require(page[6] == depth, "index proof depth");
            for (unsigned level = 0; level < depth; ++level) {
                const auto sibling = bytes(page + 56 + level * 32, 32);
                std::string parent("PFS2-NODE\0", 10);
                if ((page_number >> level) & 1) {
                    parent.append(sibling); parent.append(bytes(digest.data(), 32));
                } else {
                    parent.append(bytes(digest.data(), 32)); parent.append(sibling);
                }
                digest = sha(parent);
            }
            require(digest_at(digest, locator + 32), "index Merkle root");
            require(load(page + 12, 4) == used, "index used entries");
            }
            for (std::uint64_t slot = 0; slot < used; ++slot) {
                const auto* entry = page + (compact ? 0 : 4096) + width * slot;
                if (id < 8) {
                    while (descriptor < out.blocks.size() && out.blocks[descriptor].tag != tags[id]) ++descriptor;
                    require(descriptor < out.blocks.size(), "extra descriptor");
                    const auto& block = out.blocks[descriptor++];
                    require(load(entry, 8) == block.offset && load(entry + 8, 4) == block.length
                        && entry[12] == static_cast<unsigned char>(block.tag) && load(entry + 16, 4) == block.n
                        && load(entry + 20, 4) == block.decoded && load(entry + 24, 8) == block.rec0
                        && digest_at(block.digest, entry + 64), "descriptor facts");
                    unsigned flags = block.tag == 'E' && !compact ? 4u : 0u;
                    int a = -1, b = -1;
                    if (block.tag == 'E' || block.tag == 'X') a = 0;
                    if (block.tag == 'T') { a = 2; b = 3; }
                    if (block.tag == 'O') { a = 1; b = 2; }
                    if (block.tag == 'V') { a = 3; b = 4; }
                    for (unsigned which = 0; which < 2; ++which) {
                        const int column = which ? b : a;
                        bool valid = false;
                        std::int64_t lo = 0, hi = 0;
                        if (column >= 0) for (const auto& row : block.rows) {
                            const auto& value = row.values[static_cast<std::size_t>(column)];
                            if (value.null) continue;
                            const auto key = signed_value(value.bits);
                            if (!valid || key < lo) lo = key;
                            if (!valid || key > hi) hi = key;
                            valid = true;
                        }
                        if (valid) flags |= 1u << which;
                        require(load(entry + 32 + 16 * which, 8) == static_cast<std::uint64_t>(lo)
                                && load(entry + 40 + 16 * which, 8) == static_cast<std::uint64_t>(hi),
                                "descriptor time ranges");
                    }
                    require(entry[13] == flags && load(entry + 14, 2) == 0, "descriptor flags");
                    if (!compact && block.tag == 'E') {
                        check_series(entry + 96, block, false);
                        check_series(entry + 288, block, true);
                        require(load(entry + 480, 8) == finite_prefix, "finite_prefix");
                        finite_prefix += load(entry + 96, 4);
                    } else if (!compact) require(std::all_of(entry + 96, entry + 488, [](auto v) { return v == 0; }),
                                   "non-E summaries");
                    if (!compact) require(std::all_of(entry + 488, entry + 512, [](auto v) { return v == 0; }),
                            "descriptor padding");
                } else if (id == 18) {
                    const auto ordinal = page_number * capacity + slot;
                    require(std::equal(entry, entry + 256, figures.data() + ordinal * 256), "figure brute-force bytes");
                } else ranks.push_back(load(entry, static_cast<unsigned>(width)));
            }
            for (std::uint64_t at = (compact ? 0 : 4096) + used * width; at < page_length; ++at)
                require(page[at] == 0, "index payload padding");
            for (unsigned at = 56 + 32 * depth; !compact && at < 4092; ++at)
                require(page[at] == 0, "index proof padding");
        }
        if (id < 8) {
            const auto actual = std::count_if(out.blocks.begin(), out.blocks.end(),
                [&](const Block& b) { return b.tag == tags[id]; });
            require(count == static_cast<std::uint64_t>(actual), "descriptor count");
        } else if (id != 18) {
            const auto& trades = out.by_type['T'];
            require(count == trades.size(), "rank count");
            constexpr unsigned fields[] = {3, 7, 8, 9, 10};
            const unsigned key = fields[(id - 8) / 2];
            const bool descending = (id % 2) != 0;
            std::vector<std::uint64_t> expected(trades.size());
            std::iota(expected.begin(), expected.end(), 0);
            std::stable_sort(expected.begin(), expected.end(), [&](auto a, auto b) {
                const auto& x = trades[a].values[key];
                const auto& y = trades[b].values[key];
                if (x.null != y.null) return !x.null;
                if (x.null) return false;
                if (key == 3) return descending ? signed_value(x.bits) > signed_value(y.bits)
                    : signed_value(x.bits) < signed_value(y.bits);
                const double akey = key == 10 ? -std::fabs(real(x.bits)) : real(x.bits);
                const double bkey = key == 10 ? -std::fabs(real(y.bits)) : real(y.bits);
                return descending ? akey > bkey : akey < bkey;
            });
            require(ranks == expected, "rank permutation/comparator");
        }
        if (pages) table_end += length;
        else if (!compact) require(offset == 0 && std::all_of(locator + 32, locator + 64,
                    [](auto byte) { return byte == 0; }), "empty table locator");
    }
    require(table_end == root_at, "index/root tiling");
    pineforge::report_sink::Sha256 logical;
    logical.update(std::string_view("PFS2-LOGICAL\0", 13));
    logical.update(bytes(header_digest, 32));
    logical.update(bytes(summary_digest.data(), 32));
    for (unsigned j = 0; j < 8; ++j) {
        logical.update(tags.substr(j, 1));
        logical.update(bytes(counts + 8 * j, 8));
        const auto stream = streams[j].digest();
        logical.update(bytes(stream.data(), 32));
    }
    require(digest_at(logical.digest(), tail + 56), "logical SHA");
    out.logical_hex = logical.hex_digest();
    const auto tail_flags = load(tail + 12, 4);
    require(tail_flags <= 1 && (!require_stored_prefix || tail_flags == 1), "stored prefix SHA flags");
    if (tail_flags) require(digest_at(sha(bytes(file.data(), file.size() - 160)), tail + 88), "stored prefix SHA");
    else require(std::all_of(tail + 88, tail + 120, [](auto byte) { return byte == 0; }), "absent prefix digest");
    require(std::all_of(tail + 120, tail + 152, [](auto byte) { return byte == 0; }), "tail reserve");
    return out;
}
} // namespace sink2_test
