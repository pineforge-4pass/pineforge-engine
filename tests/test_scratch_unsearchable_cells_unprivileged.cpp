// The pipe counts cells even when each() runs the mode-0000 checks in a child.
// At round 5, root receives only three cells, so this row is RED by reading.
#include "scratch_invalid_directory_fixture.hpp"

namespace {
void row() {
    using namespace s4e_tmp;
    PlainEnvironment environment;
    InvalidDirectories directories;
    int ends[2];
    require(::pipe(ends) == 0, "cannot create permission-cell receipt pipe");
    Fd reader(ends[0]), writer(ends[1]);
    directories.each([&](const std::string& path, int number) {
        if (number == EACCES) {
            require(::geteuid() != 0, "mode-0000 cells must run without root DAC override");
            std::uint64_t type = 0;
            require(pineforge::detail::scratch_directory_filesystem_type(path.c_str(), type)
                        == EACCES,
                    "the real directory probe must execute the search-permission check");
            Environment temporary("TMPDIR", path.c_str());
            pineforge::detail::ScratchFile file;
            expect_directory_error([&] { file.open("pineforge-unsearchable"); }, EACCES,
                                   pineforge::RunFailureCode::report_sink_failed, true);
            expect_directory_error(
                [] { (void)pineforge::detail::make_command_scratch_backing(4096); }, EACCES,
                pineforge::RunFailureCode::scratch_io_failed, false);
        }
        const char receipt = number == EACCES ? 'P' : 'N';
        ssize_t written;
        do { written = ::write(writer.value, &receipt, 1); } while (written < 0 && errno == EINTR);
        require(written == 1, "cannot record a completed permission cell");
    });
    writer.close();
    int permission_cells = 0, other_cells = 0;
    for (;;) {
        char receipt = 0;
        ssize_t count;
        do { count = ::read(reader.value, &receipt, 1); } while (count < 0 && errno == EINTR);
        require(count >= 0, "cannot read permission-cell receipts");
        if (count == 0) break;
        require(receipt == 'P' || receipt == 'N', "invalid permission-cell receipt");
        if (receipt == 'P') ++permission_cells;
        else ++other_cells;
    }
    require(permission_cells == 2 && other_cells == 3,
            "both disk and tmpfs mode-0000 cells must execute on every host");
}
} // namespace

int main() {
    return s4e_tmp::test_main("test_scratch_unsearchable_cells_unprivileged", row);
}
