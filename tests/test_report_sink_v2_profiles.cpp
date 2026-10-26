#include "sink_v2_test_inputs.hpp"

#include <iostream>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/vfs.h>
#endif

namespace {
using namespace sink2_test;

void put_at(Bytes& bytes, std::size_t offset, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
}
void repair_root(Bytes& file) {
    const auto tail = file.size() - 160;
    const auto root = load(file.data() + tail, 8), length = load(file.data() + tail + 8, 4);
    put_at(file, root + length - 4, crc(bytes(file.data() + root, length - 4)), 4);
    const auto root_hash = sha(bytes(file.data() + root, length));
    std::copy(root_hash.begin(), root_hash.end(), file.begin() + tail + 24);
    const auto stored_hash = sha(bytes(file.data(), tail));
    std::copy(stored_hash.begin(), stored_hash.end(), file.begin() + tail + 88);
}
bool is_compact(const Bytes& file) {
    const auto tail = file.size() - 160;
    return load(file.data() + tail + 8, 4) == 320;
}

struct SummaryCalls {
    unsigned count = 0;
    int level = 0;
    std::size_t json_bytes = 0, stored_bytes = 0;
};
enum class SummaryMode { raw, alternate, rle, non_single, failure };
class SummaryCodec final : public StoredCodec {
    std::shared_ptr<SummaryCalls> calls_;
    SummaryMode mode_;
public:
    SummaryCodec(std::shared_ptr<SummaryCalls> calls, SummaryMode mode = SummaryMode::raw)
        : calls_(std::move(calls)), mode_(mode) {}
    void compress_summary(std::string_view json, int level, Bytes& frame) override {
        ++calls_->count;
        calls_->level = level;
        calls_->json_bytes = json.size();
        if (mode_ == SummaryMode::failure)
            throw sink::BlockError(sink::BlockFailure::io, "injected SUMMARY compression failure");
        if (mode_ == SummaryMode::alternate) {
            // Independent RFC framing: four-byte FCS and two raw blocks for
            // the 257-byte fixture, unlike the spec's two-byte FCS/one block.
            frame = {0x28, 0xb5, 0x2f, 0xfd, 0xa0};
            store(frame, json.size(), 4);
            const auto first = json.size() / 2;
            require(first && json.size() - first <= 131072, "alternate SUMMARY fixture size");
            store(frame, first << 3, 3);
            frame.insert(frame.end(), json.begin(), json.begin() + first);
            store(frame, ((json.size() - first) << 3) | 1, 3);
            frame.insert(frame.end(), json.begin() + first, json.end());
        } else if (mode_ == SummaryMode::rle) {
            // Independent standard framing, generic raw/RLE block selection.
            // Compress the actual input, including every nonrepeating byte.
            frame = {0x28, 0xb5, 0x2f, 0xfd, 0xa0};
            store(frame, json.size(), 4);
            const auto run = [&](std::size_t at) {
                std::size_t end = at + 1;
                while (end < json.size() && end - at < 131072 && json[end] == json[at]) ++end;
                return end - at;
            };
            for (std::size_t at = 0; at < json.size();) {
                const auto first = at, repeated = run(at);
                const bool rle = repeated >= 4;
                if (rle) at += repeated;
                else {
                    ++at;
                    while (at < json.size() && at - first < 131072 && run(at) < 4) ++at;
                }
                store(frame, ((at - first) << 3) | (rle ? 2u : 0u)
                    | (at == json.size() ? 1u : 0u), 3);
                if (rle) frame.push_back(static_cast<std::uint8_t>(json[first]));
                else frame.insert(frame.end(), json.begin() + first, json.begin() + at);
            }
        } else if (mode_ == SummaryMode::non_single) {
            // A standard non-single frame has one extra window byte (1 MiB).
            // At S_MAX this gives a stored-only fault, exactly one byte over.
            frame = {0x28, 0xb5, 0x2f, 0xfd, 0x80, 0x50};
            store(frame, json.size(), 4);
            for (std::size_t at = 0; at < json.size();) {
                const auto n = std::min<std::size_t>(131072, json.size() - at);
                store(frame, (n << 3) | (at + n == json.size() ? 1u : 0u), 3);
                frame.insert(frame.end(), json.begin() + at, json.begin() + at + n);
                at += n;
            }
            require(inspect_raw_frame(bytes(frame), max_summary_json_bytes).content == json.size(),
                    "stored-only fixture has an invalid frame or window");
        } else {
            sink::ColumnCodec::compress_summary(json, level, frame);
        }
        calls_->stored_bytes = frame.size();
    }
};

std::string padded_summary(std::size_t length, bool varied = false) {
    const auto folds = metadata.find(",\"sink_folds\"");
    const auto prefix = std::string(metadata.substr(0, folds)) + ",\"padding\":\"";
    const auto suffix = "\"" + std::string(metadata.substr(folds));
    require(length >= prefix.size() + suffix.size(), "SUMMARY padding target too short");
    std::string padding(length - prefix.size() - suffix.size(), 'x');
    if (varied) {
        std::uint64_t state = UINT64_C(0x8f3c6275d914ab01);
        for (auto& c : padding) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            c = static_cast<char>('!' + state % 94);
            if (c == '"' || c == '\\') c = '~';
        }
    }
    return prefix + padding + suffix;
}

template<class F> void refuses_summary_limit(F attempt) {
    bool refused = false;
    try { attempt(); }
    catch (const sink::BlockError& error) {
        require(error.reason() == sink::BlockFailure::summary_limit, "wrong SUMMARY refusal kind");
        require(std::string_view(error.what()) ==
            "finalization failure leaves an invalid file with no valid tail and receipt complete=0; "
            "it is not a partial-file result", "wrong failed-finalization diagnostic");
        refused = true;
    }
    require(refused, "SUMMARY admission ceiling was ignored");
}

void summary_budget_boundaries() {
    const auto max_json = sink::ColumnCodec::max_summary_json_bytes;
    const auto max_stored = sink::ColumnCodec::max_summary_stored_bytes;
    refuses_summary_limit([&] {
        sink::v2::check_summary_stored_length(std::numeric_limits<std::size_t>::max());
    });
    // Real SUMMARY lengths and real frames, with the production bounds.
    // Injection chooses deterministic raw/RLE frames, or the stored-only fault.
    struct Case { SummaryMode mode; std::size_t json_bytes; bool accepted; unsigned calls; };
    const Case cases[] = {
        {SummaryMode::raw, max_json - 1, true, 1},
        {SummaryMode::raw, max_json, true, 1},
        {SummaryMode::rle, max_json, true, 1},
        {SummaryMode::raw, max_json + 1, false, 0},
        {SummaryMode::non_single, max_json, false, 1}
    };
    Directory directory;
    for (bool full : {false, true}) for (const auto& test : cases) {
        const auto text = padded_summary(test.json_bytes, test.mode == SummaryMode::raw);
        const auto path = directory.file("summary-admission");
        const auto calls = std::make_shared<SummaryCalls>();
        Bytes before, after;
        {
            sink::BlockPolicy policy;
            policy.force_full = full;
            sink::BlockWriter writer(descriptor(path, directory.path), policy,
                std::make_unique<SummaryCodec>(calls, test.mode));
            const pf_equity_point_t point{42, 1.0, 0.0};
            writer.write_header({false, false, false, 0, 0});
            writer.write_e(point);
            writer.write_trailer();
            before = read_file(path);
            if (test.accepted) {
                const auto sizes = writer.finalize(text);
                after = read_file(path);
                const auto decoded = decode(after, equity_types());
                const std::vector<Row> reference{{'E', {i(point.time_ms), f(point.equity), f(point.open_profit)}}};
                const auto root = load(after.data() + after.size() - 160, 8);
                const auto at = load(after.data() + root + 8, 8);
                require(writer.receipt().complete == 1 && sizes.json_bytes == text.size()
                        && sizes.stored_bytes == calls->stored_bytes
                        && sizes.json_bytes <= max_json && 52 + sizes.stored_bytes <= max_stored
                        && load(after.data() + at + 4, 4) == sizes.stored_bytes
                        && load(after.data() + at + 8, 4) == sizes.json_bytes
                        && decoded.summary_json == text && decoded.merged == reference
                        && is_compact(after) == !full, "admitted boundary SUMMARY changed or failed");
                if (test.mode == SummaryMode::raw) {
                    const auto c = text.size() + 9 + 3 * ((text.size() + 131071) / 131072);
                    require(sizes.stored_bytes == c
                            && load(after.data() + root + (full ? 24 : 16), 4) == 52 + c,
                            "real raw-fallback SUMMARY length");
                    if (text.size() == max_json)
                        require(sizes.stored_bytes == 1048609
                                && load(after.data() + root + (full ? 24 : 16), 4) == 1048661,
                                "1 MiB raw fallback must fit the stored bound exactly");
                } else require(sizes.stored_bytes < text.size(), "compressible SUMMARY did not shrink");
            } else {
                refuses_summary_limit([&] { writer.finalize(text); });
                const auto receipt = writer.receipt();
                require(receipt.complete == 0 && receipt.sha256_hex[0] == '\0' && receipt.file_bytes == 0
                        && writer.memory().finalization_bytes == 0 && read_file(path) == before,
                        "SUMMARY refusal appended bytes or constructed indexes/root/tail");
                if (calls->count)
                    require(calls->json_bytes == max_json && 52 + calls->stored_bytes == max_stored + 1,
                            "stored refusal did not use actual J/C");
                else require(text.size() > max_json, "decoded refusal was below the ceiling");
                refuses([&] { writer.finalize(text); }, "requires data completion");
                refuses([&] { writer.finalize(metadata); }, "requires data completion");
                refuses([&] { (void)decode(read_file(path), equity_types()); }, "tail");
            }
            require(calls->count == test.calls, "SUMMARY guard/compressor order");
        }
        // A destructor neither supplies metadata nor writes a success tail.
        require(read_file(path) == (test.accepted ? after : before), "destructor changed the finalization result");
        if (!test.accepted) refuses([&] { (void)decode(read_file(path), equity_types()); }, "tail");
    }
}

void small_run_sizes() {
    // Independent v1 schema, not v1_header_json() or bytes read from v2.
    // Section 15.1's sample identity includes both E and T descriptors.
    constexpr std::string_view v1_header =
        R"({"schema":"pf-report-sink","version":1,"run_id":"vec-equity4","engine":"1.7.0","records":[)"
        R"({"type":"E","fields":["time_ms","equity","open_profit"],)"
        R"("types":["i64","f64?","f64?"],"units":["unix_ms","account_ccy","account_ccy"]},)"
        R"({"type":"T","fields":["n","side","entry_time","exit_time","entry_price","exit_price",)"
        R"("qty","pnl","pnl_pct","max_runup","max_drawdown","commission","entry_bar_index",)"
        R"("exit_bar_index","open_at_end","entry_incarnation","close_cause","entry_id","exit_id","exit_comment"],)"
        R"("types":["i64","str","i64","i64","f64?","f64?","f64?","f64?","f64?","f64?","f64?","f64?",)"
        R"("i64","i64","bool","u64","i64","str","str","str"],)"
        R"("units":["","","unix_ms","unix_ms","quote_ccy","quote_ccy","qty","account_ccy","percent",)"
        R"("account_ccy","account_ccy","account_ccy","script_bar","script_bar","","","","","",""]}]})"
        "\n";
    require(v1_header.size() == 810, "independent sample v1 header including LF");
    // A legal raw-block fixture gives C=1024 without assuming a production
    // compression ratio: seven frame-header bytes + three block-header bytes.
    constexpr std::size_t json_bytes = 1014;
    constexpr std::size_t stored_bytes = 7 + 3 + json_bytes;
    Directory directory;
    struct Run { unsigned e, t; bool small; };
    // Both sides of E=63/64, E=127/128 and T=3/4, plus the retained runs.
    const Run runs[] = {{0, 0, true}, {100, 0, true}, {63, 4, true}, {64, 3, true},
        {64, 4, false}, {127, 3, true}, {127, 4, false}, {128, 3, false}, {128, 4, false}};
    for (const auto& run : runs) {
        const auto n = run.e;
        const bool retained = run.t == 0;
        const auto name = "E" + std::to_string(n) + "-T" + std::to_string(run.t) + "-size";
        const auto path = directory.file(name.c_str());
        auto d = descriptor(path, directory.path);
        d.run_id = "vec-equity4";
        sink::BlockWriter writer(d, {}, std::make_unique<StoredCodec>());
        writer.write_header({false, false, false, 0, 0});
        std::vector<Row> reference;
        std::size_t v1_record_bytes = 0;
        for (unsigned j = 0; j < n; ++j) {
            const pf_equity_point_t point{INT64_C(1700000000000) + INT64_C(60000) * j,
                10000.0, retained ? 0.0 : 1000.0};
            writer.write_e(point);
            reference.push_back({'E', {i(point.time_ms), f(point.equity), f(point.open_profit)}});
            // These fixture doubles are exact integers; their canonical v1
            // representation is the decimal integer followed by ".0".
            const auto line = "[\"E\"," + std::to_string(point.time_ms) + ","
                + std::to_string(static_cast<std::int64_t>(point.equity)) + ".0,"
                + std::to_string(static_cast<std::int64_t>(point.open_profit)) + ".0]\n";
            v1_record_bytes += line.size();
        }
        for (unsigned j = 0; j < run.t; ++j) {
            pf_trade_t trade{};
            trade.is_long = true;
            trade.entry_time = INT64_C(1700000000000) + INT64_C(60000) * j;
            trade.exit_time = trade.entry_time + 60000;
            trade.entry_price = 100.0; trade.exit_price = 101.0; trade.qty = 1.0;
            trade.pnl = trade.pnl_pct = trade.max_runup = 1.0;
            trade.max_drawdown = -1.0;
            trade.entry_bar_index = static_cast<std::int32_t>(j);
            trade.exit_bar_index = static_cast<std::int32_t>(j + 1);
            writer.write_t(j + 1, trade, j + 1, 0, "L", "X", "");
            reference.push_back({'T', {i(j + 1), s("long"), i(trade.entry_time), i(trade.exit_time),
                f(trade.entry_price), f(trade.exit_price), f(trade.qty), f(trade.pnl), f(trade.pnl_pct),
                f(trade.max_runup), f(trade.max_drawdown), f(trade.commission),
                i(trade.entry_bar_index), i(trade.exit_bar_index), u(0), u(j + 1), i(0), s("L"), s("X"), s("")}});
            const auto line = "[\"T\"," + std::to_string(j + 1) + ",\"long\","
                + std::to_string(trade.entry_time) + "," + std::to_string(trade.exit_time)
                + ",100.0,101.0,1.0,1.0,1.0,1.0,-1.0,0.0," + std::to_string(j) + ","
                + std::to_string(j + 1) + ",false," + std::to_string(j + 1) + ",0,\"L\",\"X\",\"\"]\n";
            v1_record_bytes += line.size();
        }
        // The digest's contents do not affect its byte count. Include every
        // trailer key, both count tokens, 64 hex digits, and the final LF.
        const auto v1_trailer = "{\"trailer\":\"pf-report-sink\",\"version\":1,\"counts\":{\"E\":"
            + std::to_string(n) + ",\"T\":" + std::to_string(run.t)
            + "},\"sha256\":\"" + std::string(64, '0') + "\"}\n";
        const auto v1_equivalent_bytes = v1_header.size() + v1_record_bytes + v1_trailer.size();
        if (retained)
            require(v1_trailer.size() == (n ? 142u : 140u)
                    && v1_record_bytes == n * 32u && v1_equivalent_bytes == (n ? 4152u : 950u),
                    "independent v1 header/records/trailer byte count");
        writer.write_trailer();
        const auto summary = retained ? padded_summary(json_bytes) : metadata_for(writer.receipt());
        const auto sizes = writer.finalize(summary);
        const auto receipt = writer.receipt();
        // The report obtains C/J/counts/file bytes before any file parsing.
        require(sizes.json_bytes == summary.size()
                && (!retained || (sizes.stored_bytes == stored_bytes && sizes.json_bytes == json_bytes))
                && receipt.complete == 1 && receipt.count_e == n && receipt.count_t == run.t
                && receipt.count_o == 0 && receipt.count_v == 0 && receipt.count_k == 0
                && receipt.count_x == 0 && receipt.count_h == 0
                && receipt.block_count == (run.t ? 3 : n ? 2 : 0),
                "size report finalization result and raw counts");
        const auto retry = writer.finalize(summary);
        require(retry.stored_bytes == sizes.stored_bytes && retry.json_bytes == sizes.json_bytes,
                "idempotent finalizer lost SUMMARY sizes");
        const auto file = read_file(path);
        const auto decoded = decode(file, equity_types());
        require(decoded.merged == reference && decoded.summary_json == summary
                && file.size() == static_cast<std::uint64_t>(receipt.file_bytes)
                && is_compact(file) && 16 + load(file.data() + 8, 4) == 947,
                "small-run size fixture records/header/receipt");
        if (!n) {
            // Section 15.1's two routes: region sum and fixed overhead + C.
            require(file.size() == 947 + 52 + stored_bytes + 320 + 160
                    && file.size() == 1479 + stored_bytes && file.size() == 2503,
                    "EMPTY complete file differs from independent fixed-head ledger");
        }
        // Fixture-specific, token-preserving Q/M projections (0.5). Unknown
        // top-level padding stays counted in Q. Probe the same raw-frame
        // codec and HEAD level as the full SUMMARY, including frame overhead.
        const auto folds_at = summary.find(",\"sink_folds\"");
        const auto padding_at = summary.find(",\"padding\"");
        std::string q = "{\"engine\":\"pineforge\",\"summary\":{},\"metrics\":{},\"diagnostics\":{}";
        if (padding_at != std::string::npos) q += summary.substr(padding_at, folds_at - padding_at);
        q += summary.substr(folds_at);
        constexpr std::string_view m =
            "{\"input\":{},\"applied_inputs\":{},\"applied_overrides\":{},\"applied_runtime\":{},"
            "\"elapsed_seconds\":0.0,\"fingerprint\":null}";
        StoredCodec probe;
        Bytes probe_frame;
        probe.compress_summary(q, 3, probe_frame);
        const auto c_q = probe_frame.size();
        probe.compress_summary(m, 3, probe_frame);
        const auto c_m = probe_frame.size();
        const auto c = static_cast<std::size_t>(sizes.stored_bytes);
        const auto excluded = std::min(c_m, c > c_q ? c - c_q : 0);
        const auto comparable = file.size() - excluded;
        const bool small = (receipt.count_e < 128 && receipt.count_t < 4) || receipt.count_e < 64;
        require(small == run.small, "raw E/T size-clause boundary assignment");
        require(small ? comparable <= v1_equivalent_bytes + 3072 : comparable < v1_equivalent_bytes,
                "comparable writer file exceeds its round-8 size rule");
        std::cout << name << " v1=" << v1_equivalent_bytes << " full_v2=" << file.size()
            << " EXCLUDED_CALLER_METADATA=" << excluded << " COMPARABLE_V2=" << comparable
            << " clause=" << (small ? "ii-a" : "i") << " limit="
            << (small ? v1_equivalent_bytes + 3072 : v1_equivalent_bytes - 1) << '\n';
    }
}

Bytes population(unsigned equity, unsigned trades, unsigned ecap = 4096, bool force_full = false) {
    Directory directory;
    const auto path = directory.file("profile");
    sink::BlockPolicy policy;
    policy.block_records[0] = ecap;
    policy.force_full = force_full;
    sink::BlockWriter writer(descriptor(path, directory.path), policy, std::make_unique<StoredCodec>());
    writer.write_header({false, false, false, 0, 0});
    // A null population proves the profile uses RAW E counts.
    for (unsigned i = 0; i < equity; ++i)
        writer.write_e({i, std::numeric_limits<double>::quiet_NaN(), 0.0});
    for (unsigned i = 0; i < trades; ++i) {
        pf_trade_t trade{};
        trade.entry_price = trade.exit_price = trade.qty = 1.0;
        writer.write_t(i + 1, trade, i + 1, 0, "", "", "");
    }
    sealed(writer);
    auto file = read_file(path);
    (void)decode(file, equity_types());
    return file;
}
void profile_boundaries() {
    require(is_compact(population(10979, 0)), "N=10979 must be COMPACT");
    require(!is_compact(population(10980, 0)), "N=10980 must be FULL");
    require(is_compact(population(0, 3313)), "T=3313 must be COMPACT");
    require(!is_compact(population(0, 3314)), "T=3314 must be FULL");
    require(is_compact(population(127, 0, 1)), "128 blocks must be COMPACT");
    require(!is_compact(population(128, 0, 1)), "129 blocks must be FULL");
    require(!is_compact(population(4, 1, 4096, true)), "forced FULL below thresholds");

    const auto base = population(4, 1);
    for (unsigned which = 0; which < 3; ++which) {
        auto file = base;
        const auto root = load(file.data() + file.size() - 160, 8);
        put_at(file, root + (which == 0 ? 24 : which == 1 ? 32 : 20),
               which == 0 ? 10980 : which == 1 ? 3314 : 129, which == 2 ? 4 : 8);
        repair_root(file);
        refuses([&] { (void)decode(file, equity_types()); }, "compact thresholds");
    }
    {
        auto file = base;
        const auto root = load(file.data() + file.size() - 160, 8);
        const auto table = load(file.data() + root + 160, 8);
        file[table + 32] ^= 1;
        repair_root(file);
        refuses([&] { (void)decode(file, equity_types()); }, "compact descriptor SHA");
    }
    auto full = population(0, 1, 4096, true);
    auto root = load(full.data() + full.size() - 160, 8);
    put_at(full, root + 512 + 8 * 64 + 28, 0, 4);
    repair_root(full);
    refuses([&] { (void)decode(full, equity_types()); }, "PRESENT");
    full = population(0, 1, 4096, true);
    root = load(full.data() + full.size() - 160, 8);
    put_at(full, root + 512 + 8 * 64 + 24, 4104, 4); // one u32 rank needs 4100
    repair_root(full);
    refuses([&] { (void)decode(full, equity_types()); }, "last page length");
}

void worked_layout() {
    Directory directory;
    Bytes compact_file;
    std::string identity;
    for (bool full : {false, true}) {
        const auto path = directory.file(full ? "worked-full" : "worked-compact");
        auto d = descriptor(path, directory.path);
        d.run_id = "vec-equity4";
        sink::BlockPolicy policy;
        policy.force_full = full;
        sink::BlockWriter writer(d, policy, std::make_unique<StoredCodec>());
        writer.write_header({false, false, false, 0, 0});
        const pf_equity_point_t points[] = {
            {1700000000000, 10000.0, 0.0}, {1700000060000, 10005.25, 5.25},
            {1700000120000, 10010.5, 10.5}, {1700000180000, 10010.5, 0x1p-20}};
        for (const auto& p : points) writer.write_e(p);
        writer.write_trailer();
        const auto pending = read_file(path);
        writer.finalize(metadata);
        const auto file = read_file(path);
        require(std::equal(pending.begin(), pending.end(), file.begin()), "finalizer rewrote HEAD/data");
        const auto decoded = decode(file, equity_types());
        require(metadata.size() == 257, "worked SUMMARY input is not the specified 257 bytes");
        // Independent section-13.1 ledger, never derived from writer output.
        const std::size_t offsets[] = {947, 1176, 1260, 1579, 1771, 2091, 2251};
        require(16 + load(file.data() + 8, 4) == offsets[0], "worked HEAD length");
        require(decoded.blocks.size() == 2 && decoded.blocks[0].offset == offsets[0]
                && decoded.blocks[0].length == 229 && decoded.blocks[1].offset == offsets[1]
                && decoded.blocks[1].length == 84, "worked immutable blocks");
        const auto* summary = file.data() + offsets[2];
        const std::array<std::uint8_t, 10> preamble{{0x28, 0xb5, 0x2f, 0xfd, 0x60, 0x01, 0x00,
                                                  0x09, 0x08, 0x00}};
        require(std::memcmp(summary, "SUM2", 4) == 0 && load(summary + 4, 4) == 267
                && load(summary + 8, 4) == 257 && load(summary + 12, 4) == crc(metadata)
                && load(summary + 16, 4) == 0 && digest_at(sha(metadata), summary + 20),
                "worked 52-byte SUMMARY header");
        require(std::equal(preamble.begin(), preamble.end(), summary + 52)
                && bytes(summary + 62, 257) == metadata, "spec decode-only SUMMARY frame");
        if (!full) {
            compact_file = file;
            identity = decoded.logical_hex;
            require(file.size() == offsets[6], "worked compact file=2251");
            const auto* root = file.data() + offsets[4];
            require(std::memcmp(file.data() + offsets[2], "SUM2", 4) == 0, "worked SUMMARY offset");
            require(std::memcmp(root, "FTR2", 4) == 0 && load(root + 6, 2) == 1, "compact root flag");
            require(load(root + 8, 8) == offsets[2] && load(root + 16, 4) == 319
                    && load(root + 20, 4) == 2 && load(root + 24, 8) == 4
                    && load(root + 80, 8) == 4 && load(root + 88, 8) == 4, "compact root counts");
            require(load(root + 160, 8) == offsets[3] && load(root + 168, 8) == 192
                    && load(root + 208, 4) == 1 && load(root + 236, 4) == 1, "compact root table");
            require(std::all_of(root + 32, root + 80, [](auto b) { return !b; })
                    && std::all_of(root + 212, root + 236, [](auto b) { return !b; })
                    && std::all_of(root + 240, root + 316, [](auto b) { return !b; }), "compact root zeros");
            const auto* e = file.data() + offsets[3];
            const auto* i = e + 96;
            require(load(e, 8) == 947 && load(e + 8, 4) == 229 && e[12] == 'E' && e[13] == 1
                    && load(e + 16, 4) == 4 && load(e + 20, 4) == 229 && load(e + 24, 8) == 0
                    && load(e + 32, 8) == 1700000000000 && load(e + 40, 8) == 1700000180000,
                    "compact E descriptor ledger");
            require(load(i, 8) == 1176 && load(i + 8, 4) == 84 && i[12] == 'I' && i[13] == 0
                    && load(i + 16, 4) == 4 && load(i + 20, 4) == 84 && load(i + 24, 8) == 0
                    && std::all_of(i + 32, i + 64, [](auto b) { return !b; }), "compact I descriptor ledger");
            const auto* tail = file.data() + offsets[5];
            require(load(tail, 8) == offsets[4] && load(tail + 8, 4) == 320
                    && load(tail + 16, 8) == offsets[6], "compact tail ledger");
            // The spec's independent worked encoder may omit the optional
            // stored-prefix SHA; writer checks retain the default requirement.
            auto optional = file;
            put_at(optional, offsets[5] + 12, 0, 4);
            std::fill(optional.begin() + offsets[5] + 88, optional.begin() + offsets[5] + 120, 0);
            require(decode(optional, equity_types(), false).logical_hex == identity, "optional prefix layout");
        } else {
            require(file.size() == 20171 && decoded.logical_hex == identity, "forced FULL size/identity");
            const auto* root = file.data() + 15915;
            require(load(root + 24, 4) == 319 && load(root + 512, 8) == 1579
                    && load(root + 512 + 7 * 64, 8) == 6187
                    && load(root + 512 + 18 * 64, 8) == 10795, "worked FULL SUMMARY/table ledger");
            const auto* tail = file.data() + 20011;
            require(load(tail, 8) == 15915 && load(tail + 8, 4) == 4096
                    && load(tail + 16, 8) == 20171, "worked FULL root/tail ledger");
            require(std::equal(compact_file.begin(), compact_file.begin() + 1579, file.begin()),
                    "profile choice changed HEAD, data or SUMMARY");
        }
    }
}

void summary_contract() {
    Directory directory;
    std::vector<std::string> bad;
    const auto replace = [](std::string text, std::string_view old, std::string_view replacement) {
        const auto at = text.find(old);
        require(at != std::string::npos, "summary test replacement absent");
        text.replace(at, old.size(), replacement);
        return text;
    };
    bad.push_back(replace(std::string(metadata), "\"engine\":\"pineforge\"", "\"engine\":false"));
    bad.push_back(replace(std::string(metadata), "\"input\":{}", "\"input\":{\"a\":0,\"\\u0061\":1}"));
    bad.push_back(replace(std::string(metadata), "\"closed_count\":0", "\"closed_count\":1"));
    bad.push_back(replace(std::string(metadata), "\"open_at_end_pnl\":0.0", "\"open_at_end_pnl\":-0.0"));
    bad.push_back(replace(std::string(metadata),
        "\"closed_count\":0,\"open_at_end_count\":0", "\"open_at_end_count\":0,\"closed_count\":0"));
    const auto extension = std::string(metadata.substr(0, metadata.size() - 1));
    bad.push_back(extension + ",\"trace\":[]}");
    bad.push_back(extension + ",\"outputs\":{\"bars\":[]}}");
    bad.push_back(extension + ",\"additive_integer\":9007199254740992}");
    bad.push_back(extension + ",\"additive_number\":1e309}");
    bad.push_back(extension + ",\"deep\":" + std::string(64, '[') + "0" + std::string(64, ']') + "}");
    const auto folds = metadata.find(",\"sink_folds\"");
    bad.push_back(std::string(metadata.substr(0, folds)) + "}");
    for (std::size_t i = 0; i < bad.size(); ++i) {
        const auto path = directory.file(("bad-summary-" + std::to_string(i)).c_str());
        const auto calls = std::make_shared<SummaryCalls>();
        sink::BlockWriter writer(descriptor(path, directory.path), {}, std::make_unique<SummaryCodec>(calls));
        writer.write_header({false, false, false, 0, 0});
        writer.write_trailer();
        const auto before = read_file(path);
        refuses([&] { writer.finalize(bad[i]); });
        require(!writer.receipt().complete, "invalid summary sealed");
        require(calls->count == 0 && read_file(path) == before, "invalid SUMMARY compressed or appended bytes");
        refuses([&] { (void)decode(read_file(path), equity_types()); }, "tail");
    }
    const auto path = directory.file("additive-summary");
    sink::BlockWriter writer(descriptor(path, directory.path), {}, std::make_unique<StoredCodec>());
    writer.write_header({false, false, false, 0, 0});
    writer.write_trailer();
    const auto good = extension + ",\"extension\":{\"未知\":[-0.0,true,null,\"台灣\"],\"tiny\":-1e-999},\"depth\":"
        + std::string(63, '[') + "0" + std::string(63, ']') + "}";
    writer.finalize(good);
    const auto file = read_file(path);
    require(decode(file, equity_types()).summary_json == good, "additive metadata bytes changed");
}

void summary_frames_and_caps() {
    Directory directory;
    // Equal decoded input, two independently framed zstd representations.
    for (bool full : {false, true}) {
        Bytes first_frame;
        Digest first_stored{};
        std::string identity;
        for (auto mode : {SummaryMode::raw, SummaryMode::alternate}) {
            const auto path = directory.file("reframed-summary");
            auto d = descriptor(path, directory.path);
            d.zstd_level = 7;
            sink::BlockPolicy policy;
            policy.force_full = full;
            policy.min_zstd_raw = 1048576; // never a SUMMARY admission threshold
            const auto calls = std::make_shared<SummaryCalls>();
            sink::BlockWriter writer(d, policy, std::make_unique<SummaryCodec>(calls, mode));
            writer.write_header({false, false, false, 0, 0});
            writer.write_trailer();
            const auto sizes = writer.finalize(metadata);
            require(sizes.json_bytes == 257 && sizes.stored_bytes == (mode == SummaryMode::raw ? 267u : 272u),
                    "finalizer must expose actual C/J in both profiles without parsing");
            const auto file = read_file(path);
            const auto decoded = decode(file, equity_types());
            const auto tail = file.size() - 160;
            const auto root = load(file.data() + tail, 8);
            const auto at = load(file.data() + root + 8, 8);
            const auto c = load(file.data() + at + 4, 4);
            const Bytes frame(file.begin() + at + 52, file.begin() + at + 52 + c);
            require(c > metadata.size() && decoded.summary_json == metadata
                    && decoded.merged.empty(), "expanding SUMMARY was not a decodable frame");
            require(calls->count == 1 && calls->level == 7
                    && bytes(file.data() + 12, static_cast<std::size_t>(load(file.data() + 8, 4)))
                        .find("\"level\":7") != std::string_view::npos, "SUMMARY ignores HEAD level");
            require(load(file.data() + root + (full ? 24 : 16), 4) == 52 + c
                    && digest_at(sha(metadata), file.data() + root + (full ? 352 : 96)),
                    "SUMMARY root length/hash uses stored/decoded domains incorrectly");
            if (mode == SummaryMode::raw) {
                first_frame = frame;
                identity = decoded.logical_hex;
                std::copy_n(file.data() + tail + 88, 32, first_stored.begin());
            } else {
                require(frame != first_frame && decoded.logical_hex == identity
                        && identity == writer.receipt().sha256_hex, "SUMMARY recompression changed logical digest");
                require(!digest_at(first_stored, file.data() + tail + 88),
                        "stored-prefix digest ignored physical SUMMARY bytes");
            }
        }
    }

    // Real library compression and the default raw fallback at S_MAX, then
    // a real J=S_MAX+1 refusal through the same finalization/cleanup path.
    const auto max_json = sink::ColumnCodec::max_summary_json_bytes;
    for (auto json_length : {max_json, max_json + 1}) {
        const auto large = padded_summary(json_length);
        for (bool library : {false, true}) {
#if !defined(PINEFORGE_WITH_ZSTD) || !PINEFORGE_WITH_ZSTD
            if (library) continue;
#endif
            const auto path = directory.file(library ? "large-library" : "large-raw");
            const auto calls = std::make_shared<SummaryCalls>();
            auto codec = library ? sink::make_column_codec()
                : std::unique_ptr<sink::ColumnCodec>(std::make_unique<SummaryCodec>(calls));
            sink::BlockWriter writer(descriptor(path, directory.path), {}, std::move(codec));
            writer.write_header({false, false, false, 0, 0});
            writer.write_trailer();
            const auto before = read_file(path);
            if (json_length > max_json) {
                refuses_summary_limit([&] { writer.finalize(large); });
                require(!writer.receipt().complete && writer.receipt().file_bytes == 0
                        && read_file(path) == before && calls->count == 0,
                        "oversized SUMMARY was encoded or appended");
                refuses([&] { writer.finalize(metadata); }, "requires data completion");
                refuses([&] { (void)decode(read_file(path), equity_types()); }, "tail");
                continue;
            }
            const auto sizes = writer.finalize(large);
            const auto file = read_file(path);
            require(writer.receipt().complete && decode(file, equity_types()).summary_json == large,
                    "admitted large SUMMARY rejected or changed");
            require(std::equal(before.begin(), before.end(), file.begin()), "large SUMMARY rewrote data");
            const auto root = load(file.data() + file.size() - 160, 8), at = load(file.data() + root + 8, 8);
            require(load(file.data() + at + 8, 4) == json_length && sizes.json_bytes == json_length,
                    "large SUMMARY J");
            if (!library) {
                // Nine-byte frame header, ceil(J/131072) three-byte block headers.
                const auto c = json_length + 9 + 3 * ((json_length + 131071) / 131072);
                require(calls->count == 1 && load(file.data() + at + 4, 4) == c
                        && load(file.data() + root + 16, 4) == 52 + c && sizes.stored_bytes == c,
                        "large raw-frame fallback length");
            } else require(sizes.stored_bytes < sizes.json_bytes, "compressible 1 MiB SUMMARY did not shrink");
        }
    }
    {
        const auto path = directory.file("summary-compressor-failure");
        const auto calls = std::make_shared<SummaryCalls>();
        sink::BlockWriter writer(descriptor(path, directory.path), {},
                                 std::make_unique<SummaryCodec>(calls, SummaryMode::failure));
        writer.write_header({false, false, false, 0, 0});
        writer.write_trailer();
        const auto before = read_file(path);
        refuses([&] { writer.finalize(metadata); }, "injected SUMMARY compression");
        require(calls->count == 1 && !writer.receipt().complete && read_file(path) == before,
                "SUMMARY compression failure appended bytes");
    }

#if defined(PINEFORGE_WITH_ZSTD) && PINEFORGE_WITH_ZSTD
    // Exercise the real codec's expanding-frame path with deterministic
    // high-entropy bytes. This is a codec probe, not schema-valid metadata.
    auto codec = sink::make_column_codec();
    std::string entropy(max_json, '\0');
    std::uint64_t state = UINT64_C(0x8f3c6275d914ab01);
    for (auto& c : entropy) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        c = static_cast<char>(state >> 56);
    }
    Bytes frame;
    codec->compress_summary(entropy, 3, frame);
    const auto raw_length = entropy.size() + 9 + 3 * ((entropy.size() + 131071) / 131072);
    require(frame.size() == raw_length && 52 + frame.size() == sink::ColumnCodec::max_summary_stored_bytes,
            "incompressible 1 MiB codec probe did not fit the raw-fallback bound exactly");
    require(bytes(inflate(bytes(frame), static_cast<unsigned>(entropy.size()), max_json)) == entropy,
            "incompressible codec probe round trip");
    codec->compress_summary("{\"x\":1}", 3, frame);
    require(bytes(inflate(bytes(frame), 7, max_json)) == "{\"x\":1}", "sub-64-byte SUMMARY codec frame");
#endif
}

void raw_summary_frame_boundaries() {
    StoredCodec codec;
    Bytes frame;
    for (unsigned length : {1u, 63u, 64u, 255u, 256u, 65791u, 65792u, 131072u, 131073u, 1048576u}) {
        const std::string input(length, 'q');
        codec.compress_summary(input, 3, frame);
        const unsigned header = length < 256 ? 6 : length < 65792 ? 7 : 9;
        const unsigned blocks = (length + 131071) / 131072;
        require(frame.size() == header + 3 * blocks + length, "raw SUMMARY FCS/block boundary length");
        require(bytes(inflate(bytes(frame), length, 1048576)) == input, "raw SUMMARY frame boundary round trip");
        const auto info = frame_header(bytes(frame), 1048576);
        require(info.content == length && info.window == length && !info.checksum,
                "raw SUMMARY frame parameters");
    }
}

void summary_wire_refusals() {
    Directory directory;
    const auto path = directory.file("summary-wire-refusals");
    sink::BlockWriter writer(descriptor(path, directory.path), {}, std::make_unique<StoredCodec>());
    writer.write_header({false, false, false, 0, 0});
    sealed(writer);
    const auto base = read_file(path);
    const auto root = load(base.data() + base.size() - 160, 8);
    const auto summary = load(base.data() + root + 8, 8);
    struct Mutation { unsigned offset; std::uint32_t value; unsigned width; const char* reason; };
    const Mutation mutations[] = {
        {4, 0, 4, "summary framing"},
        {4, static_cast<std::uint32_t>(sink::ColumnCodec::max_summary_stored_bytes - 51), 4, "summary framing"},
        {8, 0, 4, "summary framing"},
        {8, static_cast<std::uint32_t>(sink::ColumnCodec::max_summary_json_bytes + 1), 4, "summary framing"},
        {16, 1, 4, "summary framing"}, {52, '{', 1, "frame magic"},
        {56, 0x61, 1, "frame dictionary"}, {56, 0, 1, "frame content size absent"},
        {57, 2, 1, "frame content size"}};
    for (const auto& mutation : mutations) {
        auto file = base;
        put_at(file, summary + mutation.offset, mutation.value, mutation.width);
        repair_root(file);
        refuses([&] { (void)decode(file, equity_types()); }, mutation.reason);
    }
    {
        auto file = base;
        put_at(file, summary + 56, 0x40, 1); // non-single, two-byte FCS
        put_at(file, summary + 57, 0x58, 1); // window = 2^(10+11), over S_MAX
        repair_root(file);
        refuses([&] { (void)decode(file, equity_types()); }, "frame window");
    }
    for (unsigned offset : {12u, 20u}) {
        auto file = base;
        file[summary + offset] ^= 1;
        repair_root(file);
        refuses([&] { (void)decode(file, equity_types()); }, offset == 12 ? "summary CRC" : "summary SHA");
    }
    {
        auto file = base;
        put_at(file, root + 16, 305, 4); // stale round-4 length for the 257-byte input
        repair_root(file);
        refuses([&] { (void)decode(file, equity_types()); }, "summary framing");
    }
    const auto length = load(base.data() + summary + 4, 4);
    const Bytes frame(base.begin() + summary + 52, base.begin() + summary + 52 + length);
    auto concatenated = frame;
    concatenated.insert(concatenated.end(), frame.begin(), frame.end());
    refuses([&] { (void)inflate(bytes(concatenated), 257, 1048576); }, "frame trailing bytes");
    concatenated = frame;
    concatenated.push_back(0);
    refuses([&] { (void)inflate(bytes(concatenated), 257, 1048576); }, "frame trailing bytes");
}

class FailOnFinalRead final : public StoredCodec {
public:
    void compress_column(std::string_view raw, int, Bytes& frame) override {
        // Deliberate codec fault injection: a shorter opaque frame is admitted
        // by this stub and the finalizer fails when it tries to decode it.
        frame.resize(8);
        for (unsigned i = 0; i < 8; ++i) frame[i] = static_cast<std::uint8_t>(raw.size() >> (8 * i));
    }
    sink::ColumnFrame inspect_frame(std::string_view frame) const override {
        if (frame.size() >= 4 && load(reinterpret_cast<const std::uint8_t*>(frame.data()), 4)
                == UINT32_C(0xfd2fb528))
            return StoredCodec::inspect_frame(frame);
        return {load(reinterpret_cast<const std::uint8_t*>(frame.data()), 8), 1024, frame.size(), false};
    }
    void decompress_column(std::string_view, std::size_t, Bytes&) override {
        throw sink::BlockError(sink::BlockFailure::io, "injected finalization read failure");
    }
};
void scratch_and_seekability() {
    Directory output, scratch;
    const auto unrelated = scratch.file("keep.bin");
    { std::ofstream file(unrelated); file << "owned by someone else"; }
    for (bool failure : {false, true}) {
        const auto path = output.file(failure ? "fail" : "success");
        std::unique_ptr<sink::ColumnCodec> codec = failure
            ? std::unique_ptr<sink::ColumnCodec>(std::make_unique<FailOnFinalRead>())
            : std::unique_ptr<sink::ColumnCodec>(std::make_unique<StoredCodec>());
        sink::BlockWriter writer(descriptor(path, scratch.path), {}, std::move(codec));
        writer.write_header({false, false, false, 0, 0});
        for (unsigned j = 0; j < 1000; ++j) writer.write_e({j, 1.0, 0.0});
        writer.write_trailer();
        if (failure) {
            refuses([&] { writer.finalize(metadata); }, "injected finalization");
            require(writer.receipt().complete == 0, "failure sealed output");
            refuses([&] { (void)decode(read_file(path), equity_types()); }, "tail");
        } else {
            writer.finalize(metadata);
            (void)decode(read_file(path), equity_types());
            const auto flags = writer.receipt().scratch_flags;
            require(flags <= 2 && (flags & 3) != 3, "scratch backing flags");
#if defined(__linux__)
            struct statfs fs{};
            require(::statfs(scratch.path.c_str(), &fs) == 0, "independent scratch filesystem query");
            const auto type = static_cast<unsigned long>(fs.f_type);
            if (type == 0x01021994UL || type == 0x858458f6UL)
                require(flags == 1, "memory scratch was not identified");
            if (type == 0x794c7630UL || type == 0x65735546UL)
                require(flags == 2, "opaque overlay/FUSE backing was called disk");
#endif
        }
        unsigned files = 0;
        for (const auto& entry : std::filesystem::directory_iterator(scratch.path)) {
            ++files;
            require(entry.path() == unrelated, "private work file/directory leaked");
        }
        require(files == 1 && std::filesystem::file_size(unrelated) == 21, "unrelated scratch file removed");
        require(writer.memory().work_file_bytes == 0, "unexpected work-file allocation");
    }
    const auto fifo = output.file("pipe");
    require(::mkfifo(fifo.c_str(), 0600) == 0, "create FIFO");
    sink::BlockWriter writer(descriptor(fifo, scratch.path), {}, std::make_unique<StoredCodec>());
    refuses([&] { writer.write_header({false, false, false, 0, 0}); }, "seekable local regular");
    require(std::filesystem::is_fifo(fifo), "nonseekable refusal replaced FIFO");
}
} // namespace

int main() {
    try {
        profile_boundaries();
        worked_layout();
        small_run_sizes();
        summary_contract();
        summary_frames_and_caps();
        summary_budget_boundaries();
        raw_summary_frame_boundaries();
        summary_wire_refusals();
        scratch_and_seekability();
        std::cout << "sink v2 profiles: thresholds, independent worked ledger, cleanup and seekability passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "sink v2 profiles: " << e.what() << '\n';
        return 1;
    }
}
