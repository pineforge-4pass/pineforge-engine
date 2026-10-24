// F-05: fixture evidence belongs to an owned temporary directory, cleaned
// after every successful row rather than written into the source checkout.
#include "report_modes_fix_fixture.hpp"
#include <cstdlib>

using namespace report_modes_fix;

int main() {
    std::string directory;
    const int setup = test_main("fixture temporary sink setup", [&] {
        directory = scratch_directory();
        const auto* configured = std::getenv("TMPDIR");
        const std::filesystem::path parent = configured && *configured ? configured : "/tmp";
        require(std::filesystem::canonical(directory).parent_path()
                    == std::filesystem::canonical(parent),
                "fixture directory is not beneath TMPDIR or /tmp");
        require(std::filesystem::path(directory).filename().string().find("pineforge-s4e-") == 0,
                "fixture directory lacks a per-run mkdtemp name");
        Host host;
        configure(host);
        const auto path = sink_path("temporary-lifecycle");
        install_sink(host, path);
        run(host, tape(8));
        Report report;
        host.fill_report(&report.value);
        require(receipt(host).complete == 1, "temporary sink did not complete");
        require_trailer(read_sink(path));
    });
    if (setup != 0) return setup;
    try {
        require(!std::filesystem::exists(directory),
                "successful fixture left its owned temporary directory behind");
        std::puts("test_s4e_fixture_worktree_clean: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_s4e_fixture_worktree_clean: FAIL: %s\n", error.what());
        return 1;
    }
}
