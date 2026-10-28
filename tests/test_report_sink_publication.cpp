// A path sink publishes only a completed report. Real windows, source callback
// faults and filesystem rename errors exercise the production lifecycle.
// The success oracle spells version-1 bytes independently of the writer.
#include "report_sink_failure_fixture.hpp"
#include "../src/report_sink_writer.hpp"
#include "../src/sha256.hpp"

#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/version.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>

namespace {
using cr150_w1::Environment;
using cr150_w1::expect_engine_failure;
using cr150_w1::expect_failure;
using cr150_w1::install_path;
using cr150_w1::reason_args;
using cr150_w1::require;
using cr150_w1::run;
using cr150_w1::tape;
namespace rs = pineforge::report_sink;
namespace fs = std::filesystem;

constexpr std::size_t kWindow = 64 * 1024;
constexpr std::int64_t kTrades = 2048;
constexpr int kBars = 96;
constexpr int kSliceCount = 8;
constexpr char kRunId[] = "atomic-report";
constexpr char kCallbackFault[] = "late report callback fault";
constexpr char kPrivatePrefix[] = ".pf-report-sink-";
constexpr char kOldBytes[] = "previous report\nwith its original bytes\n";
const rs::RecordSet kRecords{false, false, false, 0, 0};
const std::string kEntryId(64, 'x');

struct Directory {
    // The CTest working directory must be disk-backed, as for every path sink.
    std::string path = "./report-publication-XXXXXX";
    Directory() { require(::mkdtemp(path.data()) != nullptr, "create publication directory"); }
    ~Directory() { std::error_code ignored; fs::remove_all(path, ignored); }
    std::string file() const { return path + "/report.sink"; }
};

std::vector<std::string> names(const std::string& directory) {
    std::vector<std::string> result;
    for (const auto& entry : fs::directory_iterator(directory))
        result.push_back(entry.path().filename().string());
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> private_files(const std::string& directory) {
    std::vector<std::string> result;
    for (const auto& name : names(directory)) {
        if (name.compare(0, sizeof(kPrivatePrefix) - 1, kPrivatePrefix) == 0)
            result.push_back(directory + "/" + name);
    }
    return result;
}

void absent(const std::string& path) {
    struct stat info{};
    require(::lstat(path.c_str(), &info) == -1 && errno == ENOENT,
            "an unpublished public path must be absent");
}

std::string contents(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "read publication artifact " + path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void seed(const std::string& path) {
    std::ofstream output(path, std::ios::binary);
    output << kOldBytes;
    output.close();
    require(output.good() && ::chmod(path.c_str(), 0640) == 0, "seed previous public report");
}

void unchanged(const Directory& directory, bool existing) {
    if (existing) require(contents(directory.file()) == kOldBytes, "previous report changed");
    else absent(directory.file());
    require(names(directory.path) == (existing ? std::vector<std::string>{"report.sink"}
                                              : std::vector<std::string>{}),
            "an in-progress file remains after cleanup");
}

pf_report_sink_v1 descriptor(const std::string& path) {
    return {sizeof(pf_report_sink_v1), 1, path.c_str(), kRunId, nullptr};
}

// No formatter, header builder, count or byte read from the writer supplies
// this oracle. All eight trade doubles are +0.0, with a long, closed zero row.
std::string header_line() {
    const std::string engine = PINEFORGE_VERSION_STRING;
    require(!engine.empty(), "the version oracle requires a nonempty version");
    for (const unsigned char c : engine)
        require(c >= 0x20 && c < 0x80 && c != '"' && c != '\\',
                "the version oracle requires plain ASCII without JSON escapes");
    return std::string("{\"schema\":\"pf-report-sink\",\"version\":1,\"run_id\":\"") + kRunId
        + "\",\"engine\":\"" + engine + "\",\"records\":["
        "{\"type\":\"E\",\"fields\":[\"time_ms\",\"equity\",\"open_profit\"],"
        "\"types\":[\"i64\",\"f64?\",\"f64?\"],"
        "\"units\":[\"unix_ms\",\"account_ccy\",\"account_ccy\"]},"
        "{\"type\":\"T\",\"fields\":[\"n\",\"side\",\"entry_time\",\"exit_time\",\"entry_price\","
        "\"exit_price\",\"qty\",\"pnl\",\"pnl_pct\",\"max_runup\",\"max_drawdown\",\"commission\","
        "\"entry_bar_index\",\"exit_bar_index\",\"open_at_end\",\"entry_incarnation\","
        "\"close_cause\",\"entry_id\",\"exit_id\",\"exit_comment\"],"
        "\"types\":[\"i64\",\"str\",\"i64\",\"i64\",\"f64?\",\"f64?\",\"f64?\",\"f64?\",\"f64?\","
        "\"f64?\",\"f64?\",\"f64?\",\"i64\",\"i64\",\"bool\",\"u64\",\"i64\",\"str\",\"str\",\"str\"],"
        "\"units\":[\"\",\"\",\"unix_ms\",\"unix_ms\",\"quote_ccy\",\"quote_ccy\",\"qty\","
        "\"account_ccy\",\"percent\",\"account_ccy\",\"account_ccy\",\"account_ccy\","
        "\"script_bar\",\"script_bar\",\"\",\"\",\"\",\"\",\"\",\"\"]}]}\n";
}

struct Expected {
    std::string prefix, digest, bytes;
    Expected() {
        rs::Sha256 vector;
        vector.update("abc");
        require(vector.hex_digest()
                    == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "FIPS SHA-256 known answer");
        prefix = header_line() + "[\"E\",0,1000.0,-0.0]\n[\"E\",60000,1001.0,0.0]\n";
        for (std::int64_t n = 1; n <= kTrades; ++n)
            prefix += "[\"T\"," + std::to_string(n)
                + ",\"long\",0,0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0,0,false,1,0,\""
                + kEntryId + "\",\"\",\"\"]\n";
        require(prefix.size() > 4 * kWindow, "the known answer must force real T windows");
        rs::Sha256 hash;
        hash.update(prefix);
        digest = hash.hex_digest();
        bytes = prefix + "{\"trailer\":\"pf-report-sink\",\"version\":1,\"counts\":{\"E\":2,\"T\":"
            + std::to_string(kTrades) + "},\"sha256\":\"" + digest + "\"}\n";
    }
};

void write_records(rs::LineWriter& writer) {
    writer.write_e({0, 1000.0, -0.0});
    writer.write_e({60000, 1001.0, 0.0});
    pf_trade_t trade{};
    trade.is_long = 1;
    for (std::int64_t n = 1; n <= kTrades; ++n)
        writer.write_t(n, trade, 1, 0, kEntryId, "", "");
}

void expect_receipt(const rs::Trailer& receipt, const Expected& expected) {
    require(receipt.count_e == 2 && receipt.count_t == kTrades
                && receipt.count_o == 0 && receipt.count_v == 0 && receipt.count_k == 0
                && receipt.count_x == 0 && receipt.count_h == 0
                && receipt.sha256_hex == expected.digest,
            "counts/digest differ from the independent version-1 receipt");
}

void expect_incomplete(rs::LineWriter& writer) {
    require(writer.diagnostics().complete == 0, "a failed path completed its diagnostics");
    bool refused = false;
    try { (void)writer.write_trailer(); }
    catch (const rs::WriterError& error) {
        refused = error.reason() == rs::WriterFailure::invalid_state;
    }
    require(refused, "a discarded path accepted a later trailer");
}

void expect_io(const rs::WriterError& error, int number, const char* operation) {
    require(error.reason() == rs::WriterFailure::io && error.error_number() == number,
            "publication failure lost its syscall errno");
    require(error.what() == std::string("report sink ") + operation + ": "
                + std::error_code(number, std::generic_category()).message(),
            "publication failure changed the writer's I/O diagnostic format");
    expect_failure(error.run_failure(), reason_args("sink_io"));
    expect_failure(pineforge::classify_run_failure(error), reason_args("sink_io"));
}

class FaultHost final : public pineforge::source::PineStrategyHost {
public:
    const Directory& directory;
    int callbacks = 0, raises = 0;
    std::int64_t streamed_trades = 0;
    bool disk_witness = false;
    explicit FaultHost(const Directory& directory) : directory(directory) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1000.0;
        config.default_qty_type = static_cast<int>(pineforge::QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.process_orders_on_close = false;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.01);
        set_trade_history_streamable(true);
        set_trace_enabled(true);
        install_path(*this, directory.file(), directory.path);
    }
    void prepare_script_run(const pineforge::Bar*, int, bool) override {
        trades_.set_scratch_directory(directory.path);
    }
    void on_source_bar(const pineforge::Bar&) override {
        const int index = callbacks++;
        if (index + 1 == kBars) {
            // The source boundary seals prior immutable trades before this
            // body. A bare native host retains T rows until report assembly;
            // its final callback cannot witness streamed T records.
            pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
            require(report_sink_receipt_v1(&receipt) == 0 && !receipt.complete
                        && receipt.count_t > 0 && closed_trade_count() > 0,
                    "real closed trades must stream before the late callback fault");
            streamed_trades = receipt.count_t;
            trace(std::string(3 * kWindow, 'p'), 7.0);
            const auto pending = private_files(directory.path);
            require(pending.size() <= 1, "one run opened more than one private sink");
            // The public fallback lets the unchanged writer reach the same
            // real fault for red replay, with positive disk evidence first.
            const auto bytes = contents(pending.empty() ? directory.file() : pending.front());
            disk_witness = bytes.size() >= 2 * kWindow
                && bytes.find("[\"T\",") != std::string::npos
                && bytes.find("{\"trailer\":") == std::string::npos;
            require(disk_witness, "late fault needs real T-bearing disk windows");
            ++raises;
            throw std::runtime_error(kCallbackFault);
        }
        if (index % 4 == 0) strategy_entry("roundtrip", true);
        if (index % 4 == 2) strategy_close("roundtrip");
    }
};

void callback_fault(const Directory& directory, bool existing) {
    if (existing) seed(directory.file());
    Environment scratch("PINEFORGE_COMMAND_SCRATCH_DIR", directory.path.c_str());
    Environment temporary("TMPDIR", directory.path.c_str());
    FaultHost host(directory);
    run(host, tape(kBars));
    require(host.callbacks == kBars && host.raises == 1 && host.streamed_trades > 0
                && host.disk_witness, "the intended late callback was not reached");
    expect_engine_failure(host, "{}", pineforge::RunFailureCode::engine_unclassified_error);
    require(host.last_error() == kCallbackFault, "cleanup replaced the original callback failure");
    // Check before any report read or host destruction can do lazy cleanup.
    unchanged(directory, existing);
    pineforge::ReportC report{};
    host.fill_report(&report);
    require(report.total_trades == 0 && report.trades == nullptr
                && report.equity_curve == nullptr && report.trace == nullptr,
            "the faulted run published a report");
    pineforge::BacktestEngine::free_report(&report);
    unchanged(directory, existing);
}

void row_fault(const Directory& directory) { callback_fault(directory, false); }
void row_existing(const Directory& directory) { callback_fault(directory, true); }

void row_success(const Directory& directory) {
    const Expected expected;
    // Both absent and existing public names must remain so until promotion.
    for (const bool existing : {false, true}) {
        if (existing) seed(directory.file());
        const auto path = directory.file();
        rs::LineWriter writer(descriptor(path));
        writer.write_header(kRecords);
        write_records(writer);
        if (existing) require(contents(path) == kOldBytes, "begin truncated the previous report");
        else absent(path);
        const auto pending = private_files(directory.path);
        require(pending.size() == 1, "the live path sink must have one private sibling");
        struct stat progress{};
        require(::stat(pending.front().c_str(), &progress) == 0
                    && (progress.st_mode & 0777) == 0600,
                "the private sibling must remain 0600 while records are written");
        const auto written = contents(pending.front());
        require(written.size() >= 4 * kWindow && written.size() % kWindow == 0
                    && written == expected.prefix.substr(0, written.size()),
                "live T windows differ from the independent prefix");
        const auto receipt = writer.write_trailer();
        expect_receipt(receipt, expected);
        require(contents(path) == expected.bytes, "published bytes differ from version 1");
        require(names(directory.path) == std::vector<std::string>{"report.sink"},
                "successful rename left an in-progress sibling");
        require(writer.diagnostics().complete == 1, "promoted path lacks complete diagnostics");
        const auto again = writer.write_trailer();
        expect_receipt(again, expected);
        require(contents(path) == expected.bytes, "repeated trailer changed published bytes");
        struct stat info{};
        require(::stat(path.c_str(), &info) == 0
                    && (info.st_mode & 0777) == (existing ? 0640 : 0600),
                "publication changed replacement permissions or the new-file 0600 mode");
        require(::unlink(path.c_str()) == 0, "remove completed fixture");
    }
}

void row_rename(const Directory& directory) {
    const auto path = directory.file();
    rs::LineWriter writer(descriptor(path));
    writer.write_header(kRecords);
    write_records(writer);
    // Put the obstacle in place only after begin and real window writes.
    // The unchanged writer already opened path; unlink that fd's name for
    // red replay so both writers face the same real directory obstacle.
    if (::unlink(path.c_str()) != 0) require(errno == ENOENT, "prepare rename obstacle");
    require(::mkdir(path.c_str(), 0700) == 0, "create target directory");
    bool caught = false;
    try { (void)writer.write_trailer(); }
    catch (const rs::WriterError& error) {
        caught = true;
        expect_io(error, EISDIR, "rename failed");
    }
    require(caught, "a rename onto a directory returned a successful trailer");
    require(names(directory.path) == std::vector<std::string>{"report.sink"}
                && fs::is_directory(path) && names(path).empty(),
            "rename failure changed its target or left a private file");
    expect_incomplete(writer);
}

void row_abandon(const Directory& directory) {
    for (const bool existing : {false, true}) {
        if (existing) seed(directory.file());
        const auto path = directory.file();
        rs::LineWriter writer(descriptor(path));
        writer.write_header(kRecords);
        write_records(writer);
        writer.abandon();
        unchanged(directory, existing);
        expect_incomplete(writer);
        if (existing) require(::unlink(path.c_str()) == 0, "remove previous fixture");
    }
}

void row_exceptions(const Directory& directory) {
    const auto path = directory.file();
    bool unwound = false;
    try {
        rs::LineWriter writer(descriptor(path));
        writer.write_header(kRecords);
        write_records(writer);
        throw std::runtime_error("owner unwound before finalization");
    } catch (const std::runtime_error& error) {
        unwound = std::strcmp(error.what(), "owner unwound before finalization") == 0;
    }
    require(unwound, "intended owner exception was not observed");
    unchanged(directory, false);
    seed(path);
    rs::LineWriter writer(descriptor(path));
    writer.write_header(kRecords);
    write_records(writer);
    pf_trade_t trade{};
    bool refused = false;
    try { writer.write_t(kTrades + 2, trade, 1, 0, "", "", ""); }
    catch (const rs::WriterError& error) {
        refused = error.reason() == rs::WriterFailure::invalid_input;
        expect_failure(error.run_failure(), "{}", pineforge::RunFailureCode::engine_invariant);
    }
    require(refused, "the real record-number validation did not fail");
    unchanged(directory, true);
    expect_incomplete(writer);
}

void row_moved_parent(const Directory& directory) {
    const auto live = directory.path + "/live";
    const auto moved = directory.path + "/moved";
    require(::mkdir(live.c_str(), 0700) == 0, "create sink parent");
    const auto path = live + "/report.sink";
    rs::LineWriter writer(descriptor(path));
    writer.write_header(kRecords);
    write_records(writer);
    require(::rename(live.c_str(), moved.c_str()) == 0, "move sink parent during run");
    bool caught = false;
    try { (void)writer.write_trailer(); }
    catch (const rs::WriterError& error) {
        caught = true;
        expect_io(error, ENOENT, "rename failed");
    }
    require(caught, "publication succeeded after its requested parent vanished");
    require(names(moved).empty(), "cleanup stranded a private file in the moved parent");
    absent(path);
    expect_incomplete(writer);
}

void row_independent(const Directory& directory) {
    seed(directory.file());
    const auto path = directory.file();
    const Expected expected;
    rs::LineWriter first(descriptor(path)), second(descriptor(path));
    first.write_header(kRecords);
    second.write_header(kRecords);
    write_records(first);
    write_records(second);
    const auto pending = private_files(directory.path);
    require(pending.size() == 2 && pending[0] != pending[1],
            "two writers did not obtain distinct private siblings");
    require(contents(path) == kOldBytes, "a concurrent writer changed the previous report");
    first.abandon();
    require(private_files(directory.path).size() == 1 && contents(path) == kOldBytes,
            "abandon removed another writer's private file");
    expect_receipt(second.write_trailer(), expected);
    require(contents(path) == expected.bytes
                && names(directory.path) == std::vector<std::string>{"report.sink"},
            "the surviving writer did not publish cleanly");
}
} // namespace

int main(int argc, char** argv) {
    int slice = 0;
    if (argc != 1) {
        if (argc != 3 || std::strcmp(argv[1], "--slice") != 0
            || std::strlen(argv[2]) != 3
            || argv[2][0] < '1' || argv[2][0] > '0' + kSliceCount
            || argv[2][1] != '/' || argv[2][2] != '0' + kSliceCount) {
            std::fprintf(stderr, "usage: test_report_sink_publication [--slice N/8]\n");
            return 2;
        }
        slice = argv[2][0] - '0';
    }
    try {
        Environment arena_fd("PF_SCRATCH_ARENA_FD", nullptr);
        Environment arena_bytes("PF_SCRATCH_ARENA_BYTES", nullptr);
        Environment window("PF_SCRATCH_WINDOW_BYTES", nullptr);
        const struct { int slice; const char* name; void (*run)(const Directory&); } rows[] = {
            {1, "fault", row_fault}, {2, "success", row_success},
            {3, "existing", row_existing}, {4, "rename", row_rename},
            {5, "abandon", row_abandon}, {6, "exceptions", row_exceptions},
            {7, "moved_parent", row_moved_parent}, {8, "independent", row_independent},
        };
        int executed = 0;
        for (const auto& row : rows) {
            if (slice != 0 && row.slice != slice) continue;
            Directory directory;
            row.run(directory);
            ++executed;
            std::printf("report sink publication: %s passed\n", row.name);
        }
        require(executed == (slice == 0 ? kSliceCount : 1), "publication rows did not execute");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_report_sink_publication: %s\n", error.what());
        return 1;
    }
}
