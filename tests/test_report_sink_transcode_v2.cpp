#include "sink_v2_test_inputs.hpp"
#include "../tools/sink_v2/report_sink_transcode_v2.hpp"
#include "../tools/sink_v2/report_sink_reader.hpp"

#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <iterator>
#include <sys/stat.h>

namespace {
using namespace sink2_test;
namespace reader = pineforge::report_sink::v2;

void text_file(const std::string& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    require(bool(out), "open test file");
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    require(bool(out), "write test file");
}
std::string text_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    require(bool(in), "open test input");
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
struct InputFile {
    int fd;
    explicit InputFile(const std::string& path) : fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC)) {
        require(fd >= 0, "open borrowed input");
    }
    ~InputFile() { (void)::close(fd); }
};
class Source final : public reader::RandomAccessSource {
    Bytes bytes_;
public:
    explicit Source(const std::string& path) : bytes_(read_file(path)) {}
    std::uint64_t size() const override { return bytes_.size(); }
    std::string generation() const override { return "test-owned-immutable"; }
    reader::Bytes readRange(std::uint64_t at, std::size_t size) override {
        require(at <= bytes_.size() && size <= bytes_.size() - at, "test source range");
        const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(at);
        return {begin, begin + static_cast<std::ptrdiff_t>(size)};
    }
};

// Frozen captured scalars/order, supplied independently to the direct writer
// and to stdout. The transcoder must project, never parse/re-serialize them.
std::string metadata_base(bool outputs) {
    std::string text =
        "{\"engine\":\"pineforge\",\"input\":{\"bars\":41,\"ohlcv\":\"same-run.csv\"},"
        "\"applied_inputs\":{\"label\":\"\\u53f0\\u7063\\n\\ud83d\\ude00\"},"
        "\"applied_overrides\":{},\"applied_runtime\":{\"script_tf\":\"1m\"},"
        "\"elapsed_seconds\":1e-07,\"summary\":{\"total_trades\":41,\"net_pnl\":-0.0},"
        "\"diagnostics\":{\"timing\":{\"value\":1.0}},\"metrics\":{\"all\":{\"ratio\":null}}";
    if (outputs) text +=
        ",\"outputs\":{\"schema_version\":\"pineforge-outputs/v1\",\"message_format\":\"pineforge/v1\","
        "\"manifest_sha256\":\"captured-manifest-hash\",\"manifest\":{\"outputs\":[],\"series\":[]},"
        "\"constants\":[-0.0,null],\"hlines\":[]}";
    text += ",\"fingerprint\":{\"token\":\"captured-same-run\",\"provenance\":{\"schema_version\":2}},"
        "\"report_window\":{\"start_ms\":0,\"end_ms\":1000,\"additive\":[true,-0.0]},"
        "\"report_shape\":{\"equity\":\"full\"},\"\\u65b0\":{\"value\":1e+16}}";
    return text;
}
std::string metadata_for_run(const pf_report_sink_receipt_v2& r, bool outputs) {
    auto text = metadata_base(outputs);
    text.pop_back();
    const auto folds = metadata_for(r);
    text += "," + folds.substr(folds.find("\"sink_folds\""));
    return text;
}
std::string stdout_for_run(bool outputs) {
    auto text = metadata_base(outputs);
    if (outputs) {
        const auto position = text.find("\"constants\"");
        text.insert(position, "\"bars\":{\"open_ms\":[],\"close_ms\":[]},\"series\":[],\"events\":[],");
    }
    text.pop_back();
    return text + ",\"trades\":[],\"equity_curve\":[],\"report_sink\":{\"path\":\"child-locator\"}}\n";
}

std::vector<Row> fixtures(const std::string& directory, unsigned n = 41, bool equity_only = false) {
    const auto v1 = directory + "/input.v1", direct = directory + "/direct.pfs2";
    const sink::RecordSet records{!equity_only, !equity_only, !equity_only, equity_only ? 0 : 3, equity_only ? 0 : 2};
    pf_report_sink_v1 d{sizeof(pf_report_sink_v1), 1, v1.c_str(), "writer-input-reference", directory.c_str()};
    sink::LineWriter line(d);
    line.write_header(records);
    std::vector<Row> expected;
    if (equity_only) {
        for (unsigned j = 0; j < n; ++j)
            line.write_e({static_cast<std::int64_t>(j), 10000.0 + static_cast<double>(j % 64) / 4.0, -0.0});
    } else (void)feed_all(line, n, true);
    line.write_trailer();
    sink::BlockPolicy policy;
    policy.force_full = true; // direct path deliberately has a different physical layout
    policy.block_bytes = 4096;
    const unsigned caps[] = {3, 5, 3, 4, 1, 6, 7, 11};
    std::copy(std::begin(caps), std::end(caps), std::begin(policy.block_records));
    if (equity_only) {
        policy.block_records[0] = 512;
        policy.block_records[7] = 4096;
    }
    sink::BlockWriter writer(descriptor(direct, directory), policy);
    writer.write_header(records);
    if (equity_only) {
        for (unsigned j = 0; j < n; ++j) {
            const pf_equity_point_t e{static_cast<std::int64_t>(j), 10000.0 + static_cast<double>(j % 64) / 4.0, -0.0};
            writer.write_e(e);
            expected.push_back({'E', {i(e.time_ms), f(e.equity), f(e.open_profit)}});
        }
    } else expected = feed_all(writer, n, true);
    writer.write_trailer();
    const auto metadata = metadata_for_run(writer.receipt(), !equity_only);
    writer.finalize(metadata);
    text_file(directory + "/summary.json", metadata);
    text_file(directory + "/stdout.json", stdout_for_run(!equity_only));
    return expected;
}

sink::TranscodeResult convert(const std::string& directory, const char* name = "converted.pfs2",
                             std::optional<std::uint64_t> cap = {}) {
    InputFile input(directory + "/input.v1"), summary(directory + "/stdout.json");
    sink::TranscodeOptions options;
    options.in_fd = input.fd;
    options.summary_fd = summary.fd;
    options.out = directory + "/" + name;
    options.spool_dir = directory;
    options.run_id = "writer-input-reference";
    options.max_bytes = cap;
    const auto result = sink::transcode_v2(options);
    require(::fcntl(input.fd, F_GETFD) >= 0 && ::fcntl(summary.fd, F_GETFD) >= 0, "transcoder closed borrowed fd");
    return result;
}
void no_staging(const std::string& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        require(entry.path().filename().string().find(".pf-transcode-v2-") != 0, "private staging file leaked");
}
void roundtrip() {
    Directory directory;
    const auto expected = fixtures(directory.path);
    const auto result = convert(directory.path);
    const auto path = directory.file("converted.pfs2");
    const auto decoded = decode(read_file(path), all_types(3, 2));
    require(decoded.merged == expected, "independent decoded ETOVKXH fields differ");
    auto direct = reader::Reader::open(std::make_shared<Source>(directory.file("direct.pfs2")));
    auto transcoded = reader::Reader::open(std::make_shared<Source>(path));
    require(direct.verify().scope == reader::VerificationScope::whole_file, "direct verification");
    require(transcoded.verify().scope == reader::VerificationScope::whole_file, "transcoded verification");
    require(direct.logical_sha256().value == transcoded.logical_sha256().value, "logical identity differs from direct writer");
    require(transcoded.head().value.summary == text_file(directory.file("summary.json")), "SUMMARY changed captured bytes/order");
    require(decoded.logical_hex == result.receipt.sha256_hex, "receipt logical digest differs");
    auto a = direct.iter_merged(), b = transcoded.iter_merged();
    reader::Record left, right;
    std::array<std::uint64_t, 7> counts{};
    while (a.next(left)) {
        require(b.next(right), "transcoded population shorter");
        require(reader::record_json(left) == reader::record_json(right), "decoded rows differ from direct writer");
        ++counts[std::string_view("ETOVKXH").find(left.tag)];
    }
    require(!b.next(right), "transcoded population longer");
    require(std::all_of(counts.begin(), counts.end(), [](auto count) { return count != 0; }), "a record type was not tested");
    std::string v1;
    const auto inverse = transcoded.to_v1([&](std::string_view chunk) { v1.append(chunk); return true; });
    require(inverse.scope == reader::VerificationScope::whole_file && v1 == text_file(directory.file("input.v1")),
            "to_v1 is not byte identical");
    require(result.input_bytes == v1.size(), "input byte receipt");
    require(result.memory.finalization_bytes + result.memory.work_file_bytes <= 2097152, "finalization arena bound");
    struct stat status{};
    require(::stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600, "published mode is not owner-only");
    no_staging(directory.path);
}
std::string replace(std::string text, std::string_view old, std::string_view replacement) {
    const auto at = text.find(old);
    require(at != std::string::npos, "mutation missed its source");
    text.replace(at, old.size(), replacement);
    return text;
}
void failures() {
    Directory directory;
    (void)fixtures(directory.path);
    const auto original = text_file(directory.file("input.v1"));
    const auto report = text_file(directory.file("stdout.json"));
    const auto trailer = original.rfind("{\"trailer\"");
    require(trailer != std::string::npos, "no generated trailer");
    std::vector<std::pair<std::string, int>> cases{
        {original.substr(0, trailer), 6},
        {original.substr(0, original.size() - 2), 6},
        {replace(original, "[\"E\",1700000000000,0.0", "[\"E\",1700000000000,0"), 3},
        {replace(original, "\"equity\",\"open_profit\"", "\"wrong\",\"open_profit\""), 3},
        {replace(original, "\"long\"", "\"sideways\""), 3},
        {replace(original, "true", "1"), 3},
        {replace(original, "fedcba9876543210", "Fedcba9876543210"), 3},
        {replace(original, "\"entry_time\",\"exit_time\"", "\"exit_time\",\"entry_time\""), 3},
        {replace(original, "\"version\":1", "\"version\":1,\"version\":1"), 3},
        {replace(original, "[\"E\",1700000000000,0.0", "[\"E\",9007199254740992,0.0"), 3},
        {replace(original, "[\"E\",1700000000000,0.0", "[\"E\",1700000000000,1e999"), 3},
        {original + "\n", 3},
        {replace(original, "\"E\":41", "\"E\":42"), 4},
    };
    auto bad_hash = original;
    const auto digest = bad_hash.rfind("\"sha256\":\"") + std::string_view("\"sha256\":\"").size();
    bad_hash[digest] = bad_hash[digest] == '0' ? '1' : '0';
    cases.emplace_back(bad_hash, 4);
    cases.emplace_back(std::string(1048576, 'x') + "\n", 3);
    unsigned run = 0;
    for (const auto& test : cases) {
        text_file(directory.file("input.v1"), test.first);
        const auto name = "refused-" + std::to_string(run++);
        bool failed = false;
        try { (void)convert(directory.path, name.c_str()); }
        catch (const sink::TranscodeError& error) {
            require(error.status() == test.second, (std::string("wrong typed refusal: ") + error.what()).c_str());
            failed = true;
        }
        require(failed && !std::filesystem::exists(directory.path + "/" + name), "bad stream published a file");
        no_staging(directory.path);
    }
    text_file(directory.file("input.v1"), original);
    bool capped = false;
    try { (void)convert(directory.path, "capped", 0); }
    catch (const sink::TranscodeError& error) { capped = error.status() == 8; }
    require(capped && !std::filesystem::exists(directory.file("capped")), "max-bytes was not enforced");
    for (const auto& test : std::vector<std::pair<std::string, int>>{
            {replace(report, "\"elapsed_seconds\":1e-07", "\"elapsed_seconds\":NaN"), 3},
            {replace(report, "\"input\":{", "\"engine\":\"duplicate\",\"input\":{"), 3},
            {replace(report, "\"ratio\":null", "\"ratio\":1e999"), 3},
            {replace(report, "\"ratio\":null", "\"ratio\":\"\\ud800\""), 3},
            {replace(report, "\"ratio\":null", "\"ratio\":\"" + std::string(1048576, 'a') + "\""), 4},
            {replace(report, "\"ratio\":null", "\"ratio\":" + std::string(65, '[') + "0" + std::string(65, ']')), 3}}) {
        text_file(directory.file("stdout.json"), test.first);
        const auto name = "metadata-refused-" + std::to_string(run++);
        bool failed = false;
        try { (void)convert(directory.path, name.c_str()); }
        catch (const sink::TranscodeError& error) { failed = error.status() == test.second; }
        require(failed && !std::filesystem::exists(directory.path + "/" + name), "bad SUMMARY published");
        no_staging(directory.path);
    }
    text_file(directory.file("stdout.json"), report);
    text_file(directory.file("existing"), "caller-owned");
    bool exists = false;
    try { (void)convert(directory.path, "existing"); }
    catch (const sink::TranscodeError& error) { exists = error.status() == 5; }
    require(exists && text_file(directory.file("existing")) == "caller-owned", "existing output was damaged");
}
void empty_and_zero_width() {
    Directory directory;
    const auto v1 = directory.file("input.v1"), direct = directory.file("direct.pfs2");
    pf_report_sink_v1 d{sizeof(pf_report_sink_v1), 1, v1.c_str(), "writer-input-reference", directory.path.c_str()};
    sink::LineWriter line(d);
    line.write_header({true, false, false, 0, 0});
    line.write_k(nullptr, 0);
    line.write_trailer();
    sink::BlockWriter writer(descriptor(direct, directory.path));
    writer.write_header({true, false, false, 0, 0});
    writer.write_k(nullptr, 0);
    writer.write_trailer();
    const auto summary = metadata_for_run(writer.receipt(), false);
    writer.finalize(summary);
    text_file(directory.file("stdout.json"), summary);
    const auto result = convert(directory.path);
    require(std::strcmp(result.receipt.sha256_hex, writer.receipt().sha256_hex) == 0, "zero-width K identity");
    no_staging(directory.path);
}
void full_finalization() {
    Directory directory;
    (void)fixtures(directory.path, 12000, true); // above the canonical COMPACT E ceiling
    const auto result = convert(directory.path);
    require(result.receipt.count_e == 12000, "FULL fixture lost equity rows");
    require(result.memory.finalization_bytes != 0, "FULL finalization arena was not exercised");
    require(result.memory.finalization_bytes + result.memory.work_file_bytes <= 2097152, "FULL finalization arena bound");
    auto direct = reader::Reader::open(std::make_shared<Source>(directory.file("direct.pfs2")));
    auto converted = reader::Reader::open(std::make_shared<Source>(directory.file("converted.pfs2")));
    require(direct.verify().scope == reader::VerificationScope::whole_file, "FULL direct verification");
    require(converted.verify().scope == reader::VerificationScope::whole_file, "FULL converted verification");
    require(direct.logical_sha256().value == converted.logical_sha256().value, "FULL identity");
    no_staging(directory.path);
}
} // namespace

int main(int argc, char** argv) {
    if (!sink::sink_v2_available() || !reader::Reader::has_zstd()) {
        std::cout << "SKIP v2 transcoder requires the zstd encoder and reader\n";
        return 77;
    }
    try {
        if (argc == 3 && (std::string_view(argv[1]) == "--fixtures" || std::string_view(argv[1]) == "--equity-fixtures")) {
            (void)fixtures(argv[2], 41, std::string_view(argv[1]) == "--equity-fixtures");
            return 0;
        }
        require(argc == 1, "unexpected test arguments");
        roundtrip();
        failures();
        empty_and_zero_width();
        full_finalization();
        std::cout << "transcoder: direct identity, independent ETOVKXH, byte inverse, typed refusals, cleanup PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "transcoder test: " << error.what() << '\n';
        return 1;
    }
}
