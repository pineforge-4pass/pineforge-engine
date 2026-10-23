// B2-08: validate path targets without following, blocking or truncating first.
#include "p2_sink_writer_fixture.hpp"
#include <sys/vfs.h>

namespace {
bool pretend_tmpfs = false;
bool queried_filesystem = false;
}

int p2_sink_fstatfs(int fd, struct statfs* info) {
    const int result = ::fstatfs(fd, info);
    if (result == 0 && pretend_tmpfs) {
        queried_filesystem = true;
        info->f_type = 0x01021994L;
    }
    return result;
}

#define fstatfs p2_sink_fstatfs
#include "../src/report_sink_writer.cpp"
#undef fstatfs

namespace {
void symlink_refused() {
    p2_sink::Directory directory;
    const auto target = directory.file("symlink-target");
    const auto link = directory.file("symlink-sink");
    const std::string original = "operator data must survive sink refusal\n";
    p2_sink::seed_file(target, original);
    p2_sink::require(::symlink(target.c_str(), link.c_str()) == 0, "create symlink fixture");
    const pf_report_sink_v1 sink{sizeof sink, 1, link.c_str(), "B2-08", nullptr};
    p2_sink::rs::LineWriter writer(sink);
    p2_sink::expect_io([&] { writer.write_header({false, false, false, 0, 0}); }, ELOOP);
    p2_sink::require(p2_sink::read_file(target) == original,
                     "refused symlink truncated its target");
}

void fifo_refused() {
    p2_sink::Directory directory;
    const auto path = directory.file("no-reader.fifo");
    p2_sink::require(::mkfifo(path.c_str(), 0600) == 0, "create no-reader FIFO");
    p2_sink::bounded_child("B2-08 FIFO open", [&] {
        const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "B2-08", nullptr};
        p2_sink::rs::LineWriter writer(sink);
        p2_sink::expect_io([&] { writer.write_header({false, false, false, 0, 0}); }, ENXIO);
    });
}

void ram_file_refused_without_truncation() {
    p2_sink::Directory directory;
    const auto path = directory.file("ram-file");
    const std::string original = "existing ram-file bytes\n";
    p2_sink::seed_file(path, original);
    pretend_tmpfs = true;
    queried_filesystem = false;
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "B2-08", nullptr};
    p2_sink::rs::LineWriter writer(sink);
    p2_sink::expect_io([&] { writer.write_header({false, false, false, 0, 0}); }, EINVAL);
    pretend_tmpfs = false;
    p2_sink::require(queried_filesystem, "tmpfs refusal seam was not reached");
    p2_sink::require(p2_sink::read_file(path) == original,
                     "tmpfs refusal truncated the operator's data");
}

void accepted_file_is_truncated() {
    p2_sink::Directory directory;
    const auto path = directory.file("disk-file");
    p2_sink::seed_file(path, std::string(8192, 'z'));
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "B2-08", nullptr};
    p2_sink::rs::LineWriter writer(sink);
    writer.write_header({false, false, false, 0, 0});
    p2_sink::require(p2_sink::read_file(path).empty(),
                     "accepted regular file was not truncated at begin");
    (void)writer.write_trailer();
    const auto bytes = p2_sink::read_file(path);
    p2_sink::require(bytes.find("{\"schema\":\"pf-report-sink\"") == 0
                         && bytes.find("{\"trailer\":") != std::string::npos
                         && bytes.back() == '\n',
                     "accepted regular file did not finish");
}
}

int main() {
    return p2_sink::test_main("B2-08 open validation", [] {
        symlink_refused();
        fifo_refused();
        ram_file_refused_without_truncation();
        accepted_file_is_truncated();
    });
}
