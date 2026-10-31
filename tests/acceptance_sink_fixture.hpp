#pragma once

#include "acceptance_process_fixture.hpp"
#include <pineforge/engine.hpp>
#include <pineforge/report_sink.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace acceptance_sink {
struct Report {
    pineforge::ReportC value{};
    ~Report() { pineforge::BacktestEngine::free_report(&value); }
};

class File {
public:
    File() {
        const char* temporary = std::getenv("TMPDIR");
        directory_ = std::string(temporary && *temporary ? temporary : "/tmp")
            + "/pineforge-acceptance-XXXXXX";
        acceptance_process::require(::mkdtemp(directory_.data()) != nullptr,
                                    "cannot create acceptance sink directory");
        path_ = directory_ + "/report.ndjson";
    }
    ~File() {
        std::remove(path_.c_str());
        ::rmdir(directory_.c_str());
    }
    void install(pineforge::BacktestEngine& host, bool streamable) const {
        host.set_trade_history_streamable(streamable);
        const pf_report_sink_v1 sink{sizeof(pf_report_sink_v1), 1, path_.c_str(),
                                    "acceptance", directory_.c_str()};
        acceptance_process::require(host.set_report_sink_v1(&sink) == 0, "sink installation failed");
    }
    void completed(const pineforge::BacktestEngine& host) const {
        pf_report_sink_receipt_v1 receipt{};
        receipt.struct_size = sizeof receipt;
        receipt.version = 1;
        acceptance_process::require(host.report_sink_receipt_v1(&receipt) == 0
            && receipt.complete == 1 && receipt.count_t > 0, "sink did not publish complete trades");
        std::ifstream input(path_, std::ios::binary);
        acceptance_process::require(input.good() && input.peek() != std::ifstream::traits_type::eof(),
                                    "sink published no bytes");
    }
    std::string bytes() const {
        std::ifstream input(path_, std::ios::binary);
        acceptance_process::require(input.good(), "cannot read sink bytes");
        std::string result{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        acceptance_process::require(!input.bad() && !result.empty(), "incomplete sink read");
        return result;
    }
private:
    std::string directory_, path_;
};
} // namespace acceptance_sink
