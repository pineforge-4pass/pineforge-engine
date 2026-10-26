#include "sink_v2_test_inputs.hpp"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cfenv>
#include <exception>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
using namespace sink2_test;

void round_trip_and_determinism() {
    Directory directory;
    sink::BlockPolicy policy;
    const unsigned caps[] = {2, 3, 2, 3, 1, 4, 5, 7};
    std::copy(std::begin(caps), std::end(caps), std::begin(policy.block_records));
    policy.block_bytes = 4096;
    Bytes first;
    for (unsigned run = 0; run < 3; ++run) {
        const auto path = directory.file(("stored-" + std::to_string(run)).c_str());
        auto d = descriptor(path, directory.path, run == 1);
        sink::BlockWriter writer(d, policy, std::make_unique<StoredCodec>());
        writer.write_header({true, true, true, 3, 2});
        const auto reference = feed_all(writer, 501);
        sealed(writer);
        const auto file = read_file(path);
        const auto decoded = decode(file, all_types(3, 2));
        require(decoded.merged == reference, "every-record stored round trip differs from input");
        require(decoded.stored && !decoded.compressed, "stored path was not exercised");
        require(std::all_of(decoded.encodings.begin(), decoded.encodings.end(), [](bool b) { return b; }),
                "not every column encoding was exercised");
        require(decoded.logical_hex == writer.receipt().sha256_hex, "logical receipt differs");
        if (run == 0) {
            first = file;
            auto corrupt = file;
            const auto root = load(file.data() + file.size() - 160, 8);
            const auto page_at = load(file.data() + root + 512, 8);
            require(corrupt[page_at + 6] > 0, "test did not produce a Merkle proof");
            corrupt[page_at + 56] ^= 1;
            std::string checked(bytes(corrupt.data() + page_at, 4092));
            const auto pages = load(file.data() + root + 520, 4);
            const auto length = pages == 1 ? load(file.data() + root + 536, 4) : 65536;
            checked.append(bytes(corrupt.data() + page_at + 4096, length - 4096));
            const auto checksum = crc(checked);
            for (unsigned byte = 0; byte < 4; ++byte)
                corrupt[page_at + 4092 + byte] = static_cast<std::uint8_t>(checksum >> (8 * byte));
            refuses([&] { (void)decode(corrupt, all_types(3, 2)); }, "Merkle");
        }
        else require(file == first, "stored runs or threaded/inline bytes differ");
        writer.finalize(metadata_for(writer.receipt()));
        require(read_file(path) == file, "identical finalization changed bytes");
        refuses([&] { writer.finalize(std::string(metadata) + " "); }, "changed metadata");
        require(read_file(path) == file && writer.receipt().complete == 1, "changed retry damaged sealed file");
    }

#if defined(PINEFORGE_WITH_ZSTD) && PINEFORGE_WITH_ZSTD
    Bytes compressed;
    std::string identity;
    for (unsigned run = 0; run < 2; ++run) {
        const auto path = directory.file(("zstd-" + std::to_string(run)).c_str());
        auto d = descriptor(path, directory.path, run != 0, run ? 4u : 1u);
        sink::BlockPolicy large;
        sink::BlockWriter writer(d, large);
        writer.write_header({true, true, true, 3, 2});
        const auto reference = feed_all(writer, 501);
        sealed(writer);
        const auto file = read_file(path);
        const auto decoded = decode(file, all_types(3, 2));
        require(decoded.merged == reference && decoded.compressed && decoded.stored,
                "real zstd and stored column round trip");
        if (!run) { compressed = file; identity = decoded.logical_hex; }
        else require(file == compressed && identity == decoded.logical_hex, "zstd threaded/inline differs");
    }
    require(decode(first, all_types(3, 2)).logical_hex == identity, "recompression changed logical identity");
#endif
}

void refusals_and_boundaries() {
    Directory directory;
    const auto path = directory.file("boundary");
    auto d = descriptor(path, directory.path);
    sink::BlockPolicy policy;
    policy.block_bytes = 4096;
    policy.block_records[5] = 64;
    sink::BlockWriter writer(d, policy, std::make_unique<StoredCodec>());
    writer.write_header({false, true, false, 0, 0});
    const std::string name(1000, 'x');
    const auto per_block = policy.block_bytes / (3 * 8 + name.size());
    std::vector<Row> input;
    for (unsigned j = 0; j < per_block + 2; ++j) {
        const pf_trace_entry_t entry{static_cast<std::int64_t>(j), 0, 0, -0.0};
        input.push_back({'X', {i(j), i(0), s(name), f(-0.0)}});
        writer.write_x(entry, name);
    }
    sealed(writer);
    auto schema = equity_types();
    schema['X'] = all_types(0, 0)['X'];
    auto file = read_file(path);
    const auto decoded = decode(file, schema);
    require(decoded.merged == input, "soft byte boundary lost a record");
    std::vector<unsigned> sizes;
    for (const auto& b : decoded.blocks) if (b.tag == 'X') sizes.push_back(b.n);
    require(sizes.size() == 2 && sizes[0] == per_block && sizes[1] == 2, "soft cap boundary is not exact");
    auto truncated = file;
    truncated.resize(truncated.size() - 160);
    refuses([&] { (void)decode(truncated, schema); }, "tail");
    // Change a decoded string byte in a stored column. Framing stays valid;
    // this must specifically fail the decoded column CRC before prefix SHA.
    for (const auto& b : decoded.blocks) if (b.tag == 'X') {
        const auto* head = file.data() + b.offset;
        auto at = static_cast<std::size_t>(b.offset + 32 + 4 * 48);
        at += static_cast<std::size_t>(load(head + 32 + 8, 4));
        at += static_cast<std::size_t>(load(head + 48 + 8, 4));
        const auto string_len = load(head + 64 + 8, 4);
        require(string_len > 10, "string column unavailable for CRC mutation");
        file[at + 6] ^= 1;
        break;
    }
    refuses([&] { (void)decode(file, schema); }, "column CRC");

    for (auto extreme : {INT64_MIN, INT64_MAX}) {
        const auto invalid_path = directory.file(extreme == INT64_MIN ? "min" : "max");
        auto bad = descriptor(invalid_path, directory.path);
        sink::BlockWriter w(bad, {}, std::make_unique<StoredCodec>());
        w.write_header({false, false, false, 0, 0});
        refuses([&] { w.write_e({extreme, 0.0, 0.0}); }, "integer bound");
        refuses([&] { (void)w.write_trailer(); });
        refuses([&] { (void)decode(read_file(invalid_path), equity_types()); });
    }
    {
        const auto p = directory.file("unsafe-u64");
        sink::BlockWriter w(descriptor(p, directory.path), {}, std::make_unique<StoredCodec>());
        w.write_header({true, false, false, 0, 0});
        pf_output_event_v1_t event{};
        event.sequence = UINT64_MAX;
        refuses([&] { w.write_v(event, {}); }, "integer bound");
        require(w.receipt().complete == 0, "unsafe u64 sealed");
    }
    {
        const auto p = directory.file("empty");
        sink::BlockWriter empty(descriptor(p, directory.path), {}, std::make_unique<StoredCodec>());
        empty.write_header({false, false, false, 0, 0});
        sealed(empty);
        const auto value = decode(read_file(p), equity_types());
        require(value.blocks.empty() && value.merged.empty(), "empty types acquired blocks");
    }
    {
        const auto p = directory.file("zero-column-K");
        sink::BlockWriter empty(descriptor(p, directory.path), {}, std::make_unique<StoredCodec>());
        empty.write_header({true, true, true, 0, 0});
        empty.write_k(nullptr, 0);
        sealed(empty);
        require(decode(read_file(p), all_types(0, 0)).merged == std::vector<Row>{{'K', {}}},
                "zero-column K round trip");
    }
    {
        const auto p = directory.file("all-null");
        sink::BlockWriter w(descriptor(p, directory.path), {}, std::make_unique<StoredCodec>());
        w.write_header({true, false, false, 0, 0});
        w.write_o(0, INT64_MIN, INT64_MIN, nullptr, 0);
        pf_output_event_v1_t event{};
        event.sequence = 1;
        event.bar_open_ms = event.bar_close_ms = INT64_MIN;
        event.value = std::numeric_limits<double>::quiet_NaN();
        event.message_hash64 = UINT64_MAX;
        w.write_v(event, {});
        w.write_k(nullptr, 0);
        sealed(w);
        const std::vector<Row> expected{
            {'O', {i(0), null(), null()}},
            {'V', {u(1), i(0), i(0), null(), null(), i(0), i(0), i(0), null(), u(UINT64_MAX), null()}},
            {'K', {}}};
        require(decode(read_file(p), all_types(0, 0)).merged == expected, "all-null column bodies");
    }
    {
        const auto p = directory.file("utf8");
        sink::BlockWriter text(descriptor(p, directory.path), {}, std::make_unique<StoredCodec>());
        text.write_header({false, true, false, 0, 0});
        const pf_trace_entry_t entry{0, 0, 0, 1.0};
        text.write_x(entry, std::string("\xff\xc0\xaf", 3));
        sealed(text);
        const auto value = decode(read_file(p), schema);
        require(value.merged == std::vector<Row>{{'X', {i(0), i(0), s(u8"���"), f(1.0)}}},
                "UTF-8 per-bad-byte replacement changed");
    }
    for (const auto text : {
            "{\"engine\":\"a\",\"engine\":\"b\"}",
            "{\"engine\":- 1}", "{\"engine\":\"\\ud800\"}",
            "{\"report_sink\":{}}"}) {
        const auto p = directory.file("invalid-summary");
        sink::BlockWriter w(descriptor(p, directory.path), {}, std::make_unique<StoredCodec>());
        w.write_header({false, false, false, 0, 0});
        w.write_trailer();
        refuses([&] { w.finalize(text); });
        require(w.receipt().complete == 0, "invalid summary sealed");
        refuses([&] { (void)decode(read_file(p), equity_types()); });
    }
}

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
};
class SlowCodec final : public StoredCodec {
    std::shared_ptr<Gate> gate_;
public:
    explicit SlowCodec(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}
    void compress_column(std::string_view raw, int level, Bytes& frame) override {
        {
            std::unique_lock<std::mutex> lock(gate_->mutex);
            if (!gate_->entered) {
                gate_->entered = true;
                gate_->changed.notify_all();
                gate_->changed.wait(lock, [&] { return gate_->released; });
            }
        }
        StoredCodec::compress_column(raw, level, frame);
    }
};

class FailedCodec final : public StoredCodec {
public:
    void compress_column(std::string_view, int, Bytes&) override {
        throw sink::BlockError(sink::BlockFailure::io, "injected compressor I/O failure");
    }
};
void worker_failure_and_summary_fold() {
    Directory directory;
    for (const bool threaded : {false, true}) {
        const auto path = directory.file(threaded ? "failed-thread" : "failed-inline");
        sink::BlockPolicy policy;
        policy.block_records[0] = 64;
        policy.block_records[7] = 128;
        sink::BlockWriter writer(descriptor(path, directory.path, threaded), policy, std::make_unique<FailedCodec>());
        writer.write_header({false, false, false, 0, 0});
        refuses([&] {
            std::vector<Row> reference;
            feed_equity(writer, reference, 512);
            writer.write_trailer();
        }, "injected");
        require(writer.receipt().complete == 0, "worker failure published completion");
        refuses([&] { (void)decode(read_file(path), equity_types()); }, "tail");
    }
    const auto path = directory.file("summary-fold");
    sink::BlockWriter writer(descriptor(path, directory.path, true), {}, std::make_unique<StoredCodec>());
    writer.write_header({false, false, false, 0, 0});
    const double pnls[] = {-0.0, 0.0, 5.0, -5.0, 1e16, 1.0, -1e16, -3.0, 7.0, -9.0};
    std::int64_t wins = 0, losses = 0, n = 0;
    double best = 0.0, worst = 0.0, cumulative = 0.0, peak = 0.0, drawdown = 0.0, open_pnl = 0.0;
    std::uint64_t closed = 0, open = 0;
    for (const double pnl : pnls) {
        pf_trade_t trade{};
        trade.entry_price = trade.exit_price = trade.qty = 1.0;
        trade.pnl = pnl;
        trade.open_at_end = n >= 2;
        if (trade.open_at_end) { ++open; volatile double next = open_pnl + pnl; open_pnl = next; }
        else ++closed;
        if (!n) best = worst = pnl;
        else { if (pnl > best) best = pnl; if (pnl < worst) worst = pnl; }
        wins += pnl > 0;
        losses += pnl < 0;
        cumulative = cumulative + pnl;
        if (cumulative > peak) peak = cumulative;
        if (cumulative - peak < drawdown) drawdown = cumulative - peak;
        ++n;
        writer.write_t(n, trade, static_cast<std::uint64_t>(n), 0, "", "", "");
    }
    sealed(writer);
    const auto receipt = writer.receipt();
    require(receipt.nonfinite_t_field == 0 && receipt.count_t == n && receipt.wins == wins
            && receipt.losses == losses && word(receipt.best_trade) == word(best)
            && word(receipt.worst_trade) == word(worst) && word(receipt.max_drawdown) == word(drawdown)
            && receipt.closed_count == closed && receipt.open_at_end_count == open
            && word(receipt.open_at_end_pnl) == word(open_pnl),
            "canonical-order summary fold differs from the input fold");
    (void)decode(read_file(path), equity_types());
}

class GatedFailure final : public StoredCodec {
    std::shared_ptr<Gate> gate_;
public:
    explicit GatedFailure(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}
    void compress_column(std::string_view, int, Bytes&) override {
        std::unique_lock<std::mutex> lock(gate_->mutex);
        gate_->entered = true;
        gate_->changed.notify_all();
        gate_->changed.wait(lock, [&] { return gate_->released; });
        throw sink::BlockError(sink::BlockFailure::io, "gated worker failure");
    }
};
void failure_wakes_blocked_submit() {
    Directory directory;
    const auto path = directory.file("blocked-failure");
    auto gate = std::make_shared<Gate>();
    sink::BlockPolicy policy;
    policy.queue_depth = 1;
    policy.block_records[0] = 64;
    policy.block_records[7] = 256;
    sink::BlockWriter writer(descriptor(path, directory.path, true), policy,
                             std::make_unique<GatedFailure>(gate));
    writer.write_header({false, false, false, 0, 0});
    std::exception_ptr error;
    std::thread producer([&] {
        try { for (unsigned i = 0; i < 1024; ++i) writer.write_e({i, 1.0, 0.0}); }
        catch (...) { error = std::current_exception(); }
    });
    bool entered;
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        entered = gate->changed.wait_for(lock, std::chrono::seconds(10), [&] { return gate->entered; });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (entered && !writer.memory().queue_waits && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool waiting = writer.memory().queue_waits != 0;
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->released = true;
    }
    gate->changed.notify_all();
    producer.join();
    require(entered && waiting && error != nullptr, "worker failure did not wake a saturated submit");
    try { std::rethrow_exception(error); }
    catch (const sink::BlockError& e) {
        require(e.reason() == sink::BlockFailure::io && std::string(e.what()).find("gated") != std::string::npos,
                "woken submit did not throw BlockError{io}");
    }
    refuses([&] { writer.write_trailer(); }, "gated");
    require(writer.receipt().complete == 0, "failed worker sealed");
    refuses([&] { (void)decode(read_file(path), equity_types()); }, "tail");
}

void finite_figures() {
    Directory directory;
    const auto path = directory.file("finite-figures");
    sink::BlockPolicy policy;
    policy.force_full = true;
    policy.block_records[0] = 4;
    sink::BlockWriter writer(descriptor(path, directory.path), policy, std::make_unique<StoredCodec>());
    writer.write_header({false, false, false, 0, 0});
    std::vector<Row> reference;
    for (unsigned row = 0; row < 2608; ++row) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double huge = std::numeric_limits<double>::max();
        const double mtm = row < 8 || row >= 2600 || row % 11 == 0 ? nan
            : row % 19 == 0 ? huge : 10000.0 + row % 7;
        const double profit = row % 19 == 0 ? -huge : row % 5 ? 0.0 : nan;
        const pf_equity_point_t point{static_cast<std::int64_t>(row / 2), mtm, profit};
        writer.write_e(point);
        reference.push_back({'E', {i(point.time_ms), f(mtm), f(profit)}});
    }
    // The final finite E at 2599 has zero profit; this fold changes its book.
    const double pnls[] = {1e16, 1.0, -1e16, 3.0};
    for (unsigned row = 0; row < 4; ++row) {
        pf_trade_t trade{};
        trade.open_at_end = 1; trade.pnl = pnls[row];
        writer.write_t(row + 1, trade, row + 1, 0, "", "", "");
        reference.push_back({'T', {i(row + 1), s("short"), i(0), i(0), f(0), f(0), f(0),
            f(trade.pnl), f(0), f(0), f(0), f(0), i(0), i(0), u(1), u(row + 1), i(0), s(""), s(""), s("")}});
    }
    std::vector<Row> e_input, t_input;
    for (const auto& row : reference) (row.tag == 'E' ? e_input : t_input).push_back(row);
    const auto expected_figures = reference_figures(e_input, reference_folds(t_input));
    sealed(writer);
    const auto file = read_file(path);
    const auto decoded = decode(file, equity_types());
    require(decoded.merged == reference, "finite filtering/figure pass changed raw records");
    const auto root = load(file.data() + file.size() - 160, 8);
    const auto* locator = file.data() + root + 512 + 64 * 18;
    const auto first = load(locator, 8), pages = load(locator + 8, 4);
    std::size_t checked = 0;
    for (std::uint64_t page = 0; page < pages; ++page) {
        const auto* p = file.data() + first + page * 65536;
        const auto n = load(p + 12, 4) * 256;
        require(std::equal(p + 4096, p + 4096 + n, expected_figures.data() + checked),
                "ID18 differs from brute-force INPUT E/T population");
        checked += n;
    }
    require(checked == expected_figures.size(), "ID18 input reference length");
}

void x2_decimal_header_and_column_sha() {
    Directory directory;
    constexpr std::size_t limit = 1u << 20;
    for (bool accepted : {true, false}) {
        const auto path = directory.file(accepted ? "line-ok" : "line-refused");
        sink::BlockWriter writer(descriptor(path, directory.path), {}, std::make_unique<StoredCodec>());
        writer.write_header({false, true, false, 0, 0});
        const std::string name(limit - (accepted ? 17 : 16), 'x'); // L = 16+name.size()
        const pf_trace_entry_t trace{0, 0, 0, 0.0};
        if (accepted) { writer.write_x(trace, name); sealed(writer); }
        else {
            refuses([&] { writer.write_x(trace, name); }, "report sink line exceeds 1 MiB including LF");
            refuses([&] { writer.write_trailer(); });
        }
        const auto v1path = directory.file(accepted ? "line-v1-ok" : "line-v1-refused");
        const pf_report_sink_v1 d{sizeof d, 1, v1path.c_str(), "", directory.path.c_str()};
        sink::LineWriter v1(d);
        v1.write_header({false, true, false, 0, 0});
        if (accepted) { v1.write_x(trace, name); v1.write_trailer(); }
        else refuses([&] { v1.write_x(trace, name); }, "report sink line exceeds 1 MiB including LF");
    }
    {
        const auto path = directory.file("multi-string-line");
        sink::BlockWriter writer(descriptor(path, directory.path), {}, std::make_unique<StoredCodec>());
        writer.write_header({false, false, false, 0, 0});
        const std::string text(400000, 'x');
        refuses([&] { writer.write_t(1, {}, 1, 0, text, text, text); }, "line exceeds");
    }
    {
        const auto path = directory.file("head-escape");
        auto d = descriptor(path, directory.path);
        d.run_id = "a\n\t\b\f\r\"\\台";
        sink::BlockWriter writer(d, {}, std::make_unique<StoredCodec>());
        writer.write_header({false, false, false, 0, 0});
        sealed(writer);
        const auto file = read_file(path);
        const std::string head(bytes(file.data() + 12, load(file.data() + 8, 4)));
        require(head.find("\"run_id\":\"a\\u000a\\u0009\\u0008\\u000c\\u000d\\\"\\\\台\"") != std::string::npos,
                "HEAD strings do not use v1 escapes");
        require(head.find("COMPACT") == std::string::npos && head.find("\"flags\"") == std::string::npos,
                "profile flag leaked into HEAD");
    }
    {
        const auto path = directory.file("head-line-refused");
        auto d = descriptor(path, directory.path);
        const std::string id(limit / 6, '\0');
        // A NUL-terminated ABI string cannot contain embedded NUL; controls
        // other than NUL still exercise the six-byte escape expansion.
        const std::string controls(id.size(), '\n');
        d.run_id = controls.c_str();
        sink::BlockWriter writer(d, {}, std::make_unique<StoredCodec>());
        refuses([&] { writer.write_header({false, false, false, 0, 0}); }, "line exceeds");
    }
    refuses([&] { (void)column(Bytes{13, 0, 1}, 1, {'f'}, 3, 0); }, "decimal scale");
    Bytes excessive{0}; // k=0, DELTA first value 2^50 (one above DECIMAL domain)
    auto value = UINT64_C(1) << 51;
    do { excessive.push_back(static_cast<std::uint8_t>((value & 127) | (value > 127 ? 128 : 0))); value >>= 7; }
    while (value);
    excessive.push_back(1); // GCD
    refuses([&] { (void)column(excessive, 1, {'f'}, 3, 0); }, "decimal integer");
    {
        const auto path = directory.file("decimal-endpoint");
        sink::BlockPolicy policy;
        policy.block_records[0] = 4;
        sink::BlockWriter writer(descriptor(path, directory.path), policy, std::make_unique<StoredCodec>());
        writer.write_header({false, false, false, 0, 0});
        for (unsigned row = 0; row < 8; ++row)
            writer.write_e({row, row < 4 ? 1125899906842623.0 : 1125899906842624.0, 0.0});
        sealed(writer);
        auto file = read_file(path);
        const auto decoded = decode(file, equity_types());
        std::vector<unsigned> enc;
        for (const auto& b : decoded.blocks) if (b.tag == 'E') enc.push_back(file[b.offset + 32 + 16 + 1]);
        require(enc == std::vector<unsigned>{3, 2}, "DECIMAL endpoint/fallback rule");
        const auto& block = decoded.blocks.front();
        // Alter SHA only and repair framing CRC; decoded CRC still agrees.
        file[block.offset + 32 + 3 * 16] ^= 1;
        std::string input(bytes(file.data() + block.offset, 28));
        input.append(bytes(file.data() + block.offset + 32, 3 * 48));
        const auto checksum = crc(input);
        for (unsigned j = 0; j < 4; ++j)
            file[block.offset + 28 + j] = static_cast<std::uint8_t>(checksum >> (8 * j));
        refuses([&] { (void)decode(file, equity_types()); }, "column SHA");
    }
}

sink::BlockMemory pressured(unsigned count) {
    Directory directory;
    const auto path = directory.file("back-pressure");
    auto gate = std::make_shared<Gate>();
    sink::BlockPolicy policy;
    policy.block_records[0] = 64;
    // Keep I open until the producer holds a third E block: first E is
    // gated in the worker, second is queued, third waits in its builder.
    policy.block_records[7] = 256;
    policy.queue_depth = 1;
    sink::BlockWriter writer(descriptor(path, directory.path, true), policy, std::make_unique<SlowCodec>(gate));
    writer.write_header({false, false, false, 0, 0});
    std::vector<Row> input;
    std::exception_ptr error;
    std::atomic<unsigned> returned{0};
    std::thread producer([&] {
        try {
            for (unsigned row = 0; row < count; ++row) {
                const pf_equity_point_t point{static_cast<std::int64_t>(row) * 60000,
                    10000.0 + static_cast<double>(row % 64) / 4.0, 0.0};
                input.push_back({'E', {i(point.time_ms), f(point.equity), f(point.open_profit)}});
                writer.write_e(point);
                ++returned;
            }
            sealed(writer);
        } catch (...) { error = std::current_exception(); }
    });
    bool entered;
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        entered = gate->changed.wait_for(lock, std::chrono::seconds(10), [&] { return gate->entered; });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (entered && !writer.memory().queue_waits && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const auto before = writer.memory();
    const bool blocked = returned.load() < count && before.queue_waits != 0;
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->released = true;
    }
    gate->changed.notify_all();
    producer.join();
    if (error) std::rethrow_exception(error);
    require(entered && blocked, "depth-one queue did not back-pressure the producer");
    require(writer.receipt().threaded == 1, "back-pressure test did not use a writer thread");
    const auto limit = 3u + policy.queue_depth + 1u; // E,T,I builders + queue + worker
    const auto memory = writer.memory();
    require(memory.block_limit == limit && memory.peak_blocks <= limit, "blocks in flight exceed policy");
    require(decode(read_file(path), equity_types()).merged == input, "back-pressure dropped or reordered input");
    return memory;
}

void v1_bytes() {
    Directory directory;
    const auto path = directory.file("v1-direct"), polymorphic = directory.file("v1-interface");
    const pf_report_sink_v1 d{sizeof d, 1, path.c_str(), "writer-input-reference", directory.path.c_str()};
    const pf_report_sink_v1 p{sizeof p, 1, polymorphic.c_str(), "writer-input-reference", directory.path.c_str()};
    {
        sink::LineWriter writer(d);
        writer.write_header({true, true, true, 3, 2});
        (void)feed_all(writer, 17, true);
        writer.write_trailer();
    }
    {
        std::unique_ptr<sink::SinkWriter> writer = std::make_unique<sink::LineSinkWriter>(p);
        writer->write_header({true, true, true, 3, 2});
        (void)feed_all(*writer, 17, true);
        writer->write_trailer();
    }
    require(read_file(path) == read_file(polymorphic), "v1 bytes changed through common interface");
}
void rounding_mode_identity() {
    Directory directory;
    Bytes previous;
    const int original = std::fegetround();
    require(original != -1, "test cannot inspect floating environment");
    for (bool threaded : {false, true}) {
        const auto path = directory.file(threaded ? "round-thread" : "round-inline");
        sink::BlockPolicy policy;
        policy.block_records[0] = 4;
        sink::BlockWriter writer(descriptor(path, directory.path, threaded), policy, std::make_unique<StoredCodec>());
        require(std::fesetround(FE_TONEAREST) == 0, "test cannot select nearest mode");
        writer.write_header({false, false, false, 0, 0});
        require(std::fesetround(FE_DOWNWARD) == 0, "test cannot select downward mode");
        for (unsigned row = 0; row < 9; ++row) writer.write_e({static_cast<std::int64_t>(row), 0.1, 0.2});
        writer.write_trailer();
        const bool restored = std::fegetround() == FE_DOWNWARD;
        (void)std::fesetround(original);
        require(restored, "inline encoding changed the engine's floating environment");
        writer.finalize(metadata);
        const auto file = read_file(path);
        (void)decode(file, equity_types());
        if (!threaded) previous = file;
        else require(previous == file, "thread/inline output depends on caller rounding mode");
    }
}
} // namespace

int main() {
    try {
        require(crc("123456789") == UINT32_C(0xcbf43926), "independent CRC primitive");
        round_trip_and_determinism();
        refusals_and_boundaries();
        worker_failure_and_summary_fold();
        failure_wakes_blocked_submit();
        finite_figures();
        x2_decimal_header_and_column_sha();
        const auto small = pressured(512), large = pressured(4096);
        require(small.peak_blocks == large.peak_blocks && small.block_limit == large.block_limit
                && small.peak_builder_budget == large.peak_builder_budget,
                "writer working memory depends on record count");
        v1_bytes();
        rounding_mode_identity();
        std::cout << "sink v2 writer: input round trips, byte identity, CRC/tail, bounded queue and v1 passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sink v2 writer: " << error.what() << '\n';
        return 1;
    }
}
