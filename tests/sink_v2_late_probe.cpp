// TEST EXECUTABLE ONLY: real writer/validator/reader and repaired inode faults.
// No source adapter, decoder, worker or successful receipt is mocked.
#include "sink_v2_test_inputs.hpp"
#include "../tools/sink_v2/report_sink_reader.hpp"

#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <thread>

namespace sink = pineforge::report_sink;
namespace v2 = sink::v2;
namespace {
using namespace sink2_test;

void put(Bytes& file, std::size_t at, std::uint64_t value, unsigned width) {
    require(at <= file.size() && width <= file.size() - at, "fault store outside file");
    for (unsigned i = 0; i < width; ++i) file[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
struct Stage {
    std::string path;
    int fd;
    explicit Stage(const std::string& out) : path(out + ".probe-stage"),
        fd(::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) {
        require(fd >= 0, "probe staging open failed");
    }
    ~Stage() { if (fd >= 0) (void)::close(fd); (void)::unlink(path.c_str()); }
};
void replace(int fd, const Bytes& file) {
    for (std::size_t at = 0; at < file.size();) {
        const auto n = ::pwrite(fd, file.data() + at, file.size() - at, static_cast<off_t>(at));
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, "probe staging pwrite failed");
        at += static_cast<std::size_t>(n);
    }
}
std::size_t page_at(const Bytes& file, std::size_t root, unsigned table) {
    const auto locator = root + 512 + 64 * table;
    require(load(file.data() + locator + 8, 4) == 1, "probe requires one-page tables");
    return static_cast<std::size_t>(load(file.data() + locator, 8));
}
void seal_page(Bytes& file, std::size_t root, unsigned table) {
    const auto locator = root + 512 + 64 * table;
    const auto at = page_at(file, root, table);
    const auto length = static_cast<std::size_t>(load(file.data() + locator + 24, 4));
    require(at + length <= root && length >= 4096, "probe page range");
    std::string leaf("PFS2-INDEX\0", 11);
    leaf.append(bytes(file.data() + at, 24));
    leaf.append(bytes(file.data() + at + 4096, length - 4096));
    const auto digest = sha(leaf);
    std::copy(digest.begin(), digest.end(), file.begin() + static_cast<std::ptrdiff_t>(at + 24));
    std::copy(digest.begin(), digest.end(), file.begin() + static_cast<std::ptrdiff_t>(locator + 32));
    std::string checked(bytes(file.data() + at, 4092));
    checked.append(bytes(file.data() + at + 4096, length - 4096));
    put(file, at + 4092, crc(checked), 4);
}
void corrupt(int fd, const std::string& mode) {
    v2::FdSource source(fd);
    auto file = source.readRange(0, static_cast<std::size_t>(source.size()));
    const auto tail = file.size() - 160;
    const auto root = static_cast<std::size_t>(load(file.data() + tail, 8));
    require(load(file.data() + tail + 8, 4) == 4096, "probe requires FULL root");
    if (mode == "rank" || mode == "both") {
        const auto at = page_at(file, root, 8) + 4096;
        // A valid in-domain duplicate. The checksum/Merkle layer is repaired,
        // so the comparator/permutation unit must reject this independently.
        require(load(file.data() + at, 4) != load(file.data() + at + 4, 4),
                "positive rank positions were already duplicated");
        put(file, at + 4, load(file.data() + at, 4), 4);
        seal_page(file, root, 8);
        std::cerr << "rank fault sealed with independent CRC/SHA repair\n";
    }
    if (mode == "figure" || mode == "both") {
        const auto at = page_at(file, root, 18) + 4096;
        require(file[at] == 1, "probe expects a one-point figure bucket");
        // Keep ordinal/time/masks/selectors internally consistent. Only the
        // independently recomputed E witness reveals the wrong finite value.
        require(load(file.data() + at + 32, 8) != word(999.0), "figure fault would be a no-op");
        put(file, at + 32, word(999.0), 8);
        seal_page(file, root, 18);
        std::cerr << "figure fault sealed with independent CRC/SHA repair\n";
    }
    put(file, root + 4092, crc(bytes(file.data() + root, 4092)), 4);
    const auto root_sha = sha(bytes(file.data() + root, 4096));
    std::copy(root_sha.begin(), root_sha.end(), file.begin() + static_cast<std::ptrdiff_t>(tail + 24));
    const auto stored_sha = sha(bytes(file.data(), tail));
    std::copy(stored_sha.begin(), stored_sha.end(), file.begin() + static_cast<std::ptrdiff_t>(tail + 88));
    replace(fd, file);
}

void run(const std::string& mode, unsigned threads, const std::string& out) {
    Stage stage(out);
    sink::BlockPolicy policy;
    policy.threaded = false; policy.staging_fd = stage.fd; policy.force_full = true;
    policy.late_threads = threads;
    policy.block_records[0] = 4; policy.block_records[1] = 2; policy.block_records[7] = 5;
    sink::RecordSet records{};
    const std::string run_id = "late-worker-fault";
    const auto header = sink::v1_header_json(run_id, records) + "\n";
    v2::IncrementalValidator validator(policy, header);
    policy.observer = &validator;
    const pf_report_sink_v2 descriptor{sizeof(pf_report_sink_v2), 2, "borrowed-probe",
                                     run_id.c_str(), ".", 3, 1, 1, 0};
    {
        sink::BlockWriter writer(descriptor, policy);
        writer.write_header(records);
        for (unsigned j = 0; j < 16; ++j) {
            const auto equity = 2 + j % 3;
            const auto input = "[\"E\"," + std::to_string(j) + "," + std::to_string(equity) + ".0,0.0]\n";
            validator.input_line(input);
            writer.write_e({static_cast<std::int64_t>(j), static_cast<double>(equity), 0.0});
        }
        for (unsigned j = 0; j < 8; ++j) {
            pf_trade_t trade{};
            trade.is_long = true; trade.entry_time = j; trade.exit_time = 100 + j;
            trade.entry_price = 1.0; trade.exit_price = 2.0; trade.qty = 1.0;
            trade.exit_bar_index = 1;
            const auto input = "[\"T\"," + std::to_string(j + 1) + ",\"long\","
                + std::to_string(j) + "," + std::to_string(100 + j)
                + ",1.0,2.0,1.0,0.0,0.0,0.0,0.0,0.0,0,1,false," + std::to_string(j + 1)
                + ",1,\"\",\"\",\"\"]\n";
            validator.input_line(input);
            writer.write_t(j + 1, trade, j + 1, 1, "", "", "");
        }
        writer.write_trailer();
        // Preserve the input-independent proof across repaired footer faults.
        const auto proof = validator.finish(stage.fd, {{16,8,0,0,0,0,0}});
        require(proof.complete, "probe incremental positive control incomplete");
        writer.finalize(metadata_for(writer.receipt()));
        if (mode != "good") corrupt(stage.fd, mode);
        auto reader = v2::Reader::open(std::make_shared<v2::FdSource>(stage.fd));
        require(reader.verify_structure(proof, threads).scope == v2::VerificationScope::structure,
                "probe structure verification incomplete");
        require(!reader.verified().value, "structure promoted whole-file verified state");
        require(reader.verify().scope == v2::VerificationScope::whole_file,
                "probe positive full verification incomplete");
    }
    require(::fsync(stage.fd) == 0, "probe fsync failed");
    require(::close(stage.fd) == 0, "probe checked close failed");
    stage.fd = -1;
    // Same no-replace success boundary as the real tool, after verification.
    require(::link(stage.path.c_str(), out.c_str()) == 0, "probe publication failed");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 4) return 2; // MODE THREADS OUT
    const auto caller = std::this_thread::get_id();
    try {
        const std::string mode(argv[1]);
        if (mode != "good" && mode != "rank" && mode != "figure" && mode != "both") return 2;
        const auto threads = static_cast<unsigned>(std::stoul(argv[2]));
        if (!threads || threads > 4) return 2;
        run(mode, threads, argv[3]);
        std::cout << "{\"complete\":true}\n";
        return 0;
    } catch (const v2::ReaderError& error) {
        const auto status = error.failure() == v2::Failure::io ? 5 : 4;
        std::cout << "{\"complete\":false,\"exception_type\":\"ReaderError\",\"failure\":"
                  << static_cast<unsigned>(error.failure()) << ",\"worker_origin\":"
                  << (error.origin_thread() != caller ? "true" : "false")
                  << ",\"diagnostic\":" << sink::escaped_string(error.what()) << "}\n";
        return status;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 7;
    }
}
