// 1.7.0 review B1-09. The first bar submits 4,200 unmarketable requests,
// exceeding WorkingRequestCore's kBookMax (4,096). A path sink's scratch_dir
// must hold the order-core spills as well as the report scratch.
//
// Scratch files are unlinked immediately, so empty before/after directory
// listings are insufficient proof. Also inspect the new regular descriptors
// opened across submit: read their /proc/self/fd symlink text, never their
// contents or the symlink targets. Every such file must have opened in D.
// A second run makes TMPDIR a regular file (ENOTDIR even under root).
// On the unfixed head the first run exposes spills under TMPDIR; the second
// cannot finish submitting its book. Neither failure depends on permissions.
#include "p2cx_kernel_consumer_fixture.hpp"

#include <pineforge/native_host.hpp>
#include <pineforge/report_sink.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sys/stat.h>

namespace {

using namespace pineforge;
using p2cx_fixture::require;
namespace no = pineforge::native_order;

constexpr std::size_t kResidentRows = 4096; // WorkingRequestCore's kBookMax.
constexpr std::size_t kRows = 4200;
static_assert(kRows > kResidentRows, "the fixture must spill its live book");

struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
};

std::vector<int> regular_descriptors() {
    std::vector<int> result;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        const auto name = entry.path().filename().string();
        char* end = nullptr;
        const long value = std::strtol(name.c_str(), &end, 10);
        if (end == name.c_str() || *end != '\0' || value < 0
            || value > std::numeric_limits<int>::max()) continue;
        struct stat status{};
        const int descriptor = static_cast<int>(value);
        if (::fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode))
            result.push_back(descriptor);
    }
    return result;
}

class SpillHost final : public NativeStrategyHost {
public:
    std::size_t live_rows = 0;
    std::size_t accepted = 0;
    std::vector<std::string> spill_paths;

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (placed_) return;
        placed_ = true;
        const auto before = regular_descriptors();
        for (std::size_t index = 0; index < kRows; ++index) {
            no::Request request{no::Transact{1.0}, "rest", ""};
            request.trigger = no::Limit{50.0};
            const auto result = submit(request);
            require(result.status == no::SubmitStatus::Accepted && result.handle.has_value(),
                    "the spill fixture must accept every resting request");
            ++accepted;
        }
        live_rows = native_working_requests().size();
        for (const int descriptor : regular_descriptors()) {
            if (std::find(before.begin(), before.end(), descriptor) != before.end()) continue;
            spill_paths.push_back(std::filesystem::read_symlink(
                "/proc/self/fd/" + std::to_string(descriptor)).string());
        }
    }

private:
    bool placed_ = false;
};

NativeRunSpec spec() {
    NativeRunSpec result;
    result.identity = {"b1-09-core-spill-directory", 1};
    result.input_tf = result.script_tf = "5";
    result.tickerid = "TEST:B109";
    result.timezone = "UTC";
    result.session = "24x7";
    result.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    result.initial_capital = 1.0e9;
    result.point_value = result.account_fx = 1.0;
    result.price_tick = 0.25;
    result.report_policy = NativeReportPolicy::KernelRecorded;
    return result;
}

void run(bool unusable_tmpdir) {
    p2cx_fixture::Directory directory("B1-09");
    const auto temporary = directory.child("TMPDIR");
    const auto scratch = directory.child("sink-scratch");
    std::filesystem::create_directory(temporary);
    std::filesystem::create_directory(scratch);
    std::string tmpdir = temporary;
    if (unusable_tmpdir) {
        tmpdir += "/not-a-directory";
        std::ofstream blocker(tmpdir, std::ios::binary);
        require(static_cast<bool>(blocker), "create deterministic unusable TMPDIR");
        blocker << "not a directory\n";
        blocker.close();
        require(static_cast<bool>(blocker), "finish deterministic unusable TMPDIR");
    }
    const auto tmp_before = p2cx_fixture::entries(temporary, "before TMPDIR");
    const auto scratch_before = p2cx_fixture::entries(scratch, "before scratch_dir");
    require(scratch_before.empty(), "scratch_dir starts empty");
    p2cx_fixture::Tmpdir guard(tmpdir);
    SpillHost host;
    require(host.configure_native(spec()).status == NativeSetupStatus::Applied,
            "configure spill fixture");
    const auto output = directory.child("run.ndjson");
    const pf_report_sink_v1 sink{
        sizeof sink, 1, output.c_str(), "B1-09", scratch.c_str()};
    require(host.set_report_sink_v1(&sink) == 0, "install path sink with scratch_dir");
    constexpr std::int64_t t0 = 1736121600000LL;
    const Bar bars[] = {
        {100.0, 100.0, 100.0, 100.0, 1.0, t0},
        {100.0, 100.0, 100.0, 100.0, 1.0, t0 + 300000},
    };
    host.run(bars, 2);
    const auto tmp_after = p2cx_fixture::entries(temporary, "after TMPDIR");
    const auto scratch_after = p2cx_fixture::entries(scratch, "after scratch_dir");
    for (const auto& path : host.spill_paths)
        std::fprintf(stderr, "order-core spill: %s\n", path.c_str());
    if (!host.last_error().empty())
        std::fprintf(stderr, "run failure: %s\n", host.last_error().c_str());
    require(host.native_state().kind == NativeLifecycleKind::Completed,
            "a spilling run must complete using the sink's scratch_dir");
    require(host.accepted == kRows && host.live_rows == kRows,
            "all 4,200 requests must remain live, beyond the resident book");
    require(!host.spill_paths.empty(), "the callback must witness actual order-core spill files");
    for (const auto& path : host.spill_paths)
        require(path.compare(0, scratch.size() + 1, scratch + '/') == 0,
                "an order-core spill escaped scratch_dir");
    require(tmp_after == tmp_before, "TMPDIR directory entries must remain unchanged");
    require(scratch_after == scratch_before, "scratch spill names must be unlinked");
    Report report;
    host.fill_report(&report.value); // Successful report fill finishes the trailer.
    require(host.last_error().empty(), "the spill fixture's final report must succeed");
    pf_report_sink_receipt_v1 receipt{};
    receipt.struct_size = sizeof receipt;
    receipt.version = 1;
    require(host.report_sink_receipt_v1(&receipt) == 0 && receipt.complete == 1,
            "the spilling run must publish a complete sink receipt");
}

} // namespace

int main() {
    try {
        run(false);
        run(true);
        std::puts("test_core_spills_follow_sink_scratch_dir: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_core_spills_follow_sink_scratch_dir: FAIL: %s\n",
                     error.what());
        return 1;
    }
}
