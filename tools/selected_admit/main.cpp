#include "identity.hpp"
#include "stage_selected.hpp"
#if PF_SELECTED_ADMIT_HAVE_STAGE_SOURCE
#include "stage_source.hpp"
#endif

#include <csignal>
#include <cstring>

namespace {
int usage() {
    (void)pineforge::selected_admit::write_all(STDERR_FILENO,
        "usage: pineforge-selected-admit --describe\n"
        "       pineforge-selected-admit --stage source --descriptor-fd D --descriptor-sha256 H\n"
        "       pineforge-selected-admit --stage selected --descriptor-fd D --descriptor-sha256 H\n");
    return pineforge::selected_admit::kUsage;
}
}  // namespace

namespace {
int run_application(int argc, char** argv) {
    namespace sa = pineforge::selected_admit;
    std::signal(SIGPIPE, SIG_IGN);
    const sa::Seam seam{};
    sa::Outcome out;
    out.descriptor.compiled_sha = std::string(sa::compiled_git_sha());
    try {
        if (argc == 2 && std::strcmp(argv[1], "--describe") == 0) {
            if (sa::write_all(STDOUT_FILENO, sa::describe(), seam)) return 0;
            (void)sa::write_all(STDERR_FILENO, "helper:result_write_failed\n");
            return sa::kIo;
        }
        if (argc != 7 || std::strcmp(argv[1], "--stage") != 0
            || std::strcmp(argv[3], "--descriptor-fd") != 0
            || std::strcmp(argv[5], "--descriptor-sha256") != 0)
            return usage();
        sa::Stage stage;
        if (std::strcmp(argv[2], "selected") == 0) stage = sa::Stage::Selected;
#if PF_SELECTED_ADMIT_HAVE_STAGE_SOURCE
        else if (std::strcmp(argv[2], "source") == 0) stage = sa::Stage::Source;
#endif
        else return usage();
        out.descriptor.stage = stage;
        if (stage == sa::Stage::Source)
            out.forward_sha256 = pineforge::report_sink::Sha256{}.hex_digest();
        int descriptor_fd;
        try { descriptor_fd = sa::descriptor_number(argv[4]); }
        catch (const std::bad_alloc&) { throw; }
        catch (const std::exception&) { return usage(); }
        if (!sa::lower_hex(argv[6], 64, 64)) return usage();
        out.descriptor.descriptor_sha256 = argv[6];
        std::string bytes, why;
        auto decoded = sa::read_descriptor_snapshot(descriptor_fd, argv[6], bytes, why, seam);
        if (decoded == sa::DecodeStatus::Ok) {
            decoded = sa::decode_descriptor(bytes, stage, sa::compiled_git_sha(),
                                             out.descriptor, why, descriptor_fd);
            out.descriptor.descriptor_sha256 = argv[6];
        }
        if (decoded != sa::DecodeStatus::Ok) {
            out.exit = decoded == sa::DecodeStatus::UnknownVersion
                    || decoded == sa::DecodeStatus::HelperMismatch ? sa::kVersion : sa::kDescriptor;
            out.status = "input_error";
            out.reason = why;
            return sa::emit_result(out, STDOUT_FILENO, seam);
        }
#if PF_SELECTED_ADMIT_HAVE_STAGE_SOURCE
        if (stage == sa::Stage::Source)
            return sa::run_stage_source(out.descriptor, STDOUT_FILENO, seam);
#endif
        return sa::run_stage_selected(out.descriptor, STDOUT_FILENO, seam);
    } catch (const std::exception& error) {
        sa::exception_outcome(out, error);
    } catch (...) {
        out.exit = sa::kInternal;
        out.status = "internal_error";
        out.reason = "helper:internal_error";
    }
    try { return sa::emit_result(out, STDOUT_FILENO, seam); }
    catch (...) { return sa::kInternal; }
}
}  // namespace

int main(int argc, char** argv) {
    try { return run_application(argc, argv); }
    catch (const std::bad_alloc&) {
        (void)pineforge::selected_admit::write_all(STDERR_FILENO, "helper:bad_alloc\n");
    } catch (...) {
        (void)pineforge::selected_admit::write_all(STDERR_FILENO, "helper:internal_error\n");
    }
    return pineforge::selected_admit::kInternal;
}
