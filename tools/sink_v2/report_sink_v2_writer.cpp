#include "report_sink_v2_writer.hpp"

#include "report_sink_v2_codec.hpp"
#include "report_sink_v2_internal.hpp"
#include "report_sink_phase_log.hpp"
#include "report_sink_late_workers.hpp"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cfenv>
#include <exception>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_map>

namespace pineforge { namespace report_sink {
namespace {
using namespace v2;

class NearestRounding {
    int previous_ = std::fegetround();
public:
    NearestRounding() {
        if (previous_ == -1 || (previous_ != FE_TONEAREST && std::fesetround(FE_TONEAREST) != 0))
            throw BlockError(BlockFailure::invalid_state, "v2 requires round-to-nearest encoding");
    }
    ~NearestRounding() {
        if (previous_ != FE_TONEAREST) (void)std::fesetround(previous_);
    }
};

enum class Kind { sint, uint, f64, hex, boolean, str, order };
struct Spec { Kind kind; bool nullable = false; };
struct Value {
    std::uint64_t word = 0;
    std::string_view text;
    bool null = false;
};
Value integer(std::int64_t value, bool nullable = false) {
    if (nullable && value == INT64_MIN) return {0, {}, true};
    if (value < -static_cast<std::int64_t>(safe) || value > static_cast<std::int64_t>(safe))
        invalid("report sink i64 exceeds the exact JSON integer bound");
    return {static_cast<std::uint64_t>(value), {}, false};
}
Value unsigned_integer(std::uint64_t value) {
    if (value > safe) invalid("report sink u64 exceeds the exact JSON integer bound");
    return {value, {}, false};
}
Value floating(double value) { return {std::isfinite(value) ? bits(value) : 0, {}, !std::isfinite(value)}; }
Value string_value(std::string_view text, bool null = false) { return {0, text, null}; }

std::string sanitize(std::string_view text) {
    if (text.size() >= (1u << 20)) invalid("report sink line exceeds 1 MiB including LF");
    std::string out;
    out.reserve(text.size());
    for (std::size_t at = 0; at < text.size();) {
        const auto c = static_cast<unsigned char>(text[at]);
        std::size_t length = c < 128 ? 1 : c >= 0xc2 && c <= 0xdf ? 2
            : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        bool valid = length && length <= text.size() - at;
        for (std::size_t j = 1; valid && j < length; ++j) {
            const auto next = static_cast<unsigned char>(text[at + j]);
            valid = next >= 0x80 && next <= 0xbf && !(j == 1
                && ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0)
                    || (c == 0xf0 && next < 0x90) || (c == 0xf4 && next >= 0x90)));
        }
        if (valid) { out.append(text.data() + at, length); at += length; }
        else { out += "\xef\xbf\xbd"; ++at; }
        if (out.size() >= (1u << 20)) invalid("report sink line exceeds 1 MiB including LF");
    }
    return out;
}

bool present(unsigned tag, const RecordSet& set) {
    return tag == 2 || tag == 3 || tag == 4 ? set.outputs
        : tag == 5 ? set.trace : tag == 6 ? set.hashes : true;
}
std::vector<Spec> schema(unsigned tag, const RecordSet& set) {
    using K = Kind;
    switch (tag) {
    case 0: return {{K::sint}, {K::f64, true}, {K::f64, true}};
    case 1: return {{K::sint}, {K::str}, {K::sint}, {K::sint},
        {K::f64, true}, {K::f64, true}, {K::f64, true}, {K::f64, true},
        {K::f64, true}, {K::f64, true}, {K::f64, true}, {K::f64, true},
        {K::sint}, {K::sint}, {K::boolean}, {K::uint}, {K::sint},
        {K::str}, {K::str}, {K::str}};
    case 2: {
        std::vector<Spec> out(static_cast<std::size_t>(set.series_slots) + 3, {K::f64, true});
        out[0] = {K::sint}; out[1] = out[2] = {K::sint, true};
        return out;
    }
    case 3: return {{K::uint}, {K::sint}, {K::sint}, {K::sint, true}, {K::sint, true},
        {K::sint}, {K::sint}, {K::sint}, {K::f64, true}, {K::hex}, {K::str, true}};
    case 4: return std::vector<Spec>(static_cast<std::size_t>(set.constant_count), {K::f64, true});
    case 5: return {{K::sint}, {K::sint}, {K::str}, {K::f64, true}};
    case 6: return {{K::hex}};
    default: return {{K::order}};
    }
}

struct Column {
    Spec spec;
    // Numeric values use fixed 8-byte slots plus a validity byte for nullable
    // fields. Null slots are zeroed but excluded from canonical checksums.
    // STR stores canonical length-prefixed UTF-8 directly in a byte arena.
    Bytes data;
    std::size_t string_bytes = 0;
    std::uint32_t nulls = 0;
    explicit Column(Spec s) : spec(s) {}
    unsigned stride() const {
        return (spec.kind == Kind::boolean || spec.kind == Kind::order ? 1u : 8u)
            + (spec.nullable ? 1u : 0u);
    }
    Value numeric(std::uint32_t row) const {
        const auto* p = data.data() + static_cast<std::size_t>(row) * stride();
        const bool null = spec.nullable && *p++ == 0;
        return {get(p, stride() - (spec.nullable ? 1u : 0u)), {}, null};
    }
    std::string_view canonical(std::uint32_t row, std::size_t& cursor) const {
        if (spec.kind != Kind::str) {
            const auto* p = data.data() + static_cast<std::size_t>(row) * stride();
            return view(p, spec.nullable && *p == 0 ? 1 : stride());
        }
        const auto start = cursor;
        if (!spec.nullable || data[cursor++] != 0) {
            const auto length = get(data.data() + cursor, 4);
            cursor += 4 + static_cast<std::size_t>(length);
        }
        return view(data.data() + start, cursor - start);
    }
    template<class F> void strings(std::uint32_t n, F f) const {
        std::size_t at = 0;
        for (std::uint32_t row = 0; row < n; ++row) {
            const bool null = spec.nullable && data[at++] == 0;
            if (null) { f(row, std::string_view{}, true); continue; }
            const auto length = static_cast<std::size_t>(get(data.data() + at, 4));
            at += 4;
            f(row, view(data.data() + at, length), false);
            at += length;
        }
    }
};

struct Builder {
    unsigned tag;
    std::uint64_t rec0;
    std::uint32_t n = 0;
    std::size_t soft_bytes = 0;
    std::size_t budget = 0;
    std::vector<Column> columns;
    Builder(unsigned t, std::uint64_t first, const std::vector<Spec>& specs) : tag(t), rec0(first) {
        columns.reserve(specs.size());
        for (auto spec : specs) columns.emplace_back(spec);
        budget = 32 + 48 * specs.size();
    }
    std::size_t capacity() const {
        std::size_t total = 32 + 48 * columns.size();
        for (const auto& c : columns) total += c.data.capacity();
        return total;
    }
    void reserve(Column& column, std::size_t required) {
        if (required <= column.data.capacity()) return;
        if (required > column_limit) invalid("builder column exceeds hard limit");
        auto room = block_limit - capacity() + column.data.capacity();
        if (room < required) {
            // Release spare capacity before exact admission at the hard
            // ceiling. Reservation never causes an earlier block boundary.
            for (auto& c : columns)
                if (&c != &column && c.data.size() != c.data.capacity()) Bytes(c.data).swap(c.data);
            room = block_limit - capacity() + column.data.capacity();
        }
        if (room < required) invalid("builder reservation exceeds block limit");
        const auto desired = std::min({room, column_limit,
            std::max(required, std::max<std::size_t>(32, column.data.capacity() * 2))});
        column.data.reserve(desired);
    }
    template<class Get> std::size_t admitted_budget(Get value) const {
        const std::size_t count = static_cast<std::size_t>(n) + 1;
        std::size_t total = 32 + 48 * columns.size();
        for (std::size_t j = 0; j < columns.size(); ++j) {
            const auto& c = columns[j];
            const auto v = value(j);
            const std::size_t bytes = c.spec.kind == Kind::str
                ? 3 + 6 * count + c.string_bytes + (v.null ? 0 : v.text.size())
                : (c.spec.kind == Kind::boolean || c.spec.kind == Kind::order ? count : 9 * count);
            if (bytes > column_limit || bytes > block_limit - total) return block_limit + 1;
            total += bytes;
        }
        return total;
    }
    template<class Get> void add(Get value, std::size_t record_bytes, std::size_t admitted) {
        for (std::size_t j = 0; j < columns.size(); ++j) {
            auto& c = columns[j];
            const auto v = value(j);
            const auto required = c.data.size() + (c.spec.kind == Kind::str
                ? (c.spec.nullable ? 1u : 0u) + (v.null ? 0 : 4 + v.text.size()) : c.stride());
            reserve(c, required);
            if (c.spec.nullable) c.data.push_back(v.null ? 0 : 1);
            if (v.null) ++c.nulls;
            if (c.spec.kind == Kind::str) {
                if (!v.null) {
                    append(c.data, v.text.size(), 4);
                    c.data.insert(c.data.end(), v.text.begin(), v.text.end());
                    c.string_bytes += v.text.size();
                }
            } else append(c.data, v.word, c.stride() - (c.spec.nullable ? 1u : 0u));
        }
        ++n;
        soft_bytes += record_bytes;
        budget = admitted;
    }
};

constexpr double pow10[] = {1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0, 1000000.0,
    10000000.0, 100000000.0, 1000000000.0, 10000000000.0, 100000000000.0, 1000000000000.0};
bool decimal_fit(double value, unsigned k, std::int64_t& result) {
    if (k > 12) return false;
    const double scaled = mul64(value, pow10[k]);
    if (!std::isfinite(scaled) || std::fabs(scaled) >= 1125899906842624.0) return false;
    double rounded = std::floor(scaled);
    const double fraction = scaled - rounded;
    if (fraction > 0.5 || (fraction == 0.5 && std::fmod(rounded, 2.0) != 0.0)) rounded += 1.0;
    result = static_cast<std::int64_t>(rounded);
    if (result < -INT64_C(1125899906842623) || result > INT64_C(1125899906842623)) return false;
    const double restored = div64(static_cast<double>(result), pow10[k]);
    return bits(restored) == bits(value);
}

template<class Read> bool delta(Bytes& raw, std::uint32_t n, Read read) {
    const auto start = raw.size();
    bool first = true;
    std::int64_t initial = 0, previous = 0;
    std::uint64_t gcd = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto v = read(i);
        if (v.null) continue;
        const auto word = signed_word(v.word);
        if (word <= -INT64_C(4503599627370496) || word >= INT64_C(4503599627370496)) return false;
        if (first) { initial = word; first = false; }
        else {
            const auto difference = word - previous; // both strictly within 2^52
            const auto magnitude = static_cast<std::uint64_t>(difference < 0 ? -difference : difference);
            gcd = std::gcd(gcd, magnitude);
        }
        previous = word;
    }
    if (first) return false;
    if (!gcd) gcd = 1;
    varint(raw, zigzag(initial));
    varint(raw, gcd);
    first = true;
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto v = read(i);
        if (v.null) continue;
        const auto word = signed_word(v.word);
        if (!first) {
            const auto q = (word - previous) / static_cast<std::int64_t>(gcd);
            if (q <= -INT64_C(4503599627370496) || q >= INT64_C(4503599627370496)) {
                raw.resize(start);
                return false;
            }
            varint(raw, zigzag(q));
        }
        previous = word;
        first = false;
    }
    return true;
}

std::uint8_t encode_column(const Column& column, std::uint32_t n, Bytes& raw, std::size_t prefix) {
    raw.resize(prefix);
    const auto m = n - column.nulls;
    if (column.nulls) {
        raw.resize(prefix + (n + 7) / 8, 0);
        if (column.spec.kind == Kind::str) column.strings(n, [&](auto i, auto, bool null) {
            if (null) raw[prefix + (i >> 3)] |= static_cast<std::uint8_t>(1u << (i & 7));
        });
        else for (std::uint32_t i = 0; i < n; ++i)
            if (column.numeric(i).null) raw[prefix + (i >> 3)] |= static_cast<std::uint8_t>(1u << (i & 7));
    }
    const auto start = raw.size();
    auto read = [&](std::uint32_t i) { return column.numeric(i); };
    switch (column.spec.kind) {
    case Kind::sint: case Kind::uint:
        if (delta(raw, n, read) && raw.size() - start < 8u * m) return 1;
        raw.resize(start);
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto v = read(i);
            if (!v.null) append(raw, v.word, 8);
        }
        return 0;
    case Kind::f64: {
        bool fits = m != 0;
        unsigned k = 0;
        for (std::uint32_t i = 0; fits && i < n; ++i) {
            const auto v = read(i);
            if (v.null) continue;
            unsigned candidate = 0;
            std::int64_t scaled = 0;
            while (candidate <= 12 && !decimal_fit(number(v.word), candidate, scaled)) ++candidate;
            if (candidate == 13) fits = false;
            else k = std::max(k, candidate);
        }
        for (std::uint32_t i = 0; fits && i < n; ++i) {
            const auto v = read(i);
            std::int64_t scaled = 0;
            if (!v.null && !decimal_fit(number(v.word), k, scaled)) fits = false;
        }
        if (fits) {
            raw.push_back(static_cast<std::uint8_t>(k));
            const auto scaled = [&](std::uint32_t i) {
                const auto v = read(i);
                std::int64_t s = 0;
                if (!v.null && !decimal_fit(number(v.word), k, s)) invalid("decimal fit changed");
                return Value{static_cast<std::uint64_t>(s), {}, v.null};
            };
            if (delta(raw, n, scaled) && raw.size() - start < 8u * m) return 3;
        }
        raw.resize(start);
        [[fallthrough]];
    }
    case Kind::hex: {
        raw.resize(start + 8u * m, 0);
        std::uint64_t previous = 0;
        std::size_t j = 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto v = read(i);
            if (v.null) continue;
            const auto x = v.word ^ previous;
            for (unsigned p = 0; p < 8; ++p)
                raw[start + p * m + j] = static_cast<std::uint8_t>(x >> (8 * p));
            previous = v.word;
            ++j;
        }
        return 2;
    }
    case Kind::boolean: case Kind::order:
        for (std::uint32_t i = 0; i < n; ++i) raw.push_back(static_cast<std::uint8_t>(read(i).word));
        return 4;
    case Kind::str: {
        std::unordered_map<std::string_view, std::uint32_t> ids;
        std::vector<std::string_view> dictionary;
        ids.reserve(m);
        dictionary.reserve(m);
        column.strings(n, [&](auto, std::string_view value, bool null) {
            if (null) return;
            const auto next = static_cast<std::uint32_t>(dictionary.size());
            if (ids.emplace(value, next).second) dictionary.push_back(value);
        });
        if (2 * dictionary.size() <= m) {
            varint(raw, dictionary.size());
            for (const auto value : dictionary) varint(raw, value.size());
            for (const auto value : dictionary) raw.insert(raw.end(), value.begin(), value.end());
            column.strings(n, [&](auto, std::string_view value, bool null) {
                if (!null) varint(raw, ids.at(value));
            });
            return 5;
        }
        column.strings(n, [&](auto, std::string_view value, bool null) {
            if (!null) varint(raw, value.size());
        });
        column.strings(n, [&](auto, std::string_view value, bool null) {
            if (!null) raw.insert(raw.end(), value.begin(), value.end());
        });
        return 6;
    }
    }
    invalid("unknown column kind");
}

std::size_t json_size(const Spec& spec, const Value& value) {
    if (value.null) return 4;
    switch (spec.kind) {
    case Kind::f64: return f64(number(value.word)).size();
    case Kind::str: {
        // All builder strings are sanitized already. Count v1's exact escape
        // expansion without allocating a potentially sixfold JSON copy.
        std::size_t length = 2;
        for (const unsigned char c : value.text)
            length += c < 32 ? 6 : c == '"' || c == '\\' ? 2 : 1;
        return length;
    }
    case Kind::hex: return 18;
    case Kind::boolean: return value.word ? 4 : 5;
    default: return std::to_string(signed_word(value.word)).size();
    }
}

void policy_valid(const BlockPolicy& p) {
    if (p.zstd_level < 1 || p.zstd_level > 19 || p.queue_depth < 1 || p.queue_depth > 16
            || p.block_bytes < 4096 || p.block_bytes > column_limit || p.min_zstd_raw < 64
            || p.late_threads < 1 || p.late_threads > late_worker_limit)
        invalid("invalid block policy");
    for (unsigned i = 0; i < 8; ++i)
        if (!p.block_records[i] || p.block_records[i] > (i == 7 ? 1048576u : 65536u))
            invalid("invalid block record cap");
    if (p.block_records[4] != 1) invalid("K block cap must be one");
}

} // namespace

struct BlockWriter::State {
    enum class Phase { configured, writing, data_complete, complete, failed };
    std::string path, run_id, directory;
    BlockPolicy policy;
    Phase phase = Phase::configured;
    RecordSet records{};
    std::unique_ptr<ColumnCodec> codec;
    std::array<std::vector<Spec>, 8> schemas;
    std::array<std::unique_ptr<Builder>, 8> builders;
    std::array<std::uint64_t, 8> counts{};
    std::array<Sha256, 8> streams;
    std::array<Table, 19> tables;
    DecodeScratch scratch;
    Output output;
    Digest header_digest{}, summary_digest{};
    std::string summary;
    SummarySize summary_size{};
    std::uint64_t head_end = 0, data_end = 0;
    std::uint64_t finite_e_count = 0;
    pf_report_sink_receipt_v2 result{};
    double cumulative = 0.0, peak = 0.0;
    bool has_time = false;
    std::int64_t last_time = 0;
    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable ready, space;
    std::array<std::unique_ptr<Builder>, 16> queue;
    unsigned queue_head = 0, queue_size = 0;
    bool closing = false, stopped = false, worker_failed = false;
    std::exception_ptr worker_error;
    std::atomic<std::size_t> live_blocks{0}, peak_blocks{0}, max_budget{0}, workspace{0};
    std::atomic<std::uint64_t> waits{0}, blocks{0};
    std::size_t finalization_bytes = 0, decoder_bytes = 0;

    State(const pf_report_sink_v2& d, const BlockPolicy& p, std::unique_ptr<ColumnCodec> c)
        : policy(p), codec(std::move(c)) {
        static_assert(BlockPolicy::max_block_bytes == block_limit,
                      "data-block budgets use section 3.7's R");
        if (d.struct_size != sizeof d || d.version != 2 || !d.path || !*d.path
                || d.zstd_level < 0 || d.zstd_level > 19 || (d.flags & ~1u)
                || d.queue_depth > 16 || d.reserved)
            invalid("invalid report sink v2 descriptor");
        if (d.zstd_level) policy.zstd_level = d.zstd_level;
        if (d.flags & 1) policy.threaded = false;
        if (d.queue_depth) policy.queue_depth = d.queue_depth;
        policy_valid(policy);
        if (policy.observer && policy.threaded)
            invalid("block observation requires inline encoding");
        path = d.path;
        run_id = d.run_id ? d.run_id : "";
        if (d.scratch_dir) directory = d.scratch_dir;
        else {
            const auto slash = path.find_last_of('/');
            directory = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
        }
        result.struct_size = sizeof result;
        result.version = 2;
    }
    ~State() { stop(); }
    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopped = true;
            closing = true;
            for (auto& b : queue) if (b) { b.reset(); --live_blocks; }
            queue_size = 0;
        }
        ready.notify_all();
        space.notify_all();
        if (worker.joinable()) worker.join();
        for (auto& b : builders) if (b) { b.reset(); --live_blocks; }
        scratch = {};
        codec.reset();
    }
    void fail() noexcept {
        if (phase == Phase::complete) return;
        phase = Phase::failed;
        result.complete = result.data_complete = 0;
        result.sha256_hex[0] = '\0';
        result.file_bytes = 0;
        stop();
        output.invalidate(path, data_end);
    }
    void require_writing() {
        check_worker();
        if (phase != Phase::writing)
            throw BlockError(BlockFailure::invalid_state, "v2 sink is not accepting records");
    }
    void check_worker() {
        std::exception_ptr error;
        { std::lock_guard<std::mutex> lock(mutex); error = worker_error; }
        if (error) {
            try { std::rethrow_exception(error); }
            catch (const BlockError& e) { throw BlockError(e.reason(), e.what(), e.error_number()); }
            catch (const std::exception& e) { throw BlockError(BlockFailure::io, e.what()); }
            catch (...) { throw BlockError(BlockFailure::io, "v2 writer thread failed"); }
        }
    }
    static void high_water(std::atomic<std::size_t>& mark, std::size_t value) {
        auto old = mark.load();
        while (old < value && !mark.compare_exchange_weak(old, value)) {}
    }
    Builder& builder(unsigned tag) {
        if (!builders[tag]) {
            builders[tag] = std::make_unique<Builder>(tag, counts[tag], schemas[tag]);
            high_water(peak_blocks, ++live_blocks);
        }
        return *builders[tag];
    }
    void process(Builder&);
    void build_figure();
    Digest descriptors(bool compact, std::uint64_t table_offset);
    void worker_main() noexcept {
        try {
            for (;;) {
                std::unique_ptr<Builder> b;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    ready.wait(lock, [&] { return stopped || queue_size || closing; });
                    if (stopped || (!queue_size && closing)) return;
                    b = std::move(queue[queue_head]);
                    queue_head = (queue_head + 1) % policy.queue_depth;
                    --queue_size;
                }
                space.notify_one();
                try { process(*b); }
                catch (...) { b.reset(); --live_blocks; throw; }
                b.reset();
                --live_blocks;
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!worker_failed) worker_error = std::current_exception();
                worker_failed = true;
                stopped = true;
                for (auto& b : queue) if (b) { b.reset(); --live_blocks; }
                queue_size = 0;
            }
            ready.notify_all();
            space.notify_all();
        }
    }
    void close_block(unsigned tag) {
        if (!builders[tag] || !builders[tag]->n) return;
        if (result.threaded) {
            std::unique_lock<std::mutex> lock(mutex);
            if (queue_size == policy.queue_depth && !worker_failed && !stopped) ++waits;
            space.wait(lock, [&] { return worker_failed || stopped || queue_size < policy.queue_depth; });
            if (worker_failed || stopped) {
                lock.unlock();
                check_worker();
                throw BlockError(BlockFailure::io, "v2 writer stopped");
            }
            // No replacement builder is allocated while submit waits.
            queue[(queue_head + queue_size) % policy.queue_depth] = std::move(builders[tag]);
            ++queue_size;
            lock.unlock();
            ready.notify_one();
        } else {
            process(*builders[tag]);
            builders[tag].reset();
            --live_blocks;
        }
    }
    template<class Get> void record(unsigned tag, Get value) {
        require_writing();
        if (!present(tag, records)) invalid("report sink record absent from header");
        if (counts[tag] == safe || counts[7] == safe) invalid("report sink count exceeds bound");
        constexpr std::size_t line_limit = 1u << 20;
        std::size_t soft = 0, upper = 5 + schemas[tag].size();
        for (std::size_t j = 0; j < schemas[tag].size(); ++j) {
            const auto v = value(j);
            if (v.null && !schemas[tag][j].nullable) invalid("null in nonnullable column");
            if (schemas[tag][j].kind == Kind::str) {
                if (!v.null && v.text.size() >= line_limit)
                    invalid("report sink line exceeds 1 MiB including LF");
                soft += v.null ? 0 : v.text.size();
            } else soft += 8;
            const auto scalar_bound = schemas[tag][j].kind == Kind::str
                ? (v.null ? 4 : 2 + 6 * v.text.size()) : std::size_t{27};
            upper = scalar_bound >= line_limit - std::min(upper, line_limit)
                ? line_limit : upper + scalar_bound;
        }
        if (upper >= line_limit) {
            // X2: L excludes LF. ["t"] is five bytes, then one comma/field.
            std::size_t exact = 5 + schemas[tag].size();
            if (exact >= line_limit) invalid("report sink line exceeds 1 MiB including LF");
            for (std::size_t j = 0; j < schemas[tag].size(); ++j) {
                const auto length = json_size(schemas[tag][j], value(j));
                if (length >= line_limit - exact)
                    invalid("report sink line exceeds 1 MiB including LF");
                exact += length;
            }
        }
        auto* b = &builder(tag);
        auto admitted = b->admitted_budget(value);
        if (b->n && (b->n >= policy.block_records[tag]
                || b->soft_bytes + soft > policy.block_bytes || admitted > block_limit)) {
            close_block(tag);
            b = &builder(tag);
            admitted = b->admitted_budget(value);
        }
        if (admitted > block_limit) invalid("one report record exceeds decoded block budget");
        if (policy.observer) policy.observer->admitted(tags[tag]);
        b->add(value, soft, admitted);
        high_water(max_budget, admitted);
        ++counts[tag];
        auto order = [&](std::size_t) { return Value{static_cast<std::uint64_t>(tags[tag]), {}, false}; };
        auto& interleave = builder(7);
        const auto ib = interleave.admitted_budget(order);
        if (ib > block_limit) invalid("interleave block exceeds budget");
        interleave.add(order, 1, ib);
        ++counts[7];
        high_water(max_budget, ib);
        if (b->n == policy.block_records[tag]) close_block(tag);
        if (interleave.n == policy.block_records[7]) close_block(7);
    }
    template<class F> void guarded(F f) {
        try { f(); }
        catch (...) { fail(); throw; }
    }
};

namespace {
using Witness = std::array<std::uint8_t, 40>;
struct Series {
    std::array<Witness, 4> witnesses{};
    std::uint64_t count = 0;
    bool population = false;
    double minimum = 0.0, maximum = 0.0;
    void add(const Witness& witness, bool book) {
        if (!population) witnesses[0] = witness;
        witnesses[3] = witness;
        population = true;
        if (!(get(witness.data() + 32, 4) & (book ? 2 : 1))) return;
        const double value = number(get(witness.data() + (book ? 24 : 16), 8));
        if (!count || value < minimum) { minimum = value; witnesses[1] = witness; }
        if (!count || value > maximum) { maximum = value; witnesses[2] = witness; }
        ++count;
    }
    void write(std::uint8_t* out) const {
        put(out, count, 4); // only block summaries use this u32 field
        put(out + 4, count ? 15 : population ? 9 : 0, 4);
        for (unsigned i = 0; i < 4; ++i) std::memcpy(out + 8 + 40 * i, witnesses[i].data(), 40);
    }
};
Witness witness(const Frame& f, const DecodeScratch& scratch, unsigned row, double profit) {
    const auto mtm = numeric_at(scratch.columns[1], row);
    const double book = sub64(number(mtm.word), profit);
    Witness out{};
    put(out.data(), f.rec0 + row, 8);
    put(out.data() + 8, numeric_at(scratch.columns[0], row).word, 8);
    put(out.data() + 16, mtm.word, 8);
    put(out.data() + 24, std::isfinite(book) ? bits(book) : 0, 8);
    put(out.data() + 32, std::isfinite(book) ? 3 : 1, 4);
    return out;
}
void range(std::uint8_t* descriptor, const Bytes& column, unsigned n, unsigned which) {
    bool valid = false;
    std::int64_t lo = 0, hi = 0;
    for (unsigned row = 0; row < n; ++row) {
        const auto v = numeric_at(column, row);
        if (v.null) continue;
        const auto word = signed_word(v.word);
        if (!valid || word < lo) lo = word;
        if (!valid || word > hi) hi = word;
        valid = true;
    }
    if (valid) {
        descriptor[13] |= static_cast<std::uint8_t>(1u << which);
        put(descriptor + 32 + 16 * which, static_cast<std::uint64_t>(lo), 8);
        put(descriptor + 40 + 16 * which, static_cast<std::uint64_t>(hi), 8);
    }
}
} // namespace

Digest BlockWriter::State::descriptors(bool compact, std::uint64_t table_offset) {
    Sha256 compact_hash;
    compact_hash.update(std::string_view("PFS2-CINDEX\0", 12));
    std::uint64_t compact_ordinal = 0, finite = 0;
    for (unsigned tag = 0; tag < 8; ++tag) {
        if (!tables[tag].count) continue;
        std::uint64_t ordinal = 0, rec0 = 0;
        // At most eight framing passes; no descriptor directory/spool.
        for (auto at = head_end; at < data_end;) {
            std::array<std::uint8_t, 32> head{};
            output.read(at, head.data(), head.size());
            const auto length = 32 + get(head.data() + 24, 4);
            if (length > data_end - at || length > block_limit || length < 32)
                invalid("descriptor framing pass length");
            if (head[4] != static_cast<std::uint8_t>(tags[tag])) { at += length; continue; }
            const auto f = read_frame(output, at, data_end);
            if (f.rec0 != rec0 || f.ncols != schemas[tag].size()) invalid("descriptor rec0/schema");
            std::array<std::uint8_t, 512> descriptor{};
            put(descriptor.data(), at, 8);
            put(descriptor.data() + 8, f.length, 4);
            descriptor[12] = static_cast<std::uint8_t>(f.tag);
            put(descriptor.data() + 16, f.n, 4);
            put(descriptor.data() + 20, f.decoded, 4);
            put(descriptor.data() + 24, f.rec0, 8);
            std::memcpy(descriptor.data() + 64, f.digest.data(), 32);
            int a = -1, b = -1;
            if (tag == 0 || tag == 5) a = 0;
            if (tag == 1) { a = 2; b = 3; }
            if (tag == 2) { a = 1; b = 2; }
            if (tag == 3) { a = 3; b = 4; }
            if (a >= 0) {
                read_numeric(output, f, static_cast<unsigned>(a), tag == 2 || tag == 3, false, *codec, scratch, 0);
                range(descriptor.data(), scratch.columns[0], f.n, 0);
            }
            if (b >= 0) {
                read_numeric(output, f, static_cast<unsigned>(b), tag == 2 || tag == 3, false, *codec, scratch, 1);
                range(descriptor.data(), scratch.columns[1], f.n, 1);
            }
            if (tag == 0 && !compact) {
                read_numeric(output, f, 1, true, true, *codec, scratch, 1);
                read_numeric(output, f, 2, true, true, *codec, scratch, 2);
                Series mtm, book;
                for (unsigned row = 0; row < f.n; ++row) {
                    if (numeric_at(scratch.columns[1], row).null) continue;
                    const auto p = numeric_at(scratch.columns[2], row);
                    const auto w = witness(f, scratch, row, p.null ? 0.0 : number(p.word));
                    mtm.add(w, false);
                    book.add(w, true);
                }
                descriptor[13] |= 4;
                mtm.write(descriptor.data() + 96);
                book.write(descriptor.data() + 288);
                put(descriptor.data() + 480, finite, 8);
                finite += mtm.count;
            }
            if (compact) {
                output.rewrite(table_offset + 96 * compact_ordinal++, view(descriptor.data(), 96));
                compact_hash.update(view(descriptor.data(), 96));
            } else output.rewrite(entry_offset(tables[tag], ordinal), view(descriptor));
            rec0 += f.n;
            ++ordinal;
            at += length;
        }
        if (ordinal != tables[tag].count || rec0 != counts[tag]) invalid("descriptor totals");
    }
    if (!compact && finite != finite_e_count) invalid("finite E descriptor total");
    decoder_bytes = std::max(decoder_bytes, scratch.capacity());
    return compact_hash.digest();
}

void BlockWriter::State::build_figure() {
    auto& table = tables[18];
    if (!table.count) return;
    Series mtm, book;
    std::uint64_t finite = 0, bucket = 0;
    auto emit = [&] {
        std::array<std::uint8_t, 256> entry{};
        std::array<Witness, 8> selected{};
        std::array<bool, 8> exists{};
        for (unsigned j = 0; j < 8; ++j) {
            const auto& series = j < 4 ? mtm : book;
            const auto which = j % 4;
            exists[j] = which == 0 || which == 3 || series.count != 0;
            selected[j] = series.witnesses[which];
        }
        std::array<Witness, 8> sorted{};
        unsigned n = 0;
        for (unsigned j = 0; j < 8; ++j) if (exists[j]) sorted[n++] = selected[j];
        std::sort(sorted.begin(), sorted.begin() + n, [](const Witness& a, const Witness& b) {
            return get(a.data(), 8) < get(b.data(), 8);
        });
        n = static_cast<unsigned>(std::unique(sorted.begin(), sorted.begin() + n,
            [](const Witness& a, const Witness& b) { return get(a.data(), 8) == get(b.data(), 8); }) - sorted.begin());
        if (!n || n > 6) invalid("figure witness union");
        entry[0] = static_cast<std::uint8_t>(n);
        for (unsigned j = 0; j < n; ++j) std::memcpy(entry.data() + 16 + 40 * j, sorted[j].data(), 40);
        for (unsigned j = 0; j < 8; ++j) {
            entry[8 + j] = 0xff;
            if (exists[j]) for (unsigned slot = 0; slot < n; ++slot)
                if (get(sorted[slot].data(), 8) == get(selected[j].data(), 8))
                    entry[8 + j] = static_cast<std::uint8_t>(slot);
        }
        output.rewrite(entry_offset(table, bucket++), view(entry));
        mtm = {};
        book = {};
    };
    // Reuse the E descriptor population; no additional framing pass or spool.
    for (std::uint64_t i = 0; i < tables[0].count; ++i) {
        std::array<std::uint8_t, 12> descriptor{};
        output.read(entry_offset(tables[0], i), descriptor.data(), descriptor.size());
        const auto f = read_frame(output, get(descriptor.data(), 8), data_end);
        for (unsigned j = 0; j < 3; ++j) read_numeric(output, f, j, j != 0, j != 0, *codec, scratch, j);
        for (unsigned row = 0; row < f.n; ++row) {
            if (numeric_at(scratch.columns[1], row).null) continue;
            const auto p = numeric_at(scratch.columns[2], row);
            double profit = p.null ? 0.0 : number(p.word);
            if (finite + 1 == finite_e_count && result.open_at_end_count && std::fabs(profit) <= 1e-9)
                profit = result.open_at_end_pnl;
            const auto w = witness(f, scratch, row, profit);
            mtm.add(w, false);
            book.add(w, true);
            ++finite;
            // bucket<=1200 and safe N make this product fit u64.
            if (finite == (bucket + 1) * finite_e_count / table.count) emit();
        }
    }
    if (finite != finite_e_count || bucket != table.count) invalid("figure population total");
    decoder_bytes = std::max(decoder_bytes, scratch.capacity());
}

void BlockWriter::State::process(Builder& b) {
    // DECIMAL division, the E book subtraction and Python-compatible folds
    // all use nearest-even. Inline encoding restores the caller's FP mode.
    const NearestRounding rounding;
    if (blocks == UINT32_MAX) invalid("report sink block count exceeds u32");
    std::vector<std::size_t> cursors(b.columns.size(), 0);
    for (std::uint32_t row = 0; row < b.n; ++row) {
        for (std::size_t j = 0; j < b.columns.size(); ++j) {
            const auto bytes = b.columns[j].canonical(row, cursors[j]);
            streams[b.tag].update(bytes);
        }
        if (b.tag == 1) {
            for (unsigned j = 4; j <= 11; ++j)
                if (b.columns[j].numeric(row).null && !result.nonfinite_t_field)
                    result.nonfinite_t_field = j + 1;
            const auto pnl = b.columns[7].numeric(row);
            if (b.columns[14].numeric(row).word) {
                ++result.open_at_end_count;
                if (!pnl.null) result.open_at_end_pnl = add64(result.open_at_end_pnl, number(pnl.word));
            } else ++result.closed_count;
            if (!pnl.null) {
                const double value = number(pnl.word);
                if (b.rec0 + row == 0) result.best_trade = result.worst_trade = value;
                else {
                    if (value > result.best_trade) result.best_trade = value;
                    if (value < result.worst_trade) result.worst_trade = value;
                }
                result.wins += value > 0;
                result.losses += value < 0;
                cumulative = add64(cumulative, value);
                if (cumulative > peak) peak = cumulative;
                const double drawdown = sub64(cumulative, peak);
                if (drawdown < result.max_drawdown) result.max_drawdown = drawdown;
            }
        }
        if (b.tag == 0 && !b.columns[1].numeric(row).null) ++finite_e_count;
    }
    Bytes directory_bytes(16 * b.columns.size(), 0);
    Bytes column_hashes(32 * b.columns.size(), 0);
    auto& body = scratch.raw;
    auto& compressed = scratch.stored;
    body.clear();
    if (body.capacity() < b.budget) Bytes(b.budget).swap(body);
    body.clear();
    std::size_t decoded = 32 + directory_bytes.size() + column_hashes.size();
    for (std::size_t j = 0; j < b.columns.size(); ++j) {
        const auto& c = b.columns[j];
        // Explicit scratch ceiling; no encoder reserve follows run length.
        const auto start = body.size();
        const auto enc = encode_column(c, b.n, body, start);
        const auto raw_size = body.size() - start;
        if (raw_size > column_limit || raw_size > block_limit - decoded)
            invalid("encoded column exceeds decoded ceiling");
        decoded += raw_size;
        Crc checksum;
        Sha256 column_hash;
        checksum.update(std::string_view("PFS2-COL\0", 9));
        column_hash.update(std::string_view("PFS2-COL\0", 9));
        std::array<std::uint8_t, 17> prefix{};
        prefix[0] = static_cast<std::uint8_t>(tags[b.tag]);
        put(prefix.data() + 1, j, 4);
        put(prefix.data() + 5, b.rec0, 8);
        put(prefix.data() + 13, b.n, 4);
        checksum.update(view(prefix));
        column_hash.update(view(prefix));
        std::size_t cursor = 0;
        for (std::uint32_t row = 0; row < b.n; ++row) {
            const auto bytes = c.canonical(row, cursor);
            checksum.update(bytes);
            column_hash.update(bytes);
        }
        const auto column_digest = column_hash.digest();
        std::memcpy(column_hashes.data() + 32 * j, column_digest.data(), 32);
        bool use_zstd = false;
        if (raw_size >= policy.min_zstd_raw) {
            codec->compress_column(view(body.data() + start, raw_size), policy.zstd_level, compressed);
            high_water(workspace, codec->workspace_bytes());
            if (compressed.size() < raw_size) {
                const auto frame = codec->inspect_frame(view(compressed));
                if (frame.dictionary || frame.content_bytes != raw_size
                        || frame.window_bytes > column_limit || frame.frame_bytes != compressed.size())
                    invalid("codec emitted an invalid column frame");
                use_zstd = true;
            }
        }
        auto* entry = directory_bytes.data() + 16 * j;
        entry[0] = use_zstd ? 1 : 0;
        entry[1] = enc;
        entry[2] = c.nulls ? 1 : 0;
        put(entry + 4, raw_size, 4);
        put(entry + 8, use_zstd ? compressed.size() : raw_size, 4);
        put(entry + 12, checksum.digest(), 4);
        if (use_zstd) {
            std::copy(compressed.begin(), compressed.end(), body.begin() + static_cast<std::ptrdiff_t>(start));
            body.resize(start + compressed.size());
        }
    }
    const auto length = 32 + directory_bytes.size() + column_hashes.size() + body.size();
    if (length > block_limit) invalid("stored block exceeds ceiling");
    std::array<std::uint8_t, 32> head{};
    std::memcpy(head.data(), "BLK2", 4);
    head[4] = static_cast<std::uint8_t>(tags[b.tag]);
    put(head.data() + 8, b.columns.size(), 4);
    put(head.data() + 12, b.n, 4);
    put(head.data() + 16, b.rec0, 8);
    put(head.data() + 24, length - 32, 4);
    Crc framing;
    framing.update(view(head.data(), 28));
    framing.update(view(directory_bytes));
    framing.update(view(column_hashes));
    put(head.data() + 28, framing.digest(), 4);
    const auto block_offset = output.size();
    output.write(view(head));
    output.write(view(directory_bytes));
    output.write(view(column_hashes));
    output.write(view(body));
    if (policy.observer) {
        output.flush();
        policy.observer->block_written(output.fd(), block_offset, static_cast<std::uint32_t>(length),
                                      {{view(head), view(directory_bytes), view(column_hashes), view(body)}});
    }
    auto& table = tables[b.tag];
    ++table.count;
    ++blocks;
}

BlockWriter::BlockWriter(const pf_report_sink_v2& d, const BlockPolicy& p)
    : BlockWriter(d, p, {}) {}
BlockWriter::BlockWriter(const pf_report_sink_v2& d, const BlockPolicy& p, std::unique_ptr<ColumnCodec> codec)
    : state_(std::make_unique<State>(d, p, std::move(codec))) {}
BlockWriter::~BlockWriter() = default;

bool BlockWriter::accepts_records() const noexcept { return state_->phase == State::Phase::writing; }

std::unique_ptr<SinkWriter> copy_sink_configuration(const SinkWriter* writer) {
    return writer ? writer->fresh_configuration() : nullptr;
}
std::unique_ptr<SinkWriter> BlockWriter::fresh_configuration() const {
    const auto& s = *state_;
    const pf_report_sink_v2 d{sizeof d, 2, s.path.c_str(), s.run_id.c_str(), s.directory.c_str(),
        s.policy.zstd_level, s.policy.threaded ? 0u : 1u, s.policy.queue_depth, 0};
    auto policy = s.policy;
    policy.staging_fd = -1;
    policy.observer = nullptr;
    return std::make_unique<BlockWriter>(d, policy);
}
void BlockWriter::abort() noexcept {
    if (state_->phase != State::Phase::complete) state_->fail();
}

void BlockWriter::write_header(const RecordSet& records) {
    auto& s = *state_;
    s.guarded([&] {
        if (s.phase != State::Phase::configured)
            throw BlockError(BlockFailure::invalid_state, "v2 header already attempted");
        auto head = v1_header_json(s.run_id, records);
        if (head.size() >= (1u << 20)) invalid("report sink line exceeds 1 MiB including LF");
        Sha256 header_hash;
        header_hash.update(head);
        header_hash.update("\n");
        s.header_digest = header_hash.digest();
        const auto version = head.find("\"version\":1");
        head[version + 10] = '2';
        head.pop_back();
        head += ",\"format\":{\"byte_order\":\"le\",\"codec\":\"zstd\",\"level\":"
            + std::to_string(s.policy.zstd_level) + ",\"block_records\":{";
        bool first = true;
        for (unsigned i = 0; i < 8; ++i) {
            s.streams[i].update(std::string_view("PFS2-STREAM\0", 12));
            s.streams[i].update(view(&tags[i], 1));
            if (!present(i, records)) continue;
            s.schemas[i] = schema(i, records);
            if (!first) head += ',';
            first = false;
            head += '"'; head += tags[i]; head += "\":" + std::to_string(s.policy.block_records[i]);
        }
        head += "},\"block_bytes\":" + std::to_string(s.policy.block_bytes) + "}}";
        if (head.size() > (2u << 20)) invalid("v2 header exceeds hard limit");
        if (!s.codec) s.codec = make_column_codec(s.policy.zstd_level);
        s.output.limit(s.policy.max_output_bytes);
        s.output.open(s.path, s.policy.staging_fd);
        s.result.scratch_flags = scratch_backing(s.directory, s.policy.staging_fd);
        s.records = records;
        std::array<std::uint8_t, 12> prefix{{0x89, 'P', 'F', 'S', '2', '\r', '\n', 0x1a, 0, 0, 0, 0}};
        put(prefix.data() + 8, head.size(), 4);
        Crc checksum;
        checksum.update(view(prefix));
        checksum.update(head);
        std::array<std::uint8_t, 4> head_crc{};
        put(head_crc.data(), checksum.digest(), 4);
        s.output.write(view(prefix));
        s.output.write(head);
        s.output.write(view(head_crc));
        s.head_end = s.output.size();
        s.phase = State::Phase::writing;
        if (s.policy.threaded) {
            try {
                s.worker = std::thread([&s] { s.worker_main(); });
                s.result.threaded = 1;
            } catch (const std::system_error&) {
                // Section 10.3: lack of a thread is a supported inline fallback.
                s.result.threaded = 0;
            } catch (const std::bad_alloc&) {
                s.result.threaded = 0;
            }
        }
    });
}

void BlockWriter::write_e(const pf_equity_point_t& p) {
    auto& s = *state_;
    s.guarded([&] {
        s.require_writing();
        if (s.has_time && p.time_ms < s.last_time) invalid("report sink equity times are not ordered");
        const std::array<Value, 3> values{{integer(p.time_ms), floating(p.equity), floating(p.open_profit)}};
        s.record(0, [&](std::size_t j) { return values[j]; });
        s.has_time = true;
        s.last_time = p.time_ms;
    });
}
void BlockWriter::write_t(std::int64_t n, const pf_trade_t& t, std::uint64_t incarnation,
        std::int64_t cause, std::string_view entry, std::string_view exit, std::string_view comment) {
    auto& s = *state_;
    s.guarded([&] {
        s.require_writing();
        if (n != static_cast<std::int64_t>(s.counts[1]) + 1) invalid("noncontiguous report trade number");
        const auto a = sanitize(entry), b = sanitize(exit), c = sanitize(comment);
        const std::array<Value, 20> values{{integer(n), string_value(t.is_long ? "long" : "short"),
            integer(t.entry_time), integer(t.exit_time), floating(t.entry_price), floating(t.exit_price),
            floating(t.qty), floating(t.pnl), floating(t.pnl_pct), floating(t.max_runup),
            floating(t.max_drawdown), floating(t.commission), integer(t.entry_bar_index),
            integer(t.exit_bar_index), {t.open_at_end ? 1u : 0u, {}, false}, unsigned_integer(incarnation),
            integer(cause), string_value(a), string_value(b), string_value(c)}};
        s.record(1, [&](std::size_t j) { return values[j]; });
    });
}
void BlockWriter::write_o(std::int64_t row, std::int64_t open, std::int64_t close,
                          const double* values, std::int64_t count) {
    auto& s = *state_;
    s.guarded([&] {
        s.require_writing();
        if (row != static_cast<std::int64_t>(s.counts[2])) invalid("noncontiguous output row");
        if (count != s.records.series_slots || count < 0 || (count && !values))
            invalid("output values differ from header");
        const std::array<Value, 3> fixed{{integer(row), integer(open, true), integer(close, true)}};
        s.record(2, [&](std::size_t j) { return j < 3 ? fixed[j] : floating(values[j - 3]); });
    });
}
void BlockWriter::write_v(const pf_output_event_v1_t& e, std::string_view message) {
    auto& s = *state_;
    s.guarded([&] {
        const auto text = e.message ? sanitize(message) : std::string{};
        const std::array<Value, 11> values{{unsigned_integer(e.sequence), integer(e.output_index),
            integer(e.bar_index), integer(e.bar_open_ms, true), integer(e.bar_close_ms, true),
            integer(e.ordinal_in_bar), integer(e.phase), integer(e.confirmed), floating(e.value),
            {e.message_hash64, {}, false}, string_value(text, !e.message)}};
        s.record(3, [&](std::size_t j) { return values[j]; });
    });
}
void BlockWriter::write_k(const double* values, std::int64_t count) {
    auto& s = *state_;
    s.guarded([&] {
        s.require_writing();
        if (s.counts[4]) invalid("constants already written");
        if (count != s.records.constant_count || count < 0 || (count && !values))
            invalid("constant values differ from header");
        s.record(4, [&](std::size_t j) { return floating(values[j]); });
    });
}
void BlockWriter::write_x(const pf_trace_entry_t& e, std::string_view name) {
    auto& s = *state_;
    s.guarded([&] {
        const auto text = sanitize(name);
        const std::array<Value, 4> values{{integer(e.timestamp), integer(e.bar_index),
            string_value(text), floating(e.value)}};
        s.record(5, [&](std::size_t j) { return values[j]; });
    });
}
void BlockWriter::write_h(std::uint64_t hash) {
    auto& s = *state_;
    s.guarded([&] { s.record(6, [&](std::size_t) { return Value{hash, {}, false}; }); });
}

Trailer BlockWriter::write_trailer() {
    auto& s = *state_;
    if (s.phase != State::Phase::data_complete && s.phase != State::Phase::complete) s.guarded([&] {
        s.require_writing();
        for (unsigned i = 0; i < 8; ++i) s.close_block(i);
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.closing = true;
        }
        s.ready.notify_one();
        if (s.worker.joinable()) s.worker.join();
        s.check_worker();
        s.output.freeze_data();
        s.data_end = s.output.size();
        s.result.data_complete = 1;
        s.phase = State::Phase::data_complete;
        s.policy.observer = nullptr;
    });
    Trailer trailer{};
    trailer.count_e = static_cast<std::int64_t>(s.counts[0]);
    trailer.count_t = static_cast<std::int64_t>(s.counts[1]);
    trailer.count_o = static_cast<std::int64_t>(s.counts[2]);
    trailer.count_v = static_cast<std::int64_t>(s.counts[3]);
    trailer.count_k = static_cast<std::int64_t>(s.counts[4]);
    trailer.count_x = static_cast<std::int64_t>(s.counts[5]);
    trailer.count_h = static_cast<std::int64_t>(s.counts[6]);
    if (s.phase == State::Phase::complete) std::memcpy(trailer.sha256_hex, s.result.sha256_hex, 65);
    return trailer;
}

SummarySize BlockWriter::finalize(std::string_view metadata) {
    auto& s = *state_;
    if (s.phase == State::Phase::complete) {
        if (metadata == s.summary) return s.summary_size;
        throw BlockError(BlockFailure::invalid_state, "changed metadata after v2 seal");
    }
    if (s.phase != State::Phase::data_complete)
        throw BlockError(BlockFailure::invalid_state, "v2 finalization requires data completion");
    s.guarded([&] {
        // The J guard precedes parser/buffer allocation; the complete 9.1
        // grammar and receipt folds are checked before compression or output.
        check_summary_json_length(metadata.size());
        const NearestRounding rounding;
        const auto folds = validate_summary(metadata);
        if (folds.closed_count != s.result.closed_count
                || folds.open_at_end_count != s.result.open_at_end_count
                || bits(folds.open_at_end_pnl) != bits(s.result.open_at_end_pnl))
            invalid("summary sink_folds differ from the receipt");
        s.summary.assign(metadata);
        s.summary_digest = sha(metadata);
        auto& frame = s.scratch.stored;
        s.codec->compress_summary(metadata, s.policy.zstd_level, frame);
        // Section 10.6 step 3: refuse before SUMMARY/index/root/tail output.
        // Failure leaves an invalid file with no valid tail and complete=0;
        // it is not a partial-file result.
        check_summary_stored_length(frame.size());
        if (frame.empty()) invalid("empty summary frame");
        const auto info = s.codec->inspect_frame(view(frame));
        if (info.content_bytes != metadata.size() || info.window_bytes > ColumnCodec::max_summary_json_bytes
                || info.dictionary || info.frame_bytes != frame.size())
            invalid("invalid summary frame");
        State::high_water(s.workspace, s.codec->workspace_bytes());
        s.decoder_bytes = std::max(s.decoder_bytes, s.scratch.capacity());
        const auto summary_length = 52 + frame.size();
        const SummarySize summary_size{static_cast<std::uint32_t>(frame.size()),
                                       static_cast<std::uint32_t>(metadata.size())};
        std::array<std::uint8_t, 52> header{};
        std::memcpy(header.data(), "SUM2", 4);
        put(header.data() + 4, frame.size(), 4);
        put(header.data() + 8, metadata.size(), 4);
        put(header.data() + 12, crc(metadata), 4);
        std::memcpy(header.data() + 20, s.summary_digest.data(), 32);
        s.output.write(view(header));
        s.output.write(view(frame));
        // Section 10.7: SUMMARY and index/rank storage occupy separate phases.
        Bytes{}.swap(frame);
        const bool compact = !s.policy.force_full && s.counts[0] < 10980 && s.counts[1] < 3314
            && s.blocks.load() <= 128;
        const auto descriptor_offset = s.output.size();
        auto root_offset = descriptor_offset;
        const auto root_length = compact ? 320u : 4096u;
        if (compact) root_offset += 96 * s.blocks.load();
        else {
            for (unsigned id = 8; id < 18; ++id) {
                s.tables[id].count = s.counts[1];
                s.tables[id].width = s.counts[1] <= (UINT64_C(1) << 32) ? 4 : 8;
            }
            s.tables[18].width = 256;
            s.tables[18].count = std::min<std::uint64_t>(1200, s.finite_e_count);
            for (auto& table : s.tables) plan_table(table, root_offset);
        }
        // Reserve only canonical footer regions. No tail/magic yet.
        s.output.reserve_footer(root_offset);
        auto arena = compact ? std::unique_ptr<FinalArena>{} : std::make_unique<FinalArena>();
        // Parallel control/stack reuses the two unused fixed-work-file
        // allowances in F; this implementation never creates those files.
        const auto control_lanes = compact ? 1u : std::min(3u, s.policy.late_threads);
        s.finalization_bytes = (compact ? 0 : sizeof(FinalArena))
            + control_lanes * final_control_bytes + final_overhead_bytes;
        if (s.finalization_bytes > final_allowance) invalid("finalization allowance exceeded");
        const auto descriptors_digest = s.descriptors(compact, descriptor_offset);
        State::high_water(s.workspace, s.codec->workspace_bytes());
        if (!compact) {
            if (s.policy.late_threads == 1) {
                for (unsigned id = 8; id < 18; ++id) {
                    build_rank(s.output, s.tables[id], s.tables[1], (id - 8) / 2, (id & 1) != 0,
                               *s.codec, s.scratch, rank_arena(*arena));
                    s.decoder_bytes = std::max(s.decoder_bytes, s.scratch.capacity());
                }
                s.build_figure();
            } else {
                const auto workers = std::min(3u, s.policy.late_threads);
                OrderedFailures<11> errors; // ranks 8..17, then ID18
                std::array<std::size_t, 2> decoded{}, workspaces{};
                const auto infrastructure = joined_workers(workers, [&](unsigned worker) {
                    if (!worker) {
                        errors.capture(10, [&] {
                            const NearestRounding worker_rounding;
                            s.build_figure();
                        });
                        return;
                    }
                    const auto lane = worker - 1, lanes = workers - 1;
                    DecodeScratch scratch;
                    std::unique_ptr<ColumnCodec> decoder;
                    for (unsigned direction = lane; direction < 10; direction += lanes) {
                        errors.capture(direction, [&] {
                            const NearestRounding worker_rounding;
                            if (!decoder) decoder = make_column_decoder();
                            build_rank(s.output, s.tables[8 + direction], s.tables[1], direction / 2,
                                       (direction & 1) != 0, *decoder, scratch,
                                       rank_arena(*arena, lane, lanes));
                        });
                        decoded[lane] = std::max(decoded[lane], scratch.capacity());
                        if (decoder) workspaces[lane] = decoder->workspace_bytes();
                    }
                });
                errors.rethrow(); // every worker has joined, including failures
                if (infrastructure) std::rethrow_exception(infrastructure);
                s.decoder_bytes = std::max(s.decoder_bytes,
                    s.scratch.capacity() + decoded[0] + decoded[1]);
                State::high_water(s.workspace,
                    s.codec->workspace_bytes() + workspaces[0] + workspaces[1]);
            }
            State::high_water(s.workspace, s.codec->workspace_bytes());
        }
        if (s.policy.phase_log) s.policy.phase_log->mark("rank_bucket_done");
        if (!compact) {
            for (unsigned id = 0; id < 19; ++id) seal_table(s.tables[id], id, s.output, *arena);
        }
        s.codec.reset();
        s.scratch = {};
        std::array<std::uint8_t, 4096> root{};
        std::memcpy(root.data(), "FTR2", 4);
        put(root.data() + 4, 2, 2);
        put(root.data() + 6, compact ? 1 : 0, 2);
        put(root.data() + 8, s.data_end, 8);
        if (compact) {
            put(root.data() + 16, summary_length, 4);
            put(root.data() + 20, s.blocks.load(), 4);
            put(root.data() + 88, s.finite_e_count, 8);
            std::memcpy(root.data() + 96, s.summary_digest.data(), 32);
            std::memcpy(root.data() + 128, s.header_digest.data(), 32);
            put(root.data() + 160, descriptor_offset, 8);
            put(root.data() + 168, 96 * s.blocks.load(), 8);
            std::memcpy(root.data() + 176, descriptors_digest.data(), 32);
            for (unsigned i = 0; i < 8; ++i) put(root.data() + 208 + 4 * i, s.tables[i].count, 4);
        } else {
            put(root.data() + 16, s.data_end, 8);
            put(root.data() + 24, summary_length, 4);
            put(root.data() + 28, s.blocks.load(), 4);
        }
        Sha256 logical;
        logical.update(std::string_view("PFS2-LOGICAL\0", 13));
        logical.update(view(s.header_digest));
        logical.update(view(s.summary_digest));
        for (unsigned i = 0; i < 8; ++i) {
            put(root.data() + (compact ? 24 : 32) + 8 * i, s.counts[i], 8);
            const auto digest = s.streams[i].digest();
            if (!compact) std::memcpy(root.data() + 96 + 32 * i, digest.data(), 32);
            std::array<std::uint8_t, 9> item{};
            item[0] = static_cast<std::uint8_t>(tags[i]);
            put(item.data() + 1, s.counts[i], 8);
            logical.update(view(item));
            logical.update(view(digest));
        }
        if (!compact) {
            std::memcpy(root.data() + 352, s.summary_digest.data(), 32);
            std::memcpy(root.data() + 384, s.header_digest.data(), 32);
            put(root.data() + 416, s.finite_e_count, 8);
            put(root.data() + 424, 1200, 4);
            for (unsigned id = 0; id < 19; ++id) {
                const auto& table = s.tables[id];
                auto* locator = root.data() + 512 + 64 * id;
                put(locator, table.offset, 8);
                put(locator + 8, table.pages, 4);
                put(locator + 12, table.width, 4);
                put(locator + 16, table.count, 8);
                put(locator + 24, table.last_page_bytes, 4);
                put(locator + 28, table.count ? 1 : 0, 4);
                std::memcpy(locator + 32, table.root.data(), 32);
            }
        }
        put(root.data() + root_length - 4, crc(view(root.data(), root_length - 4)), 4);
        const auto root_digest = sha(view(root.data(), root_length));
        s.output.write(view(root.data(), root_length));
        if (s.policy.phase_log) s.policy.phase_log->mark("seal_root_done");
        // No optional work files were created. Release the fixed arena before
        // hashing/sealing; an error anywhere above leaves no success tail.
        arena.reset();
        std::array<std::uint8_t, 160> tail{};
        put(tail.data(), root_offset, 8);
        put(tail.data() + 8, root_length, 4);
        put(tail.data() + 12, 1, 4);
        put(tail.data() + 16, root_offset + root_length + 160, 8);
        std::memcpy(tail.data() + 24, root_digest.data(), 32);
        const auto logical_digest = logical.digest(), stored_digest = s.output.stored_digest();
        if (s.policy.phase_log) s.policy.phase_log->mark("stored_digest_done");
        std::memcpy(tail.data() + 56, logical_digest.data(), 32);
        std::memcpy(tail.data() + 88, stored_digest.data(), 32);
        std::memcpy(tail.data() + 152, "PFS2END\n", 8);
        s.output.write(view(tail));
        s.output.close();
        if (s.policy.phase_log) s.policy.phase_log->mark("tail_written");
        const auto text = hex(logical_digest);
        std::memcpy(s.result.sha256_hex, text.c_str(), 65);
        s.result.file_bytes = static_cast<std::int64_t>(s.output.size());
        s.summary_size = summary_size;
        s.result.complete = 1;
        s.phase = State::Phase::complete;
    });
    return s.summary_size;
}

pf_report_sink_receipt_v2 BlockWriter::receipt() const noexcept {
    const auto& s = *state_;
    // The worker owns summary folds until the join. An in-run query exposes
    // only producer counts; reading a live fold would be a data race.
    pf_report_sink_receipt_v2 out{};
    if (s.phase == State::Phase::data_complete || s.phase == State::Phase::complete) out = s.result;
    out.struct_size = sizeof out;
    out.version = 2;
    out.count_e = static_cast<std::int64_t>(s.counts[0]);
    out.count_t = static_cast<std::int64_t>(s.counts[1]);
    out.count_o = static_cast<std::int64_t>(s.counts[2]);
    out.count_v = static_cast<std::int64_t>(s.counts[3]);
    out.count_k = static_cast<std::int64_t>(s.counts[4]);
    out.count_x = static_cast<std::int64_t>(s.counts[5]);
    out.count_h = static_cast<std::int64_t>(s.counts[6]);
    out.block_count = static_cast<std::int64_t>(s.blocks.load());
    out.threaded = s.result.threaded;
    out.scratch_flags = s.result.scratch_flags;
    return out;
}
BlockMemory BlockWriter::memory() const noexcept {
    const auto& s = *state_;
    std::size_t builders = 0;
    for (unsigned i = 0; i < 8; ++i) if (present(i, s.records)) ++builders;
    return {s.peak_blocks.load(), builders + (s.result.threaded ? s.policy.queue_depth + 1 : 0),
        s.max_budget.load(), s.workspace.load(), s.waits.load(),
        s.finalization_bytes, 0, s.decoder_bytes};
}

}} // namespace pineforge::report_sink
