#pragma once

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

// The same real Pine tape is linked three ways: explicit no-history, explicit
// history, and no capability symbol. No new engine API is needed for RED.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#if defined(__linux__)
#include <sys/mman.h>
#elif defined(__APPLE__)
#include <libproc.h>
#endif

namespace pineforge::detail {
struct TradeLedgerStorageProbe {
    template<class Ledger>
    static auto bytes(const Ledger& ledger, int) -> decltype(ledger.scratch_storage_bytes()) {
        return ledger.scratch_storage_bytes();
    }
    // Frozen-header fallback. On Linux the old anonymous files retain their
    // mkstemp prefix in /proc; arena RED observes the whole unreclaimed arena.
    template<class Ledger>
    static std::array<std::uint64_t, 2> bytes(const Ledger&, long) {
        if (const auto* arena = ScratchArena::attached())
            return {arena->stats().bytes_used, arena->stats().bytes_used};
        std::array<std::uint64_t, 2> result{};
        for (int fd = 0; fd < 1024; ++fd) {
            std::array<char, 4096> path{};
#if defined(__linux__)
            const auto link = std::string("/proc/self/fd/") + std::to_string(fd);
            const auto count = ::readlink(link.c_str(), path.data(), path.size() - 1);
            if (count <= 0) continue;
#elif defined(__APPLE__)
            if (::fcntl(fd, F_GETPATH, path.data()) != 0) continue;
#else
            continue;
#endif
            struct stat info{};
            if (::fstat(fd, &info) != 0) continue;
            if (std::strstr(path.data(), "pineforge-trade-index"))
                result[0] += static_cast<std::uint64_t>(info.st_size);
            if (std::strstr(path.data(), "pineforge-trade-payload"))
                result[1] += static_cast<std::uint64_t>(info.st_size);
        }
        return result;
    }
};
} // namespace pineforge::detail

namespace trade_retirement_fixture {
using namespace pineforge;
constexpr std::int64_t start_ms = INT64_C(1704067200000);
constexpr std::int64_t minute = 60000;
constexpr auto chunk = detail::ScratchArena::kChunkBytes;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();

inline void require(bool value, const char* text) {
    if (!value) throw std::runtime_error(text);
}
template<class Action>
void refused(Action action) {
    bool caught = false;
    try { action(); }
    catch (const std::logic_error& error) {
        const auto failure = classify_run_failure(error);
        caught = failure.code == RunFailureCode::engine_invariant
            && (!failure.args || *failure.args == "{}");
    }
    require(caught, "retired history read did not refuse with engine_invariant/{}");
}

struct Directory {
    std::string path;
    bool owned = true;
    Directory() {
        const char* root = std::getenv("TMPDIR");
        path = root && *root ? root : "/tmp";
        path += "/pineforge-trade-retirement-XXXXXX";
        require(::mkdtemp(path.data()) != nullptr, "cannot create fixture directory");
    }
    ~Directory() { if (owned) ::rmdir(path.c_str()); }
};

// Each population gets a fresh attachment: attachment-lifetime high water
// cannot be compared after three sequential runs on the same attachment.
struct Arena {
    bool active = false;
    explicit Arena(bool attach, const std::string& directory) {
        if (!attach) {
            require(!detail::ScratchArena::attached(), "disk row inherited an arena");
            return;
        }
        constexpr std::uint64_t bytes = 256 * chunk;
        int fd = -1;
#if defined(__linux__)
        fd = ::memfd_create("trade-retirement", MFD_CLOEXEC | MFD_ALLOW_SEALING);
#else
        std::string path = directory + "/arena-XXXXXX";
        fd = ::mkstemp(path.data());
        if (fd >= 0) ::unlink(path.c_str());
#endif
        require(fd >= 0, "cannot create arena backing");
        require(::ftruncate(fd, static_cast<off_t>(bytes)) == 0, "cannot size arena backing");
#if defined(__linux__)
        require(::fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0,
                "cannot seal arena backing");
#endif
        detail::ScratchArena::attach(fd, bytes);
        ::close(fd);
        active = true;
    }
    ~Arena() { if (active) detail::ScratchArena::detach(); }
};

// A real borrowed stream transport. Its reader never retains the wire in RAM.
struct Capture {
    int writer = -1;
    int reader = -1;
    int output = -1;
    int error = 0;
    std::thread thread;
    explicit Capture(const std::string& path) {
        int pair[2]{};
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair failed");
        reader = pair[0];
        writer = ::fcntl(pair[1], F_DUPFD_CLOEXEC, 32);
        ::close(pair[1]);
        output = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
        require(writer >= 0 && output >= 0, "capture setup failed");
        thread = std::thread([this] {
            std::array<char, 64 * 1024> data{};
            for (;;) {
                const auto count = ::read(reader, data.data(), data.size());
                if (count < 0 && errno == EINTR) continue;
                if (count == 0) break;
                if (count < 0) { error = errno; break; }
                ssize_t done = 0;
                while (done < count) {
                    const auto wrote = ::write(output, data.data() + done,
                                               static_cast<std::size_t>(count - done));
                    if (wrote < 0 && errno == EINTR) continue;
                    if (wrote <= 0) { error = wrote < 0 ? errno : EIO; return; }
                    done += wrote;
                }
            }
        });
    }
    void finish() {
        if (writer >= 0) {
            ::shutdown(writer, SHUT_WR);
            ::close(writer);
            writer = -1;
        }
        if (thread.joinable()) thread.join();
        require(error == 0, "stream capture failed");
    }
    ~Capture() {
        if (writer >= 0) { ::shutdown(writer, SHUT_WR); ::close(writer); }
        if (thread.joinable()) thread.join();
        if (reader >= 0) ::close(reader);
        if (output >= 0) ::close(output);
    }
};

class Strategy final : public source::PineStrategyHost {
public:
    Strategy(bool history, bool shapes, bool wide, std::FILE* observations,
             bool indexed_fields = false)
        : history_(history), shapes_(shapes), indexed_fields_(indexed_fields),
          comment_(wide ? 513 : 0, 'x'),
          observations_(observations) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000;
        config.default_qty_value = 1;
        config.margin_long = config.margin_short = 0;
        config.commission_value = 0;
        config.pyramiding = 2;
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        // Preserve the compiler's existing streamability decision for a real
        // strategy.closedtrades.* history reader, as well as the new flag.
        set_trade_history_streamable(!indexed_fields);
        set_broker_state_hash_recording(true);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        syminfo_mintick_ = 0.01;
    }
    std::array<std::uint64_t, 2> peak{};
    std::size_t callbacks = 0, applied = 0, closes = 0, old_reads = 0;
    std::size_t cancels = 0, replacements = 0, partials = 0, brackets = 0;
    std::size_t base() const { return trades_.base(); }
    void refuse_old_field() const { (void)closed_trade_profit(0); }
    void check_copy() const {
        TradeLedger copied(trades_);
        TradeLedger moved(std::move(copied));
        require(moved.base() == trades_.base() && moved.size() == trades_.size(),
                "retired ledger copy/move changed logical indices");
        for (std::size_t i = moved.base(); i < moved.size(); ++i) {
            const auto actual = moved.read(i), expected = trades_.read(i);
            require(actual.entry_time == expected.entry_time
                        && actual.exit_time == expected.exit_time
                        && actual.qty == expected.qty && actual.pnl == expected.pnl
                        && actual.entry_id == expected.entry_id
                        && actual.exit_id == expected.exit_id
                        && actual.entry_comment == expected.entry_comment
                        && actual.exit_comment == expected.exit_comment,
                    "retired ledger copy/move changed its retained suffix");
        }
        refused([&] { (void)moved.read(0); });
    }
    void sample() {
        const auto bytes = detail::TradeLedgerStorageProbe::bytes(trades_, 0);
        for (std::size_t i = 0; i < peak.size(); ++i) peak[i] = std::max(peak[i], bytes[i]);
    }
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        ++applied;
        closes += event.closed_trade_count;
        sample(); // observes the appended extent before the next seal
    }
    void on_source_bar(const Bar&) override {
        ++callbacks;
        sample();
        BrokerStateHashSink extension;
        hash_host_extension(extension);
        if (observations_)
            require(std::fprintf(observations_, "%016llx %016llx\n",
                        static_cast<unsigned long long>(broker_state_hash()),
                        static_cast<unsigned long long>(extension.h)) > 0,
                    "cannot write per-bar hashes");
        if (history_ && closed_trade_count() > 0) {
            const auto row = closed_trade(0); // deliberately far below base later
            require(row.entry_bar_index == 0 && row.qty == 1.0
                        && row.entry_price == 100 && row.pnl == 0,
                    "old indexed closed trade changed");
            ++old_reads;
        }
        if (indexed_fields_ && closed_trade_count() > 0) {
            // Actual Pine builtin plumbing, not just the public owning row.
            require(closed_trade_profit(0) == 0 && closed_trade_entry_bar_index(0) == 0
                        && closed_trade_entry_price(0) == 100
                        && closed_trade_size(0) == 1 && closed_trade_entry_id(0) == "flip",
                    "old Pine closedtrades fields changed");
        }
        const auto i = pine_bar_index();
        if (!shapes_) {
            strategy_entry("flip", i % 2 == 0, missing, missing, 1.0, comment_);
            return;
        }
        // Every six bars close two real slices. The canceled replacement must
        // never fill; the partial close leaves a live lot for the bracket.
        switch (i % 6) {
        case 0: strategy_entry("lot", true, missing, missing, 2.0, comment_); break;
        case 1:
            strategy_entry("pending", true, 50, missing, 1.0);
            strategy_entry("pending", true, 40, missing, 1.0);
            ++replacements;
            strategy_cancel("pending");
            ++cancels;
            break;
        case 2:
            strategy_close("lot", "partial", 1.0, missing, true);
            ++partials;
            break;
        case 3:
            strategy_exit("bracket", "lot", 100, 90, missing, missing,
                          missing, 100.0, "bracket", 1.0);
            ++brackets;
            break;
        case 5: strategy_close_all(); break;
        }
    }
    std::uint64_t final_extension() const {
        BrokerStateHashSink value;
        hash_host_extension(value);
        return value.h;
    }
private:
    bool history_, shapes_, indexed_fields_;
    std::string comment_;
    std::FILE* observations_;
};

struct Sample {
    std::array<std::uint64_t, 2> peak{};
    std::uint64_t arena_peak = 0;
};

inline Sample run(const std::string& directory, const std::string& name, int population,
                  bool reads_history, bool retiring, bool arena, bool fd, bool wide,
                  bool shapes = false, bool sink_on = true, bool keep = false,
                  bool check_refusal = false, bool indexed_fields = false) {
    Arena backing(arena, directory);
    const auto path = directory + "/" + name;
    auto* hashes = std::fopen((path + ".hashes").c_str(), "wb");
    require(hashes != nullptr, "cannot create observations");
    Sample result;
    {
        Strategy host(reads_history || !sink_on, shapes, wide, hashes, indexed_fields);
        std::unique_ptr<Capture> capture;
        if (sink_on) {
            if (fd) {
                capture = std::make_unique<Capture>(path + ".sink");
                const pf_report_sink_fd_v1 descriptor{
                    sizeof descriptor, 1, capture->writer, "trade-history"};
                require(host.set_report_sink_fd_v1(&descriptor) == 0, "fd sink refused");
            } else {
                const auto output = path + ".sink";
                const pf_report_sink_v1 descriptor{
                    sizeof descriptor, 1, output.c_str(), "trade-history", directory.c_str()};
                require(host.set_report_sink_v1(&descriptor) == 0, "path sink refused");
            }
        }
        std::vector<Bar> bars(static_cast<std::size_t>(population) + 1);
        for (std::size_t i = 0; i < bars.size(); ++i)
            bars[i] = {100, 100, 100, 100, 1, start_ms + static_cast<std::int64_t>(i) * minute};
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        require(host.last_run_status() == 0 && host.last_error().empty(), "Pine run failed");
        require(host.callbacks == bars.size() && host.applied > 0 && host.closes > 0,
                "fixture did not execute bars/fills/closes");
        if (!shapes) {
            require(host.closed_trade_count() == static_cast<std::size_t>(population)
                        && host.closes == static_cast<std::size_t>(population),
                    "N bars must close N-1 reversal trades");
        } else {
            require(host.cancels > 0 && host.replacements > 0 && host.partials > 0
                        && host.brackets > 0 && host.closes == 2 * (bars.size() / 6),
                    "cancel/replace/partial/bracket fixture did not close both slices");
        }
        if (sink_on && !indexed_fields)
            require(host.base() > 0, "fixture never published a closed prefix");
        else require(host.base() == 0, "no-sink run published a closed prefix");
        if (reads_history || !sink_on)
            require(host.old_reads > 0, "history parity row never read old history");
        ReportC report{};
        host.fill_report(&report);
        try {
            require(report.total_trades > 0 && report.net_profit == 0,
                    "independent zero-PnL model changed");
            if (!shapes)
                require(report.total_trades == population + 1,
                        "terminal open lot must add exactly one report-only T row");
            pf_report_sink_receipt_v1 receipt{};
            receipt.struct_size = sizeof receipt; receipt.version = 1;
            pf_report_sink_summary_v1 summary{};
            summary.struct_size = sizeof summary; summary.version = 1;
            if (sink_on) {
                require(report.trades == nullptr && report.trades_len == 0,
                        "sink materialized a report trade array");
                require(host.report_sink_receipt_v1(&receipt) == 0 && receipt.complete == 1
                            && host.report_sink_summary_v1(&summary) == 0 && summary.complete == 1,
                        "sink did not complete");
                require(receipt.count_t == report.total_trades
                            && receipt.count_e == static_cast<std::int64_t>(bars.size())
                            && receipt.count_h == static_cast<std::int64_t>(bars.size())
                            && summary.count_t == report.total_trades
                            && summary.wins == 0 && summary.losses == 0
                            && summary.best_trade == 0 && summary.worst_trade == 0
                            && summary.max_drawdown == 0 && summary.nonfinite_t_field == 0,
                        "independent trailer/summary counts or zero-PnL fold changed");
            }
            auto* scalars = std::fopen((path + ".report").c_str(), "wb");
            require(scalars != nullptr, "cannot create report observation");
            require(std::fwrite(&report.metrics, sizeof report.metrics, 1, scalars) == 1,
                    "cannot write report metrics");
            std::fprintf(scalars, "\n%d %.17g %016llx %016llx\n",
                         report.total_trades, report.net_profit,
                         static_cast<unsigned long long>(host.broker_state_hash()),
                         static_cast<unsigned long long>(host.final_extension()));
            require(std::fclose(scalars) == 0, "cannot finish report observation");
            if (sink_on && retiring && check_refusal) {
                refused([&] { (void)host.closed_trade(0); });
                refused([&] { (void)host.get_trade(0); });
                refused([&] { host.refuse_old_field(); });
                std::size_t cursor = host.base();
                Trade value;
                refused([&] { (void)host.previous_closed_trade(cursor, value); });
                require(cursor == host.base(), "refusal advanced the backward cursor");
                host.check_copy();
            } else if (!retiring || !sink_on) {
                const auto first = host.closed_trade(0);
                require(first.entry_bar_index == 0 && first.pnl == 0,
                        "retained first closed trade changed");
                if (sink_on && host.base() != 0) {
                    require(strategy_closed_trade_close_cause(
                                static_cast<BacktestEngine*>(&host), 0) == -1
                                && run_failure_value_of(host).code == RunFailureCode::none,
                            "legacy sealed metadata sentinel/failure behavior changed");
                }
            }
            if (sink_on) {
                ReportC repeated{};
                host.fill_report(&repeated);
                require(std::memcmp(&report.metrics, &repeated.metrics,
                                    sizeof report.metrics) == 0,
                        "repeated report read changed metrics");
                BacktestEngine::free_report(&repeated);
            }
            if (sink_on && retiring && check_refusal) {
                auto* handle = static_cast<BacktestEngine*>(&host);
                const auto typed = [&] {
                    require(std::strcmp(strategy_get_last_error_code(handle), "engine_invariant") == 0
                                && std::strcmp(strategy_get_last_error_args(handle), "{}") == 0,
                            "C history refusal did not preserve its typed failure");
                };
                require(strategy_closed_trade_entry_incarnation(handle, 0) == 0,
                        "retired incarnation accessor returned a value");
                typed();
                require(strategy_closed_trade_entry_id(handle, 0) == nullptr,
                        "retired entry-id accessor returned a value");
                typed();
                require(strategy_closed_trade_exit_id(handle, 0) == nullptr,
                        "retired exit-id accessor returned a value");
                typed();
                require(strategy_closed_trade_exit_comment(handle, 0) == nullptr,
                        "retired exit-comment accessor returned a value");
                typed();
                require(strategy_closed_trade_close_cause(handle, 0) == -1,
                        "retired close-cause accessor returned a value");
                typed();
            }
            host.sample();
            result.peak = host.peak;
            if (const auto* active = detail::ScratchArena::attached())
                result.arena_peak = active->stats().high_water;
        } catch (...) {
            BacktestEngine::free_report(&report);
            std::fclose(hashes);
            throw;
        }
        BacktestEngine::free_report(&report);
        if (capture) capture->finish();
    }
    require(std::fclose(hashes) == 0, "cannot finish per-bar observations");
    if (!keep) {
        ::unlink((path + ".sink").c_str());
        ::unlink((path + ".hashes").c_str());
        ::unlink((path + ".report").c_str());
    }
    return result;
}

inline int main_impl(int argc, char** argv, bool reads_history, bool retiring) {
    try {
        Directory temporary;
        if (argc == 3 && std::string(argv[1]) == "--artifacts") {
            const std::string directory = argv[2];
            run(directory, "reversal", 4000, reads_history, retiring, false, false, false,
                false, true, true);
            run(directory, "shapes", 599, reads_history, retiring, false, false, false,
                true, true, true);
            run(directory, "wide", 4000, reads_history, retiring, false, false, true,
                false, true, true);
            run(directory, "indexed", 4000, true, false, false, false, false,
                false, true, true, false, true);
            return 0;
        }
        require(argc == 1, "unexpected fixture arguments");
        for (const bool arena : {false, true}) {
            for (const bool fd : {false, true}) {
                for (const bool wide : {false, true}) {
                    std::array<Sample, 3> samples{};
                    const int populations[]{4000, 16000, 64000};
                    for (std::size_t i = 0; i < samples.size(); ++i) {
                        samples[i] = run(temporary.path, "population", populations[i],
                                         reads_history, retiring, arena, fd, wide);
                        std::printf("closed=%d arena=%d fd=%d wide=%d index=%llu payload=%llu total_arena_peak=%llu\n",
                            populations[i], arena, fd, wide,
                            static_cast<unsigned long long>(samples[i].peak[0]),
                            static_cast<unsigned long long>(samples[i].peak[1]),
                            static_cast<unsigned long long>(samples[i].arena_peak));
                    }
                    if (retiring) {
                        for (std::size_t axis = 0; axis < 2; ++axis) {
                            const auto bounds = std::minmax({samples[0].peak[axis],
                                samples[1].peak[axis], samples[2].peak[axis]});
                            require(bounds.second <= 2 * chunk + 64 * 1024
                                        && bounds.second - bounds.first <= chunk + 64 * 1024,
                                    "ledger scratch/arena high water is not flat in trades");
                        }
                    } else {
                        require(samples[1].peak[0] > samples[0].peak[0] + chunk
                                    && samples[2].peak[0] > samples[1].peak[0] + 4 * chunk,
                                "history-positive/absent ledger did not retain full descriptors");
                        if (wide)
                            require(samples[2].peak[1] > samples[0].peak[1] + 16 * chunk,
                                    "history-positive/absent ledger did not retain full text");
                    }
                }
            }
        }
        run(temporary.path, "shapes", 599, reads_history, retiring, false, false, false, true);
        if (retiring)
            run(temporary.path, "refusal", 4000, false, true, false, false, false,
                false, true, false, true);
        run(temporary.path, "no-sink", 4000, reads_history, false, false, false, false,
            false, false);
        run(temporary.path, "indexed", 4000, true, false, false, false, false,
            false, true, false, false, true);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
} // namespace trade_retirement_fixture
