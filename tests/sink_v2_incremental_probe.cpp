// TEST EXECUTABLE ONLY. No corruption implementation is linked into the tool.
#include "../tools/sink_v2/report_sink_transcode_v2.hpp"
#include "../tools/sink_v2/report_sink_reader.hpp"
#include "../tools/sink_v2/report_sink_v2_codec.hpp"
#include "../src/sha256.hpp"
#include "../tools/sink_v2/report_sink_phase_log.hpp"
#include "sink_v2_reader_data_cases.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

namespace sink = pineforge::report_sink;
namespace v2 = sink::v2;

namespace {
// Independent little-endian framing/CRC helpers for repaired fault controls.
namespace wire {
std::string_view view(const void* data, std::size_t size) {
    return {static_cast<const char*>(data), size};
}
template<class Buffer> std::string_view view(const Buffer& bytes) {
    return view(bytes.data(), bytes.size());
}
void put(std::uint8_t* out, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
std::uint64_t get(const std::uint8_t* in, unsigned width) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= std::uint64_t(in[i]) << (8 * i);
    return value;
}
class Crc {
    std::uint32_t value_ = UINT32_C(0xffffffff);
public:
    void update(std::string_view bytes) {
        for (const unsigned char c : bytes) {
            value_ ^= c;
            for (unsigned bit = 0; bit < 8; ++bit)
                value_ = (value_ >> 1) ^ ((value_ & 1) ? UINT32_C(0xedb88320) : 0);
        }
    }
    std::uint32_t digest() const { return value_ ^ UINT32_C(0xffffffff); }
};
std::uint32_t crc(std::string_view bytes) { Crc crc; crc.update(bytes); return crc.digest(); }
v2::Digest sha(std::string_view bytes) { sink::Sha256 hash; hash.update(bytes); return hash.digest(); }
} // namespace wire

class FlipCodec final : public sink::ColumnCodec {
    std::unique_ptr<sink::ColumnCodec> real_ = sink::make_column_codec();
    bool flipped_ = false;
public:
    void compress_column(std::string_view raw, int level, v2::Bytes& frame) override {
        if (!flipped_) {
            v2::Bytes wrong(raw.begin(), raw.end());
            wrong.back() ^= 1; // valid frame of WRONG encoded column bytes
            real_->compress_column(wire::view(wrong), level, frame);
            if (frame.size() < raw.size()) {
                flipped_ = true;
                std::cerr << "fault injected before incremental decoding\n";
                return;
            }
        }
        real_->compress_column(raw, level, frame);
    }
    sink::ColumnFrame inspect_frame(std::string_view frame) const override {
        return real_->inspect_frame(frame);
    }
    void compress_summary(std::string_view raw, int level, v2::Bytes& frame) override {
        real_->compress_summary(raw, level, frame);
    }
    void decompress_column(std::string_view raw, std::size_t n, v2::Bytes& decoded) override {
        real_->decompress_column(raw, n, decoded);
    }
    std::size_t workspace_bytes() const noexcept override { return real_->workspace_bytes(); }
};

class SwapInterleave final : public sink::BlockObserver {
    v2::IncrementalValidator& validator_;
    bool swap_;
public:
    std::uint64_t h_body = 0;
    explicit SwapInterleave(v2::IncrementalValidator& validator, bool swap = true)
        : validator_(validator), swap_(swap) {}
    void admitted(char tag) override { validator_.admitted(tag); }
    void block_written(int fd, std::uint64_t offset, std::uint32_t length,
                       const std::array<std::string_view, 4>& encoded) override {
        if (encoded[0][4] == 'H')
            h_body = offset + encoded[0].size() + encoded[1].size() + encoded[2].size();
        if (swap_ && encoded[0][4] == 'I') {
            v2::Bytes wrong;
            for (const auto part : encoded) wrong.insert(wrong.end(), part.begin(), part.end());
            if (wrong.size() != 82 || wrong[32] != 0 || wrong[80] != 'E' || wrong[81] != 'H')
                throw std::runtime_error("test I mutation did not reach the intended stored block");
            std::swap(wrong[80], wrong[81]);
            sink::Sha256 hash;
            wire::Crc crc;
            std::array<std::uint8_t, 17> prefix{};
            prefix[0] = 'I';
            wire::put(prefix.data() + 13, 2, 4); // column 0, rec0 0, n 2
            hash.update(std::string_view("PFS2-COL\0", 9));
            hash.update(wire::view(prefix)); hash.update(wire::view(wrong.data() + 80, 2));
            crc.update(std::string_view("PFS2-COL\0", 9));
            crc.update(wire::view(prefix)); crc.update(wire::view(wrong.data() + 80, 2));
            wire::put(wrong.data() + 44, crc.digest(), 4);
            const auto digest = hash.digest();
            std::copy(digest.begin(), digest.end(), wrong.begin() + 48);
            wire::Crc framing;
            framing.update(wire::view(wrong.data(), 28));
            framing.update(wire::view(wrong.data() + 32, 48));
            wire::put(wrong.data() + 28, framing.digest(), 4);
            if (::pwrite(fd, wrong.data(), wrong.size(), static_cast<off_t>(offset))
                    != static_cast<ssize_t>(wrong.size()))
                throw std::runtime_error("test I pwrite failed");
            std::cerr << "fault injected before incremental I comparison; column CRC/SHA repaired\n";
            // This is the deliberately wrong encoder result presented at
            // the observer seam, before validation, with the same inode bytes.
            validator_.block_written(fd, offset, length,
                {{wire::view(wrong.data(), 32), wire::view(wrong.data() + 32, 16),
                  wire::view(wrong.data() + 48, 32), wire::view(wrong.data() + 80, 2)}});
            return;
        }
        validator_.block_written(fd, offset, length, encoded);
    }
};

struct Staging {
    std::string path;
    int fd;
    explicit Staging(std::string name) : path(std::move(name)),
        fd(::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) {
        if (fd < 0) throw std::runtime_error("test staging open failed");
    }
    ~Staging() { (void)::close(fd); (void)::unlink(path.c_str()); }
};

void replace_bytes(int fd, std::uint64_t offset, std::string_view bytes) {
    std::size_t at = 0;
    while (at < bytes.size()) {
        const auto n = ::pwrite(fd, bytes.data() + at, bytes.size() - at,
                                static_cast<off_t>(offset + at));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("test mutation pwrite failed");
        at += static_cast<std::size_t>(n);
    }
}

#if defined(PINEFORGE_SINK_V2_PROBE_TEE)
class LateStorageCodec final : public sink::ColumnCodec {
    std::unique_ptr<sink::ColumnCodec> real_ = sink::make_column_codec();
    int fd_;
    sink::PhaseLog* phases_;
public:
    LateStorageCodec(int fd, sink::PhaseLog* phases) : fd_(fd), phases_(phases) {}
    void compress_column(std::string_view raw, int level, v2::Bytes& frame) override {
        real_->compress_column(raw, level, frame);
    }
    sink::ColumnFrame inspect_frame(std::string_view frame) const override { return real_->inspect_frame(frame); }
    void decompress_column(std::string_view raw, std::size_t n, v2::Bytes& decoded) override {
        real_->decompress_column(raw, n, decoded);
    }
    std::size_t workspace_bytes() const noexcept override { return real_->workspace_bytes(); }
    void compress_summary(std::string_view raw, int level, v2::Bytes& frame) override {
        real_->compress_summary(raw, level, frame);
        // This callback runs inside the real finalize(), AFTER validator.finish
        // and destruction, BEFORE stored_digest creates the tail. H payloads
        // are not decoded by footer construction or retained rank/figure reads.
        v2::FdSource source(fd_);
        const auto prefix = source.readRange(0,12);
        auto at = 16 + wire::get(prefix.data()+8,4);
        while (at < source.size()) {
            const auto h = source.readRange(at,32);
            if (std::string_view(reinterpret_cast<const char*>(h.data()),4) != "BLK2")
                throw std::runtime_error("test late codec did not find block framing");
            const auto length = 32 + wire::get(h.data()+24,4);
            if (length < 32 || length > source.size()-at)
                throw std::runtime_error("test late codec block length");
            if (h[4] == 'H') {
                const auto payload = 32 + 48 * wire::get(h.data()+8,4);
                if (payload >= length) throw std::runtime_error("test late codec missed H payload");
                auto byte = source.readRange(at+payload,1);
#if defined(PINEFORGE_SINK_V2_PROBE_CORRUPT)
                byte[0] ^= 1;
                replace_bytes(fd_, at+payload, wire::view(byte));
                std::cerr << "late fault injected in H after incremental validation and before tail\n";
                if (phases_) phases_->mark("test_late_storage_mutated");
#else
                std::cerr << "late codec positive control reached the same H payload\n";
                if (phases_) phases_->mark("test_late_storage_control");
#endif
                return;
            }
            at += length;
        }
        throw std::runtime_error("test late codec did not reach an H block");
    }
};
#endif

namespace fixture = sink_v2_reader_fixture;

// The reader prefixes each refusal with "sink v2: " (and the incremental path wraps a reader refusal as
// "incremental validation: sink v2: <text>"). Remove only that fixed prefix; the remaining text is compared exactly.
std::string without_reader_prefix(std::string text) {
    const std::string prefix = "sink v2: ";
    for (auto at = text.find(prefix); at != std::string::npos; at = text.find(prefix, at))
        text.erase(at, prefix.size());
    return text;
}

template<class F> void check_reader_result(F&& action, bool faulty, const std::string& expected,
                                          v2::Failure failure, const std::string& label) {
    try { action(); }
    catch (const v2::ReaderError& error) {
        const std::string actual = without_reader_prefix(error.what());
        if (!faulty || error.failure() != failure || actual != expected)
            throw std::runtime_error(label + ": unexpected refusal: " + error.what());
        std::cerr << label << ": " << error.what() << '\n';
        return;
    }
    if (faulty) throw std::runtime_error(label + ": defect was accepted");
    std::cerr << label << ": positive control passed\n";
}

v2::IncrementalProof replay(int fd, const fixture::Built& built, const fixture::Writer& config) {
    if (::ftruncate(fd, 0) < 0) throw std::runtime_error("test replay truncate failed");
    const auto head_end = 16 + fixture::get(built.bytes, 8, 4);
    replace_bytes(fd, 0, std::string_view(built.bytes).substr(0, head_end));
    sink::BlockPolicy policy;
    policy.threaded = false;
    policy.block_bytes = config.soft_bytes;
    for (std::size_t t=0; t<8; ++t) policy.block_records[t] = config.caps[t];
    v2::IncrementalValidator validator(policy, built.v1_header);
    sink::BlockObserver& observer = validator; // the actual production seam
    std::array<std::uint64_t,7> counts{};
    std::size_t admissions = 0, blocks = 0;
    for (const auto& event : built.replay) {
        if (event.kind == fixture::ReplayEvent::Kind::admission) {
            fixture::require(event.input == admissions++, "replay INPUT admission order");
            fixture::require(event.admissions > 0 && event.admissions <= config.caps.at(fixture::tags.find(event.tag)),
                             "replay INPUT admission count outside cap");
            for (unsigned n=0; n<event.admissions; ++n) {
                validator.input_line(built.input_lines.at(event.input));
                observer.admitted(event.tag);
                ++counts.at(fixture::tags.find(event.tag));
            }
        } else {
            const auto block = std::string_view(built.bytes).substr(event.offset, event.length);
            const auto cols = fixture::get(block, 8, 4);
            const auto dir = static_cast<std::size_t>(16 * cols);
            const auto shas = static_cast<std::size_t>(32 * cols);
            fixture::require(block[4] == event.tag && block.size() >= 32+dir+shas,
                             "replay block framing export");
            // Both inode bytes and supplied encoder views carry the defect
            // BEFORE block_written rereads/decodes them.
            replace_bytes(fd, event.offset, block);
            ++blocks;
            observer.block_written(fd, event.offset, event.length,
                {{block.substr(0,32), block.substr(32,dir), block.substr(32+dir,shas),
                  block.substr(32+dir+shas)}});
        }
    }
    const auto proof = validator.finish(fd, counts);
    fixture::require(proof.complete && admissions == built.input_lines.size()
        && proof.data_end == built.data_end
        && blocks == std::accumulate(proof.blocks.begin(), proof.blocks.end(), UINT64_C(0)),
        "positive replay did not finish all exported INPUT/blocks");
    return proof;
}

void reader_negatives(const std::string& out) {
    std::size_t number = 0;
    const auto cases = fixture::data_cases();
    fixture::require(cases.size() == 36, "reader DATA replay case lost");
    for (const auto& test : cases) {
        for (const bool faulty : {false, true}) {
            const auto built = test.build(faulty);
            Staging stage(out + "." + std::to_string(number++) + ".probe-stage");
            replace_bytes(stage.fd, 0, built.bytes);
            // Open is outside the refusal assertion: repaired framing/SUMMARY/
            // footer must pass, and the SAME bytes must reach full verify.
            {
                auto full = v2::Reader::open(std::make_shared<v2::FdSource>(stage.fd));
                check_reader_result([&] {
                    fixture::require(full.verify().scope == v2::VerificationScope::whole_file,
                                     "full control scope");
                }, faulty, test.diagnostic,
                test.integrity ? v2::Failure::integrity : v2::Failure::malformed,
                test.name + " / full");
            }
            const auto diagnostic = test.incremental_diagnostic.empty()
                ? test.diagnostic : test.incremental_diagnostic;
            check_reader_result([&] { (void)replay(stage.fd, built, test.control); }, faulty,
                "incremental validation: " + diagnostic,
                test.integrity || !test.incremental_diagnostic.empty()
                    ? v2::Failure::integrity : v2::Failure::malformed,
                test.name + " / incremental");
        }
    }
    std::cerr << "reader DATA replay: " << cases.size()
              << " negatives and " << cases.size() << " positive controls, full and incremental\n";
}

void mutate_descriptor_claim(int fd, const std::string& mode) {
    v2::FdSource source(fd);
    auto bytes = source.readRange(0, static_cast<std::size_t>(source.size()));
    const auto tail = bytes.size() - 160;
    const auto root = static_cast<std::size_t>(wire::get(bytes.data() + tail, 8));
    const auto root_length = static_cast<std::size_t>(wire::get(bytes.data() + tail + 8, 4));
    if (mode == "descriptor-count") {
        // FULL defers descriptor-population comparison to verify. COMPACT
        // compares descriptor record totals during open and cannot prove the
        // retained late count guard. Leave every physical block intact.
        if (root_length != 4096) throw std::runtime_error("test count mutation requires FULL");
        wire::put(bytes.data() + root + 32 + 6*8, 2, 8);
        wire::put(bytes.data() + root + 32 + 7*8, 3, 8);
        // Keep root-only logical consistency valid at open. The late walk
        // must independently compare actual descriptor totals with the claims.
        sink::Sha256 logical;
        logical.update(std::string_view("PFS2-LOGICAL\0", 13));
        logical.update(wire::view(bytes.data() + root + 384, 32));
        logical.update(wire::view(bytes.data() + root + 352, 32));
        for (std::size_t t=0; t<8; ++t) {
            const char tag = "ETOVKXHI"[t];
            logical.update(std::string_view(&tag, 1));
            logical.update(wire::view(bytes.data() + root + 32 + 8*t, 8));
            logical.update(wire::view(bytes.data() + root + 96 + 32*t, 32));
        }
        const auto identity = logical.digest();
        std::copy(identity.begin(), identity.end(), bytes.begin() + static_cast<std::ptrdiff_t>(tail + 56));
    } else {
        if (root_length != 320) throw std::runtime_error("test descriptor mutation requires COMPACT");
        const auto descriptors = static_cast<std::size_t>(wire::get(bytes.data() + root + 160, 8));
        const auto length = static_cast<std::size_t>(wire::get(bytes.data() + root + 168, 8));
        if (bytes[descriptors + 12] != 'E')
            throw std::runtime_error("test mutation missed E descriptor");
        if (mode == "descriptor") {
            wire::put(bytes.data() + descriptors + 32, 1, 8);
            wire::put(bytes.data() + descriptors + 40, 1, 8);
        } else if (mode == "descriptor-sha") {
            bytes[descriptors + 64] ^= 1; // e.sha, after validation; block is intact
        } else throw std::runtime_error("unknown descriptor fault");
        sink::Sha256 index;
        index.update(std::string_view("PFS2-CINDEX\0", 12));
        index.update(wire::view(bytes.data() + descriptors, length));
        const auto index_sha = index.digest();
        std::copy(index_sha.begin(), index_sha.end(), bytes.begin() + static_cast<std::ptrdiff_t>(root + 176));
    }
    wire::put(bytes.data() + root + root_length - 4,
              wire::crc(wire::view(bytes.data() + root, root_length - 4)), 4);
    const auto root_sha = wire::sha(wire::view(bytes.data() + root, root_length));
    std::copy(root_sha.begin(), root_sha.end(), bytes.begin() + static_cast<std::ptrdiff_t>(tail + 24));
    const auto stored_sha = wire::sha(wire::view(bytes.data(), tail));
    std::copy(stored_sha.begin(), stored_sha.end(), bytes.begin() + static_cast<std::ptrdiff_t>(tail + 88));
    replace_bytes(fd, 0, wire::view(bytes));
}

void direct(const std::string& mode, const std::string& out) {
    Staging stage(out + ".probe-stage");
    sink::BlockPolicy policy;
    policy.threaded = false; policy.staging_fd = stage.fd;
    policy.block_records[0] = 1;
    policy.block_records[6] = 2;
    policy.block_records[7] = 3;
    policy.force_full = mode == "descriptor-count" || mode == "structure-full-good";
    sink::RecordSet records{};
    records.hashes = true;
    const std::string run = "incremental-fault";
    const auto header = sink::v1_header_json(run, records) + "\n";
    v2::IncrementalValidator validator(policy, header);
    SwapInterleave swap(validator, mode == "interleave");
    policy.observer = &swap;
    const pf_report_sink_v2 descriptor{sizeof(pf_report_sink_v2), 2, "borrowed-test-stage",
                                     run.c_str(), ".", 3, 1, 1, 0};
    sink::BlockWriter writer(descriptor, policy);
    writer.write_header(records);
    validator.input_line("[\"E\",0,2.0,0.0]\n");
    writer.write_e({0, mode == "row" ? 3.0 : 2.0, 0.0});
    validator.input_line("[\"H\",\"0000000000000001\"]\n");
    writer.write_h(1);
    writer.write_trailer();
    const auto proof = validator.finish(stage.fd, {{1, 0, 0, 0, 0, 0, 1}});
    if (!proof.complete) throw std::runtime_error("test positive incremental proof incomplete");
    if (mode == "row" || mode == "interleave" || mode == "good") return;
    if (mode == "stored") {
        // Mutate H AFTER incremental validation but BEFORE stored_digest()
        // creates the tail. Footer reconstruction never decodes H.
        std::uint8_t byte = 0;
        if (!swap.h_body || ::pread(stage.fd, &byte, 1, static_cast<off_t>(swap.h_body)) != 1)
            throw std::runtime_error("test mutation missed written H payload");
        byte ^= 1;
        replace_bytes(stage.fd, swap.h_body, wire::view(&byte, 1));
    }
    writer.finalize("{\"engine\":\"pineforge\",\"input\":{},\"applied_inputs\":{},"
                    "\"applied_overrides\":{},\"applied_runtime\":{},\"elapsed_seconds\":0.0,"
                    "\"summary\":{},\"metrics\":{},\"diagnostics\":{},\"fingerprint\":null,"
                    "\"sink_folds\":{\"closed_count\":0,\"open_at_end_count\":0,\"open_at_end_pnl\":0.0}}");
    const bool descriptor_fault = mode == "descriptor" || mode == "descriptor-sha" || mode == "descriptor-count";
    if (descriptor_fault) mutate_descriptor_claim(stage.fd, mode);
    auto source = std::make_shared<v2::FdSource>(stage.fd);
    if (mode == "stored" || descriptor_fault) {
        auto full = v2::Reader::open(source);
        bool refused = false;
        try { (void)full.verify(); }
        catch (const v2::ReaderError& error) {
            const std::string text = without_reader_prefix(error.what());
            const auto expected = mode == "stored" ? "decoded column CRC mismatch"
                : mode == "descriptor-sha" ? "column SHA table/block SHA mismatch"
                : mode == "descriptor-count" ? "stream counts mismatch" : "descriptor time-range mismatch";
            const auto failure = mode == "descriptor-count"
                ? v2::Failure::malformed : v2::Failure::integrity;
            if (text != expected || error.failure() != failure) throw;
            refused = true;
            std::cerr << "full reader caught: " << text << '\n';
        }
        if (!refused) throw std::runtime_error("test negative unexpectedly passed full verify");
    }
    auto structural = v2::Reader::open(source);
    if (structural.verify_structure(proof).scope != v2::VerificationScope::structure
        || structural.verified().value)
        throw std::runtime_error("structure scope incorrectly promoted to whole_file");
    if (mode == "structure-good" || mode == "structure-full-good") {
        if (structural.verify().scope != v2::VerificationScope::whole_file || !structural.verified().value)
            throw std::runtime_error("positive full verification failed");
        auto missing = proof;
        missing.complete = false;
        bool refused = false;
        try { (void)structural.verify_structure(missing); }
        catch (const v2::ReaderError& error) {
            if (error.failure() != v2::Failure::integrity
                    || without_reader_prefix(error.what()) != "missing completed incremental validation") throw;
            refused = true;
        }
        if (!refused || structural.verified().value)
            throw std::runtime_error("incomplete proof retained verified state");
    }
}
} // namespace

#if defined(PINEFORGE_SINK_V2_PROBE_TEE)
namespace pineforge { namespace report_sink {
TranscodeResult tee_probe_transcode_v2(const TranscodeOptions& options) {
    if (!options.publish_none || options.staging_fd < 0
            || options.late_verify != TranscodeOptions::LateVerify::structure)
        throw TranscodeError(2, "test late codec requires tee structure mode");
    return transcode_v2(options, std::make_unique<LateStorageCodec>(options.staging_fd, options.phase_log));
}
}} // namespace pineforge::report_sink

// Reuse the real CLI's option validation, failure/ready receipts and exit
// mapping verbatim. Only this test build redirects its library call to the
// existing codec overload. No fault option/environment enters production.
#define transcode_v2 tee_probe_transcode_v2
#include "../tools/pf_sink_transcode_v2.cpp"
#undef transcode_v2
#else
int main(int argc, char** argv) {
    if (argc != 5) return 2; // MODE OUT SUMMARY PHASE_FD
    sink::PhaseLog phases(std::stoi(argv[4]));
    try {
        const std::string mode(argv[1]);
        if (mode == "codec") {
            const int summary = ::open(argv[3], O_RDONLY | O_CLOEXEC);
            if (summary < 0) throw std::runtime_error("test SUMMARY open failed");
            sink::TranscodeOptions options;
            options.out = argv[2]; options.summary_fd = summary; options.phase_log = &phases;
            options.late_verify = sink::TranscodeOptions::LateVerify::structure;
            try { (void)sink::transcode_v2(options, std::make_unique<FlipCodec>()); }
            catch (...) { (void)::close(summary); throw; }
            (void)::close(summary);
        } else if (mode == "reader-negatives") {
            reader_negatives(argv[2]);
        } else if (mode == "row" || mode == "interleave" || mode == "good"
                   || mode == "stored" || mode == "descriptor" || mode == "descriptor-sha"
                   || mode == "descriptor-count" || mode == "structure-good" || mode == "structure-full-good") {
            direct(mode, argv[2]);
        } else return 2;
        phases.status(0);
        std::cout << "{\"complete\":true}\n";
        return 0;
    } catch (const sink::TranscodeError& error) {
        phases.status(error.status());
        std::cout << "{\"complete\":false,\"reason\":"
                  << sink::escaped_string(sink::transcode_failure_reason(error.status()))
                  << ",\"diagnostic\":" << sink::escaped_string(error.what()) << "}\n";
        return error.status();
    } catch (const v2::ReaderError& error) {
        phases.status(4);
        std::cout << "{\"complete\":false,\"reason\":\"v2_input_mismatch\",\"diagnostic\":"
                  << sink::escaped_string(error.what()) << "}\n";
        return 4;
    } catch (const std::exception& error) {
        phases.status(7);
        std::cerr << error.what() << '\n';
        return 7;
    }
}
#endif
